//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/io_pressure.h"

#include <string>

namespace dagda::store {

void DiskServiceMonitor::note(std::chrono::nanoseconds elapsed, uint64_t bytes) noexcept {
  const auto micros = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
  _operations.fetch_add(1, std::memory_order_relaxed);
  _bytes.fetch_add(bytes, std::memory_order_relaxed);

  // Deliberately racy EWMA: a lost update is noise; a lock would sit on the
  // hot completion path of every extent write.
  const auto mean = _mean_us.load(std::memory_order_relaxed);
  _mean_us.store(mean ? (mean * (weight_denominator - 1) + micros) / weight_denominator : micros, std::memory_order_relaxed);

  auto worst = _worst_us.load(std::memory_order_relaxed);
  while (micros > worst && !_worst_us.compare_exchange_weak(worst, micros, std::memory_order_relaxed)) {
  }

  // What this operation should have cost on a coping device, given its size.
  constexpr uint64_t mib = 1024 * 1024;
  const auto expected_us =
      static_cast<uint64_t>(_thresholds.overhead.count()) * 1000 + (bytes * static_cast<uint64_t>(_thresholds.per_mib.count()) * 1000) / mib;
  const auto ratio = expected_us ? (micros * 100) / expected_us : 100;

  const auto slowdown = _slowdown_percent.load(std::memory_order_relaxed);
  const auto next_slowdown = slowdown ? (slowdown * (weight_denominator - 1) + ratio) / weight_denominator : ratio;
  _slowdown_percent.store(next_slowdown, std::memory_order_relaxed);

  // Hysteresis evaluated here so admission is a single relaxed load. Each
  // transition is logged once.
  if (next_slowdown > _thresholds.slowdown_percent || (_thresholds.outlier_percent && ratio > _thresholds.outlier_percent)) {
    if (!_pressured.exchange(true, std::memory_order_relaxed)) {
      _pressure_onsets.fetch_add(1, std::memory_order_relaxed);
      log_transition("DATA device pressure onset", next_slowdown, ratio, micros, bytes);
    }
  } else if (next_slowdown < _thresholds.release_percent) {
    if (_pressured.exchange(false, std::memory_order_relaxed)) {
      log_transition("DATA device pressure released", next_slowdown, ratio, micros, bytes);
      try {
        dannan::contract::Lock lock(_listener_mutex);
        if (_on_release) _on_release();
      } catch (...) {
        // A listener that fails costs only its own wake.
      }
    }
  }
}

void DiskServiceMonitor::on_release(std::function<void()> released) {
  dannan::contract::Lock lock(_listener_mutex);
  _on_release = std::move(released);
}

DiskServiceMonitor::Sample DiskServiceMonitor::sample() const noexcept {
  return Sample{_operations.load(std::memory_order_relaxed),       _bytes.load(std::memory_order_relaxed),
                _mean_us.load(std::memory_order_relaxed),          _worst_us.load(std::memory_order_relaxed),
                _slowdown_percent.load(std::memory_order_relaxed), _pressured.load(std::memory_order_relaxed)};
}

void DiskServiceMonitor::log_transition(const char* what, uint64_t slowdown, uint64_t ratio, uint64_t micros, uint64_t bytes) const noexcept {
  try {
    _logger.info(std::string(what) + " slowdown_percent=" + std::to_string(slowdown) + " last_percent=" + std::to_string(ratio) +
                 " last_us=" + std::to_string(micros) + " last_bytes=" + std::to_string(bytes));
  } catch (...) {
    // A log line must never fail the I/O it describes.
  }
}

}  // namespace dagda::store
