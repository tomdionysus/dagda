//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "dagda/store/io_pressure.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/class_waits.h"
#include "dannan/sched/work.h"
#include "dannan/status/log.h"

// Admission of work on this node's devices (requirements S11, S20 and
// section 7 item 4; Macha's cluster/data_work.hpp): the DATA credit arbiter,
// which hands DATA work byte credit by class, and the device I/O reserve,
// which keeps a share of the device's concurrent operations that only
// control work may take. CONTROL never enters the DATA credit arbiter; it
// enters the reserve, whose DATA share leaves it room.
namespace dagda::store {

// Macha's dht.data_credit_no_progress_deadline_ms default.
inline constexpr std::chrono::milliseconds default_no_progress{120000};

// Provenance and bounds for DATA-plane work (Macha's DataWorkContext): a
// class other than control, a deadline on the injected time, a cancellation,
// and a no-progress budget distinct from the deadline: slow work is not
// cancelled, but work not moving at all fails. The counter is shared by a
// pipeline's workers, so progress anywhere re-arms the window.
class DataWorkContext : public dannan::sched::WorkContext {
 public:
  // Throws std::invalid_argument for the control frame: control work never
  // enters the DATA arbiter (law 1).
  explicit DataWorkContext(dannan::sched::FrameType frame = dannan::sched::FrameType::loader, uint64_t quantum_bytes = 0,
                           dannan::contract::TimePoint deadline = {}, std::atomic_bool* cancelled = nullptr, const std::atomic_uint64_t* progress = nullptr,
                           std::chrono::milliseconds no_progress_budget = {});

  // Present only when the caller supplied both a counter and a budget.
  const std::atomic_uint64_t* progress() const noexcept { return _no_progress_budget.count() ? _progress : nullptr; }
  std::chrono::milliseconds no_progress_budget() const noexcept { return _no_progress_budget; }
  uint64_t quantum_bytes() const noexcept { return _quantum_bytes; }

 private:
  uint64_t _quantum_bytes = 0;
  const std::atomic_uint64_t* _progress = nullptr;
  std::chrono::milliseconds _no_progress_budget{};
};

// Macha's figures, "viewer" named foreground as Dannan names the class.
struct DataResourceStats {
  uint64_t capacity_bytes = 0;
  uint64_t foreground_reserve_bytes = 0;
  uint64_t used_bytes = 0;
  uint64_t peak_used_bytes = 0;
  uint64_t foreground_admissions = 0;
  uint64_t loader_admissions = 0;
  uint64_t speculative_admissions = 0;
  uint64_t foreground_waits = 0;
  uint64_t loader_waits = 0;
  uint64_t speculative_waits = 0;
  uint64_t cancelled_waits = 0;
  // Background effort ceiling and its use (loader and speculative leases).
  uint64_t background_limit = 0;
  uint64_t background_active = 0;
  uint64_t peak_background_active = 0;
  // Device pressure, so an operator can see why a loader slowed.
  bool device_pressured = false;
  uint64_t device_service_us = 0;
  uint64_t device_worst_us = 0;
  // Actual over expected service time, percent (100: as expected for the
  // operations' size).
  uint64_t device_slowdown_percent = 0;
  uint64_t device_pressure_onsets = 0;
  // Work the pressure gate turned away; with the onsets, tells a
  // deliberately throttled loader from an unwell node.
  uint64_t pressure_refusals = 0;
  // Waits that gave up with no release anywhere in the arbiter.
  uint64_t no_progress_failures = 0;
};

// Event-driven byte admission at blocking DATA resource boundaries (Macha's
// DataResourceArbiter). Lower classes may borrow all capacity except the
// foreground reserve, and a waiting foreground request closes lower-class
// admission until it is served. Admission also consults measured device
// service time and refuses loader and speculative work while the disk is
// slow; foreground work is never refused for pressure (law 2), and under
// pressure min_background leases are still admitted, so background work
// trickles rather than stops (laws 3 and 4). CONTROL does not enter it.
//
// Every wait has a deadline: the work's own, or, without one, the
// no-progress window, which any release anywhere re-arms, so only a wholly
// stalled arbiter gives up. Deadlines are on the injected time; a time
// source that moves other than with the steady clock calls time_advanced().
// Thread-safe.
class DataResourceArbiter {
 public:
  class Lease {
   public:
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;
    ~Lease() { reset(); }
    void reset();
    explicit operator bool() const noexcept { return _owner != nullptr; }
    uint64_t bytes() const noexcept { return _bytes; }

