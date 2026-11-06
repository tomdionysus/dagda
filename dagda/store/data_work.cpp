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

// How often a wait looks at what nothing notifies: a caller's cancellation flag.
constexpr std::chrono::milliseconds cancellation_poll{25};

constexpr auto nothing_to_ask = [](auto&) { return false; };

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
      _time(time),
      _waits(&time) {
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
  if (_waits.waiting(WorkClass::foreground)) return false;
  // Law 3: the loader yields to pressure only when a foreground user is
  // present; a slow device nobody is reading from must not hold a bulk
  // write back. Speculative work yields to pressure alone. Presence is asked only
  // under pressure, keeping the uncontended path cheap.
  if (_monitor && _monitor->pressured() && _lower_active >= _min_background_under_pressure) {
    const bool present =
        _waits.waiting(WorkClass::foreground) > 0 || _used_bytes > _lower_used_bytes || (_foreground_recently_active && _foreground_recently_active());
    if (dannan::sched::work_class(frame) == WorkClass::speculative || present) {
      if (refused_for_pressure) *refused_for_pressure = true;
      return false;
    }
  }
  if (_background_concurrency && _lower_active >= _background_concurrency) return false;
  const auto lower_capacity = _capacity_bytes - _foreground_reserve_bytes;
  if (bytes > lower_capacity) return false;
  if (_lower_used_bytes > lower_capacity - bytes) return false;
  if (dannan::sched::work_class(frame) == WorkClass::speculative && _waits.waiting(WorkClass::loader)) return false;
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
  const auto work_class = dannan::sched::work_class(frame);
  std::string abandoned;
  {
    Lock lock(_mutex);
    bool refused_for_pressure = false;
    dannan::sched::ClassWaits::Bounds bounds;
    bounds.deadline = work.deadline();
    bounds.cancelled = work.cancellation();
    bounds.no_progress = _no_progress_deadline;
    if (work.cancellation()) bounds.cancellation_poll = cancellation_poll;
    const auto outcome = _waits.wait(
        lock, _changed, work_class, bounds, [&]() DANNAN_REQUIRES(_mutex) { return _stopping; },
        [&]() DANNAN_REQUIRES(_mutex) { return available_locked(frame, bytes, &refused_for_pressure); },
        // Once per waiter, not per wakeup: it counts work turned away.
        [&]() DANNAN_REQUIRES(_mutex) {
          if (refused_for_pressure) ++_pressure_refusals;
        },
        nothing_to_ask);
    if (outcome == dannan::sched::ClassWaits::Outcome::admitted) return admit_locked(frame, bytes);
    if (outcome == dannan::sched::ClassWaits::Outcome::gave_up) return std::nullopt;
    // Macha's line word for word, its field names too: what an operator's search finds.
    abandoned = "DATA credit wait abandoned after " + std::to_string(_no_progress_deadline.count()) +
                " ms with no release anywhere: class=" + dannan::sched::frame_type_name(frame) + " bytes=" + std::to_string(bytes) +
                " used=" + std::to_string(_used_bytes) + "/" + std::to_string(_capacity_bytes) + " lower_active=" + std::to_string(_lower_active) + "/" +
                std::to_string(_background_concurrency) + " waiting_viewers=" + std::to_string(_waits.waiting(WorkClass::foreground)) +
                " waiting_loaders=" + std::to_string(_waits.waiting(WorkClass::loader));
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
    _waits.released();
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
  out.foreground_waits = _waits.waits(WorkClass::foreground);
  out.loader_waits = _waits.waits(WorkClass::loader);
  out.speculative_waits = _waits.waits(WorkClass::speculative);
  out.cancelled_waits = _waits.gave_up();
  out.background_limit = _background_concurrency;
  out.background_active = _lower_active;
  out.peak_background_active = _peak_lower_active;
  out.device_pressured = device.pressured;
  out.device_service_us = device.mean_us;
  out.device_worst_us = device.worst_us;
  out.device_slowdown_percent = device.slowdown_percent;
  out.device_pressure_onsets = _monitor ? _monitor->pressure_onsets() : 0;
  out.pressure_refusals = _pressure_refusals;
  out.no_progress_failures = _waits.no_progress();
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

IoReserve::IoReserve(IoReserveOptions options, const dannan::contract::TimeSource& time) : _options(options), _time(time), _waits(&time) {
  if (!_options.slots) throw std::invalid_argument("a device admits at least one operation");
  if (_options.control_reserve >= _options.slots) throw std::invalid_argument("the control reserve must leave DATA work a slot");
  Lock lock(_mutex);
  _stats.slots = _options.slots;
  _stats.control_reserve = _options.control_reserve;
}

std::optional<IoReserve::Slot> IoReserve::data(const DataWorkContext& work, std::chrono::milliseconds wait) {
  Lock lock(_mutex);
  const auto free = [&]() DANNAN_REQUIRES(_mutex) { return !_waits.waiting(WorkClass::control) && _data < data_share() && _data + _control < _options.slots; };
  const auto refused = _stopping || work.cancelled();
  // A free slot is taken whatever the time: only one to wait for has a deadline.
  if (refused || !free()) {
    bool admitted = false;
    if (!refused) {
      dannan::sched::ClassWaits::Bounds bounds;
      bounds.deadline = _time.now() + wait;
      if (work.deadline() != TimePoint{}) bounds.deadline = std::min(bounds.deadline, work.deadline());
      bounds.cancelled = work.cancellation();
      if (work.cancellation()) bounds.cancellation_poll = cancellation_poll;
      admitted = _waits.wait(
                     lock, _changed, work.work(), bounds, [&]() DANNAN_REQUIRES(_mutex) { return _stopping; }, free,
                     [&]() DANNAN_REQUIRES(_mutex) { ++_stats.data_waits; }, nothing_to_ask) == dannan::sched::ClassWaits::Outcome::admitted;
    }
    if (!admitted) {
      ++_stats.data_refusals;
      return std::nullopt;
    }
  }
  ++_data;
  ++_stats.data_admissions;
  _stats.peak_data_in_flight = std::max(_stats.peak_data_in_flight, _data);
  return Slot(*this, false);
}

std::optional<IoReserve::Slot> IoReserve::try_data(const DataWorkContext& work) { return data(work, std::chrono::milliseconds::zero()); }

std::optional<IoReserve::Slot> IoReserve::control(std::chrono::milliseconds wait) {
  Lock lock(_mutex);
  const auto free = [&]() DANNAN_REQUIRES(_mutex) { return _data + _control < _options.slots; };
  // A free slot is taken whatever the time: only one to wait for has a deadline.
  if (_stopping || !free()) {
    bool admitted = false;
    if (!_stopping) {
      dannan::sched::ClassWaits::Bounds bounds;
      bounds.deadline = _time.now() + wait;
      bool waited = false;
      // While control work waits, DATA admission is closed (law 1): its count is
      // what DATA reads, so DATA looks again when it ends.
      admitted = _waits.wait(
                     lock, _changed, WorkClass::control, bounds, [&]() DANNAN_REQUIRES(_mutex) { return _stopping; }, free,
                     [&]() DANNAN_REQUIRES(_mutex) {
                       waited = true;
                       ++_stats.control_waits;
                     },
                     nothing_to_ask) == dannan::sched::ClassWaits::Outcome::admitted;
      if (waited) _changed.notify_all();
    }
    if (!admitted) {
      ++_stats.control_refusals;
      return std::nullopt;
    }
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

std::optional<DataAdmission> admit_data(DataResourceArbiter* credit, IoReserve* io, FrameType frame, uint64_t bytes,
                                        std::optional<std::chrono::milliseconds> wait, std::atomic_bool* cancelled) {
  DataAdmission out;
  if (credit) {
    // No deadline here is the arbiter's no-progress window, not an unbounded wait.
    const bool once = wait && *wait <= std::chrono::milliseconds::zero();
    const DataWorkContext work(frame, bytes, wait && !once ? credit->now() + *wait : TimePoint{}, cancelled);
    out.credit = once ? credit->try_acquire(work, bytes) : credit->acquire(work, bytes);
    if (!out.credit) return std::nullopt;
  }
  if (io) {
    out.io = io->data(DataWorkContext(frame, bytes, {}, cancelled), wait.value_or(credit ? credit->no_progress_deadline() : default_no_progress));
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
