//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The availability survey, repair by diff, the claim walk's restorer, the
// served requests' admission and the device reserve, on in-process clusters
// (requirements L7 to L11, S20, S30, section 7 item 4).
//
// Moved from Macha (develop 5a06d3c), converted to GoogleTest on real Dannan
// nodes, every assertion kept where it applies. Repair is diff only (Tom,
// 2026-11-04): Macha's cases that probed a peer (have_valid_objects,
// have_object) or walked the store keep their assertions about what is sent
// and settled, with the lists a survey would give in place of the probes,
// and where Macha asserted a probe, assert that none is made.
// - test_rpc_cluster.cpp: test_repair_push_probes_by_window_and_sends_together,
//   test_a_long_push_pass_does_not_hold_back_the_pull,
//   test_repair_push_sends_nothing_to_a_peer_that_cannot_say,
//   test_repair_pushes_past_a_peer_with_no_room,
//   test_repair_pushes_from_what_peers_hold_without_asking,
//   test_repair_pushes_only_what_the_list_names,
//   test_repair_probes_without_credit_and_transfers_only_with_it,
//   test_repair_pass_keeps_its_place_across_live_sets_and_restarts (its
//   restart half is the repair_position file, not moved),
//   test_repair_bandwidth_estimate_ignores_small_transfers,
//   test_repair_decides_already_held_without_reading_the_extent,
//   test_repair_keeps_a_pull_that_was_in_flight_when_its_turn_ended,
//   test_repair_is_paced_not_stopped_while_higher_classes_stay_busy (a
//   peer's viewers from its telemetry, as Macha; with this node's own
//   foreground beside them),
//   test_retained_missing_copy_repairs_without_namespace_reachability,
//   test_held_retention_claims_cost_repair_no_credit,
//   test_storage_data_credit_reserves_viewer_headroom_and_control (Dannan's
//   ping is Dannan's; the CONTROL request is have_control_objects).
// - test_storage_v18.cpp: test_repair_passes_unavailable_and_counts_unsourceable_objects,
//   test_a_busy_peer_still_says_what_it_does_not_hold,
//   test_min_write_floor_publishes_then_repair_converges_to_r2 (VALIDATION
//   "Placement and the write floor"),
//   test_nodes_replicating_to_each_other_do_not_hold_their_slots_across_the_peer,
//   test_repair_keeps_a_copy_it_has_nowhere_to_send,
//   test_node_that_stops_hosting_drains_through_repair.
// - test_storage_metadata.cpp: test_repair_step_is_bounded_and_yields.
// - test_availability.cpp: test_what_only_a_host_out_of_reach_holds_is_unavailable,
//   test_two_nodes_survey_what_neither_holds,
//   test_a_disk_coming_back_is_surveyed_without_a_namespace_change (the
//   survey's assertions; their path table is Macha's).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <map>
#include <thread>
#include <vector>

#include "dagda/store/distributed_store.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/reachability.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/net/scheduler.h"
#include "dannan/test/fixtures/gate.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using dannan::identity::NodeId;
using dannan::net::Handled;
using dannan::net::MessageType;
using dannan::sched::FrameType;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::pattern;
using test::wait_until;

// Repair's tests use 1 MiB extents, so credit for one accrues in a fraction
// of a second.
constexpr uint64_t extent = 1024 * 1024;

ClusterNodeOptions options(size_t replication, size_t write_copies = 1, uint64_t limit = 64ULL * 1024 * 1024) {
  ClusterNodeOptions out;
  out.limit = limit;
  out.store.replication = replication;
  out.store.write_copies = write_copies;
  out.store.extent_size = extent;
  return out;
}

// `count` distinct objects of `size` bytes held by `node`'s pool, sorted
// (Macha's local_objects).
std::vector<ObjectId> local_objects(ClusterNode& node, size_t count, size_t size, uint8_t salt) {
  std::vector<ObjectId> ids;
  for (size_t i = 0; i < count; ++i) {
    auto bytes = pattern(size, salt);
    bytes[0] = static_cast<uint8_t>(i);
    bytes[1] = static_cast<uint8_t>(i >> 8);
    bytes[2] = salt;
    const auto id = object_id(bytes);
    EXPECT_TRUE(node.data().put(id, bytes));
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::shared_ptr<const contract::InventoryHorizon> inventory_of(std::vector<ObjectId> ids, uint64_t generation = 1) {
  return std::make_shared<const contract::InventoryHorizon>(generation, true, std::move(ids), std::vector<ObjectId>{}, std::vector<contract::Garbage>{});
}

std::shared_ptr<const std::vector<ObjectId>> list_of(std::vector<ObjectId> ids) {
  std::sort(ids.begin(), ids.end());
  return std::make_shared<const std::vector<ObjectId>>(std::move(ids));
}

// What a survey would say each peer lacks.
std::shared_ptr<const ledger::PeerLacks> lacks_of(const std::map<NodeId, std::vector<ObjectId>>& lacks, bool complete = true, uint64_t generation = 1) {
  ledger::PeerLacks out;
  out.generation = generation;
  out.complete = complete;
  for (const auto& [peer, ids] : lacks) out.lacks.emplace(peer, list_of(ids));
  return std::make_shared<const ledger::PeerLacks>(std::move(out));
}

size_t probes(ClusterNode& node) {
  return node.calls_of(message::have_valid_objects) + node.calls_of(message::have_object) + node.calls_of(message::have_objects);
}

dannan::membership::NodeRecord record_of(ClusterNode& observer, const NodeId& id) {
  for (const auto& record : observer.context().nodes().active())
    if (record.id == id) return record;
  throw std::runtime_error("the observer does not see that node");
}

// Holds every request of `type` at `gate` until it opens.
test::Interpose hold(MessageType type, dannan::test::Gate& gate) {
  return [type, &gate](MessageType seen, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (seen == type) gate.enter_and_wait();
    return std::nullopt;
  };
}

// An owner's provider that refers to `ids`.
test::TestProvider referring_to(std::vector<ObjectId> ids) {
  test::TestProvider provider;
  provider.answer.data = std::move(ids);
  provider.answer.complete = true;
  return provider;
}

// Each node publishes what `provider` refers to, then runs its maintenance
// passes until `done` (step 7b: the node's own passes, which its thread runs
// when on; no provider stays registered, so its collector removes nothing).
bool converge(const std::vector<ClusterNode*>& nodes, test::TestProvider& provider, const std::function<bool()>& done,
              std::chrono::milliseconds bound = std::chrono::seconds(30)) {
  for (auto* node : nodes) node->publish_inventory(provider);
  return wait_until(
      [&] {
        for (auto* node : nodes) (void)node->run_maintenance_once();
        return done();
      },
      bound);
}

// An object of `size` bytes whose first placement choice, as `observer`
// ranks it, is `preferred` (Macha's preferred_for).
Bytes preferred_for(ClusterNode& observer, const NodeId& preferred, size_t size, uint8_t salt = 0) {
  auto data = pattern(size, salt);
  for (uint32_t i = 0; i < 4096; ++i) {
    for (size_t b = 0; b < 4 && b < data.size(); ++b) data[b] = static_cast<uint8_t>(i >> (8 * b));
    const auto ranked = observer.store().ranked(object_id(data));
    if (!ranked.empty() && ranked.front().id == preferred) return data;
  }
  throw std::runtime_error("could not find an object preferring the requested node");
}

// ---- Repair's steps (Macha's test_rpc_cluster.cpp) ---------------------------

// test_repair_push_probes_by_window_and_sends_together: a push step has
// nothing to look at when no peer lacks anything (Macha probed each object
// and settled it; here the survey already has, and nothing is asked), and
// sends what a peer lacks together, at speculative priority. Macha's assertion that eight puts are in
// flight at once meets Dannan's server, which runs at most its DATA workers
// less the foreground reserve of one class at a time: the sender sends all
// eight together, and the peer is seen running as many as it may.
TEST(StoreRepair, APushSendsItsBatchTogetherAndAsksNothing) {
  {
    Cluster cluster;
    auto& writer = cluster.add("writer", options(2));
    auto& peer = cluster.add("peer", options(2));
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    const auto live = local_objects(writer, 40, 8 * 1024, 30);
    const auto step = writer.store().repair_step({inventory_of(live), {}, lacks_of({{peer.id(), {}}}), {}}, 8ULL * 1024 * 1024, 16);
    EXPECT_EQ(step.push_examined, 0u);
    EXPECT_TRUE(step.complete);
    EXPECT_EQ(probes(peer), 0u);
    EXPECT_EQ(peer.calls_of(message::put_object), 0u);
  }
  {
    Cluster cluster;
    auto& writer = cluster.add("writer", options(2));
    auto& peer = cluster.add("peer", options(2));
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    const auto live = local_objects(writer, 8, 24 * 1024, 40);
    dannan::test::Gate puts;
    dannan::test::GateOpener opener(puts);
    peer.interpose(hold(message::put_object, puts));
    const dannan::net::ServerLimits limits;
    const auto concurrent = std::min(live.size(), limits.data_workers - limits.foreground_data_reserve);
    bool together = false;
    std::jthread release([&] {
      together = puts.wait_for_entries(concurrent, 5s);
      puts.open();
    });
    const auto step = writer.store().repair_step({inventory_of(live), {}, lacks_of({{peer.id(), live}}), {}}, 64ULL * 1024 * 1024, 16);
    release.join();
    EXPECT_TRUE(together);
    EXPECT_EQ(peer.calls_of(message::put_object), live.size());
    EXPECT_EQ(step.push_examined, live.size());
    EXPECT_EQ(step.bytes_transferred, live.size() * 24 * 1024);
    EXPECT_EQ(peer.frames_of(message::put_object), std::set<FrameType>{FrameType::speculative});
    EXPECT_EQ(probes(peer), 0u);
    for (const auto& id : live) EXPECT_TRUE(peer.data().has(id));
  }
}

// test_a_long_push_pass_does_not_hold_back_the_pull: the pull has its own
// share of a step: a push pass with more to look at than a step allows (here
// owners a survey has not yet heard from) does not hold the pull back.
TEST(StoreRepair, ALongPushPassDoesNotHoldBackThePull) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(3));
  auto& peer = cluster.add("peer", options(3));
  cluster.add("late", options(3));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto missing_bytes = pattern(2048, 7);
  const auto missing = object_id(missing_bytes);
  ASSERT_TRUE(peer.data().put(missing, missing_bytes));
  auto live = local_objects(writer, 100, 1024, 60);
  live.push_back(missing);
  std::sort(live.begin(), live.end());
  ASSERT_TRUE(writer.store().should_own(missing));
  const auto step = writer.store().repair_step({inventory_of(live), list_of({missing}), lacks_of({{peer.id(), {}}}), {}}, 8ULL * 1024 * 1024, 16);
  EXPECT_LT(step.push_examined, 100u);
  EXPECT_GT(step.pull_examined, 0u);
  EXPECT_TRUE(writer.data().has(missing));
}

