//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/data_work.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace dagda::store {
namespace {

using dannan::contract::Lock;
using dannan::contract::TimePoint;
using dannan::sched::FrameType;
using dannan::sched::WorkClass;

// How long one wait sleeps before it looks again at what nothing notifies:
// a caller's cancellation flag.
constexpr std::chrono::milliseconds cancellation_poll{25};

// The wait from `now` to `until`, at least a millisecond, and no longer than
// the cancellation poll when the work can be cancelled.
std::chrono::nanoseconds slice(TimePoint now, TimePoint until, bool cancellable) {
  auto left = std::max<std::chrono::nanoseconds>(until - now, std::chrono::milliseconds(1));
  if (cancellable) left = std::min<std::chrono::nanoseconds>(left, cancellation_poll);
  return left;
}

}  // namespace

DataWorkContext::DataWorkContext(FrameType frame, uint64_t quantum_bytes, TimePoint deadline, std::atomic_bool* cancelled, const std::atomic_uint64_t* progress,
                                 std::chrono::milliseconds no_progress_budget)
    : WorkContext(frame, deadline, cancelled, "dagda.data"), _quantum_bytes(quantum_bytes), _progress(progress), _no_progress_budget(no_progress_budget) {
  if (dannan::sched::work_class(frame) == WorkClass::control) throw std::invalid_argument("control is not a DATA work class");
}

// ---- The DATA credit arbiter ------------------------------------------------

DataResourceArbiter::Lease::Lease(Lease&& other) noexcept : _owner(other._owner), _frame(other._frame), _bytes(other._bytes) {
  other._owner = nullptr;
  other._bytes = 0;
}

DataResourceArbiter::Lease& DataResourceArbiter::Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    reset();
    _owner = std::exchange(other._owner, nullptr);
    _frame = other._frame;
    _bytes = std::exchange(other._bytes, 0);
  }
  return *this;
}

void DataResourceArbiter::Lease::reset() {
  if (!_owner) return;
  _owner->release(_frame, _bytes);
  _owner = nullptr;
  _bytes = 0;
}

DataResourceArbiter::DataResourceArbiter(uint64_t capacity_bytes, uint64_t foreground_reserve_bytes, uint64_t background_concurrency,
                                         std::chrono::milliseconds no_progress_deadline, std::function<bool()> foreground_recently_active,
                                         std::optional<dannan::status::ScopedLogger> logger, const dannan::contract::TimeSource& time)
    : _capacity_bytes(capacity_bytes),
      _foreground_reserve_bytes(foreground_reserve_bytes),
      _background_concurrency(background_concurrency),
      _no_progress_deadline(no_progress_deadline),
      _foreground_recently_active(std::move(foreground_recently_active)),
      _logger(std::move(logger)),
      _time(time) {
  if (!_capacity_bytes || !_foreground_reserve_bytes || _foreground_reserve_bytes >= _capacity_bytes)
    throw std::invalid_argument("DATA resource capacity must exceed the foreground reserve");
  if (_no_progress_deadline <= std::chrono::milliseconds::zero()) throw std::invalid_argument("a DATA credit wait needs a no-progress deadline");
}

void DataResourceArbiter::observe_device(const DiskServiceMonitor* monitor, uint64_t min_background_under_pressure) {
  Lock lock(_mutex);
  _monitor = monitor;
  _min_background_under_pressure = std::max<uint64_t>(1, min_background_under_pressure);
}

bool DataResourceArbiter::foreground(FrameType frame) noexcept { return dannan::sched::work_class(frame) == WorkClass::foreground; }

