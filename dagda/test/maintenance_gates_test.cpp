//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The node loop's two destructive gates, as Macha's maintenance pass keeps them
// (service/maintenance.cpp at f0597a4; the store map, GC and deletion, gaps 1 and 4):
// garbage is marked only once metadata has converged, and the sweep waits out a quiet
// window after any node event, any move of a space's committed generation, and the pass
// that rebuilt the inventory.

#include "dagda/maintenance_gates.h"

#include <gtest/gtest.h>

#include <chrono>

#include "dannan/test/fixtures/stepped_time.h"

namespace dagda {
namespace {

using namespace std::chrono_literals;

// Macha: garbage_due = !busy && !metadata_dirty (maintenance.cpp:575).
TEST(MaintenanceGates, GarbageIsMarkedOnlyOnceMetadataHasConverged) {
  dannan::test::SteppedTime time;
  MaintenanceGates gates(1s);
  gates.observe(time.now(), 0, 0);
  time.advance(2s);
  EXPECT_FALSE(gates.due(time.now(), false, true).garbage_due);
  EXPECT_TRUE(gates.due(time.now(), false, false).garbage_due);
  EXPECT_FALSE(gates.due(time.now(), true, false).garbage_due);
}

// Macha: an event starts a quiet window the sweep waits out, so a sweep cannot race the
// claim that makes newly arrived bytes reachable (maintenance.cpp:355-362, 576).
TEST(MaintenanceGates, AnEventHoldsTheSweepForTheQuietWindow) {
  dannan::test::SteppedTime time;
  MaintenanceGates gates(1s);
  gates.observe(time.now(), 5, 0);
  time.advance(2s);
  EXPECT_TRUE(gates.due(time.now(), false, false).gc_due);
  gates.observe(time.now(), 6, 0);
  EXPECT_FALSE(gates.due(time.now(), false, false).gc_due);
  time.advance(999ms);
  EXPECT_FALSE(gates.due(time.now(), false, false).gc_due);
  time.advance(1ms);
  EXPECT_TRUE(gates.due(time.now(), false, false).gc_due);
}

// Macha: a move of the local committed generation does the same (maintenance.cpp:347-354).
TEST(MaintenanceGates, AGenerationMoveHoldsTheSweepForTheQuietWindow) {
  dannan::test::SteppedTime time;
  MaintenanceGates gates(1s);
  gates.observe(time.now(), 0, 10);
  time.advance(2s);
  gates.observe(time.now(), 0, 11);
  EXPECT_FALSE(gates.due(time.now(), false, false).gc_due);
  time.advance(1s);
  EXPECT_TRUE(gates.due(time.now(), false, false).gc_due);
}

// Macha: "A newly derived reachability set is never consumed destructively in the pass
// that built it" (maintenance.cpp:649-652).
TEST(MaintenanceGates, AnInventoryIsNotSweptInThePassThatBuiltIt) {
  dannan::test::SteppedTime time;
  MaintenanceGates gates(1s);
  gates.observe(time.now(), 0, 0);
  time.advance(2s);
  ASSERT_TRUE(gates.due(time.now(), false, false).gc_due);
  gates.rebuilt_inventory(time.now());
  EXPECT_FALSE(gates.due(time.now(), false, false).gc_due);
  time.advance(1s);
  EXPECT_TRUE(gates.due(time.now(), false, false).gc_due);
}

// Busy holds both, as before.
TEST(MaintenanceGates, BusyHoldsBoth) {
  dannan::test::SteppedTime time;
  MaintenanceGates gates(1s);
  gates.observe(time.now(), 0, 0);
  time.advance(2s);
  const auto due = gates.due(time.now(), true, false);
  EXPECT_FALSE(due.garbage_due);
  EXPECT_FALSE(due.gc_due);
  EXPECT_TRUE(due.busy);
}

}  // namespace
}  // namespace dagda
