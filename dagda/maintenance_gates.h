//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>

#include "dagda/ledger/collector.h"
#include "dannan/contract/time_source.h"

namespace dagda {

// The node loop's two destructive gates, Macha's (service/maintenance.cpp at f0597a4):
// garbage is marked only once metadata has converged (`garbage_due = !busy &&
// !metadata_dirty`, :575), and the sweep waits out a quiet window, `foreground_quiet`
// long, that any node event, any move of a space's committed generation (:347-362) and
// the pass that rebuilt the inventory (:649-652: "never consumed destructively in the
// pass that built it") start again. Owned by the maintenance thread; not thread-safe.
class MaintenanceGates {
 public:
  explicit MaintenanceGates(std::chrono::milliseconds quiet) : _quiet(quiet) {}

  // Seen at the start of each pass: the node's event count and the sum of its spaces'
  // committed generations. A change from the last pass starts the quiet window; the
  // first observation only records.
  void observe(dannan::contract::TimePoint now, uint64_t events, uint64_t generations) {
    if (_observed && (events != _events || generations != _generations)) _quiet_until = now + _quiet;
    _observed = true;
    _events = events;
    _generations = generations;
  }

  // The pass rebuilt the inventory: it is not swept until the window has passed.
  void rebuilt_inventory(dannan::contract::TimePoint now) { _quiet_until = now + _quiet; }

  ledger::PassDue due(dannan::contract::TimePoint now, bool busy, bool metadata_dirty) const {
    ledger::PassDue out;
    out.busy = busy;
    out.garbage_due = !busy && !metadata_dirty;
    out.gc_due = !busy && now >= _quiet_until;
    return out;
  }

  dannan::contract::TimePoint quiet_until() const noexcept { return _quiet_until; }

 private:
  const std::chrono::milliseconds _quiet;
  bool _observed = false;
  uint64_t _events = 0;
  uint64_t _generations = 0;
  dannan::contract::TimePoint _quiet_until{};
};

}  // namespace dagda