bool DataResourceArbiter::available_locked(FrameType frame, uint64_t bytes, bool* refused_for_pressure) const {
  if (refused_for_pressure) *refused_for_pressure = false;
  if (_used_bytes > _capacity_bytes - bytes) return false;
  if (foreground(frame)) return true;
  if (_waiting_foreground) return false;
  // Law 3: the loader yields to pressure only when a foreground user is
  // present; a slow device nobody is reading from must not hold a bulk
  // write back. Speculative work yields to pressure alone. Presence is asked only
  // under pressure, keeping the uncontended path cheap.
  if (_monitor && _monitor->pressured() && _lower_active >= _min_background_under_pressure) {
    const bool present = _waiting_foreground > 0 || _used_bytes > _lower_used_bytes || (_foreground_recently_active && _foreground_recently_active());
    if (dannan::sched::work_class(frame) == WorkClass::speculative || present) {
      if (refused_for_pressure) *refused_for_pressure = true;
      return false;
    }
  }
  if (_background_concurrency && _lower_active >= _background_concurrency) return false;
  const auto lower_capacity = _capacity_bytes - _foreground_reserve_bytes;
  if (bytes > lower_capacity) return false;
  if (_lower_used_bytes > lower_capacity - bytes) return false;
  if (dannan::sched::work_class(frame) == WorkClass::speculative && _waiting_loaders) return false;
  return true;
}

DataResourceArbiter::Lease DataResourceArbiter::admit_locked(FrameType frame, uint64_t bytes) {
  _used_bytes += bytes;
  if (!foreground(frame)) {
    _lower_used_bytes += bytes;
    ++_lower_active;
    _peak_lower_active = std::max(_peak_lower_active, _lower_active);
  }
  _peak_used_bytes = std::max(_peak_used_bytes, _used_bytes);
  switch (dannan::sched::work_class(frame)) {
    case WorkClass::foreground:
      ++_foreground_admissions;
      break;
    case WorkClass::loader:
      ++_loader_admissions;
      break;
    default:
      ++_speculative_admissions;
      break;
  }
  return Lease(*this, frame, bytes);
}

std::optional<DataResourceArbiter::Lease> DataResourceArbiter::acquire(const DataWorkContext& work, uint64_t requested_bytes) {
  const auto frame = work.frame_type();
  if (work.work() == WorkClass::control) throw std::invalid_argument("control cannot acquire DATA resource credit");
  const auto bytes = charge(requested_bytes);
  const auto class_capacity = foreground(frame) ? _capacity_bytes : _capacity_bytes - _foreground_reserve_bytes;
  // Larger than the whole class budget: never admissible, so not waited for.
  if (bytes > class_capacity) return std::nullopt;
  std::string abandoned;
  {
    Lock lock(_mutex);
    const auto work_class = dannan::sched::work_class(frame);
    auto& waiters = work_class == WorkClass::foreground ? _waiting_foreground : work_class == WorkClass::loader ? _waiting_loaders : _waiting_speculative;
    bool counted = false;
    bool refused_for_pressure = false;
    auto window_start = _time.now();
    auto seen = _releases;
    const auto settled = [&]() DANNAN_REQUIRES(_mutex) {
      return _stopping || work.cancelled() || work.expired(_time.now()) || available_locked(frame, bytes, &refused_for_pressure);
    };
    while (!settled()) {
      if (!counted) {
        counted = true;
        // Once per waiter, not per wakeup: it counts work turned away.
        if (refused_for_pressure) ++_pressure_refusals;
        ++waiters;
        if (work_class == WorkClass::foreground)
          ++_foreground_waits;
        else if (work_class == WorkClass::loader)
          ++_loader_waits;
        else
          ++_speculative_waits;
      }
      const auto now = _time.now();
      if (work.deadline() == TimePoint{}) {
        // No-progress windows: any release anywhere re-arms the window, so
        // only a wholly stalled arbiter gives up.
        if (_releases != seen) {
          seen = _releases;
          window_start = now;
        } else if (now - window_start >= _no_progress_deadline) {
          ++_no_progress_failures;
          --waiters;
          _changed.notify_all();
          abandoned = "DATA credit wait abandoned after " + std::to_string(_no_progress_deadline.count()) +
                      " ms with no release anywhere: class=" + dannan::sched::frame_type_name(frame) + " bytes=" + std::to_string(bytes) +
                      " used=" + std::to_string(_used_bytes) + "/" + std::to_string(_capacity_bytes) + " background_active=" + std::to_string(_lower_active) +
                      "/" + std::to_string(_background_concurrency) + " waiting_foreground=" + std::to_string(_waiting_foreground) +
                      " waiting_loaders=" + std::to_string(_waiting_loaders);
          break;
        }
        _changed.wait_for(lock.native(), slice(now, window_start + _no_progress_deadline, work.cancellation() != nullptr));
      } else {
        _changed.wait_for(lock.native(), slice(now, work.deadline(), work.cancellation() != nullptr));
      }
    }
    if (abandoned.empty()) {
      if (counted) --waiters;
      if (_stopping || work.cancelled() || work.expired(_time.now())) {
        ++_cancelled_waits;
        _changed.notify_all();
        return std::nullopt;
      }
      return admit_locked(frame, bytes);
    }
  }
  if (_logger) _logger->warn(abandoned);
  return std::nullopt;
}