// test_repair_push_sends_nothing_to_a_peer_that_cannot_say: a peer the
// survey could not settle is neither counted as keeping a copy nor sent one:
// an incomplete survey pushes nothing at all, and a peer missing from a
// complete one (here, one that joined since) is left undecided while the
// peers the survey heard from are pushed to. Once the survey says it lacks
// the copy, the copy is pushed. Nothing is ever asked of it.
TEST(StoreRepair, APushSendsNothingToAPeerTheSurveyCouldNotSettle) {
  for (const bool complete : {false, true}) {
    Cluster cluster;
    auto& writer = cluster.add("writer", options(3));
    auto& peer = cluster.add("peer", options(3));
    auto& late = cluster.add("late", options(3));
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    const auto live = local_objects(writer, 4, 8 * 1024, 50);
    const auto unsettled = complete ? lacks_of({{peer.id(), live}}) : lacks_of({{peer.id(), live}, {late.id(), live}}, false);
    const auto step = writer.store().repair_step({inventory_of(live), {}, unsettled, {}}, 8ULL * 1024 * 1024, 16);
    EXPECT_EQ(step.push_examined, complete ? live.size() : 0u);
    EXPECT_EQ(peer.calls_of(message::put_object), complete ? live.size() : 0u);
    EXPECT_EQ(late.calls_of(message::put_object), 0u);
    EXPECT_EQ(probes(peer) + probes(late), 0u);
    for (const auto& id : live) EXPECT_TRUE(writer.data().has(id));

    const auto said = lacks_of({{peer.id(), complete ? std::vector<ObjectId>{} : live}, {late.id(), live}});
    EXPECT_TRUE(wait_until(
        [&] {
          (void)writer.store().repair_step({inventory_of(live), {}, said, {}}, 8ULL * 1024 * 1024, 16);
          return late.calls_of(message::put_object) == live.size();
        },
        10s));
    EXPECT_EQ(peer.calls_of(message::put_object), live.size());
    EXPECT_EQ(probes(peer) + probes(late), 0u);
  }
}

// test_repair_pushes_past_a_peer_with_no_room: a peer whose advertised free
// space cannot take an extent is sent nothing; the copy goes to the next node
// in rank, and the local copy stays. Once the peer has room it is pushed to.
TEST(StoreRepair, APushPassesAPeerWithNoRoom) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(3));
  auto& full = cluster.add("full", options(3, 1, 2 * extent));
  auto& fallback = cluster.add("fallback", options(3));
  auto& owner = cluster.add("owner", options(3));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  // Within an extent of its limit, as its adverts say.
  const auto filler = local_objects(full, 3, 512 * 1024, 90);
  full.server().advertise();
  ASSERT_TRUE(wait_until([&] { return record_of(writer, full.id()).advert_u64(used_advert).value_or(0) + extent > 2 * extent; }));

  // An object the full peer would own: among the first three in rank.
  Bytes bytes;
  for (uint32_t salt = 0;; ++salt) {
    ASSERT_LT(salt, 4096u);
    bytes = pattern(64 * 1024 + salt, 31);
    const auto ranked = writer.store().ranked(object_id(bytes));
    if (ranked.size() == 4 && ranked[3].id == fallback.id() &&
        std::any_of(ranked.begin(), ranked.begin() + 3, [&](const dannan::membership::NodeRecord& node) { return node.id == full.id(); }))
      break;
  }
  const auto id = object_id(bytes);
  ASSERT_TRUE(writer.data().put(id, bytes));
  const std::vector<ObjectId> live{id};
  const auto lacks = lacks_of({{full.id(), live}, {fallback.id(), live}, {owner.id(), live}});
  for (int pass = 0; pass < 4; ++pass) (void)writer.store().repair_step({inventory_of(live), {}, lacks, {}}, 8ULL * 1024 * 1024, 16);
  EXPECT_EQ(full.calls_of(message::put_object), 0u);
  EXPECT_EQ(probes(full), 0u);
  EXPECT_GT(fallback.calls_of(message::put_object), 0u);
  EXPECT_GT(owner.calls_of(message::put_object), 0u);
  EXPECT_TRUE(writer.data().has(id));

  for (const auto& filled : filler) ASSERT_TRUE(full.data().remove(filled));
  full.server().advertise();
  ASSERT_TRUE(wait_until([&] { return record_of(writer, full.id()).advert_u64(used_advert).value_or(0) + extent <= 2 * extent; }));
  const auto again = lacks_of({{full.id(), live}, {fallback.id(), {}}, {owner.id(), {}}});
  for (int pass = 0; pass < 4 && full.calls_of(message::put_object) == 0; ++pass)
    (void)writer.store().repair_step({inventory_of(live), {}, again, {}}, 8ULL * 1024 * 1024, 16);
  EXPECT_GT(full.calls_of(message::put_object), 0u);
}

// test_repair_pushes_from_what_peers_hold_without_asking: what a peer holds
// is known from the diff of its held trie: an object every owner holds
// settles without a call, one the peer lacks is sent without a probe, and
// one the survey knows nothing of is left alone (Macha probed it; diff only,
// nothing is asked).
TEST(StoreRepair, APushWorksFromWhatPeersHoldWithoutAsking) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(writer, 3, 64 * 1024, 60);
  constexpr uint64_t budget = 64ULL * 1024 * 1024;

  const auto settled = writer.store().repair_step({inventory_of(live), {}, lacks_of({{peer.id(), {}}}), {}}, budget, 16);
  // Nothing listed: nothing to look at.
  EXPECT_EQ(settled.push_examined, 0u);
  EXPECT_TRUE(settled.complete);
  EXPECT_EQ(probes(peer), 0u);
  EXPECT_EQ(peer.calls_of(message::put_object), 0u);

  const auto lacks_one = lacks_of({{peer.id(), {live[1]}}});
  for (int step = 0; step < 8 && peer.calls_of(message::put_object) == 0; ++step)
    (void)writer.store().repair_step({inventory_of(live), {}, lacks_one, {}}, budget, 16);
  EXPECT_EQ(peer.calls_of(message::put_object), 1u);
  EXPECT_EQ(probes(peer), 0u);

  (void)writer.store().repair_step({inventory_of(live), {}, lacks_of({}), {}}, budget, 16);
  EXPECT_EQ(probes(peer), 0u);
  EXPECT_EQ(peer.calls_of(message::put_object), 1u);
}