   private:
    friend class DataResourceArbiter;
    Lease(DataResourceArbiter& owner, dannan::sched::FrameType frame, uint64_t bytes) : _owner(&owner), _frame(frame), _bytes(bytes) {}
    DataResourceArbiter* _owner = nullptr;
    dannan::sched::FrameType _frame = dannan::sched::FrameType::speculative;
    uint64_t _bytes = 0;
  };

  // `foreground_recently_active` answers whether a foreground user is
  // present though it holds no credit now (playback holds none between
  // extents): the node's activity clock over its quiet window. Called under
  // the arbiter's lock; it must not re-enter. Null: credit alone decides.
  // `background_concurrency` caps concurrent loader and speculative leases
  // (zero: no cap). Throws std::invalid_argument unless 0 < reserve <
  // capacity and the no-progress window is positive (no wait is unbounded).
  DataResourceArbiter(uint64_t capacity_bytes, uint64_t foreground_reserve_bytes, uint64_t background_concurrency,
                      std::chrono::milliseconds no_progress_deadline, std::function<bool()> foreground_recently_active = {},
                      std::optional<dannan::status::ScopedLogger> logger = std::nullopt,
                      const dannan::contract::TimeSource& time = dannan::contract::steady_time_source());
  DataResourceArbiter(const DataResourceArbiter&) = delete;
  DataResourceArbiter& operator=(const DataResourceArbiter&) = delete;

  // Set once, when the node's DATA store opens, before it admits work.
  void observe_device(const DiskServiceMonitor* monitor, uint64_t min_background_under_pressure);

  // Waits for credit to the work's deadline or the no-progress window;
  // nothing when it gives up, is cancelled, or the arbiter stops. Nothing at
  // once for more than the class may ever hold. Throws std::invalid_argument
  // for control work. Waits on other holders' releases.
  std::optional<Lease> acquire(const DataWorkContext& work, uint64_t bytes);
  // Admits now or never; waits on nothing.
  std::optional<Lease> try_acquire(const DataWorkContext& work, uint64_t bytes);
  // Refuses new leases and wakes every waiter. Idempotent.
  void stop();
  DataResourceStats stats() const;
  // The injected time moved other than with the steady clock: deadlines are
  // looked at again.
  void time_advanced();
  // The time a work's deadline is set on.
  dannan::contract::TimePoint now() const { return _time.now(); }
  std::chrono::milliseconds no_progress_deadline() const noexcept { return _no_progress_deadline; }

 private:
  static bool foreground(dannan::sched::FrameType frame) noexcept;
  static uint64_t charge(uint64_t bytes) noexcept { return bytes ? bytes : 1; }
  // `refused_for_pressure` says whether a refusal was the pressure gate's;
  // callers count it once per waiter.
  bool available_locked(dannan::sched::FrameType frame, uint64_t bytes, bool* refused_for_pressure) const DANNAN_REQUIRES(_mutex);
  Lease admit_locked(dannan::sched::FrameType frame, uint64_t bytes) DANNAN_REQUIRES(_mutex);
  void release(dannan::sched::FrameType frame, uint64_t bytes);

  const uint64_t _capacity_bytes;
  const uint64_t _foreground_reserve_bytes;
  const uint64_t _background_concurrency;
  const std::chrono::milliseconds _no_progress_deadline;
  const std::function<bool()> _foreground_recently_active;
  // A wait abandoned for no progress is logged here, when given.
  const std::optional<dannan::status::ScopedLogger> _logger;
  const dannan::contract::TimeSource& _time;