std::optional<DataResourceArbiter::Lease> DataResourceArbiter::try_acquire(const DataWorkContext& work, uint64_t requested_bytes) {
  const auto frame = work.frame_type();
  if (work.work() == WorkClass::control) throw std::invalid_argument("control cannot acquire DATA resource credit");
  const auto bytes = charge(requested_bytes);
  const auto class_capacity = foreground(frame) ? _capacity_bytes : _capacity_bytes - _foreground_reserve_bytes;
  if (bytes > class_capacity || work.cancelled() || work.expired(_time.now())) return std::nullopt;
  Lock lock(_mutex);
  bool refused_for_pressure = false;
  if (_stopping || !available_locked(frame, bytes, &refused_for_pressure)) {
    if (refused_for_pressure) ++_pressure_refusals;
    return std::nullopt;
  }
  return admit_locked(frame, bytes);
}

void DataResourceArbiter::release(FrameType frame, uint64_t bytes) {
  {
    Lock lock(_mutex);
    ++_releases;
    _used_bytes -= std::min(_used_bytes, bytes);
    if (!foreground(frame)) {
      _lower_used_bytes -= std::min(_lower_used_bytes, bytes);
      if (_lower_active) --_lower_active;
    }
  }
  _changed.notify_all();
}

void DataResourceArbiter::stop() {
  {
    Lock lock(_mutex);
    _stopping = true;
  }
  _changed.notify_all();
}

void DataResourceArbiter::time_advanced() {
  // Taken and released so a waiter between its check and its sleep sees
  // the notification.
  {
    Lock lock(_mutex);
  }
  _changed.notify_all();
}

DataResourceStats DataResourceArbiter::stats() const {
  Lock lock(_mutex);
  const auto device = _monitor ? _monitor->sample() : DiskServiceMonitor::Sample{};
  DataResourceStats out;
  out.capacity_bytes = _capacity_bytes;
  out.foreground_reserve_bytes = _foreground_reserve_bytes;
  out.used_bytes = _used_bytes;
  out.peak_used_bytes = _peak_used_bytes;
  out.foreground_admissions = _foreground_admissions;
  out.loader_admissions = _loader_admissions;
  out.speculative_admissions = _speculative_admissions;
  out.foreground_waits = _foreground_waits;
  out.loader_waits = _loader_waits;
  out.speculative_waits = _speculative_waits;
  out.cancelled_waits = _cancelled_waits;
  out.background_limit = _background_concurrency;
  out.background_active = _lower_active;
  out.peak_background_active = _peak_lower_active;
  out.device_pressured = device.pressured;
  out.device_service_us = device.mean_us;
  out.device_worst_us = device.worst_us;
  out.device_slowdown_percent = device.slowdown_percent;
  out.device_pressure_onsets = _monitor ? _monitor->pressure_onsets() : 0;
  out.pressure_refusals = _pressure_refusals;
  out.no_progress_failures = _no_progress_failures;
  return out;
}

// ---- The device I/O reserve ----------------------------------------------------

IoReserve::Slot& IoReserve::Slot::operator=(Slot&& other) noexcept {
  if (this != &other) {
    reset();
    _owner = std::exchange(other._owner, nullptr);
    _control = other._control;
  }
  return *this;
}

void IoReserve::Slot::reset() {
  if (!_owner) return;
  std::exchange(_owner, nullptr)->release(_control);
}

IoReserve::IoReserve(IoReserveOptions options, const dannan::contract::TimeSource& time) : _options(options), _time(time) {
  if (!_options.slots) throw std::invalid_argument("a device admits at least one operation");
  if (_options.control_reserve >= _options.slots) throw std::invalid_argument("the control reserve must leave DATA work a slot");
  Lock lock(_mutex);
  _stats.slots = _options.slots;
  _stats.control_reserve = _options.control_reserve;
}