// test_repair_pushes_only_what_the_list_names: the push considers only what
// peers lack: it sends what is listed and held here, examines nothing else
// the store holds, passes over a listed object this node no longer holds,
// and an empty list examines nothing.
TEST(StoreRepair, APushSendsOnlyWhatTheListNames) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(writer, 6, 64 * 1024, 90);
  constexpr uint64_t budget = 64ULL * 1024 * 1024;
  const auto lacking = live[2];

  const auto idle = writer.store().repair_step({inventory_of(live), {}, lacks_of({{peer.id(), {}}}), {}}, budget, 16);
  EXPECT_EQ(idle.push_examined, 0u);
  EXPECT_EQ(peer.calls_of(message::put_object), 0u);

  // Listed and gone from here: passed over, nothing read or sent for it.
  const auto gone = object_id(pattern(64 * 1024, 99));
  auto referenced = live;
  referenced.push_back(gone);
  std::sort(referenced.begin(), referenced.end());
  const auto listed = lacks_of({{peer.id(), {lacking, gone}}});
  size_t examined = 0;
  for (int step = 0; step < 8 && peer.calls_of(message::put_object) == 0; ++step)
    examined += writer.store().repair_step({inventory_of(referenced), {}, listed, {}}, budget, 16).push_examined;
  EXPECT_EQ(peer.calls_of(message::put_object), 1u);
  EXPECT_TRUE(peer.data().has(lacking));
  EXPECT_EQ(probes(peer), 0u);
  EXPECT_LE(examined, 2u);
  EXPECT_EQ(writer.store().repair_diagnostics().local_unreadable, 0u);
}

// test_repair_probes_without_credit_and_transfers_only_with_it: credit pays
// only for a transfer's bytes. A push waits for credit of its object's size;
// a pull passes held objects by index, waits for an extent of credit, and
// stays on the object it could not afford.
TEST(StoreRepair, ATransferWaitsForItsCredit) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto held_bytes = pattern(256 * 1024, 41);
  const auto held = object_id(held_bytes);
  const auto wanted_bytes = pattern(256 * 1024, 42);
  const auto wanted = object_id(wanted_bytes);
  ASSERT_TRUE(peer.data().put(wanted, wanted_bytes));
  ASSERT_TRUE(writer.data().put(held, held_bytes));
  ASSERT_TRUE(writer.store().should_own(wanted));

  const auto push_lacks = lacks_of({{peer.id(), {held}}});
  auto push = writer.store().repair_step({inventory_of({held}), {}, push_lacks, {}}, held_bytes.size() - 1, 16);
  EXPECT_EQ(peer.calls_of(message::put_object), 0u);
  EXPECT_TRUE(push.credit_limited);
  EXPECT_FALSE(push.complete);
  push = writer.store().repair_step({inventory_of({held}), {}, push_lacks, {}}, held_bytes.size(), 16);
  EXPECT_EQ(peer.calls_of(message::put_object), 1u);
  EXPECT_FALSE(push.credit_limited);

  const RepairLists pull_lists{inventory_of({held, wanted}), list_of({held, wanted}), lacks_of({{peer.id(), {}}}), {}};
  RepairResult pull;
  for (int step = 0; step < 8 && !pull.credit_limited; ++step) pull = writer.store().repair_step(pull_lists, extent - 1, 16);
  EXPECT_TRUE(pull.credit_limited);
  EXPECT_EQ(peer.calls_of(message::get_object), 0u);
  EXPECT_FALSE(writer.data().has(wanted));
  for (int step = 0; step < 8 && !writer.data().has(wanted); ++step) (void)writer.store().repair_step(pull_lists, extent, 16);
  EXPECT_EQ(peer.calls_of(message::get_object), 1u);
  EXPECT_TRUE(writer.data().has(wanted));
}

// test_repair_pass_keeps_its_place_across_live_sets_and_restarts: the push
// keeps its place when a new survey's lists arrive (each object passed again
// would be sent again), and a pass a new list arrived part-way through is not
// reported settled; a pass that one list saw whole is. (The restart half kept
// the place in the repair_position file, which is not moved: repair is diff
// only.)
TEST(StoreRepair, APushKeepsItsPlaceAcrossANewList) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(writer, 100, 4 * 1024, 50);
  constexpr uint64_t budget = 64ULL * 1024 * 1024;
  const auto first = writer.store().repair_step({inventory_of(live), {}, lacks_of({{peer.id(), live}}), {}}, budget, 16);
  EXPECT_EQ(first.push_examined, 16u);
  EXPECT_EQ(peer.calls_of(message::put_object), 16u);

  // A new survey, taken before those copies landed, saying the same: the
  // pass goes on from where it was, and sends none of the first again.
  const auto second_lists = lacks_of({{peer.id(), live}});
  const auto second = writer.store().repair_step({inventory_of(live), {}, second_lists, {}}, budget, 16);
  EXPECT_EQ(second.push_examined, 16u);
  EXPECT_EQ(peer.calls_of(message::put_object), 32u);
  for (size_t i = 0; i < live.size(); ++i) EXPECT_EQ(peer.data().has(live[i]), i < 32) << i;
  std::optional<bool> spanning;
  for (int step = 0; step < 20 && !spanning; ++step) {
    const auto passes = writer.store().repair_diagnostics().passes_completed;
    const auto result = writer.store().repair_step({inventory_of(live), {}, second_lists, {}}, budget, 16);
    if (writer.store().repair_diagnostics().passes_completed > passes) spanning = result.complete;
  }
  ASSERT_TRUE(spanning.has_value());
  EXPECT_FALSE(*spanning);
  EXPECT_EQ(peer.calls_of(message::put_object), live.size());
  // A survey taken now: the peer lacks nothing, and the pass is settled.
  const auto now_lists = lacks_of({{peer.id(), {}}});
  for (int pass = 0; pass < 2; ++pass) {
    const auto settled = writer.store().repair_step({inventory_of(live), {}, now_lists, {}}, budget, 16);
    EXPECT_TRUE(settled.complete);
  }
  EXPECT_EQ(peer.calls_of(message::put_object), live.size());
}

// test_repair_bandwidth_estimate_ignores_small_transfers: small pushes
// measure round trips and the far end's writes, not bandwidth, so only a
// transfer of half an extent or more moves the link's estimate.
TEST(StoreRepair, TheLinkEstimateIgnoresSmallTransfers) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto small = local_objects(writer, 1, 16 * 1024, 71);
  (void)writer.store().repair_step({inventory_of(small), {}, lacks_of({{peer.id(), small}}), {}}, 64ULL * 1024 * 1024, 16);
  ASSERT_EQ(peer.calls_of(message::put_object), 1u);
  EXPECT_EQ(writer.store().estimated_network_bps(), 0.0);

  const auto large = local_objects(writer, 1, extent / 2, 72);
  (void)writer.store().repair_step({inventory_of(large), {}, lacks_of({{peer.id(), large}}), {}}, 64ULL * 1024 * 1024, 16);
  ASSERT_EQ(peer.calls_of(message::put_object), 2u);
  EXPECT_GT(writer.store().estimated_network_bps(), 0.0);
}

// test_repair_decides_already_held_without_reading_the_extent: the pull
// decides "already held" by index, never by reading the object (no DATA
// admission), and the progress shows in the diagnostics.
TEST(StoreRepair, APullDecidesHeldByIndex) {
  Cluster cluster;
  auto& node = cluster.add("solo", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(node, 24, 64 * 1024, 200);
  const auto before = node.credit()->stats().speculative_admissions;
  size_t examined = 0;
  for (int pass = 0; pass < 8 && examined < live.size(); ++pass)
    examined += node.store().repair_step({inventory_of(live), list_of(live), lacks_of({}), {}}, 8ULL * 1024 * 1024, 16).pull_examined;
  EXPECT_EQ(examined, live.size());
  EXPECT_EQ(node.credit()->stats().speculative_admissions, before);
  EXPECT_EQ(node.store().repair_diagnostics().pull_examined, examined);
}

// test_repair_keeps_a_pull_that_was_in_flight_when_its_turn_ended: repair
// yields between operations, never inside one: a pull whose turn ends while
// the peer is answering still lands.
TEST(StoreRepair, APullInFlightWhenItsTurnEndsIsKept) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(512 * 1024, 77);
  const auto id = object_id(bytes);
  ASSERT_TRUE(peer.data().put(id, bytes));
  std::atomic<unsigned> fetches{0};
  std::atomic<unsigned> polls_in_flight{0};
  std::atomic<bool> fetching{false};
  peer.interpose([&](MessageType type, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (type != message::get_object) return std::nullopt;
    ++fetches;
    fetching = true;
    // Holds the reply long enough for a poll of the yield predicate.
    (void)wait_until([&] { return polls_in_flight.load() > 0; }, 100ms);
    fetching = false;
    return std::nullopt;
  });
  ASSERT_TRUE(writer.store().should_own(id));
  const auto result = writer.store().repair_step({inventory_of({id}), list_of({id}), lacks_of({{peer.id(), {}}}), {}}, 8ULL * 1024 * 1024, 8, [&] {
    if (fetching.load()) ++polls_in_flight;
    return fetches.load() > 0;
  });
  EXPECT_EQ(fetches.load(), 1u);
  EXPECT_EQ(result.bytes_transferred, bytes.size());
  ASSERT_TRUE(writer.data().valid(id));
  EXPECT_EQ(writer.data().get(id), bytes);
}