  mutable dannan::contract::Mutex _mutex;
  std::condition_variable_any _changed;
  // Null where no device is measured.
  const DiskServiceMonitor* _monitor DANNAN_GUARDED_BY(_mutex) = nullptr;
  uint64_t _min_background_under_pressure DANNAN_GUARDED_BY(_mutex) = 1;
  uint64_t _used_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _lower_used_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _lower_active DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _peak_lower_active DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _peak_used_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  bool _stopping DANNAN_GUARDED_BY(_mutex) = false;
  uint64_t _foreground_admissions DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _loader_admissions DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _speculative_admissions DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _pressure_refusals DANNAN_GUARDED_BY(_mutex) = 0;
  // Who waits, by class, and how each wait ended.
  dannan::sched::ClassWaits _waits DANNAN_GUARDED_BY(_mutex);
};

// What a device admits at once, and how much of it only control work may
// take (section 7 item 4). New: Macha has no such reserve.
struct IoReserveOptions {
  // Device operations in flight at once, every class together.
  size_t slots = 8;
  // Of those, how many DATA work may never take: always free for control
  // work (state journals, claims, head commits, CONTROL objects), so a
  // loader saturating DATA cannot queue a commit behind it.
  size_t control_reserve = 2;
};

struct IoReserveStats {
  size_t slots = 0;
  size_t control_reserve = 0;
  size_t data_in_flight = 0;
  size_t control_in_flight = 0;
  size_t peak_data_in_flight = 0;
  size_t peak_control_in_flight = 0;
  uint64_t data_admissions = 0;
  uint64_t control_admissions = 0;
  // Admissions that had to wait for a slot, by class.
  uint64_t data_waits = 0;
  uint64_t control_waits = 0;
  // Waits that ended without a slot (deadline, cancellation, stop).
  uint64_t data_refusals = 0;
  uint64_t control_refusals = 0;
};

// The device I/O reserve: a count of operations in flight on the node's
// devices, of which DATA work holds at most `slots - control_reserve` and
// control work any free one. A control request never waits behind DATA:
// while one waits, DATA admission is closed, and DATA can never fill the
// reserve. So a control operation waits at most for other control work, and
// shares the device with at most the DATA share's operations: that is the
// bound a commit or a claim sees under a saturating loader. A slot is held
// across one device operation only, never across a peer's answer.
//
// One per node, over the devices its stores share (the state path, the
// control store and the DATA backends), as Macha's monitor is one per pool.
// Every wait has a deadline. Thread-safe.
class IoReserve {
 public:
  class Slot {
   public:
    Slot() = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    Slot(Slot&& other) noexcept : _owner(std::exchange(other._owner, nullptr)), _control(other._control) {}
    Slot& operator=(Slot&& other) noexcept;
    ~Slot() { reset(); }
    void reset();
    explicit operator bool() const noexcept { return _owner != nullptr; }

   private:
    friend class IoReserve;
    Slot(IoReserve& owner, bool control) : _owner(&owner), _control(control) {}
    IoReserve* _owner = nullptr;
    bool _control = false;
  };

  // Throws std::invalid_argument for no slots, or a reserve that leaves
  // DATA none (discipline 5: a share smaller than one operation is not a
  // share).
  explicit IoReserve(IoReserveOptions options = {}, const dannan::contract::TimeSource& time = dannan::contract::steady_time_source());
  IoReserve(const IoReserve&) = delete;
  IoReserve& operator=(const IoReserve&) = delete;

  // A DATA slot: waits up to `wait`, or to the work's deadline if sooner,
  // and while control work waits. Nothing when cancelled, past either, or
  // stopped.
  std::optional<Slot> data(const DataWorkContext& work, std::chrono::milliseconds wait);
  std::optional<Slot> try_data(const DataWorkContext& work);
  // A control slot: any free slot, the reserve included; waits only behind
  // other control work, up to `wait`.
  std::optional<Slot> control(std::chrono::milliseconds wait);
  std::optional<Slot> try_control();

  void stop();
  IoReserveStats stats() const;
  size_t data_share() const noexcept { return _options.slots - _options.control_reserve; }
  void time_advanced();

 private:
  void release(bool control);

  const IoReserveOptions _options;
  const dannan::contract::TimeSource& _time;
  mutable dannan::contract::Mutex _mutex;
  std::condition_variable_any _changed;
  size_t _data DANNAN_GUARDED_BY(_mutex) = 0;
  size_t _control DANNAN_GUARDED_BY(_mutex) = 0;
  bool _stopping DANNAN_GUARDED_BY(_mutex) = false;
  IoReserveStats _stats DANNAN_GUARDED_BY(_mutex);
  dannan::sched::ClassWaits _waits DANNAN_GUARDED_BY(_mutex);
};

// DATA credit and a DATA device slot, held together across one device
// operation, never across a peer's answer.
struct DataAdmission {
  std::optional<DataResourceArbiter::Lease> credit;
  std::optional<IoReserve::Slot> io;
};

// The wait of a caller that named no deadline.
inline constexpr std::optional<std::chrono::milliseconds> no_caller_deadline = std::nullopt;

// Admits DATA work of `frame` for `bytes` on this node's devices: credit,
// then a slot. `wait` is the caller's own deadline, each waited for up to it;
// zero tries once. With no_caller_deadline the credit wait runs under the
// arbiter's no-progress window, as Macha's does, and the slot is waited for
// one such window. A null arbiter or reserve admits at once. Nothing when
// either refuses. Throws std::invalid_argument for the control frame.
std::optional<DataAdmission> admit_data(DataResourceArbiter* credit, IoReserve* io, dannan::sched::FrameType frame, uint64_t bytes,
                                        std::optional<std::chrono::milliseconds> wait, std::atomic_bool* cancelled = nullptr);
// A control slot, waited for up to `wait`; an empty slot when there is no
// reserve.
std::optional<IoReserve::Slot> admit_control(IoReserve* io, std::chrono::milliseconds wait);

}  // namespace dagda::store