std::optional<IoReserve::Slot> IoReserve::data(const DataWorkContext& work, std::chrono::milliseconds wait) {
  Lock lock(_mutex);
  auto until = _time.now() + wait;
  if (work.deadline() != TimePoint{}) until = std::min(until, work.deadline());
  const auto free = [&]() DANNAN_REQUIRES(_mutex) {
    return _stopping || work.cancelled() || (!_control_waiting && _data < data_share() && _data + _control < _options.slots);
  };
  bool counted = false;
  while (!free()) {
    const auto now = _time.now();
    if (now >= until) break;
    if (!counted) {
      counted = true;
      ++_stats.data_waits;
    }
    _changed.wait_for(lock.native(), slice(now, until, work.cancellation() != nullptr));
  }
  if (_stopping || work.cancelled() || !free()) {
    ++_stats.data_refusals;
    return std::nullopt;
  }
  ++_data;
  ++_stats.data_admissions;
  _stats.peak_data_in_flight = std::max(_stats.peak_data_in_flight, _data);
  return Slot(*this, false);
}

std::optional<IoReserve::Slot> IoReserve::try_data(const DataWorkContext& work) { return data(work, std::chrono::milliseconds::zero()); }

std::optional<IoReserve::Slot> IoReserve::control(std::chrono::milliseconds wait) {
  Lock lock(_mutex);
  const auto until = _time.now() + wait;
  const auto free = [&]() DANNAN_REQUIRES(_mutex) { return _stopping || _data + _control < _options.slots; };
  bool counted = false;
  while (!free()) {
    const auto now = _time.now();
    if (now >= until) break;
    if (!counted) {
      counted = true;
      ++_stats.control_waits;
      // While control work waits, DATA admission is closed (law 1).
      ++_control_waiting;
    }
    _changed.wait_for(lock.native(), slice(now, until, false));
  }
  if (counted) {
    --_control_waiting;
    _changed.notify_all();
  }
  if (_stopping || !free()) {
    ++_stats.control_refusals;
    return std::nullopt;
  }
  ++_control;
  ++_stats.control_admissions;
  _stats.peak_control_in_flight = std::max(_stats.peak_control_in_flight, _control);
  return Slot(*this, true);
}

std::optional<IoReserve::Slot> IoReserve::try_control() { return control(std::chrono::milliseconds::zero()); }

void IoReserve::release(bool control) {
  {
    Lock lock(_mutex);
    if (control) {
      if (_control) --_control;
    } else if (_data) {
      --_data;
    }
  }
  _changed.notify_all();
}

void IoReserve::stop() {
  {
    Lock lock(_mutex);
    _stopping = true;
  }
  _changed.notify_all();
}

void IoReserve::time_advanced() {
  {
    Lock lock(_mutex);
  }
  _changed.notify_all();
}

IoReserveStats IoReserve::stats() const {
  Lock lock(_mutex);
  auto out = _stats;
  out.data_in_flight = _data;
  out.control_in_flight = _control;
  return out;
}

std::optional<DataAdmission> admit_data(DataResourceArbiter* credit, IoReserve* io, FrameType frame, uint64_t bytes, std::chrono::milliseconds wait,
                                        std::atomic_bool* cancelled) {
  DataAdmission out;
  if (credit) {
    const bool waits = wait > std::chrono::milliseconds::zero();
    const DataWorkContext work(frame, bytes, waits ? credit->now() + wait : TimePoint{}, cancelled);
    out.credit = waits ? credit->acquire(work, bytes) : credit->try_acquire(work, bytes);
    if (!out.credit) return std::nullopt;
  }
  if (io) {
    out.io = io->data(DataWorkContext(frame, bytes, {}, cancelled), wait);
    if (!out.io) return std::nullopt;
  }
  if (!credit && !io) (void)DataWorkContext(frame, bytes);
  return out;
}

std::optional<IoReserve::Slot> admit_control(IoReserve* io, std::chrono::milliseconds wait) {
  if (!io) return IoReserve::Slot{};
  return io->control(wait);
}

}  // namespace dagda::store