// test_repair_step_is_bounded_and_yields (test_storage_metadata.cpp): a step
// that must yield moves nothing; one whose budget cannot cover the transfer
// says it is incomplete rather than quiescent; then the push completes
// within its budget, and nothing walked the store.
TEST(StoreRepair, AStepIsBoundedAndYields) {
  Cluster cluster;
  auto& first = cluster.add("first", options(2));
  auto& second = cluster.add("second", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(512 * 1024);
  const auto id = object_id(bytes);
  ASSERT_TRUE(first.data().put(id, bytes));
  ASSERT_FALSE(second.data().has(id));
  const RepairLists lists{inventory_of({id}), {}, lacks_of({{second.id(), {id}}}), {}};
  const auto full_lists_before = first.data().full_list_scans();

  const auto yielded = first.store().repair_step(lists, 8ULL * 1024 * 1024, 8, [] { return true; });
  EXPECT_TRUE(yielded.yielded);
  EXPECT_FALSE(yielded.complete);
  EXPECT_EQ(yielded.bytes_transferred, 0u);
  EXPECT_FALSE(second.data().has(id));

  // Macha's step here had one call for a probe and none for the upload;
  // with no probe, the budget that cannot cover the upload is the bytes'.
  const auto bounded = first.store().repair_step(lists, bytes.size() - 1, 8);
  EXPECT_FALSE(bounded.complete);
  EXPECT_EQ(bounded.bytes_transferred, 0u);
  EXPECT_EQ(bounded.remote_operations, 0u);
  EXPECT_FALSE(second.data().has(id));

  const auto completed = first.store().repair_step(lists, 8ULL * 1024 * 1024, 8);
  EXPECT_EQ(completed.bytes_transferred, bytes.size());
  EXPECT_TRUE(completed.complete);
  EXPECT_LE(completed.remote_operations, 8u);
  EXPECT_TRUE(second.data().has(id));
  EXPECT_EQ(first.data().full_list_scans(), full_lists_before);
  for (const auto& step : {yielded, bounded, completed}) {
    EXPECT_LE(step.push_examined, 64u);
    EXPECT_LE(step.pull_examined, 64u);
    EXPECT_LE(step.push_examined + step.pull_examined, 64u);
  }
}

// test_repair_is_paced_not_stopped_while_higher_classes_stay_busy: the pass
// paces repair by weight against busier classes and never stops it: with a
// peer's foreground traffic throughout (seen in its telemetry), then this
// node's own foreground work, then its loader never going quiet, each
// missing claimed copy still comes back, and the share is seen turning
// passes away between repair's turns.
TEST(StoreRepair, RepairIsPacedNotStoppedWhileHigherClassesStayBusy) {
  Cluster cluster;
  auto busy = options(1);
  busy.repair.foreground_quiet = 500ms;
  auto& s1 = cluster.add("s1", busy);
  auto& s2 = cluster.add("s2", busy);
  auto watching = busy;
  watching.hosts_extents = false;
  auto& viewer = cluster.add("viewer", watching);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto storage = s2.context().events().kind("storage");

  // An object s1 holds and s2 is claimed for; s2 is told storage changed.
  const auto lose_a_copy = [&](uint8_t salt) {
    const auto bytes = pattern(96 * 1024 + 31, salt);
    const auto id = object_id(bytes);
    EXPECT_TRUE(s1.data().put(id, bytes));
    const contract::RetentionDot claim{s1.id(), 0xfeed00u + salt};
    s1.claims().retain(contract::RetentionClass::data, id, claim);
    s2.claims().retain(contract::RetentionClass::data, id, claim);
    EXPECT_FALSE(s2.data().valid(id));
    s2.context().events().notify(storage);
    return std::pair{id, bytes};
  };
  const auto run_until = [&](const ObjectId& id) {
    return wait_until(
        [&] {
          (void)s2.run_maintenance_once();
          // A second repair pass straight after: a whole node pass now runs
          // the local stages too, which can outlast a rest, and the share is
          // seen refusing only a pass made within one.
          (void)s2.repair().pass();
          return s2.data().valid(id);
        },
        10s);
  };

  // Another node playing from s1 throughout: s1's foreground traffic, in
  // its telemetry, is a peer's foreground to s2; s2's own clocks stay quiet.
  {
    const auto playing = pattern(256 * 1024, 9);
    const auto playing_id = object_id(playing);
    ASSERT_TRUE(s1.data().put(playing_id, playing));
    // The share's refusals are counted over the whole busy period: credit
    // banked meanwhile can bring the copy back on the first turn.
    const auto share_before = s2.repair().gates().share;
    std::atomic<bool> watching_on{true};
    std::thread player([&] {
      ReadOptions read;
      read.frame = FrameType::foreground;
      while (watching_on.load()) (void)viewer.store().get(playing_id, read);
    });
    const bool seen = wait_until([&] { return s2.run_maintenance_once().repair.paced_by & paced_by_peer_foreground; }, 10s);
    const auto [id, bytes] = lose_a_copy(1);
    const bool restored = run_until(id);
    const auto gates = s2.repair().gates();
    watching_on = false;
    player.join();
    ASSERT_TRUE(seen);
    ASSERT_TRUE(restored);
    EXPECT_EQ(s2.data().get(id), bytes);
    EXPECT_GT(gates.share, share_before);
    EXPECT_TRUE(gates.paced_by & paced_by_peer_foreground);
  }

  for (const auto frame : {FrameType::foreground, FrameType::loader}) {
    std::atomic<bool> working{true};
    std::thread worker([&] {
      while (working.load()) {
        s2.context().activity().note(frame, 64 * 1024);
        std::this_thread::sleep_for(5ms);
      }
    });
    // The busy class seen holding repair to its share, then a copy lost.
    const auto share_before = s2.repair().gates().share;
    ASSERT_TRUE(wait_until([&] {
      (void)s2.run_maintenance_once();
      return s2.repair().gates().share > share_before;
    }));
    const auto [id, bytes] = lose_a_copy(frame == FrameType::foreground ? 2 : 3);
    const bool restored = run_until(id);
    const auto gates = s2.repair().gates();
    working = false;
    worker.join();
    ASSERT_TRUE(restored);
    EXPECT_EQ(s2.data().get(id), bytes);
    EXPECT_GT(gates.share, share_before);
    EXPECT_TRUE(gates.paced_by & (frame == FrameType::foreground ? paced_by_foreground : paced_by_loader));
  }
}

// test_retained_missing_copy_repairs_without_namespace_reachability: a
// claimed copy lost here, which nothing referenced names (the owner's state
// does not reach it), is brought back by the claim walk alone, with a peer
// that held it gone.
TEST(StoreRepair, ARetainedMissingCopyIsRestoredWithoutReachability) {
  Cluster cluster(std::chrono::milliseconds(600));
  auto& s1 = cluster.add("s1", options(1));
  auto& s2 = cluster.add("s2", options(1));
  auto& s3 = cluster.add("s3", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(96 * 1024 + 13);
  const auto id = object_id(bytes);
  ASSERT_TRUE(s1.data().put(id, bytes));
  ASSERT_TRUE(s2.data().put(id, bytes));
  const contract::RetentionDot claim{s1.id(), 0xf00d};
  s1.claims().retain(contract::RetentionClass::data, id, claim);
  s2.claims().retain(contract::RetentionClass::data, id, claim);

  const auto s3_id = s3.id();
  s3.stop();
  ASSERT_TRUE(cluster.dead_to(s1, s3_id));
  ASSERT_TRUE(cluster.dead_to(s2, s3_id));
  ASSERT_TRUE(s2.data().remove(id));
  EXPECT_TRUE(s2.claims().retained(contract::RetentionClass::data, id));
  EXPECT_FALSE(s2.data().valid(id));
  s2.context().events().notify(s2.context().events().kind("storage"));

  // Nothing referenced names it: no inventory is even published.
  ASSERT_TRUE(wait_until(
      [&] {
        (void)s2.run_maintenance_once();
        return s2.data().valid(id);
      },
      10s));
  EXPECT_EQ(s2.data().get(id), bytes);
  EXPECT_TRUE(s2.claims().retained(contract::RetentionClass::data, id));
}

// test_held_retention_claims_cost_repair_no_credit: claims already held cost
// no credit: with credit at one extent a second and 64 claimed objects held,
// the one this node lacks comes back promptly.
TEST(StoreRepair, HeldClaimsCostRepairNoCredit) {
  Cluster cluster;
  auto metered = options(1);
  metered.repair.idle_bandwidth_fraction = 1.0;
  metered.repair.max_bandwidth = extent;
  auto& s1 = cluster.add("s1", metered);
  auto& s2 = cluster.add("s2", metered);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const contract::RetentionDot claim{s2.id(), 0xc0de};
  for (int i = 0; i < 64; ++i) {
    const auto held_bytes = pattern(16 * 1024, static_cast<uint8_t>(500 + i));
    auto distinct = held_bytes;
    distinct[0] = static_cast<uint8_t>(i);
    const auto held = object_id(distinct);
    ASSERT_TRUE(s2.data().put(held, distinct));
    s2.claims().retain(contract::RetentionClass::data, held, claim);
  }
  const auto bytes = pattern(96 * 1024 + 7);
  const auto id = object_id(bytes);
  ASSERT_TRUE(s1.data().put(id, bytes));
  s2.claims().retain(contract::RetentionClass::data, id, claim);
  ASSERT_FALSE(s2.data().valid(id));
  s2.context().events().notify(s2.context().events().kind("storage"));

  ASSERT_TRUE(wait_until(
      [&] {
        (void)s2.run_maintenance_once();
        return s2.data().valid(id);
      },
      5s));
  EXPECT_EQ(s2.data().get(id), bytes);
}

// test_storage_data_credit_reserves_viewer_headroom_and_control: with every
// byte the lower classes may hold held by loaders, a loader's request waits,
// a foreground request uses the reserve and is answered at once, a CONTROL
// request takes no DATA credit at all, and a validation that must read is
// refused rather than queued; a release lets the waiting loader through.
TEST(StoreRepair, ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit) {
  Cluster cluster;
  auto server_options = options(1);
  server_options.data_inflight_bytes = 4 * extent;
  server_options.foreground_reserve_bytes = extent;
  server_options.background_concurrency = 4;
  auto& server = cluster.add("server", server_options);
  auto& client = cluster.add("client", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 77);
  const auto id = object_id(bytes);
  ASSERT_TRUE(server.data().put(id, bytes));
  auto& credit = *server.credit();
  const DataWorkContext loader(FrameType::loader, extent);
  auto first = credit.acquire(loader, extent);
  auto second = credit.acquire(loader, extent);
  auto third = credit.acquire(loader, extent);
  ASSERT_TRUE(first && second && third);

  const auto target = record_of(client, server.id());
  auto& routes = client.context().routes();
  auto blocked_loader =
      std::async(std::launch::async, [&] { return routes.call_and_wait(target, message::get_object, message::encode_id(id), FrameType::loader, 20s); });
  ASSERT_TRUE(wait_until([&] { return credit.stats().loader_waits >= 1; }, 5s));

  // The foreground request uses the reserved credit.
  auto started = Clock::now();
  const auto foreground = routes.call_and_wait(target, message::get_object, message::encode_id(id), FrameType::foreground, 2s);
  ASSERT_TRUE(foreground.reply);
  EXPECT_EQ(foreground.reply->type, message::object_reply);
  EXPECT_LT(Clock::now() - started, 1s);

  // CONTROL is outside the arbiter.
  started = Clock::now();
  const auto control = routes.call_and_wait(target, message::have_control_objects, message::encode_ids(std::vector<ObjectId>{id}), FrameType::control, 2s);
  ASSERT_TRUE(control.reply);
  EXPECT_EQ(control.reply->type, message::have_control_objects_reply);
  EXPECT_LT(Clock::now() - started, 1s);

  // Validation reads the whole object, so it goes through speculative DATA
  // admission, and is refused while there is none.
  const auto validation = routes.call_and_wait(target, message::have_object, message::encode_id(id), FrameType::speculative, 2s);
  ASSERT_TRUE(validation.reply);
  ASSERT_TRUE(dannan::net::refusal_of(*validation.reply).has_value());
  EXPECT_EQ(dannan::net::refusal_of(*validation.reply)->status, 503);
  EXPECT_EQ(blocked_loader.wait_for(20ms), std::future_status::timeout);

  first.reset();
  ASSERT_EQ(blocked_loader.wait_for(5s), std::future_status::ready);
  const auto loaded = blocked_loader.get();
  ASSERT_TRUE(loaded.reply);
  EXPECT_EQ(loaded.reply->type, message::object_reply);
  const auto validated = routes.call_and_wait(target, message::have_object, message::encode_id(id), FrameType::speculative, 2s);
  ASSERT_TRUE(validated.reply);
  EXPECT_EQ(validated.reply->type, message::bool_reply);

  const auto stats = credit.stats();
  EXPECT_GE(stats.foreground_admissions, 1u);
  EXPECT_GE(stats.loader_waits, 1u);
  EXPECT_EQ(stats.peak_used_bytes, 4 * extent);
}

// ---- Repair's cases from Macha's test_storage_v18.cpp -------------------------

// test_repair_passes_unavailable_and_counts_unsourceable_objects: repair does
// no work that cannot complete, and says what it cannot do: an object the
// survey marks unavailable is passed without a fetch; one nobody can answer
// for now is asked again, not counted; and one every peer says it does not
// hold is counted and sampled once, never a held one.
TEST(StoreRepair, RepairPassesUnavailableAndCountsUnsourceable) {
  Cluster cluster;
  auto& node = cluster.add("lonely", options(2, 1, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto present_bytes = pattern(64 * 1024, 7);
  const auto present = object_id(present_bytes);
  ASSERT_TRUE(node.store().put(present, present_bytes));
  const auto missing = object_id(pattern(64 * 1024, 9));
  ASSERT_FALSE(node.data().has(missing));

  bool marked = true;
  std::vector<ObjectId> asked;
  const auto unavailable = [&](const ObjectId& id) {
    asked.push_back(id);
    return marked;
  };
  const auto step = [&](const std::vector<ObjectId>& live, std::vector<ObjectId> pull) {
    return node.store().repair_step({inventory_of(live), list_of(std::move(pull)), lacks_of({}), unavailable}, 4ULL * 1024 * 1024, 16);
  };

  // A whole pass over the marked object examines it and attempts nothing.
  const auto passed = step({missing}, {missing});
  EXPECT_EQ(asked, std::vector<ObjectId>{missing});
  EXPECT_EQ(passed.pull_examined, 1u);
  EXPECT_EQ(passed.bytes_transferred, 0u);
  EXPECT_EQ(node.store().repair_diagnostics().pull_unsourceable, 0u);

  // Objects that need nothing do not spend the step's scan budget: a long
  // stretch of them passes in one step.
  {
    std::vector<ObjectId> settled;
    for (int i = 0; i < 1000; ++i) settled.push_back(object_id(pattern(256 + static_cast<size_t>(i), static_cast<uint8_t>(i))));
    std::sort(settled.begin(), settled.end());
    const auto long_pass = step(settled, settled);
    EXPECT_EQ(long_pass.pull_examined, settled.size());
    EXPECT_EQ(long_pass.bytes_transferred, 0u);
    asked.clear();

    // Given what this node lacks, the pull considers only that, however
    // large what is referenced beside it.
    std::vector<ObjectId> lacking{settled[10], settled[500]};
    std::sort(lacking.begin(), lacking.end());
    const auto listed = step(settled, lacking);
    EXPECT_EQ(listed.pull_examined, lacking.size());
    EXPECT_EQ(asked, lacking);
    asked.clear();
    // Lacking nothing, the pull does nothing.
    const auto none = step(settled, {});
    EXPECT_EQ(none.pull_examined, 0u);
    EXPECT_TRUE(asked.empty());
  }

  // Unmarked, with no peer to ask, nothing settles it now: not counted.
  marked = false;
  (void)step({missing}, {missing});
  (void)step({missing}, {missing});
  EXPECT_EQ(node.store().repair_diagnostics().pull_unsourceable, 0u);

  // A peer with every disk online that does not hold it says so, and the
  // peer's arrival makes the waiting object due at once.
  auto& peer = cluster.add("peer", options(2, 1, 8ULL * 1024 * 1024));
  peer.start();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until(
      [&] {
        (void)step({missing}, {missing});
        return node.store().repair_diagnostics().pull_unsourceable > 0;
      },
      10s));
  EXPECT_FALSE(peer.data().has(missing));
  // With a held object beside it, only the missing one is unsourceable.
  std::vector<ObjectId> live{present, missing};
  std::sort(live.begin(), live.end());
  (void)step(live, live);
  const auto after = node.store().repair_diagnostics();
  ASSERT_EQ(after.unsourceable_sample.size(), 1u);
  EXPECT_EQ(after.unsourceable_sample.front(), missing);
  EXPECT_TRUE(node.data().has(present));

  // Repeated passes keep the sample unique while the counter climbs.
  (void)step(live, live);
  const auto repeated = node.store().repair_diagnostics();
  EXPECT_EQ(repeated.unsourceable_sample.size(), 1u);
  EXPECT_GE(repeated.pull_unsourceable, after.pull_unsourceable);
}

// test_a_busy_peer_still_says_what_it_does_not_hold: a peer whose DATA
// admission is full still says what it does not hold: absence is an index
// lookup and needs no admission. Only a held copy, which must be read back to
// count, is "cannot say now" until admission frees.
TEST(StoreRepair, ABusyPeerStillSaysWhatItDoesNotHold) {
  Cluster cluster;
  auto& a = cluster.add("busy-a", options(2, 1, 8ULL * 1024 * 1024));
  auto& b = cluster.add("busy-b", options(2, 1, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto peer = record_of(a, b.id());
  const auto held_bytes = pattern(64 * 1024, 31);
  const auto held = object_id(held_bytes);
  ASSERT_TRUE(b.data().put(held, held_bytes));
  const auto absent = object_id(pattern(64 * 1024, 32));
  const auto ask = [&] {
    const auto outcome = a.context().routes().call_and_wait(peer, message::have_valid_objects, message::encode_ids(std::vector<ObjectId>{held, absent}),
                                                            FrameType::speculative, 5s);
    EXPECT_TRUE(outcome.reply && outcome.reply->type == message::have_valid_objects_reply);
    if (!outcome.reply || outcome.reply->type != message::have_valid_objects_reply) return std::vector<message::Validity>{};
    return message::decode_validity(outcome.reply->payload, 2);
  };
  using V = message::Validity;
  EXPECT_EQ(ask(), (std::vector<V>{V::intact, V::absent}));
  EXPECT_EQ(a.store().valid_on(peer, {held, absent}), (std::map<ObjectId, bool>{{held, true}, {absent, false}}));
  {
    // Every byte the background classes may hold, held.
    auto& credit = *b.credit();
    const auto stats = credit.stats();
    const auto full = credit.try_acquire(DataWorkContext(FrameType::speculative, 0), stats.capacity_bytes - stats.foreground_reserve_bytes);
    ASSERT_TRUE(full);
    EXPECT_EQ(ask(), (std::vector<V>{V::cannot_say, V::absent}));
    EXPECT_EQ(a.store().valid_on(peer, {held, absent}), (std::map<ObjectId, bool>{{absent, false}}));
  }
  EXPECT_EQ(ask(), (std::vector<V>{V::intact, V::absent}));
}

// test_min_write_floor_publishes_then_repair_converges_to_r2 (VALIDATION
// "Placement and the write floor"): a write at a floor of one publishes on
// the one node there is, and when a second owner joins, repair by diff
// brings the object to it.
TEST(StoreRepair, AWriteAtTheFloorConvergesToTwoCopiesThroughRepair) {
  Cluster cluster;
  auto& first = cluster.add("first", options(2, 1, 8ULL * 1024 * 1024));
  first.start();
  ASSERT_TRUE(cluster.converged());
  const auto data = pattern(256 * 1024, 42);
  const auto id = object_id(data);
  ASSERT_TRUE(first.store().put(id, data));
  EXPECT_TRUE(first.data().has(id));

  auto& second = cluster.add("second", options(2, 1, 8ULL * 1024 * 1024));
  second.start();
  ASSERT_TRUE(cluster.converged());
  EXPECT_FALSE(second.data().has(id));

  auto provider = referring_to({id});
  ASSERT_TRUE(converge({&first, &second}, provider, [&] { return first.data().has(id) && second.data().has(id); }));
  ASSERT_TRUE(second.data().get(id).has_value());
  EXPECT_EQ(*second.data().get(id), data);
}

// test_nodes_replicating_to_each_other_do_not_hold_their_slots_across_the_peer:
// each node has one background DATA slot and replicates its own objects to
// the other. A sender that kept its slot while the peer admitted the object
// would leave both nodes waiting on each other's slot.
TEST(StoreRepair, NodesReplicatingToEachOtherDoNotHoldTheirSlotsAcrossThePeer) {
  Cluster cluster;
  auto cross = options(2);
  cross.background_concurrency = 1;
  auto& a = cluster.add("cross-a", cross);
  auto& b = cluster.add("cross-b", cross);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  constexpr size_t threads_per_node = 4;
  constexpr size_t objects_per_thread = 8;
  std::atomic<size_t> done{0};
  std::atomic<size_t> on_both{0};
  std::vector<std::jthread> writers;
  for (auto* node : {&a, &b})
    for (size_t t = 0; t < threads_per_node; ++t)
      writers.emplace_back([&, node, t] {
        for (size_t i = 0; i < objects_per_thread; ++i) {
          const auto seed = static_cast<uint8_t>((node == &a ? 0 : 100) + t * 10 + i);
          const auto data = pattern(64 * 1024, seed);
          if (node->store().replicate_all(object_id(data), data) == 2) on_both.fetch_add(1);
          done.fetch_add(1);
        }
      });
  const size_t total = 2 * threads_per_node * objects_per_thread;
  ASSERT_TRUE(wait_until([&] { return done.load() == total; }, 20s));
  EXPECT_EQ(on_both.load(), total);
}

// test_repair_keeps_a_copy_it_has_nowhere_to_send: a node that stops
// hosting while it knows no other host has nowhere to send its copies, and
// keeps them: no owner is not every owner holding them.
TEST(StoreRepair, RepairKeepsACopyItHasNowhereToSend) {
  Cluster cluster;
  auto& alone = cluster.add("alone", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  std::vector<ObjectId> held;
  for (uint8_t salt = 1; held.size() < 4 && salt < 250; ++salt) {
    const auto id = alone.store().put(pattern(96 * 1024, salt));
    ASSERT_TRUE(alone.data().has(id));
    held.push_back(id);
  }
  alone.stop();
  alone.options().hosts_extents = false;
  alone.options().keeps_backend = true;
  alone.start();
  ASSERT_TRUE(cluster.converged());
  ASSERT_FALSE(alone.store().holds_extents());
  // No node hosts: the objects have no owner at all.
  for (const auto& id : held) ASSERT_FALSE(alone.store().should_own(id));

  auto provider = referring_to(held);
  alone.publish_inventory(provider);
  for (int pass = 0; pass < 3; ++pass) (void)alone.run_maintenance_once();
  for (const auto& id : held) EXPECT_TRUE(alone.data().has(id));
}

// test_node_that_stops_hosting_drains_through_repair: a node that stops
// hosting drains through ordinary repair: the survey finds the owner lacks
// its copies, the push sends each, and the local copy goes once the owner
// holds it. This is how objects move between nodes as placement changes.
TEST(StoreRepair, ANodeThatStopsHostingDrainsThroughRepair) {
  Cluster cluster;
  auto& s1 = cluster.add("s1", options(1));
  auto& s2 = cluster.add("s2", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  // Objects s2 owns while it still hosts.
  std::vector<ObjectId> owned;
  for (uint8_t salt = 1; owned.size() < 4 && salt < 250; ++salt) {
    const auto data = preferred_for(s2, s2.id(), 96 * 1024, salt);
    if (std::find(owned.begin(), owned.end(), object_id(data)) != owned.end()) continue;
    const auto id = s2.store().put(data);
    ASSERT_TRUE(s2.data().has(id));
    owned.push_back(id);
  }
  ASSERT_EQ(owned.size(), 4u);

  // s2 restarts hosting nothing; its backend stays, so the objects remain to
  // be drained.
  s2.stop();
  s2.options().hosts_extents = false;
  s2.options().keeps_backend = true;
  s2.start();
  EXPECT_FALSE(s2.store().holds_extents());
  ASSERT_TRUE(cluster.converged());
  for (const auto& id : owned) {
    EXPECT_FALSE(s2.store().should_own(id));
    EXPECT_TRUE(s1.store().should_own(id));
  }
  auto provider = referring_to(owned);
  ASSERT_TRUE(converge({&s2}, provider,
                       [&] { return std::all_of(owned.begin(), owned.end(), [&](const ObjectId& id) { return s1.data().has(id) && !s2.data().has(id); }); }));
  EXPECT_GE(s2.store().repair_diagnostics().local_drops, owned.size());
  ReadOptions read;
  read.frame = FrameType::speculative;
  for (const auto& id : owned) EXPECT_TRUE(s2.store().get(id, read).has_value());
}

// ---- The survey (Macha's test_availability.cpp) ----------------------------------

// test_what_only_a_host_out_of_reach_holds_is_unavailable: what only a node
// out of reach holds is unavailable; once it is back, it is not.
TEST(StoreRepair, WhatOnlyAHostOutOfReachHoldsIsUnavailable) {
  Cluster cluster;
  auto& first = cluster.add("first", options(2, 2));
  auto& second = cluster.add("second", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  std::vector<ObjectId> ids;
  for (uint8_t i = 0; i < 2; ++i) {
    const auto bytes = pattern(128 * 1024, static_cast<uint8_t>(1 + i));
    ids.push_back(object_id(bytes));
    ASSERT_TRUE(first.data().put(ids.back(), bytes));
    ASSERT_TRUE(second.data().put(ids.back(), bytes));
  }
  std::sort(ids.begin(), ids.end());
  auto provider = referring_to(ids);
  first.publish_inventory(provider);
  ASSERT_TRUE(wait_until([&] {
    (void)first.availability().refresh();
    const auto snapshot = first.availability().snapshot();
    return snapshot && snapshot->survey.peers_asked == 1 && snapshot->survey.unavailable.empty();
  }));

  // The second goes out of reach, and only then does the first lose its own
  // copies, so only the absent second holds them.
  const auto second_id = second.id();
  second.stop();
  ASSERT_TRUE(cluster.dead_to(first, second_id));
  for (const auto& id : ids) ASSERT_TRUE(first.data().remove(id));
  first.publish_inventory(provider);
  ASSERT_TRUE(wait_until([&] {
    (void)first.availability().refresh();
    const auto snapshot = first.availability().snapshot();
    return snapshot && snapshot->survey.unavailable == ids;
  }));
  for (const auto& id : ids) EXPECT_TRUE(first.availability().unavailable(id));

  second.start();
  ASSERT_TRUE(cluster.converged());
  EXPECT_TRUE(wait_until(
      [&] {
        (void)first.availability().refresh();
        const auto snapshot = first.availability().snapshot();
        return snapshot && snapshot->survey.unavailable.empty() && snapshot->survey.peers_asked == 1 && snapshot->survey.peers_failed == 0;
      },
      30s));
}

// test_two_nodes_survey_what_neither_holds: one object leaves both nodes; each
// surveys it unavailable, asking the other, and nothing unknown.
TEST(StoreRepair, TwoNodesSurveyWhatNeitherHolds) {
  Cluster cluster;
  auto& first = cluster.add("first", options(2, 2));
  auto& second = cluster.add("second", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  std::vector<ObjectId> ids;
  for (uint8_t i = 0; i < 7; ++i) {
    const auto bytes = pattern(64 * 1024, static_cast<uint8_t>(10 + i));
    ids.push_back(object_id(bytes));
    ASSERT_TRUE(first.data().put(ids.back(), bytes));
    ASSERT_TRUE(second.data().put(ids.back(), bytes));
  }
  auto provider = referring_to(ids);
  for (auto* node : {&first, &second}) {
    node->publish_inventory(provider);
    ASSERT_TRUE(wait_until([&] {
      (void)node->availability().refresh();
      const auto snapshot = node->availability().snapshot();
      return snapshot && snapshot->survey.unavailable.empty() && snapshot->survey.peers_asked == 1;
    }));
  }
  const auto lost = ids[3];
  ASSERT_TRUE(first.data().remove(lost));
  ASSERT_TRUE(second.data().remove(lost));
  for (auto* node : {&first, &second}) {
    ASSERT_TRUE(wait_until([&] {
      (void)node->availability().refresh();
      const auto snapshot = node->availability().snapshot();
      return snapshot && snapshot->survey.unknown.empty() && snapshot->survey.unavailable.size() == 1;
    }));
    const auto snapshot = node->availability().snapshot();
    EXPECT_EQ(snapshot->survey.unavailable, std::vector<ObjectId>{lost});
    EXPECT_TRUE(snapshot->survey.unknown.empty());
    EXPECT_EQ(snapshot->survey.peers_asked, 1u);
    EXPECT_EQ(snapshot->survey.peers_failed, 0u);
    EXPECT_TRUE(node->availability().unavailable(lost));
    EXPECT_EQ(*node->availability().missing_here(), std::vector<ObjectId>{lost});
  }
}

// test_a_disk_coming_back_is_surveyed_without_a_namespace_change: a disk
// that goes and comes back is seen without anything referenced changing:
// the survey follows the holdings' identity, not the writes.
TEST(StoreRepair, ADiskComingBackIsSurveyedWithNothingReferencedChanging) {
  Cluster cluster;
  auto& node = cluster.add("solo", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto held = local_objects(node, 2, 128 * 1024, 1);
  auto provider = referring_to(held);
  node.publish_inventory(provider);
  const auto missing = [&]() -> std::optional<size_t> {
    (void)node.availability().refresh();
    const auto here = node.availability().missing_here();
    if (!here) return std::nullopt;
    return here->size();
  };
  ASSERT_TRUE(wait_until([&] { return missing() == 0u; }));
  const auto inventory = node.ledger().inventory();

  auto away = node.backend();
  away += ".away";
  std::filesystem::rename(node.backend(), away);
  node.data().refresh();
  ASSERT_EQ(node.data().online_backends(), 0u);
  ASSERT_TRUE(wait_until([&] { return missing() == 2u; }));

  std::filesystem::rename(away, node.backend());
  node.data().refresh();
  ASSERT_EQ(node.data().online_backends(), 1u);
  EXPECT_TRUE(wait_until([&] { return missing() == 0u; }));
  EXPECT_EQ(node.ledger().inventory(), inventory);
}

// ---- New --------------------------------------------------------------------

// The survey through an owner's provider: what is referenced is the
// provider's answer; what this node lacks, what a peer lacks and what nobody
// holds follow from it and from the two held tries alone. Before any
// inventory nothing is surveyed.
TEST(StoreRepair, TheSurveyGoesThroughTheProvider) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  EXPECT_FALSE(a.availability().refresh());
  EXPECT_EQ(a.availability().snapshot(), nullptr);
  EXPECT_EQ(b.calls_of(message::trie_diff), 0u);

  const auto both = local_objects(a, 10, 4096, 1);
  for (const auto& id : both) ASSERT_TRUE(b.data().put(id, *a.data().get(id)));
  const auto only_b = local_objects(b, 10, 4096, 2);
  const auto only_a = local_objects(a, 5, 4096, 3);
  std::vector<ObjectId> nobody;
  for (uint8_t i = 0; i < 5; ++i) nobody.push_back(object_id(pattern(4096, static_cast<uint8_t>(40 + i))));
  std::sort(nobody.begin(), nobody.end());
  // Garbage a holds that nothing refers to.
  (void)local_objects(a, 3, 4096, 5);

  std::vector<ObjectId> referenced;
  for (const auto* part : std::initializer_list<const std::vector<ObjectId>*>{&both, &only_b, &only_a, &nobody})
    referenced.insert(referenced.end(), part->begin(), part->end());
  auto provider = referring_to(referenced);
  a.publish_inventory(provider);
  ASSERT_TRUE(a.availability().refresh());
  const auto snapshot = a.availability().snapshot();
  ASSERT_NE(snapshot, nullptr);
  std::vector<ObjectId> lacking_here = only_b;
  lacking_here.insert(lacking_here.end(), nobody.begin(), nobody.end());
  std::sort(lacking_here.begin(), lacking_here.end());
  EXPECT_EQ(*a.availability().missing_here(), lacking_here);
  const auto lacks = a.availability().peer_lacks();
  ASSERT_NE(lacks, nullptr);
  EXPECT_TRUE(lacks->complete);
  ASSERT_EQ(lacks->lacks.size(), 1u);
  EXPECT_EQ(*lacks->lacks.at(b.id()), only_a);
  EXPECT_EQ(snapshot->survey.unavailable, nobody);
  EXPECT_TRUE(snapshot->survey.unknown.empty());
  EXPECT_EQ(snapshot->survey.peers_asked, 1u);
  EXPECT_EQ(snapshot->generation, provider.current_generation);
  EXPECT_GT(b.calls_of(message::trie_diff), 0u);
  EXPECT_EQ(probes(b), 0u);
}

// A trie question names the snapshot its root question pinned; one no longer
// pinned is refused 410, and a records question past a leaf 413.
TEST(StoreRepair, ATrieQuestionNamesAPinnedSnapshot) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2));
  auto& b = cluster.add("b", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  (void)local_objects(b, 300, 1024, 6);
  const auto peer = record_of(a, b.id());
  const auto ask = [&](const ledger::TrieQuestion& question) {
    return a.context().routes().call_and_wait(peer, message::trie_diff, ledger::encode_trie_question(question), FrameType::speculative, 5s);
  };
  ledger::TrieQuestion children;
  children.kind = ledger::TrieQuestionKind::children;
  children.prefixes = {ledger::ObjectTrie::Prefix{}};
  children.at.bytes.fill(0x5a);
  const auto gone = ask(children);
  ASSERT_TRUE(gone.reply);
  ASSERT_TRUE(dannan::net::refusal_of(*gone.reply).has_value());
  EXPECT_EQ(dannan::net::refusal_of(*gone.reply)->status, 410);

  const auto root = ask(ledger::TrieQuestion{});
  ASSERT_TRUE(root.reply && root.reply->type == message::trie_diff_reply);
  const auto summary = ledger::decode_trie_root(root.reply->payload);
  EXPECT_EQ(summary.count, 300u);
  children.at = summary.hash;
  const auto below = ask(children);
  ASSERT_TRUE(below.reply && below.reply->type == message::trie_diff_reply);
  EXPECT_EQ(ledger::decode_trie_children(below.reply->payload).size(), 1u);

  ledger::TrieQuestion whole;
  whole.kind = ledger::TrieQuestionKind::records;
  whole.at = summary.hash;
  whole.prefixes = {ledger::ObjectTrie::Prefix{}};
  const auto too_large = ask(whole);
  ASSERT_TRUE(too_large.reply);
  ASSERT_TRUE(dannan::net::refusal_of(*too_large.reply).has_value());
  EXPECT_EQ(dannan::net::refusal_of(*too_large.reply)->status, 413);
}

// A node down while writes land, then back: the survey finds what it lacks
// by diffing held tries, and repair brings it every object, with no probe
// and no walk of any store.
TEST(StoreRepair, RepairConvergesAfterAPartitionHeals) {
  Cluster cluster;
  auto& a = cluster.add("a", options(3, 2));
  auto& b = cluster.add("b", options(3, 2));
  auto& c = cluster.add("c", options(3, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto c_id = c.id();
  c.stop();
  ASSERT_TRUE(cluster.dead_to(a, c_id));
  ASSERT_TRUE(cluster.dead_to(b, c_id));

  std::vector<ObjectId> written;
  WriteOptions write;
  write.level = Durability::replicated;
  for (uint8_t i = 0; i < 5; ++i) {
    const auto bytes = pattern(96 * 1024, static_cast<uint8_t>(20 + i));
    const auto evidence = a.store().write(object_id(bytes), bytes, write);
    ASSERT_TRUE(evidence.met);
    written.push_back(object_id(bytes));
  }
  c.start();
  ASSERT_TRUE(cluster.converged());
  for (const auto& id : written) ASSERT_FALSE(c.data().has(id));

  std::map<ClusterNode*, uint64_t> walks;
  for (auto* node : {&a, &b, &c}) walks[node] = node->data().full_list_scans();
  auto provider = referring_to(written);
  ASSERT_TRUE(
      converge({&a, &b, &c}, provider, [&] { return std::all_of(written.begin(), written.end(), [&](const ObjectId& id) { return c.data().has(id); }); }));
  for (auto* node : {&a, &b, &c}) {
    EXPECT_EQ(probes(*node), 0u) << node->name();
    EXPECT_EQ(node->data().full_list_scans(), walks[node]) << node->name();
  }
  EXPECT_GT(cluster.calls_of(message::trie_diff), 0u);
  for (const auto& id : written) EXPECT_EQ(c.data().get(id), a.data().get(id));
}

// A ticket names a member that was down "pending" and settles at its level;
// when repair lands the member's copy, the ticket records it and reports
// again, to a wait for it made before.
TEST(StoreRepair, ATicketReportsTheCopiesRepairMade) {
  Cluster cluster;
  auto& a = cluster.add("a", options(3, 2));
  cluster.add("b", options(3, 2));
  auto& c = cluster.add("c", options(3, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto c_id = c.id();
  c.stop();
  ASSERT_TRUE(cluster.dead_to(a, c_id));

  const auto bytes = pattern(96 * 1024, 61);
  const auto id = object_id(bytes);
  WriteOptions write;
  write.level = Durability::replicated;
  const auto evidence = a.store().write(id, bytes, write);
  ASSERT_TRUE(evidence.met);
  EXPECT_TRUE(evidence.settled);
  EXPECT_FALSE(evidence.complete);
  ASSERT_NE(evidence.on(c_id), nullptr);
  EXPECT_EQ(evidence.on(c_id)->state, ReplicaState::pending);
  // Settled, so wait() answers at once; complete is what is still owed.
  const auto settled = a.store().wait(evidence.ticket, 1s);
  ASSERT_TRUE(settled);
  EXPECT_TRUE(settled->settled);
  const auto owed = a.store().wait_complete(evidence.ticket, 200ms);
  ASSERT_TRUE(owed);
  EXPECT_FALSE(owed->complete);

  // A wait for the next report, made before repair runs.
  std::promise<WriteEvidence> reported;
  auto report = reported.get_future();
  a.store().wait_complete(evidence.ticket, 60s, cluster.io().context.get_executor(), [&](WriteEvidence later) { reported.set_value(std::move(later)); });

  c.start();
  ASSERT_TRUE(cluster.converged());
  auto provider = referring_to({id});
  ASSERT_TRUE(converge({&a}, provider, [&] { return c.data().has(id); }));
  ASSERT_EQ(report.wait_for(10s), std::future_status::ready);
  const auto again = report.get();
  EXPECT_TRUE(again.complete);
  EXPECT_TRUE(again.settled);
  ASSERT_NE(again.on(c_id), nullptr);
  EXPECT_EQ(again.on(c_id)->state, ReplicaState::acknowledged);
  EXPECT_TRUE(again.on(c_id)->repaired);
  EXPECT_FALSE(again.on(a.id())->repaired);
  EXPECT_EQ(a.store().status(evidence.ticket)->acknowledged(), 3u);
  EXPECT_GE(a.store().repair_diagnostics().copies_pushed, 1u);
}

// Section 7 item 4: a loader saturating DATA on the device the node's state
// and its DATA backend share does not delay a CONTROL write or a claim past
// a bound. The device reserve keeps two of four slots from DATA: the loader
// is held to the other two and waits for them, while every control
// operation is admitted at once, never waiting for a slot.
TEST(StoreRepair, ALoaderSaturatingDataLeavesControlWithinABound) {
  Cluster cluster;
  auto solo = options(1, 1, 512ULL * 1024 * 1024);
  solo.io = IoReserveOptions{4, 2};
  auto& node = cluster.add("solo", solo);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  auto& io = *node.io();
  const auto claimed_bytes = pattern(4096, 200);
  const auto claimed = object_id(claimed_bytes);
  ASSERT_TRUE(node.data().put(claimed, claimed_bytes));

  std::atomic<bool> loading{true};
  std::atomic<uint32_t> counter{0};
  std::vector<std::jthread> loaders;
  for (int t = 0; t < 8; ++t)
    loaders.emplace_back([&] {
      WriteOptions write;
      write.level = Durability::persisted;
      while (loading.load()) {
        auto bytes = pattern(128 * 1024, 7);
        const auto n = counter.fetch_add(1);
        std::memcpy(bytes.data(), &n, sizeof(n));
        (void)node.store().write(object_id(bytes), bytes, write);
      }
    });
  struct Stop {
    std::atomic<bool>& loading;
    ~Stop() { loading = false; }
  } stop{loading};
  ASSERT_TRUE(wait_until([&] { return io.stats().data_waits > 0 && io.stats().peak_data_in_flight == io.data_share(); }));

  std::chrono::nanoseconds worst{0};
  for (uint8_t i = 0; i < 20; ++i) {
    // A CONTROL write and its claim, as a commit makes them.
    const auto control_bytes = pattern(8 * 1024, static_cast<uint8_t>(100 + i));
    const auto control_id = object_id(control_bytes);
    auto started = Clock::now();
    ASSERT_TRUE(node.control().put(control_id, control_bytes));
    ASSERT_TRUE(node.store().retain_control({control_id}, contract::RetentionDot{node.id(), 1u + i}));
    worst = std::max<std::chrono::nanoseconds>(worst, Clock::now() - started);
    // A DATA claim.
    started = Clock::now();
    EXPECT_TRUE(node.store().retain_data({claimed}, contract::RetentionDot{node.id(), 100u + i}).empty());
    worst = std::max<std::chrono::nanoseconds>(worst, Clock::now() - started);
  }
  const auto stats = io.stats();
  loading = false;
  loaders.clear();
  EXPECT_LT(worst, 2s);
  EXPECT_EQ(stats.control_waits, 0u);
  EXPECT_GE(stats.control_admissions, 40u);
  EXPECT_EQ(stats.peak_data_in_flight, io.data_share());
  EXPECT_GT(stats.data_waits, 0u);
  EXPECT_TRUE(node.claims().retained(contract::RetentionClass::data, claimed));
}

// A stop during the survey's walk ends it, rather than waiting for every lookup
// (Macha's hung stop, 0.92.4). The pass throws, and the same pass without a stop
// runs to the end.
TEST(StoreRepair, AStopEndsTheSurveysWalk) {
  Cluster cluster;
  auto& node = cluster.add("node", options(1));
  (void)cluster.add("peer", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto held = test::numbered(1);
  ASSERT_TRUE(node.data().put(object_id(held), held));
  auto provider = referring_to({object_id(held)});
  node.publish_inventory(provider);
  ASSERT_TRUE(wait_until([&] { return node.data().held_view().identity.complete; }));
  std::stop_source stop;
  stop.request_stop();
  EXPECT_THROW((void)node.repair().pass(stop.get_token()), MaintenanceStopping);
  EXPECT_NO_THROW((void)node.repair().pass());
}

}  // namespace
}  // namespace dagda::store
