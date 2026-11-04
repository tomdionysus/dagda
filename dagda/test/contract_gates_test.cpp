//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirements L16 to L19: the horizons' value types and the three
// destructive gates, each gate over every combination of the facts it reads
// against a reference model of the pass's condition, reason and trace.
//
// Moved from Macha's tests/test_ledger_gates.cpp (develop 5a06d3c), every
// assertion kept: test_referenced_sets_are_sorted_unique_and_per_class,
// test_inventory_splits_tombstones_by_whether_data_still_refers_to_them,
// test_release_horizon_carries_its_head_and_clock,
// test_tombstone_gate_is_the_pass_condition_over_every_input,
// test_control_gate_is_the_pass_condition_over_every_input,
// test_data_gate_is_the_pass_condition_and_reason_over_every_input,
// test_each_shut_gate_names_the_first_condition_that_failed. Macha's
// GarbageRef is the provider's Garbage, its catalogue_complete() the
// inventory's complete(); the gates' words are Macha's.

#include <gtest/gtest.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dagda/contract/gates.h"
#include "dagda/contract/horizon.h"

namespace dagda::contract {
namespace {

ObjectId id(uint8_t n) {
  ObjectId out{};
  out.bytes[0] = n;
  return out;
}

Garbage garbage(uint8_t n, int64_t retired_at_ns) {
  Garbage out;
  out.id = id(n);
  out.retired_at_ns = retired_at_ns;
  return out;
}

std::vector<ObjectId> ids_of(std::span<const ObjectId> span) { return {span.begin(), span.end()}; }

constexpr auto data = RetentionClass::data;
constexpr auto control = RetentionClass::control;

// Macha: test_referenced_sets_are_sorted_unique_and_per_class.
TEST(ContractGates, ReferencedSetsAreSortedUniqueAndPerClass) {
  const ReferencedSets sets({id(3), id(1), id(3), id(2)}, {id(9), id(9), id(5)});
  EXPECT_EQ(ids_of(sets.all(data)), (std::vector<ObjectId>{id(1), id(2), id(3)}));
  EXPECT_EQ(ids_of(sets.all(control)), (std::vector<ObjectId>{id(5), id(9)}));
  EXPECT_EQ(sets.size(data), 3u);
  EXPECT_EQ(sets.size(control), 2u);
  for (uint8_t n = 0; n < 12; ++n) {
    EXPECT_EQ(sets.referenced(data, id(n)), n >= 1 && n <= 3);
    EXPECT_EQ(sets.referenced(control, id(n)), n == 5 || n == 9);
  }
  const ReferencedSets empty({}, {});
  EXPECT_EQ(empty.size(data), 0u);
  EXPECT_TRUE(empty.all(control).empty());
  EXPECT_FALSE(empty.referenced(data, id(0)));
}

// Macha: test_inventory_splits_tombstones_by_whether_data_still_refers_to_them.
// A retirement whose id is live DATA again is stale; one referenced only as
// CONTROL is not.
TEST(ContractGates, TheInventorySplitsRetirementsByWhetherDataStillRefersToThem) {
  const InventoryHorizon inventory(7, false, {id(2), id(4)}, {id(6)}, {garbage(4, 10), garbage(1, 0), garbage(6, 30), garbage(2, 40), garbage(1, 50)});
  EXPECT_EQ(inventory.generation(), 7u);
  EXPECT_FALSE(inventory.complete());
  ASSERT_EQ(inventory.garbage().size(), 3u);
  EXPECT_TRUE(inventory.garbage()[0].id == id(1) && inventory.garbage()[0].retired_at_ns == 0);
  EXPECT_TRUE(inventory.garbage()[1].id == id(6) && inventory.garbage()[1].retired_at_ns == 30);
  EXPECT_TRUE(inventory.garbage()[2].id == id(1) && inventory.garbage()[2].retired_at_ns == 50);
  ASSERT_EQ(inventory.stale_garbage().size(), 2u);
  EXPECT_TRUE(inventory.stale_garbage()[0].id == id(4) && inventory.stale_garbage()[0].retired_at_ns == 10);
  EXPECT_TRUE(inventory.stale_garbage()[1].id == id(2) && inventory.stale_garbage()[1].retired_at_ns == 40);
  EXPECT_TRUE(InventoryHorizon(0, true, {}, {}, {}).complete());
}

// Macha: test_release_horizon_carries_its_head_and_clock.
TEST(ContractGates, TheReleaseHorizonCarriesItsHeadAndClock) {
  Hash256 head{};
  head.bytes[0] = 0xab;
  NodeId node{};
  node.bytes[0] = 1;
  const ReleaseHorizon release(head, {{node, 12}}, {id(2), id(1)}, {id(3)});
  EXPECT_EQ(release.head(), head);
  EXPECT_EQ(release.clock().at(node), 12u);
  EXPECT_EQ(ids_of(release.all(data)), (std::vector<ObjectId>{id(1), id(2)}));
  EXPECT_TRUE(release.referenced(control, id(3)));
}

// The reference model's state: an inventory or none, its generation, its
// completeness (true before the first inventory).
struct Reference {
  bool garbage_due, gc_due, busy, waiting, rebuilt, release_view;
  std::optional<uint64_t> generation;
  bool complete;
  uint64_t known;
};

std::string flag(std::string_view name, bool value) { return std::string(name) + (value ? "=1" : "=0"); }

// Each kind of inventory the gates can see.
std::vector<std::optional<InventoryHorizon>> inventories() {
  std::vector<std::optional<InventoryHorizon>> out;
  out.emplace_back(std::nullopt);
  for (uint64_t generation : {4, 5, 6})
    for (bool complete : {false, true}) out.emplace_back(InventoryHorizon(generation, complete, {id(1)}, {id(2)}, {}));
  return out;
}

template <class Check>
void over_every_pass(Check&& check) {
  for (unsigned bits = 0; bits < (1U << 6); ++bits)
    for (const auto& inventory : inventories()) {
      const auto bit = [bits](unsigned n) { return ((bits >> n) & 1U) != 0; };
      Reference ref{bit(0),
                    bit(1),
                    bit(2),
                    bit(3),
                    bit(4),
                    bit(5),
                    inventory ? std::optional<uint64_t>(inventory->generation()) : std::nullopt,
                    inventory ? inventory->complete() : true,
                    5};
      PassFacts facts;
      facts.garbage_due = ref.garbage_due;
      facts.gc_due = ref.gc_due;
      facts.busy = ref.busy;
      facts.gc_waiting_for_event = ref.waiting;
      facts.rebuilt_inventory = ref.rebuilt;
      facts.release_view = ref.release_view;
      facts.known_generation = ref.known;
      check(ref, facts, inventory ? &*inventory : nullptr);
    }
}

// Macha: test_tombstone_gate_is_the_pass_condition_over_every_input.
TEST(ContractGates, TheTombstoneGateIsThePassConditionOverEveryInput) {
  over_every_pass([](const Reference& r, const PassFacts& facts, const InventoryHorizon* inventory) {
    const bool expected = r.garbage_due && !r.rebuilt && r.complete;
    const auto verdict = tombstone_gate(facts, inventory);
    EXPECT_EQ(verdict.permitted, expected);
    EXPECT_EQ(verdict.reason.empty(), expected);
    EXPECT_EQ(verdict.conditions, flag("due", r.garbage_due) + " " + flag("rebuilt", r.rebuilt) + " " + flag("catalogue_complete", r.complete));
  });
}

// Macha: test_control_gate_is_the_pass_condition_over_every_input.
TEST(ContractGates, TheControlGateIsThePassConditionOverEveryInput) {
  over_every_pass([](const Reference& r, const PassFacts& facts, const InventoryHorizon* inventory) {
    const uint64_t generation = r.generation.value_or(0);
    const bool expected = r.gc_due && !r.rebuilt && r.release_view && r.complete && r.generation.has_value() && generation >= r.known;
    const auto verdict = control_gate(facts, inventory);
    EXPECT_EQ(verdict.permitted, expected);
    EXPECT_EQ(verdict.reason.empty(), expected);
    EXPECT_EQ(verdict.conditions, flag("due", r.gc_due) + " " + flag("rebuilt", r.rebuilt) + " " + flag("release_view", r.release_view) + " " +
                                      flag("catalogue_complete", r.complete) + " " + flag("control_live", r.generation.has_value()) + " " +
                                      flag("generation_current", generation >= r.known));
  });
}

// Macha: test_data_gate_is_the_pass_condition_and_reason_over_every_input.
TEST(ContractGates, TheDataGateIsThePassConditionAndReasonOverEveryInput) {
  over_every_pass([](const Reference& r, const PassFacts& facts, const InventoryHorizon* inventory) {
    const uint64_t generation = r.generation.value_or(0);
    const bool expected = r.gc_due && !r.rebuilt && r.release_view && r.complete && r.generation.has_value() && generation >= r.known;
    std::string reason;
    if (!r.gc_due)
      reason = r.busy ? "foreground busy" : r.waiting ? "complete; waiting for an event" : "quiet window";
    else if (r.rebuilt)
      reason = "inventory rebuilt this pass";
    else if (!r.release_view)
      reason = "no sole accepted head for retention release";
    else if (!r.complete)
      reason = "catalogue inventory incomplete";
    else if (!r.generation)
      reason = "no reachability inventory";
    else if (generation < r.known)
      reason = "inventory generation " + std::to_string(generation) + " behind known " + std::to_string(r.known);
    const auto verdict = data_gate(facts, inventory);
    EXPECT_EQ(verdict.permitted, expected);
    EXPECT_EQ(verdict.reason, reason);
    EXPECT_EQ(verdict.conditions, flag("due", r.gc_due) + " " + flag("rebuilt", r.rebuilt) + " " + flag("release_view", r.release_view) + " " +
                                      flag("catalogue_complete", r.complete) + " " + flag("live", r.generation.has_value()) + " " +
                                      flag("generation_current", generation >= r.known));
  });
}

// Macha: test_each_shut_gate_names_the_first_condition_that_failed.
TEST(ContractGates, EachShutGateNamesTheFirstConditionThatFailed) {
  PassFacts open;
  open.garbage_due = open.gc_due = true;
  open.release_view = true;
  open.known_generation = 5;
  const InventoryHorizon current(5, true, {}, {}, {});
  EXPECT_TRUE(tombstone_gate(open, &current).permitted);
  EXPECT_TRUE(control_gate(open, &current).permitted);
  EXPECT_TRUE(data_gate(open, &current).permitted);

  auto shut = open;
  shut.garbage_due = false;
  EXPECT_EQ(tombstone_gate(shut, &current).reason, "not due");
  shut = open;
  shut.rebuilt_inventory = true;
  EXPECT_EQ(tombstone_gate(shut, &current).reason, "inventory rebuilt this pass");
  EXPECT_EQ(control_gate(shut, &current).reason, "inventory rebuilt this pass");
  shut = open;
  shut.release_view = false;
  EXPECT_TRUE(tombstone_gate(shut, &current).permitted);
  EXPECT_EQ(control_gate(shut, &current).reason, "no sole accepted head for retention release");
  const InventoryHorizon incomplete(5, false, {}, {}, {});
  EXPECT_EQ(tombstone_gate(open, &incomplete).reason, "catalogue inventory incomplete");
  EXPECT_EQ(control_gate(open, &incomplete).reason, "catalogue inventory incomplete");
  EXPECT_EQ(control_gate(open, nullptr).reason, "no reachability inventory");
  const InventoryHorizon behind(4, true, {}, {}, {});
  EXPECT_EQ(control_gate(open, &behind).reason, "inventory generation behind known");
  shut = open;
  shut.gc_due = false;
  EXPECT_EQ(control_gate(shut, &current).reason, "quiet window");
  shut.busy = true;
  EXPECT_EQ(control_gate(shut, &current).reason, "foreground busy");
}

}  // namespace
}  // namespace dagda::contract
