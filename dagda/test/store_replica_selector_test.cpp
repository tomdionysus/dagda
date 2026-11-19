//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The read-source policy (requirement S27; Macha's cluster/replica_selector.*,
// which Macha exercised only through the store's reads), with Dannan's
// measured round trip ordering peers nothing has been fetched from yet.

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <vector>

#include "dagda/store/replica_selector.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using dannan::membership::NodeRecord;

NodeRecord node(uint8_t fill) {
  NodeRecord record;
  record.id.bytes.fill(fill);
  return record;
}

std::vector<uint8_t> firsts(const std::vector<NodeRecord>& ordered) {
  std::vector<uint8_t> out;
  for (const auto& record : ordered) out.push_back(record.id.bytes[0]);
  return out;
}

// Unmeasured and equal, the stripe rotates the order, so a file's stripes
// spread over the replicas.
TEST(ReplicaSelector, EqualCandidatesRotateByStripe) {
  ReplicaSelector selector;
  const std::vector<NodeRecord> candidates{node(1), node(2), node(3)};
  EXPECT_EQ(firsts(selector.order(candidates, 0, ReplicaWorkClass::foreground)), (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(firsts(selector.order(candidates, 1, ReplicaWorkClass::foreground)), (std::vector<uint8_t>{2, 3, 1}));
  EXPECT_EQ(firsts(selector.order(candidates, 5, ReplicaWorkClass::foreground)), (std::vector<uint8_t>{3, 1, 2}));
  EXPECT_EQ(firsts(selector.order({node(9)}, 7, ReplicaWorkClass::foreground)), std::vector<uint8_t>{9});
}

// Before any fetch, Dannan's round trip says which is nearer; one measured
// transfer then speaks for itself.
TEST(ReplicaSelector, UnmeasuredPeersAreOrderedByRoundTrip) {
  ReplicaSelector selector;
  const std::vector<NodeRecord> candidates{node(1), node(2), node(3)};
  const std::map<uint8_t, std::chrono::milliseconds> round_trips{{1, 80ms}, {2, 3ms}};
  const auto latency = [&](const dannan::identity::NodeId& id) -> std::optional<std::chrono::milliseconds> {
    const auto found = round_trips.find(id.bytes[0]);
    if (found == round_trips.end()) return std::nullopt;
    return found->second;
  };
  // Node 3 is unmeasured: Macha's 100 ms guess puts it last.
  EXPECT_EQ(firsts(selector.order(candidates, 0, ReplicaWorkClass::foreground, latency)), (std::vector<uint8_t>{2, 1, 3}));
  selector.started(node(3).id, ReplicaWorkClass::foreground);
  selector.finished(node(3).id, ReplicaWorkClass::foreground, 1024, 1ms, true);
  EXPECT_EQ(firsts(selector.order(candidates, 0, ReplicaWorkClass::foreground, latency)), (std::vector<uint8_t>{3, 2, 1}));
}

// Failures cost 500 ms each, up to eight; a success clears them.
TEST(ReplicaSelector, FailuresPushAPeerBackUntilItSucceeds) {
  ReplicaSelector selector;
  const std::vector<NodeRecord> candidates{node(1), node(2)};
  selector.started(node(1).id, ReplicaWorkClass::foreground);
  selector.finished(node(1).id, ReplicaWorkClass::foreground, 0, 10ms, false);
  EXPECT_EQ(firsts(selector.order(candidates, 0, ReplicaWorkClass::foreground)), (std::vector<uint8_t>{2, 1}));
  EXPECT_EQ(selector.stats(node(1).id).failures, 1u);
  selector.started(node(1).id, ReplicaWorkClass::foreground);
  selector.finished(node(1).id, ReplicaWorkClass::foreground, 10, 10ms, true);
  EXPECT_EQ(firsts(selector.order(candidates, 0, ReplicaWorkClass::foreground)), (std::vector<uint8_t>{1, 2}));
  const auto stats = selector.stats(node(1).id);
  EXPECT_EQ(stats.successes, 1u);
  EXPECT_EQ(stats.selections, 2u);
  EXPECT_DOUBLE_EQ(stats.ewma_transfer_ms, 10.0);
}

// Speculative work yields to any foreground transfer in flight, and prefers
// an idle peer; a promotion moves a transfer between the classes.
TEST(ReplicaSelector, SpeculationAvoidsPeersBusyWithForegroundWork) {
  ReplicaSelector selector;
  const std::vector<NodeRecord> candidates{node(1), node(2)};
  selector.started(node(1).id, ReplicaWorkClass::foreground);
  EXPECT_EQ(firsts(selector.order(candidates, 0, ReplicaWorkClass::speculative)), (std::vector<uint8_t>{2, 1}));
  selector.started(node(2).id, ReplicaWorkClass::speculative);
  EXPECT_EQ(selector.stats(node(2).id).speculative_in_flight, 1u);
  selector.promoted(node(2).id);
  EXPECT_EQ(selector.stats(node(2).id).speculative_in_flight, 0u);
  EXPECT_EQ(selector.stats(node(2).id).foreground_in_flight, 1u);
  selector.finished(node(2).id, ReplicaWorkClass::foreground, 0, 5ms, true);
  EXPECT_EQ(selector.stats(node(2).id).foreground_in_flight, 0u);
}

// Macha (tests/test_hydration_catalogue.cpp): test_replica_selector, what the
// cases above leave out. Speculative work rotates over the whole set when
// nothing is measured; its own work in flight makes an equal peer less useful;
// a success clears the failure penalty without erasing the failure's history.
TEST(ReplicaSelector, SpeculativeWorkSpreadsAndASuccessKeepsTheFailureCount) {
  ReplicaSelector selector;
  const std::vector<NodeRecord> nodes{node(1), node(2), node(3)};
  const auto first = [&](size_t stripe, ReplicaWorkClass work) { return selector.order(nodes, stripe, work).front().id.bytes[0]; };

  EXPECT_EQ(first(0, ReplicaWorkClass::speculative), 1u);
  EXPECT_EQ(first(1, ReplicaWorkClass::speculative), 2u);
  EXPECT_EQ(first(2, ReplicaWorkClass::speculative), 3u);

  selector.started(nodes[0].id, ReplicaWorkClass::speculative);
  EXPECT_NE(first(0, ReplicaWorkClass::speculative), 1u);
  selector.finished(nodes[0].id, ReplicaWorkClass::speculative, 1024, 100ms, true);

  // Foreground reads optimise latency rather than symmetry.
  selector.started(nodes[0].id, ReplicaWorkClass::foreground);
  selector.finished(nodes[0].id, ReplicaWorkClass::foreground, 1024, 20ms, true);
  selector.started(nodes[1].id, ReplicaWorkClass::foreground);
  selector.finished(nodes[1].id, ReplicaWorkClass::foreground, 1024, 200ms, true);
  EXPECT_EQ(first(0, ReplicaWorkClass::foreground), 1u);

  // Speculative work yields to foreground traffic when a replica is idle.
  selector.started(nodes[0].id, ReplicaWorkClass::foreground);
  EXPECT_NE(first(0, ReplicaWorkClass::speculative), 1u);
  selector.finished(nodes[0].id, ReplicaWorkClass::foreground, 1024, 20ms, true);

  selector.started(nodes[1].id, ReplicaWorkClass::foreground);
  selector.finished(nodes[1].id, ReplicaWorkClass::foreground, 0, 10ms, false);
  EXPECT_EQ(selector.stats(nodes[1].id).failures, 1u);
  EXPECT_NE(first(0, ReplicaWorkClass::foreground), 2u);
  selector.started(nodes[1].id, ReplicaWorkClass::foreground);
  selector.finished(nodes[1].id, ReplicaWorkClass::foreground, 1024, 25ms, true);
  EXPECT_EQ(selector.stats(nodes[1].id).failures, 1u);
}

}  // namespace
}  // namespace dagda::store
