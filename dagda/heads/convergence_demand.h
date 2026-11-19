//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace dagda::heads {

struct ConvergenceDemandSnapshot {
  uint64_t epoch = 0;
  uint64_t generation = 0;
};

struct ConvergenceDemandDiagnostics {
  uint64_t events_received = 0;
  uint64_t runs_scheduled = 0;
  uint64_t runs_completed = 0;
  uint64_t requested_epoch = 0;
  uint64_t completed_epoch = 0;
  uint64_t latest_generation = 0;
  bool scheduled = false;
};

// Single-consumer, multi-producer edge-triggered demand: Macha's, unchanged
// (service/convergence_demand.hpp). Every event that matters advances the epoch, a
// same-generation sibling or a topology transition included. Generations are a
// diagnostic high-water mark, not the demand's identity. Waits on nothing.
class ConvergenceDemand {
 public:
  // Whether this event scheduled a run: none was scheduled yet.
  bool request(uint64_t generation) noexcept {
    auto current = _latest_generation.load(std::memory_order_relaxed);
    while (current < generation && !_latest_generation.compare_exchange_weak(current, generation, std::memory_order_relaxed)) {
    }
    _events_received.fetch_add(1, std::memory_order_relaxed);
    _requested_epoch.fetch_add(1, std::memory_order_release);
    return schedule_edge();
  }

  std::optional<ConvergenceDemandSnapshot> begin() const noexcept {
    if (!_scheduled.load(std::memory_order_acquire)) return std::nullopt;
    // request() publishes the generation before the epoch release.
    const auto epoch = _requested_epoch.load(std::memory_order_acquire);
    return ConvergenceDemandSnapshot{epoch, _latest_generation.load(std::memory_order_relaxed)};
  }

  // True when an event arrived during the completed run and exactly one follow-up is
  // now scheduled. Clearing the edge before rechecking the epoch closes both orders of
  // a request racing a completion, without polling.
  bool complete(const ConvergenceDemandSnapshot& run) noexcept {
    _completed_epoch.store(run.epoch, std::memory_order_release);
    _runs_completed.fetch_add(1, std::memory_order_relaxed);
    _scheduled.store(false, std::memory_order_release);
    if (_requested_epoch.load(std::memory_order_acquire) != run.epoch) schedule_edge();
    return _scheduled.load(std::memory_order_acquire);
  }

  bool pending() const noexcept { return _scheduled.load(std::memory_order_acquire); }

  ConvergenceDemandDiagnostics diagnostics() const noexcept {
    return {
        _events_received.load(std::memory_order_relaxed), _runs_scheduled.load(std::memory_order_relaxed),  _runs_completed.load(std::memory_order_relaxed),
        _requested_epoch.load(std::memory_order_acquire), _completed_epoch.load(std::memory_order_acquire), _latest_generation.load(std::memory_order_relaxed),
        _scheduled.load(std::memory_order_acquire),
    };
  }

 private:
  bool schedule_edge() noexcept {
    bool expected = false;
    if (_scheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      _runs_scheduled.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  std::atomic_uint64_t _requested_epoch{0};
  std::atomic_uint64_t _completed_epoch{0};
  std::atomic_uint64_t _latest_generation{0};
  std::atomic_bool _scheduled{false};
  std::atomic_uint64_t _events_received{0};
  std::atomic_uint64_t _runs_scheduled{0};
  std::atomic_uint64_t _runs_completed{0};
};

}  // namespace dagda::heads
