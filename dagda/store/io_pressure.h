//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"

namespace dagda::store {

// What a device is held to (DiskServiceMonitor::Thresholds) and what it
// reports (DiskServiceMonitor::Sample). At namespace scope so their default
// member initializers are complete before the monitor uses them, which GCC
// requires.
struct DiskServiceThresholds {
  // Expected cost on a healthy device: fixed overhead plus per-MiB time.
  // Deliberately generous (about 8 MB/s): the question is "far worse than it
  // should be", not "fast". Scaling by size keeps a 4 MiB write and a 4 KiB
  // read from being held to the same figure.
  std::chrono::milliseconds overhead{25};
  std::chrono::milliseconds per_mib{120};
  // Pressure when the moving average of actual/expected exceeds this
  // percentage; released below release_percent.
  uint32_t slowdown_percent = 300;
  uint32_t release_percent = 150;
  // One operation past this ratio trips pressure at once. The 1/16 average
  // cannot catch a lone catastrophic write among healthy ones; a single
  // sample would need about 4,800% to carry it over 300%. 1000% of a budget
  // this loose is a device that has stopped serving, well clear of ordinary
  // variance. Zero disables the trip.
  uint32_t outlier_percent = 1000;
};

struct DiskServiceSample {
  uint64_t operations = 0;
  uint64_t bytes = 0;
  // EWMA completion latency, microseconds. Reported only; not size-aware, so
  // pressure is not decided on it.
  uint64_t mean_us = 0;
  // Worst completion latency in the recent window, microseconds; a mean
  // hides the single slow write that starves a viewer.
  uint64_t worst_us = 0;
  // Moving average of actual/expected, percent (100 = as expected). The
  // signal pressure is decided on.
  uint64_t slowdown_percent = 0;
  bool pressured = false;
};

// Measured DATA device service time (requirement S21): answers whether the
// device is slow enough that work nobody is waiting for should stand aside.
//
// One monitor covers a whole StoragePool and cannot tell its backends apart,
// so one slow backend pressures work bound anywhere in the pool. Per-device
// pressure is not possible because admission happens before placement picks a
// backend. The control store is not observed: it is a different device and
// must not be gated by DATA pressure.
//
// Cost: two time reads and a few relaxed atomics per operation; no locks, no
// timer thread.
//
// Only operations through the pool are observed: a device saturated by
// another process raises the signal only once the store's own I/O slows.
// Pressure matters only when there is work to protect, and that work is the
// probe.
//
// What it does not give, and section 7 of the requirements asks for: a
// reserve of device I/O for control work (state journals, claims, head
// commits), so a loader cannot delay a commit or a health answer. That is the
// admission side's (data_work.h: the DATA credit arbiter, which this monitor
// feeds, and the device reserve); the monitor is kept as Macha has it.
class DiskServiceMonitor {
 public:
  using Thresholds = DiskServiceThresholds;
  using Sample = DiskServiceSample;

  // Each pressure transition is logged once, at info, through `logger`.
  explicit DiskServiceMonitor(dannan::status::ScopedLogger logger) : _logger(std::move(logger)) {}
  DiskServiceMonitor(dannan::status::ScopedLogger logger, Thresholds thresholds) : _logger(std::move(logger)), _thresholds(thresholds) {}
  DiskServiceMonitor(const DiskServiceMonitor&) = delete;
  DiskServiceMonitor& operator=(const DiskServiceMonitor&) = delete;

  // Called once, before any operation is recorded: the thresholds are read
  // without a lock. Not assignment: the counters are atomics and the monitor
  // is owned in place.
  void configure(Thresholds thresholds) noexcept { _thresholds = thresholds; }

  // Records one completed DATA read or write, whatever class of work issued
  // it. Waits on nothing; thread-safe.
  void note(std::chrono::nanoseconds elapsed, uint64_t bytes) noexcept;

  // Whether deferrable work should stand aside. Interactive work must never
  // consult this: a person waiting on a slow disk gets all of it. Waits on
  // nothing.
  bool pressured() const noexcept { return _pressured.load(std::memory_order_relaxed); }
  uint64_t pressure_onsets() const noexcept { return _pressure_onsets.load(std::memory_order_relaxed); }
  Sample sample() const noexcept;
  // Clears the worst-case high-water mark. The mean and pressure state are
  // not resettable: they describe now.
  void forget_worst() noexcept { _worst_us.store(0, std::memory_order_relaxed); }
  const Thresholds& thresholds() const noexcept { return _thresholds; }

 private:
  void log_transition(const char* what, uint64_t slowdown, uint64_t ratio, uint64_t micros, uint64_t bytes) const noexcept;

  // 1/16 weight per sample: a modest outlier is absorbed, a catastrophic one
  // moves the mean on its own, and recovery takes many fast samples rather
  // than one lucky write.
  static constexpr uint64_t weight_denominator = 16;

  const dannan::status::ScopedLogger _logger;
  Thresholds _thresholds;
  std::atomic<uint64_t> _operations{0};
  std::atomic<uint64_t> _bytes{0};
  std::atomic<uint64_t> _mean_us{0};
  std::atomic<uint64_t> _worst_us{0};
  std::atomic<uint64_t> _slowdown_percent{0};
  std::atomic<uint64_t> _pressure_onsets{0};
  std::atomic<bool> _pressured{false};
};

// Times one local store operation on the injected time and reports it on
// destruction, so early returns and exceptions are still measured. A null
// monitor measures nothing.
class DiskServiceTimer {
 public:
  DiskServiceTimer(DiskServiceMonitor* monitor, uint64_t bytes, const dannan::contract::TimeSource& time) noexcept
      : _monitor(monitor), _time(time), _bytes(bytes), _started(monitor ? time.now() : dannan::contract::TimePoint{}) {}
  ~DiskServiceTimer() {
    if (_monitor) _monitor->note(_time.now() - _started, _bytes);
  }
  DiskServiceTimer(const DiskServiceTimer&) = delete;
  DiskServiceTimer& operator=(const DiskServiceTimer&) = delete;

  // For a read, the size is only known once it has succeeded.
  void note_bytes(uint64_t bytes) noexcept { _bytes = bytes; }

 private:
  DiskServiceMonitor* const _monitor;
  const dannan::contract::TimeSource& _time;
  uint64_t _bytes = 0;
  const dannan::contract::TimePoint _started;
};

}  // namespace dagda::store
