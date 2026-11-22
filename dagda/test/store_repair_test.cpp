//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The availability survey, repair by diff, the claim walk's restorer, the
// served requests' admission and the device reserve, on in-process clusters
// (requirements L7 to L11, S20, S30, section 7 item 4).
//
// Moved from Macha (develop 5a06d3c), converted to GoogleTest on real Dannan
// nodes, every assertion kept where it applies. Repair is diff only (the maintainer,
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
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "dagda/store/distributed_store.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/reachability.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/net/scheduler.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using dannan::identity::NodeId;
using dannan::net::CallOutcome;
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

// What a survey would say each peer holds of what this node lacks.
std::shared_ptr<const ledger::PeerHolds> holds_of(const std::map<NodeId, std::vector<ObjectId>>& holds, bool complete = true, uint64_t generation = 1) {
  ledger::PeerHolds out;
  out.generation = generation;
  out.complete = complete;
  for (const auto& [peer, ids] : holds) out.holds.emplace(peer, list_of(ids));
  return std::make_shared<const ledger::PeerHolds>(std::move(out));
}

// A window wider than anything a case moves.
constexpr RepairWindow wide{1024ULL * 1024 * 1024};

// One repair step, then what it started landed and reported: the figures of both, as
// one step that waited for its transfers gave them before the window (the repair
// redesign, stage 4).
RepairResult landed_step(DistributedStore& store, const RepairLists& lists, uint64_t budget, const std::function<bool()>& yield = {}) {
  auto out = store.repair_step(lists, budget, wide, yield);
  const auto landed = store.repair_drain(std::chrono::seconds(60));
  out.bytes_transferred += landed.bytes_transferred;
  out.offer_bytes += landed.offer_bytes;
  out.credit_spent += landed.credit_spent;
  out.push_examined += landed.push_examined;
  out.pull_examined += landed.pull_examined;
  out.pull_unsourceable += landed.pull_unsourceable;
  out.complete = out.complete || landed.complete;
  out.in_flight = landed.in_flight;
  return out;
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

// Holds every request of `type` for one of `ids` at `gate`.
test::Interpose hold_ids(MessageType type, std::set<ObjectId> ids, dannan::test::Gate& gate) {
  return [type, ids = std::move(ids), &gate](MessageType seen, const dannan::membership::NodeRecord&,
                                             const dannan::net::Message& request) -> std::optional<Handled> {
    if (seen == type && ids.contains(message::decode_id(request.payload))) gate.enter_and_wait();
    return std::nullopt;
  };
}

// A node whose repair has a window of one extent: the credit's burst is the larger of
// an extent and five seconds of the link, and the link is taken at 100 KiB/s until one
// is measured. Its credit and share run on `time`, and its credit earns the whole rate.
ClusterNodeOptions windowed(dannan::test::SteppedTime& time) {
  auto out = options(2);
  out.repair.extent_size = extent;
  out.repair.initial_bandwidth = 100 * 1024;
  out.repair.idle_bandwidth_fraction = 1.0;
  out.repair.time = &time;
  return out;
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
    const auto step = landed_step(writer.store(), {inventory_of(live), {}, lacks_of({{peer.id(), {}}}), {}}, 8ULL * 1024 * 1024);
    EXPECT_EQ(step.push_examined, 0u);
    EXPECT_TRUE(step.complete);
    EXPECT_EQ(probes(peer), 0u);
    EXPECT_EQ(peer.calls_of(message::put_object), 0u);
  }
  {
    dannan::test::Gate puts;
    Cluster cluster;
    auto& writer = cluster.add("writer", options(2));
    auto& peer = cluster.add("peer", options(2));
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    const auto live = local_objects(writer, 8, 24 * 1024, 40);
    dannan::test::GateOpener opener(puts);
    peer.interpose(hold(message::put_object, puts));
    const dannan::net::ServerLimits limits;
    const auto concurrent = std::min(live.size(), limits.data_workers - limits.foreground_data_reserve);
    bool together = false;
    std::jthread release([&] {
      together = puts.wait_for_entries(concurrent, 5s);
      puts.open();
    });
    const auto step = landed_step(writer.store(), {inventory_of(live), {}, lacks_of({{peer.id(), live}}), {}}, 64ULL * 1024 * 1024);
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
  const auto step = landed_step(writer.store(), {inventory_of(live), list_of({missing}), lacks_of({{peer.id(), {}}}), {}}, 8ULL * 1024 * 1024);
  EXPECT_LT(step.push_examined, 100u);
  EXPECT_GT(step.pull_examined, 0u);
  EXPECT_TRUE(writer.data().has(missing));
}

// test_repair_push_sends_nothing_to_a_peer_that_cannot_say: a peer the
// survey could not settle is neither counted as keeping a copy nor sent one,
// and the peers the survey heard from are pushed to, whether the survey is
// incomplete (a peer could not finish its diff: the maintainer, 2026-11-14, "push to the
// peers that answered", where before an incomplete survey pushed nothing) or a
// peer is missing from a complete one (one that joined since). Once the survey
// says it lacks the copy, the copy is pushed. Nothing is ever asked of it.
TEST(StoreRepair, APushSendsNothingToAPeerTheSurveyCouldNotSettle) {
  for (const bool complete : {false, true}) {
    Cluster cluster;
    auto& writer = cluster.add("writer", options(3));
    auto& peer = cluster.add("peer", options(3));
    auto& late = cluster.add("late", options(3));
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    const auto live = local_objects(writer, 4, 8 * 1024, 50);
    // As availability publishes them: only the peers whose diff ran to its end.
    const auto unsettled = lacks_of({{peer.id(), live}}, complete);
    const auto step = landed_step(writer.store(), {inventory_of(live), {}, unsettled, {}}, 8ULL * 1024 * 1024);
    EXPECT_EQ(step.push_examined, live.size());
    EXPECT_EQ(peer.calls_of(message::put_object), live.size());
    EXPECT_EQ(late.calls_of(message::put_object), 0u);
    EXPECT_EQ(probes(peer) + probes(late), 0u);
    for (const auto& id : live) EXPECT_TRUE(writer.data().has(id));

    const auto said = lacks_of({{peer.id(), {}}, {late.id(), live}});
    EXPECT_TRUE(wait_until(
        [&] {
          (void)landed_step(writer.store(), {inventory_of(live), {}, said, {}}, 8ULL * 1024 * 1024);
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
  ASSERT_TRUE(wait_until([&] { return used_of(record_of(writer, full.id())) + extent > 2 * extent; }));

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
  for (int pass = 0; pass < 4; ++pass) (void)landed_step(writer.store(), {inventory_of(live), {}, lacks, {}}, 8ULL * 1024 * 1024);
  EXPECT_EQ(full.calls_of(message::put_object), 0u);
  EXPECT_EQ(probes(full), 0u);
  EXPECT_GT(fallback.calls_of(message::put_object), 0u);
  EXPECT_GT(owner.calls_of(message::put_object), 0u);
  EXPECT_TRUE(writer.data().has(id));

  for (const auto& filled : filler) ASSERT_TRUE(full.data().remove(filled));
  full.server().advertise();
  ASSERT_TRUE(wait_until([&] { return used_of(record_of(writer, full.id())) + extent <= 2 * extent; }));
  const auto again = lacks_of({{full.id(), live}, {fallback.id(), {}}, {owner.id(), {}}});
  for (int pass = 0; pass < 4 && full.calls_of(message::put_object) == 0; ++pass)
    (void)landed_step(writer.store(), {inventory_of(live), {}, again, {}}, 8ULL * 1024 * 1024);
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

  const auto settled = landed_step(writer.store(), {inventory_of(live), {}, lacks_of({{peer.id(), {}}}), {}}, budget);
  // Nothing listed: nothing to look at.
  EXPECT_EQ(settled.push_examined, 0u);
  EXPECT_TRUE(settled.complete);
  EXPECT_EQ(probes(peer), 0u);
  EXPECT_EQ(peer.calls_of(message::put_object), 0u);

  const auto lacks_one = lacks_of({{peer.id(), {live[1]}}});
  for (int step = 0; step < 8 && peer.calls_of(message::put_object) == 0; ++step)
    (void)landed_step(writer.store(), {inventory_of(live), {}, lacks_one, {}}, budget);
  EXPECT_EQ(peer.calls_of(message::put_object), 1u);
  EXPECT_EQ(probes(peer), 0u);

  (void)landed_step(writer.store(), {inventory_of(live), {}, lacks_of({}), {}}, budget);
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

  const auto idle = landed_step(writer.store(), {inventory_of(live), {}, lacks_of({{peer.id(), {}}}), {}}, budget);
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
    examined += landed_step(writer.store(), {inventory_of(referenced), {}, listed, {}}, budget).push_examined;
  EXPECT_EQ(peer.calls_of(message::put_object), 1u);
  EXPECT_TRUE(peer.data().has(lacking));
  EXPECT_EQ(probes(peer), 0u);
  EXPECT_LE(examined, 2u);
  EXPECT_EQ(writer.store().repair_diagnostics().local_unreadable, 0u);
}

// Macha's distributed_store.cpp:2382-2397 at 98b816d: an object the store lists that
// will not read back is counted, and said at WARN with the total, once a minute.
TEST(StoreRepair, APushOfALocalObjectThatWillNotReadIsWarnedWithItsTotal) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(writer, 1, 64 * 1024, 91);
  const auto id = live.front();
  // Its sealed bytes damaged on the disk, where the store still lists it.
  std::filesystem::path file;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(writer.backend()))
    if (entry.is_regular_file() && entry.path().filename().string().find(to_string(id)) != std::string::npos) file = entry.path();
  ASSERT_FALSE(file.empty());
  {
    std::fstream damage(file, std::ios::in | std::ios::out | std::ios::binary);
    damage.seekp(static_cast<std::streamoff>(std::filesystem::file_size(file) / 2));
    damage.write("damage", 6);
  }
  ASSERT_TRUE(writer.data().has(id));
  for (int step = 0; step < 8 && writer.store().repair_diagnostics().local_unreadable == 0; ++step)
    (void)landed_step(writer.store(), {inventory_of(live), {}, lacks_of({{peer.id(), {id}}}), {}}, 64ULL * 1024 * 1024);
  EXPECT_EQ(writer.store().repair_diagnostics().local_unreadable, 1u);
  EXPECT_TRUE(test::saw(
      writer.log(), dannan::status::LogLevel::warn,
      "repair found a local object it cannot read back object=" + to_string(id) + " local_unreadable_total=1; the store listed it but the read failed"));
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
  auto push = landed_step(writer.store(), {inventory_of({held}), {}, push_lacks, {}}, held_bytes.size() - 1);
  EXPECT_EQ(peer.calls_of(message::put_object), 0u);
  EXPECT_TRUE(push.credit_limited);
  EXPECT_FALSE(push.complete);
  push = landed_step(writer.store(), {inventory_of({held}), {}, push_lacks, {}}, held_bytes.size());
  EXPECT_EQ(peer.calls_of(message::put_object), 1u);
  EXPECT_FALSE(push.credit_limited);

  const RepairLists pull_lists{inventory_of({held, wanted}), list_of({held, wanted}), lacks_of({{peer.id(), {}}}), {}};
  RepairResult pull;
  for (int step = 0; step < 8 && !pull.credit_limited; ++step) pull = landed_step(writer.store(), pull_lists, extent - 1);
  EXPECT_TRUE(pull.credit_limited);
  EXPECT_EQ(peer.calls_of(message::get_object), 0u);
  EXPECT_FALSE(writer.data().has(wanted));
  for (int step = 0; step < 8 && !writer.data().has(wanted); ++step) (void)landed_step(writer.store(), pull_lists, extent);
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
  // A step's credit covers sixteen of them (Macha's step made sixteen calls; the
  // repair redesign's stage 4 took the call count away, and the credit bounds a step).
  constexpr uint64_t budget = 16 * 4 * 1024;
  const auto first = landed_step(writer.store(), {inventory_of(live), {}, lacks_of({{peer.id(), live}}), {}}, budget);
  EXPECT_EQ(first.push_examined, 16u);
  EXPECT_EQ(peer.calls_of(message::put_object), 16u);

  // A new survey, taken before those copies landed, saying the same: the
  // pass goes on from where it was, and sends none of the first again.
  const auto second_lists = lacks_of({{peer.id(), live}});
  const auto second = landed_step(writer.store(), {inventory_of(live), {}, second_lists, {}}, budget);
  EXPECT_EQ(second.push_examined, 16u);
  EXPECT_EQ(peer.calls_of(message::put_object), 32u);
  for (size_t i = 0; i < live.size(); ++i) EXPECT_EQ(peer.data().has(live[i]), i < 32) << i;
  std::optional<bool> spanning;
  for (int step = 0; step < 20 && !spanning; ++step) {
    const auto passes = writer.store().repair_diagnostics().passes_completed;
    const auto result = landed_step(writer.store(), {inventory_of(live), {}, second_lists, {}}, budget);
    if (writer.store().repair_diagnostics().passes_completed > passes) spanning = result.complete;
  }
  ASSERT_TRUE(spanning.has_value());
  EXPECT_FALSE(*spanning);
  EXPECT_EQ(peer.calls_of(message::put_object), live.size());
  // A survey taken now: the peer lacks nothing, and the pass is settled.
  const auto now_lists = lacks_of({{peer.id(), {}}});
  for (int pass = 0; pass < 2; ++pass) {
    const auto settled = landed_step(writer.store(), {inventory_of(live), {}, now_lists, {}}, budget);
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
  (void)landed_step(writer.store(), {inventory_of(small), {}, lacks_of({{peer.id(), small}}), {}}, 64ULL * 1024 * 1024);
  ASSERT_EQ(peer.calls_of(message::put_object), 1u);
  EXPECT_EQ(writer.store().estimated_network_bps(), 0.0);

  const auto large = local_objects(writer, 1, extent / 2, 72);
  (void)landed_step(writer.store(), {inventory_of(large), {}, lacks_of({{peer.id(), large}}), {}}, 64ULL * 1024 * 1024);
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
    examined += landed_step(node.store(), {inventory_of(live), list_of(live), lacks_of({}), {}}, 8ULL * 1024 * 1024).pull_examined;
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
  const auto result = landed_step(writer.store(), {inventory_of({id}), list_of({id}), lacks_of({{peer.id(), {}}}), {}}, 8ULL * 1024 * 1024, [&] {
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

  const auto yielded = landed_step(first.store(), lists, 8ULL * 1024 * 1024, [] { return true; });
  EXPECT_TRUE(yielded.yielded);
  EXPECT_FALSE(yielded.complete);
  EXPECT_EQ(yielded.bytes_transferred, 0u);
  EXPECT_FALSE(second.data().has(id));

  // Macha's step here had one call for a probe and none for the upload;
  // with no probe, the budget that cannot cover the upload is the bytes'.
  const auto bounded = landed_step(first.store(), lists, bytes.size() - 1);
  EXPECT_FALSE(bounded.complete);
  EXPECT_EQ(bounded.bytes_transferred, 0u);
  EXPECT_EQ(bounded.remote_operations, 0u);
  EXPECT_FALSE(second.data().has(id));

  const auto completed = landed_step(first.store(), lists, 8ULL * 1024 * 1024);
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
//
// The repairing node's pass, credit, share and activity clocks are stepped, so
// a turn that runs long on a loaded machine earns no longer a rest and the
// bound is on repair's own time. A peer's telemetry is Dannan's, on real time:
// it is waited for, and that wait's bound only guards against a hang.
TEST(StoreRepair, RepairIsPacedNotStoppedWhileHigherClassesStayBusy) {
  constexpr auto step = 100ms;
  // Ten seconds of repair's time.
  constexpr int bound = 100;
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto busy = options(1);
  busy.repair.foreground_quiet = 500ms;
  auto& s1 = cluster.add("s1", busy);
  auto stepped = busy;
  stepped.maintenance.time = &time;
  auto& s2 = cluster.add("s2", stepped);
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
  // One step of repair's time, a whole node pass, then a repair pass at the same
  // instant: if the first took a turn, the share refuses the second, whatever
  // the turn took on the real clock.
  const auto pass = [&](std::chrono::milliseconds by, const std::function<void()>& busy_now) {
    time.advance(by);
    if (busy_now) busy_now();
    (void)s2.run_maintenance_once();
    (void)s2.repair().pass();
  };
  const auto within_bound = [&](const std::function<void()>& busy_now, const std::function<bool()>& done) {
    for (int i = 0; i < bound; ++i) {
      pass(step, busy_now);
      if (done()) return true;
    }
    return false;
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
    const bool seen = wait_until(
        [&] {
          time.advance(step);
          return s2.run_maintenance_once().repair.paced_by & paced_by_peer_foreground;
        },
        60s);
    const auto [id, bytes] = lose_a_copy(1);
    const bool restored = within_bound({}, [&] { return s2.data().valid(id); });
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
    // Never quiet: noted at every step, and a step is well inside foreground_quiet.
    const auto working = [&] { s2.dagda_node().activity().note(frame, 64 * 1024); };
    // The busy class seen holding repair to its share, then a copy lost. On a
    // stepped clock a turn earns the least rest, 19 ms: a millisecond after the
    // turn that restored the last copy, the share refuses the pass.
    const auto share_before = s2.repair().gates().share;
    pass(1ms, working);
    ASSERT_GT(s2.repair().gates().share, share_before);
    const auto [id, bytes] = lose_a_copy(frame == FrameType::foreground ? 2 : 3);
    const bool restored = within_bound(working, [&] { return s2.data().valid(id); });
    const auto gates = s2.repair().gates();
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
  // The case is about credit; the device's pressure is another gate (its own cases are
  // StoreIoPressure's), which a device slowed by a sanitizer trips (seen under TSan,
  // 2026-11-19: 316 to 482% of the expected service time).
  server_options.device_pressure.slowdown_percent = 0;
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
  const auto refused = dannan::net::refusal_of(*validated.reply);
  const auto at_refusal = credit.stats();
  EXPECT_EQ(validated.reply->type, message::bool_reply)
      << "refused " << (refused ? std::to_string(refused->status) + " " + refused->code + ": " + refused->message : "") << "; pressured "
      << at_refusal.device_pressured << ", pressure refusals " << at_refusal.pressure_refusals << ", slowdown " << at_refusal.device_slowdown_percent
      << "%, background " << at_refusal.background_active << " of " << at_refusal.background_limit << ", used " << at_refusal.used_bytes;

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
    return landed_step(node.store(), {inventory_of(live), list_of(std::move(pull)), lacks_of({}), unavailable}, 4ULL * 1024 * 1024);
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
  // Macha's warning, once a minute (distributed_store.cpp:2377 at 98b816d).
  EXPECT_TRUE(test::saw(node.log(), dannan::status::LogLevel::warn,
                        "repair cannot source an object this node should own object=" + to_string(missing) +
                            " unsourceable_total=1; the live namespace references it and no peer would supply it"));
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

// A node as Macha runs one for these cases: no device reserve, and a wide
// no-progress window beside limits so small that any absolute limit would end
// the waits.
ClusterNodeOptions waits_on_credit() {
  auto out = options(2, 1, 8ULL * 1024 * 1024);
  out.device_reserve = false;
  out.no_progress = 1h;
  out.admission_wait = 1ms;
  return out;
}

// src/cluster/storage_server.cpp at Macha 9a08846 (get_object :195, put_object
// :237): a served read or write with credit taken waits on, however long the
// arbiter has been full; the answer comes when credit does.
TEST(StoreRepair, AServedRequestWaitsForCreditPastAnyLimit) {
  Cluster cluster;
  auto& a = cluster.add("a", waits_on_credit());
  auto& b = cluster.add("b", waits_on_credit());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto peer = record_of(a, b.id());
  const auto bytes = pattern(64 * 1024, 41);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.data().put(id, bytes));
  const auto new_bytes = pattern(64 * 1024, 42);
  const auto new_id = object_id(new_bytes);

  auto& credit = *b.credit();
  auto full = credit.try_acquire(DataWorkContext(FrameType::foreground, 0), credit.stats().capacity_bytes);
  ASSERT_TRUE(full.has_value());
  auto read = std::async(std::launch::async,
                         [&] { return a.context().routes().call_and_wait(peer, message::get_object, message::encode_id(id), FrameType::foreground, 60s); });
  auto write = std::async(std::launch::async, [&] {
    return a.context().routes().call_and_wait(peer, message::put_object, message::encode_object(new_id, new_bytes), FrameType::speculative, 60s);
  });
  ASSERT_TRUE(wait_until([&] {
    const auto stats = credit.stats();
    return stats.foreground_waits == 1 && stats.speculative_waits == 1;
  }));
  // Well past the 1 ms any absolute limit would have allowed.
  EXPECT_EQ(read.wait_for(50ms), std::future_status::timeout);
  EXPECT_EQ(write.wait_for(0ms), std::future_status::timeout);
  full.reset();
  const auto answered_read = read.get();
  ASSERT_TRUE(answered_read.reply);
  EXPECT_EQ(answered_read.reply->type, message::object_reply);
  const auto answered_write = write.get();
  ASSERT_TRUE(answered_write.reply);
  EXPECT_FALSE(dannan::net::refusal_of(*answered_write.reply).has_value());
  EXPECT_TRUE(b.data().has(new_id));
  EXPECT_EQ(credit.stats().no_progress_failures, 0u);
}

// The same handlers (:197-198, :239-240): after a stall a waited request answers
// 503 stopping, and the probe that tries once (have_object :88-89) still answers
// 503 storage_busy. Both are logged and counted as Macha's are.
TEST(StoreRepair, AServedRequestAfterAStallAnswersStopping) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto stalls = waits_on_credit();
  stalls.no_progress = 120s;
  stalls.time = &time;
  auto& a = cluster.add("a", waits_on_credit());
  auto& b = cluster.add("b", stalls);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto peer = record_of(a, b.id());
  const auto bytes = pattern(64 * 1024, 43);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.data().put(id, bytes));

  auto& credit = *b.credit();
  const auto full = credit.try_acquire(DataWorkContext(FrameType::foreground, 0), credit.stats().capacity_bytes);
  ASSERT_TRUE(full.has_value());
  const auto answer = [](const CallOutcome& outcome) {
    EXPECT_TRUE(outcome.reply);
    return outcome.reply ? dannan::net::refusal_of(*outcome.reply) : std::nullopt;
  };

  const auto busy = answer(a.context().routes().call_and_wait(peer, message::have_object, message::encode_id(id), FrameType::speculative, 30s));
  ASSERT_TRUE(busy.has_value());
  EXPECT_EQ(busy->status, 503);
  EXPECT_EQ(busy->code, "storage_busy");
  EXPECT_EQ(busy->message, "DATA resource admission busy or stopping");

  auto read = std::async(std::launch::async,
                         [&] { return a.context().routes().call_and_wait(peer, message::get_object, message::encode_id(id), FrameType::foreground, 30s); });
  ASSERT_TRUE(wait_until([&] { return credit.stats().foreground_waits == 1; }));
  time.advance(121s);
  credit.time_advanced();
  const auto stopped = answer(read.get());
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->status, 503);
  EXPECT_EQ(stopped->code, "stopping");
  EXPECT_EQ(stopped->message, "DATA resource admission stopping");
  EXPECT_EQ(credit.stats().no_progress_failures, 1u);
  EXPECT_EQ(credit.stats().cancelled_waits, 0u);
}

// The store's own paths with no caller deadline (src/cluster/distributed_store.cpp
// at Macha 9a08846: launch :307, put_here :1020, get_shared :1741 with no deadline,
// ensure_local :2270): each waits for credit under the
// no-progress window, past admission_wait.
TEST(StoreRepair, AStoreCallWithNoDeadlineWaitsForCreditPastAnyLimit) {
  Cluster cluster;
  auto limited = waits_on_credit();
  auto& node = cluster.add("node", limited);
  node.start();
  ASSERT_TRUE(cluster.converged());
  const auto held_bytes = pattern(64 * 1024, 44);
  const auto held = object_id(held_bytes);
  ASSERT_TRUE(node.data().put(held, held_bytes));

  auto& credit = *node.credit();
  uint8_t salt = 45;
  const auto waits_then_succeeds = [&](const char* what, const std::function<bool()>& call) {
    SCOPED_TRACE(what);
    const auto before = credit.stats();
    const auto waited = before.foreground_waits + before.loader_waits + before.speculative_waits;
    auto full = credit.try_acquire(DataWorkContext(FrameType::foreground, 0), before.capacity_bytes);
    ASSERT_TRUE(full.has_value());
    auto result = std::async(std::launch::async, call);
    ASSERT_TRUE(wait_until([&] {
      const auto stats = credit.stats();
      return stats.foreground_waits + stats.loader_waits + stats.speculative_waits > waited;
    }));
    EXPECT_EQ(result.wait_for(50ms), std::future_status::timeout);
    full.reset();
    EXPECT_TRUE(result.get());
  };
  const auto fresh = [&] { return pattern(64 * 1024, salt++); };

  waits_then_succeeds("get", [&] {
    ReadOptions read;
    read.frame = FrameType::foreground;
    return node.store().get(held, read).has_value();
  });
  waits_then_succeeds("put", [&, bytes = fresh()] { return node.store().put(object_id(bytes), bytes); });
  waits_then_succeeds("put_here", [&, bytes = fresh()] { return node.store().put_here(object_id(bytes), bytes); });
  waits_then_succeeds("ensure_local", [&] { return node.store().ensure_local(held, true); });
  EXPECT_EQ(credit.stats().no_progress_failures, 0u);
  EXPECT_EQ(credit.stats().cancelled_waits, 0u);
}

// A caller with its own deadline keeps it: the read gives up, as Macha's
// context.expired() does (:1741), with the window far off.
TEST(StoreRepair, AStoreReadWithItsOwnDeadlineGivesUpAtIt) {
  Cluster cluster;
  auto& node = cluster.add("node", waits_on_credit());
  node.start();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 49);
  const auto id = object_id(bytes);
  ASSERT_TRUE(node.data().put(id, bytes));
  auto& credit = *node.credit();
  const auto full = credit.try_acquire(DataWorkContext(FrameType::foreground, 0), credit.stats().capacity_bytes);
  ASSERT_TRUE(full.has_value());
  ReadOptions read;
  read.frame = FrameType::foreground;
  read.deadline = 1ms;
  EXPECT_FALSE(node.store().get(id, read).has_value());
  EXPECT_EQ(credit.stats().cancelled_waits, 1u);
  EXPECT_EQ(credit.stats().no_progress_failures, 0u);
}

// src/cluster/distributed_store.cpp at Macha 9a08846, replicate (:305-312, :454-461):
// no clock runs on a write while its local copy waits. A whole-window stall here
// loses the local copy and the write is placed on a peer, however long that took.
TEST(StoreRepair, AWriteAfterAWholeWindowStallHereLandsOnAPeer) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto stalls = waits_on_credit();
  stalls.no_progress = 120s;
  stalls.time = &time;
  auto& a = cluster.add("a", stalls);
  auto& b = cluster.add("b", waits_on_credit());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 50);
  const auto id = object_id(bytes);
  auto& credit = *a.credit();
  const auto full = credit.try_acquire(DataWorkContext(FrameType::foreground, 0), credit.stats().capacity_bytes);
  ASSERT_TRUE(full.has_value());
  auto write = std::async(std::launch::async, [&] { return a.store().put(id, bytes); });
  ASSERT_TRUE(wait_until([&] { return credit.stats().loader_waits + credit.stats().foreground_waits == 1; }));
  time.advance(121s);
  credit.time_advanced();
  ASSERT_EQ(write.wait_for(30s), std::future_status::ready);
  EXPECT_TRUE(write.get());
  EXPECT_TRUE(b.data().has(id));
  EXPECT_FALSE(a.data().has(id));
  EXPECT_EQ(credit.stats().no_progress_failures, 1u);
}

// The same loop (:226-229, :346-351, :450-451): a write seeking two copies is not
// out of time because its local admission was slow; the second copy still counts.
TEST(StoreRepair, AWriteSeeksItsSecondCopyAfterASlowLocalAdmission) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto slow = options(2, 2, 8ULL * 1024 * 1024);
  slow.device_reserve = false;
  slow.no_progress = 120s;
  slow.time = &time;
  auto& a = cluster.add("a", slow);
  auto peer = options(2, 2, 8ULL * 1024 * 1024);
  peer.device_reserve = false;
  auto& b = cluster.add("b", peer);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 51);
  const auto id = object_id(bytes);
  auto& credit = *a.credit();
  auto full = credit.try_acquire(DataWorkContext(FrameType::foreground, 0), credit.stats().capacity_bytes);
  ASSERT_TRUE(full.has_value());
  auto write = std::async(std::launch::async, [&] { return a.store().put(id, bytes); });
  ASSERT_TRUE(wait_until([&] { return credit.stats().loader_waits + credit.stats().foreground_waits == 1; }));
  // Past the 60 s a write had once, inside the window.
  time.advance(61s);
  credit.time_advanced();
  EXPECT_EQ(write.wait_for(50ms), std::future_status::timeout);
  full.reset();
  ASSERT_EQ(write.wait_for(30s), std::future_status::ready);
  EXPECT_TRUE(write.get());
  EXPECT_TRUE(a.data().has(id));
  EXPECT_TRUE(b.data().has(id));
  EXPECT_EQ(credit.stats().no_progress_failures, 0u);
}

// A write with a deadline of its own keeps it across the local wait.
TEST(StoreRepair, AWriteWithItsOwnDeadlineGivesUpAtIt) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto stalls = waits_on_credit();
  stalls.no_progress = 120s;
  stalls.time = &time;
  auto& a = cluster.add("a", stalls);
  auto& b = cluster.add("b", waits_on_credit());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 52);
  const auto id = object_id(bytes);
  auto& credit = *a.credit();
  const auto full = credit.try_acquire(DataWorkContext(FrameType::foreground, 0), credit.stats().capacity_bytes);
  ASSERT_TRUE(full.has_value());
  WriteOptions own;
  own.deadline = 10s;
  auto write = std::async(std::launch::async, [&] { return a.store().put(id, bytes, own); });
  ASSERT_TRUE(wait_until([&] { return credit.stats().loader_waits + credit.stats().foreground_waits == 1; }));
  time.advance(11s);
  credit.time_advanced();
  ASSERT_EQ(write.wait_for(30s), std::future_status::ready);
  EXPECT_FALSE(write.get());
  EXPECT_FALSE(b.data().has(id));
  EXPECT_EQ(credit.stats().cancelled_waits, 1u);
  EXPECT_EQ(credit.stats().no_progress_failures, 0u);
}

// src/cluster/distributed_store.cpp at Macha 9a08846, get_shared (:1741, :1770, :1801-1804)
// and get_remote (:1445): one absolute deadline across the local stage, the cache
// stage and the remote stage. Four seconds in each of the first two, and the
// remote stage has what is left of ten, not ten.
TEST(StoreRepair, AReadsDeadlineIsOneTimeAcrossItsStages) {
  dannan::test::SteppedTime time;
  // Stepping the reader's clock must not make its peer look dead.
  Cluster cluster(std::chrono::hours(24));
  auto reader = waits_on_credit();
  reader.time = &time;
  reader.store.read_trace = [&](std::string_view) { time.advance(4s); };
  auto& a = cluster.add("a", reader);
  auto& b = cluster.add("b", waits_on_credit());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 53);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.data().put(id, bytes));
  // The peer holds its answer back: the remote stage waits on it.
  auto& held_up = *b.credit();
  const auto full = held_up.try_acquire(DataWorkContext(FrameType::foreground, 0), held_up.stats().capacity_bytes);
  ASSERT_TRUE(full.has_value());
  ReadOptions read;
  read.frame = FrameType::foreground;
  read.deadline = 10s;
  auto result = std::async(std::launch::async, [&] { return a.store().get(id, read).has_value(); });
  ASSERT_TRUE(wait_until([&] { return held_up.stats().foreground_waits == 1; }));
  time.advance(3s);
  const bool ended_at_its_deadline = result.wait_for(5s) == std::future_status::ready;
  // However that went, nothing is left waiting for the test's end.
  time.advance(1h);
  EXPECT_TRUE(ended_at_its_deadline);
  EXPECT_FALSE(result.get());
}

// The same code: a read past its deadline as a stage ends goes no further, at
// either stage (:1741-1745, :1770-1774, :1801-1802), and no peer is asked.
TEST(StoreRepair, AReadPastItsDeadlineGoesNoFurther) {
  for (const std::string_view stage : {"local", "cache"}) {
    SCOPED_TRACE(stage);
    dannan::test::SteppedTime time;
    std::vector<std::string> stages;
    // Stepping the reader's clock must not make its peer look dead.
    Cluster cluster(std::chrono::hours(24));
    auto reader = waits_on_credit();
    reader.time = &time;
    reader.store.read_trace = [&](std::string_view ended) {
      stages.emplace_back(ended);
      if (ended == stage) time.advance(11s);
    };
    auto& a = cluster.add("a", reader);
    auto& b = cluster.add("b", waits_on_credit());
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    const auto bytes = pattern(64 * 1024, 54);
    const auto id = object_id(bytes);
    ASSERT_TRUE(b.data().put(id, bytes));
    const auto asked = b.calls_of(message::get_object);
    ReadOptions read;
    read.frame = FrameType::foreground;
    read.deadline = 10s;
    EXPECT_FALSE(a.store().get(id, read).has_value());
    EXPECT_EQ(b.calls_of(message::get_object), asked);
    EXPECT_EQ(stages.back(), stage);
  }
}

// A read with no deadline names none for its admissions, and its remote stage asks a
// peer afterwards, however long the local wait.
TEST(StoreRepair, AReadWithNoDeadlineHasItsWholeRemoteLimitAfterALocalWait) {
  dannan::test::SteppedTime time;
  // Stepping the reader's clock must not make its peer look dead.
  Cluster cluster(std::chrono::hours(24));
  auto reader = waits_on_credit();
  reader.time = &time;
  reader.no_progress = 120s;
  auto& a = cluster.add("a", reader);
  auto& b = cluster.add("b", waits_on_credit());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 55);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.data().put(id, bytes));
  auto& credit = *a.credit();
  auto full = credit.try_acquire(DataWorkContext(FrameType::foreground, 0), credit.stats().capacity_bytes);
  ASSERT_TRUE(full.has_value());
  ReadOptions read;
  read.frame = FrameType::foreground;
  auto result = std::async(std::launch::async, [&] { return a.store().get(id, read); });
  ASSERT_TRUE(wait_until([&] { return credit.stats().foreground_waits == 1; }));
  time.advance(61s);
  credit.time_advanced();
  full.reset();
  ASSERT_EQ(result.wait_for(30s), std::future_status::ready);
  EXPECT_EQ(result.get(), bytes);
}

// src/cluster/distributed_store.cpp at Macha 98b816d, replicate (:305-312, :346-351,
// :454-461): a write has no absolute limit past its local wait. A peer that takes
// longer than any figure Dagda had (write_deadline 60 s, call_horizon 120 s) inside
// the no-progress window is waited for, and its copy counts.
TEST(StoreRepair, AWriteWithNoDeadlineWaitsForAPeerInsideTheWindowPastAnyLimit) {
  dannan::test::SteppedTime time;
  // Stepping the writer's clock must not make its peer look dead.
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto writer = options(2, 2, 8ULL * 1024 * 1024);
  writer.time = &time;
  writer.store.call_no_progress = 300s;
  auto& a = cluster.add("a", writer);
  auto& b = cluster.add("b", options(2, 2, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(a, b.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::put_object, gate));
  const auto bytes = pattern(64 * 1024, 57);
  const auto id = object_id(bytes);
  WriteOptions durable;
  durable.ack_level = Durability::durable;
  auto write = std::async(std::launch::async, [&] { return a.store().write(id, bytes, durable); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  time.advance(200s);
  // The watch looks at most every 250 ms: a second is several looks.
  EXPECT_EQ(write.wait_for(1s), std::future_status::timeout);
  gate.open();
  ASSERT_EQ(write.wait_for(30s), std::future_status::ready);
  const auto evidence = write.get();
  ASSERT_NE(evidence.on(b.id()), nullptr);
  EXPECT_EQ(evidence.on(b.id())->state, ReplicaState::acknowledged);
  EXPECT_TRUE(b.data().has(id));
}

// The bound: a peer silent for the whole window fails, in Dannan's words, and the
// write says what is owed.
TEST(StoreRepair, AWriteWithNoDeadlineFailsAPeerSilentForTheWindow) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto writer = options(2, 2, 8ULL * 1024 * 1024);
  writer.time = &time;
  writer.store.call_no_progress = 300s;
  auto& a = cluster.add("a", writer);
  auto& b = cluster.add("b", options(2, 2, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(a, b.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::put_object, gate));
  const auto bytes = pattern(64 * 1024, 58);
  const auto id = object_id(bytes);
  WriteOptions durable;
  durable.ack_level = Durability::durable;
  auto write = std::async(std::launch::async, [&] { return a.store().write(id, bytes, durable); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  // The local copy landing re-arms the window, and the write may see it after
  // the first step: a window after that, it ends.
  for (int step = 0; step < 3 && write.wait_for(5s) != std::future_status::ready; ++step) time.advance(301s);
  ASSERT_EQ(write.wait_for(30s), std::future_status::ready);
  const auto evidence = write.get();
  ASSERT_NE(evidence.on(b.id()), nullptr);
  EXPECT_EQ(evidence.on(b.id())->state, ReplicaState::pending);
  EXPECT_NE(evidence.on(b.id())->why.find("made no progress"), std::string::npos) << evidence.on(b.id())->why;
  EXPECT_TRUE(a.data().has(id));
}

// Macha's publication budget (98b816d, distributed_store.cpp:352-386): a window
// over the whole write. A member silent for it is cancelled and the write ends
// with the copy that landed, long before the call's own 300 s bound.
TEST(StoreRepair, AWriteWithABudgetEndsWhenNothingMovesForIt) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto writer = options(2, 2, 8ULL * 1024 * 1024);
  writer.time = &time;
  writer.store.call_no_progress = 300s;
  auto& a = cluster.add("a", writer);
  auto& b = cluster.add("b", options(2, 2, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(a, b.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::put_object, gate));
  const auto bytes = pattern(64 * 1024, 61);
  const auto id = object_id(bytes);
  WriteOptions publication;
  publication.ack_level = Durability::durable;
  publication.no_progress_budget = 30s;
  auto write = std::async(std::launch::async, [&] { return a.store().write(id, bytes, publication); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  // The local copy landing re-arms the window, and the write may see it after
  // the first step: a budget after that, it ends. Three steps are 93 s, far
  // short of the call's 300 s, so only the budget can end it.
  for (int step = 0; step < 3 && write.wait_for(5s) != std::future_status::ready; ++step) time.advance(31s);
  ASSERT_EQ(write.wait_for(30s), std::future_status::ready);
  const auto evidence = write.get();
  ASSERT_NE(evidence.on(a.id()), nullptr);
  EXPECT_EQ(evidence.on(a.id())->state, ReplicaState::acknowledged);
  ASSERT_NE(evidence.on(b.id()), nullptr);
  EXPECT_EQ(evidence.on(b.id())->state, ReplicaState::pending);
  EXPECT_TRUE(a.data().has(id));
}

// Progress anywhere in the caller's pipeline re-arms the window, as Macha's shared
// counter does: the write goes on past the budget while the counter moves, and
// ends a whole budget after it stops.
TEST(StoreRepair, AWritesBudgetIsReArmedByItsPipelinesProgress) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto writer = options(2, 2, 8ULL * 1024 * 1024);
  writer.time = &time;
  writer.store.call_no_progress = 300s;
  auto& a = cluster.add("a", writer);
  auto& b = cluster.add("b", options(2, 2, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(a, b.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::put_object, gate));
  const auto bytes = pattern(64 * 1024, 62);
  const auto id = object_id(bytes);
  auto progress = std::make_shared<std::atomic_uint64_t>(0);
  WriteOptions publication;
  publication.ack_level = Durability::durable;
  publication.no_progress_budget = 30s;
  publication.progress = progress;
  auto write = std::async(std::launch::async, [&] { return a.store().write(id, bytes, publication); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  for (int step = 0; step < 4; ++step) {
    time.advance(20s);
    progress->fetch_add(1);
  }
  // 80 s on, past two budgets, but the counter moved within each.
  EXPECT_EQ(write.wait_for(2s), std::future_status::timeout);
  // The last move is seen when the write next looks, which may be after the
  // first of these: a whole budget after that, it ends.
  for (int step = 0; step < 3 && write.wait_for(5s) != std::future_status::ready; ++step) time.advance(31s);
  ASSERT_EQ(write.wait_for(30s), std::future_status::ready);
  const auto evidence = write.get();
  ASSERT_NE(evidence.on(b.id()), nullptr);
  EXPECT_EQ(evidence.on(b.id())->state, ReplicaState::pending);
  EXPECT_TRUE(a.data().has(id));
}

// A caller's own deadline stays: the write returns at it with the peer's copy
// pending, the peer's answer reaches its ticket, and no window has passed.
TEST(StoreRepair, AWriteWithItsOwnDeadlineReturnsAtItAndItsCallRunsOn) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto writer = options(2, 2, 8ULL * 1024 * 1024);
  writer.time = &time;
  writer.store.call_no_progress = 300s;
  auto& a = cluster.add("a", writer);
  auto& b = cluster.add("b", options(2, 2, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(a, b.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::put_object, gate));
  const auto bytes = pattern(64 * 1024, 59);
  const auto id = object_id(bytes);
  WriteOptions durable;
  durable.ack_level = Durability::durable;
  durable.deadline = 10s;
  auto write = std::async(std::launch::async, [&] { return a.store().write(id, bytes, durable); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  time.advance(11s);
  ASSERT_EQ(write.wait_for(30s), std::future_status::ready);
  const auto evidence = write.get();
  ASSERT_NE(evidence.on(b.id()), nullptr);
  EXPECT_EQ(evidence.on(b.id())->state, ReplicaState::pending);
  gate.open();
  EXPECT_TRUE(wait_until([&] {
    const auto now = a.store().status(evidence.ticket);
    return now && now->on(b.id()) && now->on(b.id())->state == ReplicaState::acknowledged;
  }));
}

// get_from and get_remote at Macha 98b816d (:1401-1417, :1554-1555): with no deadline a
// read waits for its peer with no limit of its own.
TEST(StoreRepair, AReadWithNoDeadlineWaitsForAPeerInsideTheWindowPastAnyLimit) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto reader = waits_on_credit();
  reader.time = &time;
  reader.store.call_no_progress = 300s;
  auto& a = cluster.add("a", reader);
  auto& b = cluster.add("b", waits_on_credit());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 60);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.data().put(id, bytes));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::get_object, gate));
  ReadOptions read;
  read.frame = FrameType::foreground;
  auto result = std::async(std::launch::async, [&] { return a.store().get(id, read); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  time.advance(200s);
  EXPECT_EQ(result.wait_for(1s), std::future_status::timeout);
  gate.open();
  ASSERT_EQ(result.wait_for(30s), std::future_status::ready);
  EXPECT_EQ(result.get(), bytes);
}

// The bound on a read: the peer silent for the window is passed over, the next is
// asked and has its own window, and the read lands however long that all took.
TEST(StoreRepair, AReadWithNoDeadlineMovesPastAPeerSilentForTheWindow) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto reader = waits_on_credit();
  reader.time = &time;
  reader.store.call_no_progress = 300s;
  auto& a = cluster.add("a", reader);
  auto& b = cluster.add("b", waits_on_credit());
  auto& c = cluster.add("c", waits_on_credit());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 61);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.data().put(id, bytes));
  ASSERT_TRUE(c.data().put(id, bytes));
  // Whichever peer is asked first stays silent.
  dannan::test::GateOpener opener(gate);
  std::atomic_bool taken{false};
  const test::Interpose first_is_silent = [&](MessageType seen, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (seen == message::get_object && !taken.exchange(true)) gate.enter_and_wait();
    return std::nullopt;
  };
  b.interpose(first_is_silent);
  c.interpose(first_is_silent);
  ReadOptions read;
  read.frame = FrameType::foreground;
  const auto sent = [&] { return a.node().traffic().out_bytes[static_cast<size_t>(FrameType::foreground)]; };
  const auto sent_before = sent();
  auto result = std::async(std::launch::async, [&] { return a.store().get(id, read); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  // The clock moves only once a has counted its request sent. Dannan records a call's
  // progress as its bytes leave, then counts them (session.cpp:337-350 at 0.8.0); the
  // peer can enter the gate before that, and progress noted after the move restarted
  // the window, so the read never moved on (failed twice on node-c, 2026-11-15/16).
  ASSERT_TRUE(wait_until([&] { return sent() > sent_before; }));
  time.advance(301s);
  ASSERT_EQ(result.wait_for(30s), std::future_status::ready);
  EXPECT_EQ(result.get(), bytes);
}

// The same with one peer: silent past the window, the read ends with nothing rather
// than waiting on, and says to try later.
TEST(StoreRepair, AReadWithNoDeadlineEndsWhenItsOnlyPeerIsSilentForTheWindow) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto reader = waits_on_credit();
  reader.time = &time;
  reader.store.call_no_progress = 300s;
  auto& a = cluster.add("a", reader);
  auto& b = cluster.add("b", waits_on_credit());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 62);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.data().put(id, bytes));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::get_object, gate));
  ReadOptions read;
  read.frame = FrameType::foreground;
  const auto sent = [&] { return a.node().traffic().out_bytes[static_cast<size_t>(FrameType::foreground)]; };
  const auto sent_before = sent();
  auto result = std::async(std::launch::async, [&] { return a.store().read(id, read); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  // As the case above: moved only once the request's progress is recorded.
  ASSERT_TRUE(wait_until([&] { return sent() > sent_before; }));
  time.advance(301s);
  ASSERT_EQ(result.wait_for(30s), std::future_status::ready);
  const auto answer = result.get();
  EXPECT_FALSE(answer.object);
  EXPECT_EQ(answer.answer, FetchAnswer::try_later);
}

// Macha's port (`call_deadline.hpp`, the maintainer 2026-11-11): each call names the latest it may
// end by its purpose, an object transfer 30 minutes, whatever its no-progress window.
// A put still held then ends there, and the copy is owed to prompt replication.
TEST(StoreRepair, APutToAPeerEndsAtTheObjectTransferDeadline) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto writer = options(2, 2, 8ULL * 1024 * 1024);
  writer.time = &time;
  writer.store.call_no_progress = 1h;
  auto& a = cluster.add("a", writer);
  auto& b = cluster.add("b", options(2, 2, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(a, b.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::put_object, gate));
  const auto bytes = pattern(64 * 1024, 63);
  const auto id = object_id(bytes);
  WriteOptions durable;
  durable.ack_level = Durability::durable;
  auto write = std::async(std::launch::async, [&] { return a.store().write(id, bytes, durable); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  time.advance(1801s);
  ASSERT_EQ(write.wait_for(30s), std::future_status::ready);
  const auto evidence = write.get();
  ASSERT_NE(evidence.on(b.id()), nullptr);
  EXPECT_EQ(evidence.on(b.id())->state, ReplicaState::pending);
  EXPECT_TRUE(a.data().has(id));
}

// The same for a read: its only peer held past 30 minutes, it ends and says to try later.
TEST(StoreRepair, AReadFromAPeerEndsAtTheObjectTransferDeadline) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto reader = waits_on_credit();
  reader.time = &time;
  reader.store.call_no_progress = 1h;
  auto& a = cluster.add("a", reader);
  auto& b = cluster.add("b", waits_on_credit());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 64);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.data().put(id, bytes));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::get_object, gate));
  ReadOptions read;
  read.frame = FrameType::foreground;
  auto result = std::async(std::launch::async, [&] { return a.store().read(id, read); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  time.advance(1801s);
  ASSERT_EQ(result.wait_for(30s), std::future_status::ready);
  const auto answer = result.get();
  EXPECT_FALSE(answer.object);
  EXPECT_EQ(answer.answer, FetchAnswer::try_later);
}

// A presence batch is a presence probe there, two minutes, with no window of its own
// (Macha's port sends it asynchronously): a silent peer is waited for that long.
TEST(StoreRepair, APresenceBatchToASilentPeerEndsAtThePresenceDeadline) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto asker = options(2, 1, 8ULL * 1024 * 1024);
  asker.time = &time;
  auto& a = cluster.add("a", asker);
  auto& b = cluster.add("b", options(2, 1, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 65);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.data().put(id, bytes));
  const auto peer = record_of(a, b.id());
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::have_valid_objects, gate));
  auto asked = std::async(std::launch::async, [&] { return a.store().valid_on(peer, {id}); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  time.advance(31s);
  EXPECT_EQ(asked.wait_for(2s), std::future_status::timeout);
  time.advance(90s);
  ASSERT_EQ(asked.wait_for(30s), std::future_status::ready);
  EXPECT_TRUE(asked.get().empty());
}

// A delete asked of a peer is a control call, two minutes, and Macha's port waits on it
// synchronously, so its no-progress window ends a silent one first: not the 30 s it
// was, but the window.
TEST(StoreRepair, ADeleteAskedOfASilentPeerEndsAtTheNoProgressWindow) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto asker = options(2, 1, 8ULL * 1024 * 1024);
  asker.time = &time;
  asker.store.control_no_progress = 60s;
  auto& a = cluster.add("a", asker);
  auto& b = cluster.add("b", options(2, 1, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 66);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.data().put(id, bytes));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::delete_object, gate));
  auto erased = std::async(std::launch::async, [&] { a.store().erase_all(id); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  time.advance(31s);
  EXPECT_EQ(erased.wait_for(2s), std::future_status::timeout);
  // Past the window however late the call noted its request sent (a slow host can
  // note it after the first step), and inside the purpose deadline.
  time.advance(61s);
  EXPECT_EQ(erased.wait_for(30s), std::future_status::ready);
}

// A CONTROL object fetched from a peer is metadata there, five minutes, waited on
// synchronously: the no-progress window ends a silent peer, not the 30 s it was.
TEST(StoreRepair, AControlFetchFromASilentPeerEndsAtTheNoProgressWindow) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto asker = options(2, 1, 8ULL * 1024 * 1024);
  asker.time = &time;
  asker.store.control_no_progress = 60s;
  auto& a = cluster.add("a", asker);
  auto& b = cluster.add("b", options(2, 1, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(4 * 1024, 67);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.control().put(id, bytes));
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::get_control_object, gate));
  auto fetched = std::async(std::launch::async, [&] { return a.store().ensure_control_local(id); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  time.advance(31s);
  EXPECT_EQ(fetched.wait_for(2s), std::future_status::timeout);
  // Past the window however late the call noted its request sent (a slow host can
  // note it after the first step), and inside the purpose deadline.
  time.advance(61s);
  ASSERT_EQ(fetched.wait_for(30s), std::future_status::ready);
  EXPECT_FALSE(fetched.get());
}

// A trie question is a presence probe there, two minutes, waited on synchronously: the
// no-progress window ends a silent peer, and the survey counts it failed.
TEST(StoreRepair, ATrieQuestionToASilentPeerEndsAtTheNoProgressWindow) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  Cluster cluster(std::chrono::hours(24));
  auto asker = options(2, 2);
  asker.time = &time;
  asker.availability = {};
  asker.availability.call_no_progress = 60s;
  auto& a = cluster.add("a", asker);
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(64 * 1024, 68);
  const auto id = object_id(bytes);
  ASSERT_TRUE(a.data().put(id, bytes));
  ASSERT_TRUE(b.data().put(id, bytes));
  auto provider = referring_to({id});
  a.publish_inventory(provider);
  dannan::test::GateOpener opener(gate);
  b.interpose(hold(message::trie_diff, gate));
  auto surveyed = std::async(std::launch::async, [&] { return a.availability().refresh(); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  time.advance(31s);
  EXPECT_EQ(surveyed.wait_for(2s), std::future_status::timeout);
  // Past the window however late the call noted its request sent (a slow host can
  // note it after the first step), and inside the purpose deadline.
  time.advance(61s);
  ASSERT_EQ(surveyed.wait_for(30s), std::future_status::ready);
  (void)surveyed.get();
  const auto snapshot = a.availability().snapshot();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->survey.peers_failed, 1u);
}

// src/cluster/distributed_store.cpp at Macha 9a08846, prompt_replication_loop (:1306):
// the read for a prompt copy takes no DATA credit, so a node whose credit is taken
// still sends its copy. The local write fits what is left; the extent a lease
// would charge does not.
TEST(StoreRepair, APromptCopyReadsWithoutCreditWhenTheNodeSaysSo) {
  Cluster cluster;
  auto machas = waits_on_credit();
  machas.store.prompt_read_admission = false;
  auto& a = cluster.add("a", machas);
  auto& b = cluster.add("b", machas);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(a, b.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto bytes = pattern(64 * 1024, 56);
  const auto id = object_id(bytes);
  auto& credit = *a.credit();
  const auto most = credit.try_acquire(DataWorkContext(FrameType::foreground, 0), credit.stats().capacity_bytes - 128 * 1024);
  ASSERT_TRUE(most.has_value());
  ASSERT_TRUE(a.store().put_here(id, bytes));
  EXPECT_TRUE(wait_until([&] { return b.data().has(id); }));
  EXPECT_EQ(credit.stats().speculative_waits, 0u);
  // Counted once the peer's answer is back, which is after the peer holds it.
  EXPECT_TRUE(wait_until([&] { return a.store().prompt_replication_stats().copies == 1; }));
}

// Dagda's default for a node that is not Macha's: the read is admitted as other
// speculative device work is, and waits for credit; the copy follows when it comes.
TEST(StoreRepair, APromptCopyWaitsForCreditByDefault) {
  Cluster cluster;
  auto& a = cluster.add("a", waits_on_credit());
  auto& b = cluster.add("b", waits_on_credit());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(a, b.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto bytes = pattern(64 * 1024, 57);
  const auto id = object_id(bytes);
  auto& credit = *a.credit();
  auto most = credit.try_acquire(DataWorkContext(FrameType::foreground, 0), credit.stats().capacity_bytes - 128 * 1024);
  ASSERT_TRUE(most.has_value());
  ASSERT_TRUE(a.store().put_here(id, bytes));
  ASSERT_TRUE(wait_until([&] { return credit.stats().speculative_waits == 1; }));
  EXPECT_FALSE(b.data().has(id));
  most.reset();
  EXPECT_TRUE(wait_until([&] { return b.data().has(id); }));
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

// test_replica_repair_does_not_count_corrupt_remote_as_healthy, as scrub then
// diff (the Macha session's answers, 2026-11-05; repair is diff only, the maintainer,
// 2026-11-04). A corrupt copy reads as held until its own node's scrub, so a
// peer's repair sends nothing, and it is never served: valid_on says it is not
// intact. The scrub discards it, and the sender's next pass, from the diff
// alone, restores the bytes: only the sender runs a pass here, and it sends the
// copy from the first list that names it, with no event in between.
TEST(StoreRepair, ACorruptRemoteCopyIsRestoredByItsScrubThenTheDiff) {
  Cluster cluster;
  auto& sender = cluster.add("sender", options(2, 2));
  auto& corrupt = cluster.add("corrupt", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(128 * 1024, 9);
  const auto id = object_id(bytes);
  ASSERT_TRUE(sender.store().put(id, bytes));
  ASSERT_TRUE(corrupt.data().has(id));
  test::corrupt_object(corrupt.backend(), id);

  auto provider = referring_to({id});
  sender.publish_inventory(provider);
  corrupt.publish_inventory(provider);
  const auto puts = corrupt.calls_of(message::put_object);
  for (int pass = 0; pass < 4; ++pass) (void)sender.run_maintenance_once();
  EXPECT_EQ(corrupt.calls_of(message::put_object), puts);
  const auto valid = sender.store().valid_on(record_of(sender, corrupt.id()), {id});
  const auto said = valid.find(id);
  EXPECT_TRUE(said == valid.end() || !said->second);

  (void)corrupt.data().scrub_step(128ULL * 1024 * 1024, 256);
  EXPECT_FALSE(corrupt.data().has(id));
  EXPECT_TRUE(wait_until(
      [&] {
        (void)sender.run_maintenance_once();
        return corrupt.data().has(id);
      },
      30s));
  EXPECT_EQ(corrupt.data().get(id), std::optional<Bytes>{bytes});
  EXPECT_GT(corrupt.calls_of(message::put_object), puts);
}

// A list that arrives after an empty one is walked by the step that first sees
// it (Macha's reset of its completed pass leaves the push not complete). A step
// that passed over it would report a whole pass with nothing done, the pass
// would rest on these very lists, and nothing would move until an event.
TEST(StoreRepair, TheStepThatFirstSeesANewListWalksIt) {
  Cluster cluster;
  auto& node = cluster.add("node", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto pushed = pattern(64 * 1024, 71);
  const auto pulled = pattern(64 * 1024, 72);
  ASSERT_TRUE(node.data().put(object_id(pushed), pushed));
  ASSERT_TRUE(peer.data().put(object_id(pulled), pulled));
  const auto inventory = inventory_of({object_id(pushed), object_id(pulled)});

  const RepairLists empty{inventory, list_of({}), lacks_of({{peer.id(), {}}}), {}};
  EXPECT_TRUE(landed_step(node.store(), empty, 128ULL * 1024 * 1024).complete);

  const RepairLists named{inventory, list_of({object_id(pulled)}), lacks_of({{peer.id(), {object_id(pushed)}}}), {}};
  const auto filled = landed_step(node.store(), named, 128ULL * 1024 * 1024);
  EXPECT_EQ(peer.data().get(object_id(pushed)), std::optional<Bytes>{pushed});
  EXPECT_EQ(node.data().get(object_id(pulled)), std::optional<Bytes>{pulled});
  // Macha's test_the_repair_step_that_first_sees_a_new_pull_list_walks_it: the new
  // list's one object examined by that step.
  EXPECT_EQ(filled.pull_examined, 1u);
}

// Macha's test_a_pull_list_new_while_the_push_runs_is_not_passed_over, its fix the
// library matches: while a pass is still open, a new pull list arriving is walked by the
// step that first sees it, not passed over until the pass ends. In Macha's shape: its
// peer holds everything, so the push examines and sends nothing. Dagda's push is
// diff-only (a listed object is one the peer lacks), so the same is a peer with no room
// for an extent: examined, never sent. Macha's pass stayed open because its step examined
// a bounded number; the repair redesign's stage 4 took that bound away, and a pass is
// open while what it started is in flight, so here the first list's pull is held at the
// peer while the second list arrives.
TEST(StoreRepair, APullListNewWhileThePushRunsIsNotPassedOver) {
  dannan::test::Gate gate;
  Cluster cluster;
  auto& node = cluster.add("node", options(2));
  // Room for the objects to be pulled from it, none for an extent pushed to it.
  auto& peer = cluster.add("peer", options(2, 1, 512 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(node, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto first = pattern(64 * 1024, 73);
  const auto second = pattern(64 * 1024, 74);
  ASSERT_TRUE(peer.data().put(object_id(first), first));
  ASSERT_TRUE(peer.data().put(object_id(second), second));
  ASSERT_TRUE(node.store().should_own(object_id(first)));
  ASSERT_TRUE(node.store().should_own(object_id(second)));
  auto pushing = local_objects(node, 100, 4 * 1024, 60);
  auto everything = pushing;
  everything.push_back(object_id(first));
  everything.push_back(object_id(second));
  const auto inventory = inventory_of(everything);
  constexpr uint64_t budget = 64ULL * 1024 * 1024;

  dannan::test::GateOpener opener(gate);
  peer.interpose(hold_ids(message::get_object, {object_id(first)}, gate));
  const RepairLists opening{inventory, list_of({object_id(first)}), lacks_of({{peer.id(), pushing}}), {}};
  ASSERT_FALSE(node.store().repair_step(opening, budget, wide).complete);
  ASSERT_TRUE(gate.wait_for_entries(1));
  const auto gets = peer.calls_of(message::get_object);
  const RepairLists next{inventory, list_of({object_id(second)}), lacks_of({{peer.id(), pushing}}), {}};
  (void)node.store().repair_step(next, budget, wide);
  EXPECT_TRUE(wait_until([&] { return node.data().has(object_id(second)); }, 10s));
  EXPECT_EQ(peer.calls_of(message::get_object), gets + 1);
  EXPECT_EQ(node.data().get(object_id(second)), std::optional<Bytes>{second});
  gate.open();
  (void)node.store().repair_drain(30s);
  // The push sent nothing: it is Macha's shape.
  EXPECT_EQ(peer.calls_of(message::put_object), 0u);
}

// The repair redesign's (TODO/2026-11-16-repair-redesign.md, finding 1): a push that
// ends its step out of credit must not keep the pull from the step. Every object here
// has one copy, and the one to pull sorts before every one to push, so it is the first
// transfer due; a step with credit for an extent makes it. Today the push takes the
// credit first and the pull is skipped while the push is credit-limited, step after step.
// Seen failing first (39fe114, node-a); stage 2's queue makes it pass. The survey
// says the peer holds the object to pull, so its one copy is known.
TEST(StoreRepair, APushOutOfCreditDoesNotHoldBackThePull) {
  Cluster cluster;
  auto& node = cluster.add("node", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(node, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto pushing = local_objects(node, 8, extent, 80);
  // One the peer alone holds, its id before every pushed one's.
  Bytes wanted;
  for (uint8_t salt = 81; salt != 0; ++salt) {
    wanted = pattern(64 * 1024, salt);
    if (object_id(wanted) < pushing.front()) break;
  }
  ASSERT_LT(object_id(wanted), pushing.front());
  ASSERT_TRUE(peer.data().put(object_id(wanted), wanted));
  ASSERT_TRUE(node.store().should_own(object_id(wanted)));
  auto everything = pushing;
  everything.push_back(object_id(wanted));
  const RepairLists lists{
      inventory_of(everything), list_of({object_id(wanted)}), lacks_of({{peer.id(), pushing}}), {}, holds_of({{peer.id(), {object_id(wanted)}}})};

  // Credit for one extent a step, and no limit on calls.
  const auto step = landed_step(node.store(), lists, extent);
  EXPECT_EQ(node.data().get(object_id(wanted)), std::optional<Bytes>{wanted})
      << "credit_limited=" << step.credit_limited << " bytes=" << step.bytes_transferred;
}

// The repair redesign's stage 1: the survey publishes, per peer that answered, what it
// holds of what is referenced and this node lacks, so repair can count an object's
// copies and knows a source for each. Asked of nobody new: it comes from the same diffs.
TEST(StoreRepair, TheSurveyPublishesWhoHoldsWhatThisNodeLacks) {
  Cluster cluster;
  auto& a = cluster.add("a", options(3, 3));
  auto& b = cluster.add("b", options(3, 3));
  auto& c = cluster.add("c", options(3, 3));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  EXPECT_EQ(a.availability().peer_holds(), nullptr);

  const auto only_b = local_objects(b, 6, 4096, 11);
  const auto only_c = local_objects(c, 4, 4096, 12);
  const auto both = local_objects(b, 5, 4096, 13);
  for (const auto& id : both) ASSERT_TRUE(c.data().put(id, *b.data().get(id)));
  const auto here = local_objects(a, 3, 4096, 14);
  // What b holds that nothing refers to.
  (void)local_objects(b, 2, 4096, 15);

  std::vector<ObjectId> referenced;
  for (const auto* part : std::initializer_list<const std::vector<ObjectId>*>{&only_b, &only_c, &both, &here})
    referenced.insert(referenced.end(), part->begin(), part->end());
  auto provider = referring_to(referenced);
  a.publish_inventory(provider);
  const auto trie_diffs = b.calls_of(message::trie_diff) + c.calls_of(message::trie_diff);
  ASSERT_TRUE(a.availability().refresh());
  const auto holds = a.availability().peer_holds();
  ASSERT_NE(holds, nullptr);
  EXPECT_TRUE(holds->complete);
  EXPECT_EQ(holds->generation, provider.current_generation);
  ASSERT_EQ(holds->holds.size(), 2u);
  const auto merged = [](std::vector<ObjectId> one, const std::vector<ObjectId>& other) {
    one.insert(one.end(), other.begin(), other.end());
    std::sort(one.begin(), one.end());
    return one;
  };
  EXPECT_EQ(*holds->holds.at(b.id()), merged(only_b, both));
  EXPECT_EQ(*holds->holds.at(c.id()), merged(only_c, both));
  // The same questions as before, and no probe.
  EXPECT_GT(b.calls_of(message::trie_diff) + c.calls_of(message::trie_diff), trie_diffs);
  EXPECT_EQ(probes(b) + probes(c), 0u);
}

// The repair redesign's stage 2: the object with fewest copies goes first, whichever
// way its bytes travel. Three nodes keep three copies. One object has a single copy,
// on a peer, and this node should hold it; many others have two copies and lack their
// third on that same peer. With credit for one extent a step, the first step fetches
// the one at risk, though it sorts after every object to push.
TEST(StoreRepair, TheObjectWithFewestCopiesGoesFirst) {
  Cluster cluster;
  auto& node = cluster.add("node", options(3));
  auto& other = cluster.add("other", options(3));
  auto& peer = cluster.add("peer", options(3));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] {
    return record_of(node, peer.id()).advert_u64(capacity_advert).value_or(0) > 0 && record_of(node, other.id()).advert_u64(capacity_advert).value_or(0) > 0;
  }));
  // Two copies each, here and on `other`; `peer` lacks them.
  const auto pushing = local_objects(node, 6, extent, 90);
  for (const auto& id : pushing) ASSERT_TRUE(other.data().put(id, *node.data().get(id)));
  // One copy, on `peer` alone, its id after every pushed one's.
  Bytes at_risk;
  for (uint8_t salt = 91; salt != 0; ++salt) {
    at_risk = pattern(64 * 1024, salt);
    if (pushing.back() < object_id(at_risk)) break;
  }
  ASSERT_LT(pushing.back(), object_id(at_risk));
  ASSERT_TRUE(peer.data().put(object_id(at_risk), at_risk));
  auto everything = pushing;
  everything.push_back(object_id(at_risk));
  const RepairLists lists{inventory_of(everything),
                          list_of({object_id(at_risk)}),
                          lacks_of({{peer.id(), pushing}, {other.id(), {}}}),
                          {},
                          holds_of({{peer.id(), {object_id(at_risk)}}, {other.id(), {}}})};

  const auto step = landed_step(node.store(), lists, extent);
  EXPECT_EQ(node.data().get(object_id(at_risk)), std::optional<Bytes>{at_risk})
      << "credit_limited=" << step.credit_limited << " bytes=" << step.bytes_transferred;
  // And then the rest, a step's credit at a time.
  for (int more = 0; more < 12; ++more) (void)landed_step(node.store(), lists, extent);
  for (const auto& id : pushing) EXPECT_TRUE(peer.data().has(id));
}

// The repair redesign's stage 3, the offer: a holder asks an owner which ids it wants
// before it reads or sends a byte. An owner that holds the object already (the survey
// that said it lacked it is out of date) is sent nothing and counted a keeper, so the
// next step offers it nothing either. The offer is not a probe.
TEST(StoreRepair, APushToAPeerThatHoldsTheObjectMovesNoObjectBytes) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(writer, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto live = local_objects(writer, 4, 64 * 1024, 100);
  for (const auto& id : live) ASSERT_TRUE(peer.data().put(id, *writer.data().get(id)));
  const RepairLists lists{inventory_of(live), {}, lacks_of({{peer.id(), live}}), {}};

  const auto step = landed_step(writer.store(), lists, 8ULL * 1024 * 1024);
  EXPECT_EQ(peer.calls_of(message::put_object), 0u);
  EXPECT_EQ(step.bytes_transferred, 0u);
  EXPECT_EQ(step.push_examined, live.size());
  EXPECT_EQ(peer.calls_of(message::offer_objects), 1u);
  EXPECT_EQ(probes(peer), 0u);
  (void)landed_step(writer.store(), lists, 8ULL * 1024 * 1024);
  EXPECT_EQ(peer.calls_of(message::offer_objects), 1u) << "a keeper is remembered as a pushed copy is";
  EXPECT_EQ(peer.calls_of(message::put_object), 0u);
  for (const auto& id : live) EXPECT_TRUE(writer.data().has(id));
}

// A peer whose own repair is fetching the object declines the offer and is sent
// nothing; the holder offers it again on the retry's backoff, by when the peer holds it.
TEST(StoreRepair, APeerFetchingAnObjectDeclinesItsOfferAndIsSentNothing) {
  dannan::test::Gate gate;
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(writer, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto bytes = pattern(256 * 1024, 101);
  const auto id = object_id(bytes);
  ASSERT_TRUE(writer.data().put(id, bytes));
  ASSERT_TRUE(peer.store().should_own(id));
  const RepairLists pull{inventory_of({id}), list_of({id}), lacks_of({{writer.id(), {}}}), {}, holds_of({{writer.id(), {id}}})};
  const RepairLists push{inventory_of({id}), {}, lacks_of({{peer.id(), {id}}}), {}};

  dannan::test::GateOpener opener(gate);
  writer.interpose(hold(message::get_object, gate));
  auto pulling = std::async(std::launch::async, [&] { return landed_step(peer.store(), pull, 8ULL * 1024 * 1024); });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  const auto step = landed_step(writer.store(), push, 8ULL * 1024 * 1024);
  EXPECT_EQ(peer.calls_of(message::offer_objects), 1u);
  EXPECT_EQ(peer.calls_of(message::put_object), 0u);
  EXPECT_EQ(step.bytes_transferred, 0u);
  gate.open();
  ASSERT_EQ(pulling.wait_for(30s), std::future_status::ready);
  ASSERT_TRUE(peer.data().has(id));

  ASSERT_TRUE(wait_until(
      [&] {
        (void)landed_step(writer.store(), push, 8ULL * 1024 * 1024);
        return peer.calls_of(message::offer_objects) >= 2;
      },
      15s));
  EXPECT_EQ(peer.calls_of(message::put_object), 0u);
  EXPECT_TRUE(writer.data().has(id));
}

// A peer that has said it wants an offered object does not fetch it too while the copy
// is on its way. If the copy never comes, it fetches it after all, once the transfer's
// deadline has passed.
TEST(StoreRepair, APeerThatWantedAnOfferedObjectFetchesItOnlyIfTheCopyNeverComes) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto receiving = options(2);
  receiving.store.object_transfer_call = 2s;
  auto& peer = cluster.add("peer", receiving);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(writer, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto bytes = pattern(256 * 1024, 102);
  const auto id = object_id(bytes);
  ASSERT_TRUE(writer.data().put(id, bytes));
  ASSERT_TRUE(peer.store().should_own(id));
  // The copy is lost on its way.
  peer.interpose([](MessageType type, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (type == message::put_object) return Handled::failed(503, "storage_busy", "lost by the case");
    return std::nullopt;
  });
  (void)landed_step(writer.store(), {inventory_of({id}), {}, lacks_of({{peer.id(), {id}}}), {}}, 8ULL * 1024 * 1024);
  EXPECT_EQ(peer.calls_of(message::offer_objects), 1u);
  EXPECT_FALSE(peer.data().has(id));

  const RepairLists pull{inventory_of({id}), list_of({id}), lacks_of({{writer.id(), {}}}), {}, holds_of({{writer.id(), {id}}})};
  (void)landed_step(peer.store(), pull, 8ULL * 1024 * 1024);
  EXPECT_EQ(writer.calls_of(message::get_object), 0u) << "the copy promised is not fetched too";
  EXPECT_FALSE(peer.data().has(id));
  ASSERT_TRUE(wait_until(
      [&] {
        (void)landed_step(peer.store(), pull, 8ULL * 1024 * 1024);
        return peer.data().has(id);
      },
      15s));
  EXPECT_EQ(writer.calls_of(message::get_object), 1u);
}

// A holder whose credit covers one object offers only that one, so an owner is promised
// no more than is about to move and fetches the rest itself at once. Stage 3's
// regression under a viewer (the redesign document, "What stage 3 measured"): a paced
// holder's whole batch was promised and held the joiner back.
TEST(StoreRepair, AHolderOffersOnlyWhatItsCreditWillSend) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(writer, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto live = local_objects(writer, 4, 256 * 1024, 104);
  for (const auto& id : live) ASSERT_TRUE(peer.store().should_own(id));

  // Credit for one object and not two.
  (void)landed_step(writer.store(), {inventory_of(live), {}, lacks_of({{peer.id(), live}}), {}}, 300 * 1024);
  EXPECT_EQ(peer.calls_of(message::put_object), 1u);
  const auto held = std::count_if(live.begin(), live.end(), [&](const ObjectId& id) { return peer.data().has(id); });
  EXPECT_EQ(held, 1);

  (void)landed_step(peer.store(), {inventory_of(live), list_of(live), lacks_of({{writer.id(), {}}}), {}, holds_of({{writer.id(), live}})}, 8ULL * 1024 * 1024);
  for (const auto& id : live) EXPECT_TRUE(peer.data().has(id));
  EXPECT_EQ(writer.calls_of(message::get_object), live.size() - 1);
}

// A pull a promise holds back is looked at again each second, not on the doubling
// backoff, so it fetches as soon as the promise lapses: here five seconds after the
// offer, where the backoff would have waited until seven.
TEST(StoreRepair, APromisedPullIsLookedAtAgainEachSecond) {
  dannan::test::SteppedTime time;
  // Stepping the peer's clock must not make the writer look dead.
  Cluster cluster(std::chrono::hours(24));
  auto& writer = cluster.add("writer", options(2));
  auto receiving = options(2);
  receiving.time = &time;
  receiving.store.object_transfer_call = 5s;
  auto& peer = cluster.add("peer", receiving);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(writer, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto bytes = pattern(64 * 1024, 105);
  const auto id = object_id(bytes);
  ASSERT_TRUE(writer.data().put(id, bytes));
  ASSERT_TRUE(peer.store().should_own(id));
  // Offered and wanted, and the copy lost on its way.
  peer.interpose([](MessageType type, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (type == message::put_object) return Handled::failed(503, "storage_busy", "lost by the case");
    return std::nullopt;
  });
  (void)landed_step(writer.store(), {inventory_of({id}), {}, lacks_of({{peer.id(), {id}}}), {}}, 8ULL * 1024 * 1024);
  ASSERT_EQ(peer.calls_of(message::offer_objects), 1u);

  const RepairLists pull{inventory_of({id}), list_of({id}), lacks_of({{writer.id(), {}}}), {}, holds_of({{writer.id(), {id}}})};
  (void)landed_step(peer.store(), pull, 8ULL * 1024 * 1024);
  for (int second = 1; second < 5; ++second) {
    time.advance(1s);
    peer.store().time_advanced();
    (void)landed_step(peer.store(), pull, 8ULL * 1024 * 1024);
    EXPECT_FALSE(peer.data().has(id)) << "promised still, at " << second << " s";
  }
  EXPECT_EQ(writer.calls_of(message::get_object), 0u);
  time.advance(1s);
  peer.store().time_advanced();
  (void)landed_step(peer.store(), pull, 8ULL * 1024 * 1024);
  EXPECT_TRUE(peer.data().has(id)) << "the promise lapsed at 5 s";
  EXPECT_EQ(writer.calls_of(message::get_object), 1u);
}

// Both ends at once, as a node rejoins: its pull of one object is in flight while the
// holder pushes everything it lacks. Each object crosses the link once (the baseline
// measured one and a half to two times).
TEST(StoreRepair, ARejoiningNodeIsSentEachObjectOnce) {
  dannan::test::Gate gate;
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& rejoining = cluster.add("rejoining", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(writer, rejoining.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto live = local_objects(writer, 6, 128 * 1024, 103);
  for (const auto& id : live) ASSERT_TRUE(rejoining.store().should_own(id));
  const RepairLists pull{inventory_of(live), list_of(live), lacks_of({{writer.id(), {}}}), {}, holds_of({{writer.id(), live}})};
  const RepairLists push{inventory_of(live), {}, lacks_of({{rejoining.id(), live}}), {}};

  // The joiner's fetches are held at the holder while the holder pushes.
  dannan::test::GateOpener opener(gate);
  writer.interpose(hold(message::get_object, gate));
  auto pulling = std::async(std::launch::async, [&] { return landed_step(rejoining.store(), pull, 64ULL * 1024 * 1024); });
  dannan::test::GateOpener release(gate);
  ASSERT_TRUE(gate.wait_for_entries(1));
  (void)landed_step(writer.store(), push, 64ULL * 1024 * 1024);
  gate.open();
  ASSERT_EQ(pulling.wait_for(30s), std::future_status::ready);
  ASSERT_TRUE(wait_until(
      [&] {
        (void)landed_step(writer.store(), push, 64ULL * 1024 * 1024);
        (void)landed_step(rejoining.store(), pull, 64ULL * 1024 * 1024);
        return std::all_of(live.begin(), live.end(), [&](const ObjectId& id) { return rejoining.data().has(id); });
      },
      15s));
  EXPECT_EQ(rejoining.calls_of(message::put_object) + writer.calls_of(message::get_object), live.size());
}

// ---- The repair redesign's stage 4: a window of transfers ---------------------
//
// Repair's turn tops up a window of transfers and returns: it does not wait for what it
// starts. What lands is kept whatever the turn is doing, and is reported by the next
// step; the window is the credit's burst; a drained window raises the storage event once
// so an owner's pass refills it at once; and no call count bounds a turn.

// A transfer returns with its copies in flight: the turn is not held by the peer's
// answer. Nothing has landed yet, so its result moves no bytes and is not complete, and
// what it leaves owing is more work. The copies land with no further turn, and the next
// turn's result reports them.
TEST(StoreRepair, ATransferReturnsWithItsCopiesInFlight) {
  dannan::test::Gate puts;
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(writer, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto live = local_objects(writer, 2, 64 * 1024, 110);
  const RepairLists lists{inventory_of(live), {}, lacks_of({{peer.id(), live}}), {}};
  auto& repair = writer.repair();
  auto& time = writer.context().time().source();
  (void)repair.earn_credit(time.now(), 60s);

  dannan::test::GateOpener opener(puts);
  peer.interpose(hold(message::put_object, puts));
  auto transferring = std::async(std::launch::async, [&] {
    auto turn = repair.begin_turn(time.now(), 0);
    const auto step = repair.transfer(turn, lists);
    return std::pair{step, repair.settle({}, step)};
  });
  // Opens the gate before the future waits on its thread, so a failure ends the case.
  dannan::test::GateOpener release(puts);
  ASSERT_EQ(transferring.wait_for(10s), std::future_status::ready) << "the transfer waited for its copies";
  const auto [step, settled] = transferring.get();
  EXPECT_TRUE(puts.wait_for_entries(live.size(), 5s));
  EXPECT_EQ(step.bytes_transferred, 0u);
  EXPECT_FALSE(step.complete);
  EXPECT_EQ(settled, RepairSettled::more_work);
  for (const auto& id : live) EXPECT_FALSE(peer.data().has(id));

  puts.open();
  EXPECT_TRUE(wait_until([&] { return std::all_of(live.begin(), live.end(), [&](const ObjectId& id) { return peer.data().has(id); }); }, 10s));
  // The sender has them landed once its window is empty.
  EXPECT_TRUE(wait_until([&] { return writer.store().repair_diagnostics().in_flight == 0; }, 10s));
  RepairResult next;
  {
    auto turn = repair.begin_turn(time.now(), 0);
    next = repair.transfer(turn, lists);
  }
  EXPECT_EQ(next.bytes_transferred, live.size() * 64 * 1024);
}

// With nothing of a higher class present, repair's credit accrues at the whole link; while
// one is, at `idle_bandwidth_fraction` of it, and the share holds repair's turns besides
// (the maintainer, 2026-11-19: "Let idle repair use the link"). Measured (the redesign document, "(b)
// built and measured"): a joiner on an idle pair earned half the link and pulled one 4 MiB
// object a second on a link that carried one in a third of a second.
TEST(StoreRepair, AnIdleNodesRepairEarnsTheWholeLinkAndAPacedOneItsFraction) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto alone = options(2);
  alone.repair.extent_size = extent;
  alone.repair.initial_bandwidth = 100 * 1024;
  alone.repair.idle_bandwidth_fraction = 0.5;
  alone.repair.time = &time;
  auto& node = cluster.add("node", alone);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  auto& repair = node.repair();
  ASSERT_EQ(repair.paced_by(), 0);
  (void)repair.earn_credit(time.now());
  time.advance(1s);
  const auto idle = repair.earn_credit(time.now());
  EXPECT_DOUBLE_EQ(idle.rate, 100.0 * 1024) << "an idle node's repair earns the whole link";

  node.dagda_node().activity().note(FrameType::foreground, 64 * 1024);
  ASSERT_NE(repair.paced_by(), 0);
  time.advance(1s);
  const auto paced = repair.earn_credit(time.now());
  EXPECT_DOUBLE_EQ(paced.rate, 0.5 * 100.0 * 1024) << "under a viewer, the fraction";
}

// No call count bounds a turn (it was 16 calls a step, which set the rate for small
// objects: the redesign document, stage 0). A hundred small objects the credit covers
// all go in one turn and land with no other.
TEST(StoreRepair, ATransferIsNotBoundedByACallCount) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(writer, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto live = local_objects(writer, 100, 4 * 1024, 111);
  const RepairLists lists{inventory_of(live), {}, lacks_of({{peer.id(), live}}), {}};
  auto& repair = writer.repair();
  auto& time = writer.context().time().source();
  (void)repair.earn_credit(time.now(), 60s);
  {
    auto turn = repair.begin_turn(time.now(), 0);
    (void)repair.transfer(turn, lists);
  }
  EXPECT_TRUE(wait_until([&] { return std::all_of(live.begin(), live.end(), [&](const ObjectId& id) { return peer.data().has(id); }); }, 10s));
  const auto held = std::count_if(live.begin(), live.end(), [&](const ObjectId& id) { return peer.data().has(id); });
  EXPECT_EQ(held, static_cast<std::ptrdiff_t>(live.size()));
}

// The window is the credit's burst, and never holds more. The first turn's credit, a
// burst, covers two of the objects; while those are in flight a second turn, its credit
// earned back to a burst, starts nothing, as a third would take the window past it.
TEST(StoreRepair, TheWindowHoldsNoMoreThanTheCreditsBurst) {
  dannan::test::SteppedTime time;
  dannan::test::Gate puts;
  Cluster cluster;
  auto& writer = cluster.add("writer", windowed(time));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(writer, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto live = local_objects(writer, 6, 400 * 1024, 112);
  const RepairLists lists{inventory_of(live), {}, lacks_of({{peer.id(), live}}), {}};
  auto& repair = writer.repair();
  (void)repair.earn_credit(time.now(), 60s);
  ASSERT_EQ(repair.credit(), extent);

  dannan::test::GateOpener opener(puts);
  peer.interpose(hold(message::put_object, puts));
  const auto turn_of = [&] {
    return std::async(std::launch::async, [&] {
      auto turn = repair.begin_turn(time.now(), 0);
      return repair.transfer(turn, lists);
    });
  };
  auto first = turn_of();
  dannan::test::GateOpener release(puts);
  ASSERT_EQ(first.wait_for(10s), std::future_status::ready) << "the transfer waited for its copies";
  (void)first.get();
  ASSERT_TRUE(puts.wait_for_entries(2, 5s));

  time.advance(20s);
  (void)repair.earn_credit(time.now());
  ASSERT_EQ(repair.credit(), extent);
  auto second = turn_of();
  ASSERT_EQ(second.wait_for(10s), std::future_status::ready);
  (void)second.get();
  EXPECT_FALSE(puts.wait_for_entries(3, 1s)) << "the window took a third object";
  puts.open();
  EXPECT_TRUE(wait_until([&] { return std::count_if(live.begin(), live.end(), [&](const ObjectId& id) { return peer.data().has(id); }) == 2; }, 10s));
}

// A window drained to half or less raises the node's storage event, once a drain, so an
// owner's pass that waits on its events refills the window at once rather than an
// interval later. Two objects fill more than half the window; the first to land drains
// it to half, the second raises nothing more.
TEST(StoreRepair, ADrainedWindowRaisesTheStorageEventOnce) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto& writer = cluster.add("writer", windowed(time));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(wait_until([&] { return record_of(writer, peer.id()).advert_u64(capacity_advert).value_or(0) > 0; }));
  const auto live = local_objects(writer, 2, 400 * 1024, 113);
  const RepairLists lists{inventory_of(live), {}, lacks_of({{peer.id(), live}}), {}};
  auto& repair = writer.repair();
  (void)repair.earn_credit(time.now(), 60s);
  auto& events = writer.context().events();
  const auto storage = events.kind("storage");
  const auto before = events.count(storage);
  {
    auto turn = repair.begin_turn(time.now(), 0);
    (void)repair.transfer(turn, lists);
  }
  ASSERT_TRUE(wait_until([&] { return std::all_of(live.begin(), live.end(), [&](const ObjectId& id) { return peer.data().has(id); }); }, 10s));
  EXPECT_TRUE(wait_until([&] { return events.count(storage) > before; }, 5s));
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(events.count(storage), before + 1);
}

// A pull that lands is kept with no further turn, and a node stopped with pulls still in
// flight keeps every one that landed: two of four are held at the holder while the
// joiner stops, and the joiner restarts holding the other two.
TEST(StoreRepair, APullThatLandsIsKeptThoughTheNodeStopsMidWindow) {
  dannan::test::Gate gets;
  Cluster cluster;
  auto& holder = cluster.add("holder", options(2));
  auto& joiner = cluster.add("joiner", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(holder, 4, 64 * 1024, 114);
  for (const auto& id : live) ASSERT_TRUE(joiner.store().should_own(id));
  // No holder known: each is fetched alone, so two can be held while two land.
  const RepairLists lists{inventory_of(live), list_of(live), lacks_of({{holder.id(), {}}}), {}};
  const std::set<ObjectId> held{live[0], live[2]};
  auto& repair = joiner.repair();
  auto& time = joiner.context().time().source();
  (void)repair.earn_credit(time.now(), 60s);

  dannan::test::GateOpener opener(gets);
  holder.interpose(hold_ids(message::get_object, held, gets));
  auto transferring = std::async(std::launch::async, [&] {
    auto turn = repair.begin_turn(time.now(), 0);
    return repair.transfer(turn, lists);
  });
  dannan::test::GateOpener release(gets);
  ASSERT_EQ(transferring.wait_for(10s), std::future_status::ready) << "the transfer waited for its pulls";
  (void)transferring.get();
  EXPECT_TRUE(wait_until([&] { return joiner.data().has(live[1]) && joiner.data().has(live[3]); }, 10s));
  ASSERT_TRUE(gets.wait_for_entries(held.size(), 5s));

  joiner.stop();
  gets.open();
  joiner.start();
  EXPECT_TRUE(joiner.data().has(live[1]));
  EXPECT_TRUE(joiner.data().has(live[3]));
}

// The link's estimate is taught by what repair moved while its window was busy, not by
// each transfer alone: with a window, transfers share the link, and each one's bytes
// over its own time is only its share. Measured (the redesign document, "What stage 4
// measured"): a joiner's four concurrent pulls, sharing the link with its holder's
// pushes, took its estimate to 0.69 MB/s on an 8 MB/s link, and its credit, earned at
// half that, held it idle for 13 s. Here several pulls are held at the holder together,
// then answered together: the estimate is near their combined rate, not one's.
TEST(StoreRepair, TheLinksEstimateIsWhatTheWindowMovedNotOneTransfersShare) {
  dannan::test::Gate gets;
  Cluster cluster;
  auto& holder = cluster.add("holder", options(2));
  auto& joiner = cluster.add("joiner", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const dannan::net::ServerLimits limits;
  const size_t together = std::min<size_t>(6, limits.data_workers - limits.foreground_data_reserve);
  ASSERT_GE(together, 3u);
  constexpr size_t size = 512 * 1024;
  const auto live = local_objects(holder, together, size, 118);
  for (const auto& id : live) ASSERT_TRUE(joiner.store().should_own(id));
  const RepairLists lists{inventory_of(live), list_of(live), lacks_of({{holder.id(), {}}}), {}, holds_of({{holder.id(), live}})};
  ASSERT_EQ(joiner.store().estimated_network_bps(), 0.0);

  dannan::test::GateOpener opener(gets);
  holder.interpose(hold(message::get_object, gets));
  constexpr auto held = 500ms;
  std::jthread answer([&] {
    if (gets.wait_for_entries(together, 10s)) std::this_thread::sleep_for(held);
    gets.open();
  });
  (void)joiner.store().repair_step(lists, 64ULL * 1024 * 1024, wide);
  answer.join();
  (void)joiner.store().repair_drain(30s);
  for (const auto& id : live) ASSERT_TRUE(joiner.data().has(id));
  const double one = static_cast<double>(size) / std::chrono::duration<double>(held).count();
  EXPECT_GT(joiner.store().estimated_network_bps(), one * static_cast<double>(together) / 2) << "one transfer's share: " << one;
}

// A sample is never an extent over microseconds: bytes count over the time they were in
// flight. live-a, 2026-11-21: transfers still in flight when a sample reset its span landed
// together just after it, each a sample of an extent over almost no time, and the estimate
// went from 10 MiB/s to 20 GiB/s; the burst, five seconds of it, let one step read until the
// node was killed for memory. Here pulls are in flight under a wide window, the next step
// shrinks the window to nothing, so every landing past an extent is a sample, and the holder
// answers them all at once: the estimate stays near what they moved over the time they took.
TEST(StoreRepair, TheLinksEstimateIsNeverAnExtentOverMicroseconds) {
  dannan::test::Gate gets;
  Cluster cluster;
  constexpr size_t size = 4 * 1024 * 1024;
  const auto four_mib = [] {
    auto out = options(2);
    out.store.extent_size = size;
    return out;
  };
  auto& holder = cluster.add("holder", four_mib());
  auto& joiner = cluster.add("joiner", four_mib());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const dannan::net::ServerLimits limits;
  const size_t together = std::min<size_t>(6, limits.data_workers - limits.foreground_data_reserve);
  ASSERT_GE(together, 3u);
  const auto live = local_objects(holder, together, size, 119);
  for (const auto& id : live) ASSERT_TRUE(joiner.store().should_own(id));
  const RepairLists lists{inventory_of(live), list_of(live), lacks_of({{holder.id(), {}}}), {}, holds_of({{holder.id(), live}})};
  ASSERT_EQ(joiner.store().estimated_network_bps(), 0.0);

  dannan::test::GateOpener opener(gets);
  holder.interpose(hold(message::get_object, gets));
  constexpr auto held = 500ms;
  const auto started = std::chrono::steady_clock::now();
  (void)joiner.store().repair_step(lists, 64ULL * 1024 * 1024, wide);
  ASSERT_TRUE(gets.wait_for_entries(together, 10s));
  // The window shrinks while every pull is still in flight, as a slower link's would.
  (void)joiner.store().repair_step(lists, 64ULL * 1024 * 1024, RepairWindow{1});
  std::this_thread::sleep_for(held);
  gets.open();
  (void)joiner.store().repair_drain(30s);
  const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  for (const auto& id : live) ASSERT_TRUE(joiner.data().has(id));
  const double moved = static_cast<double>(size * together) / took;
  EXPECT_LE(joiner.store().estimated_network_bps(), 4 * moved) << "what the pulls moved over the time they took: " << moved << " B/s";
}

// The burst, and so what one step may read and hold, is bounded by memory whatever the
// link's estimate says: an estimate wrong by a thousand times cannot make a step read more
// than the window's bound (live-a's OOM, as above).
TEST(StoreRepair, TheBurstIsBoundedWhateverTheLinksEstimate) {
  Cluster cluster;
  auto absurd = options(2);
  absurd.repair.initial_bandwidth = 100ULL * 1024 * 1024 * 1024;
  auto& node = cluster.add("node", absurd);
  cluster.start_all();
  ASSERT_EQ(node.store().estimated_network_bps(), 0.0);
  // A minute at the absurd rate: the credit banks to the burst, so the burst is what shows.
  auto& time = node.context().time().source();
  (void)node.repair().earn_credit(time.now(), 60s);
  const auto earned = node.repair().earn_credit(time.now(), 60s);
  EXPECT_GT(earned.rate, 1e9) << "the case needs the absurd rate to stand";
  EXPECT_LE(earned.credit, 256ULL * 1024 * 1024) << "one step may read and hold the burst";
}

#if defined(__GLIBC__)
// What the process's allocator says: bytes handed out and still in use, and bytes it holds
// from the system (its arenas and the mappings of large blocks).
struct Heap {
  uint64_t in_use = 0;
  uint64_t held = 0;
};
Heap heap_now() {
  const auto info = mallinfo2();
  return {info.uordblks + info.hblkhd, info.arena + info.hblkhd};
}

// Repair holds no memory once its window drains: every round moves new extents, and neither
// what is in use nor what the allocator holds grows with the rounds (live-a, 2026-11-22: on
// 0.3.3, with the estimate right, its resident memory grew with what repair moved, in glibc's
// arenas and the main heap, past anything Macha counts).
TEST(StoreRepair, RepairHoldsNoMemoryOnceItsWindowDrains) {
  Cluster cluster;
  constexpr size_t size = 16 * 1024 * 1024;
  const auto big = [] {
    auto out = options(2, 1, 8ULL * 1024 * 1024 * 1024);
    out.store.extent_size = size;
    return out;
  };
  auto& holder = cluster.add("holder", big());
  auto& joiner = cluster.add("joiner", big());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  constexpr size_t rounds = 8;
  constexpr size_t per_round = 8;
  std::vector<Heap> after;
  for (size_t round = 0; round < rounds; ++round) {
    const auto live = local_objects(holder, per_round, size, static_cast<uint8_t>(40 + round));
    const RepairLists lists{inventory_of(live), list_of(live), lacks_of({{holder.id(), {}}}), {}, holds_of({{holder.id(), live}})};
    for (int step = 0; step < 50; ++step) {
      if (std::all_of(live.begin(), live.end(), [&](const auto& id) { return joiner.data().has(id); })) break;
      (void)joiner.store().repair_step(lists, 1ULL << 30, wide);
      (void)joiner.store().repair_drain(30s);
    }
    for (const auto& id : live) ASSERT_TRUE(joiner.data().has(id)) << "round " << round;
    after.push_back(heap_now());
  }
  // The first rounds set the high-water mark; the later ones must not move it.
  const auto& settled = after[1];
  const auto& last = after.back();
  constexpr uint64_t mib = 1024 * 1024;
  std::string figures;
  for (size_t i = 0; i < after.size(); ++i)
    figures +=
        " round " + std::to_string(i) + ": in use " + std::to_string(after[i].in_use / mib) + " MiB, held " + std::to_string(after[i].held / mib) + " MiB;";
  EXPECT_LE(last.in_use, settled.in_use + 16 * mib) << "repair's bytes still in use after its window drained:" << figures;
  EXPECT_LE(last.held, settled.held + 64 * mib) << "the allocator holds more with every round:" << figures;
}
#endif

// Small objects travel many to a message: a pull's size is not known until it lands, so
// each fetch holds an extent of the window, and one fetch a holder carries up to an
// extent of objects (get_objects). With a window of one extent, one turn fetches forty
// small objects in one message; one at a time, it would fetch one a turn.
TEST(StoreRepair, APullTurnFetchesManySmallObjectsInOneMessage) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto& holder = cluster.add("holder", options(2));
  auto& joiner = cluster.add("joiner", windowed(time));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(holder, 40, 8 * 1024, 115);
  for (const auto& id : live) ASSERT_TRUE(joiner.store().should_own(id));
  const RepairLists lists{inventory_of(live), list_of(live), lacks_of({{holder.id(), {}}}), {}, holds_of({{holder.id(), live}})};
  auto& repair = joiner.repair();
  (void)repair.earn_credit(time.now(), 60s);
  {
    auto turn = repair.begin_turn(time.now(), 0);
    (void)repair.transfer(turn, lists);
  }
  EXPECT_TRUE(wait_until([&] { return std::all_of(live.begin(), live.end(), [&](const ObjectId& id) { return joiner.data().has(id); }); }, 10s));
  EXPECT_EQ(holder.calls_of(message::get_objects), 1u);
  EXPECT_EQ(holder.calls_of(message::get_object), 0u);
}

// A fetch of many carries what an extent holds, in order, and says the rest were left;
// those are fetched by the steps after, many to a message again.
TEST(StoreRepair, AFetchOfManyStopsAtAnExtentAndTheRestFollow) {
  Cluster cluster;
  auto& holder = cluster.add("holder", options(2));
  auto& joiner = cluster.add("joiner", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(holder, 6, 400 * 1024, 116);
  for (const auto& id : live) ASSERT_TRUE(joiner.store().should_own(id));
  const RepairLists lists{inventory_of(live), list_of(live), lacks_of({{holder.id(), {}}}), {}, holds_of({{holder.id(), live}})};
  for (int step = 0; step < 10; ++step) {
    if (std::all_of(live.begin(), live.end(), [&](const ObjectId& id) { return joiner.data().has(id); })) break;
    (void)landed_step(joiner.store(), lists, extent);
  }
  for (const auto& id : live) EXPECT_EQ(joiner.data().get(id), holder.data().get(id));
  // Two of 400 KiB to an extent of 1 MiB, and the credit and the window cover one
  // fetch a step here: three fetches.
  EXPECT_EQ(holder.calls_of(message::get_objects), 3u);
  EXPECT_EQ(holder.calls_of(message::get_object), 0u);
}

// What a holder is asked for goes in as many fetches as the credit and the window cover,
// each an extent of them, so a step pulls a window's worth of large objects, not one
// extent's. Measured (the redesign document, "What stage 4 measured"): one fetch a holder
// a step brought one large object a pass, and the joiner of a large rejoin sat idle
// while its holder pushed everything on its own credit. Six objects of most of an
// extent, a window of eight extents: one step brings them all.
TEST(StoreRepair, AStepFetchesAWindowsWorthOfLargeObjectsFromOneHolder) {
  Cluster cluster;
  auto& holder = cluster.add("holder", options(2));
  auto& joiner = cluster.add("joiner", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(holder, 6, 900 * 1024, 117);
  for (const auto& id : live) ASSERT_TRUE(joiner.store().should_own(id));
  const RepairLists lists{inventory_of(live), list_of(live), lacks_of({{holder.id(), {}}}), {}, holds_of({{holder.id(), live}})};
  (void)joiner.store().repair_step(lists, 8 * extent, RepairWindow{8 * extent});
  (void)joiner.store().repair_drain(30s);
  for (const auto& id : live) EXPECT_TRUE(joiner.data().has(id));
  // One fetch an object: each part of one id is a fetch of one.
  EXPECT_EQ(holder.calls_of(message::get_objects) + holder.calls_of(message::get_object), live.size());
}

// What a fetch of many leaves is asked for again as its answer lands, on the extent of
// the window it already holds, while the step's credit covers it; it does not wait for
// the next step. Measured (the redesign document, "The holder's side, traced", "Read"):
// with a window of one extent, a joiner pulled one 4 MiB object a step, a second apart,
// on a link that carried one in a third of a second. Six objects of most of an extent,
// a window of one extent, credit for six: one step and its drain bring them all.
TEST(StoreRepair, WhatAFetchOfManyLeavesIsAskedForAgainAsItLands) {
  Cluster cluster;
  auto& holder = cluster.add("holder", options(2));
  auto& joiner = cluster.add("joiner", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(holder, 6, 900 * 1024, 119);
  for (const auto& id : live) ASSERT_TRUE(joiner.store().should_own(id));
  const RepairLists lists{inventory_of(live), list_of(live), lacks_of({{holder.id(), {}}}), {}, holds_of({{holder.id(), live}})};
  (void)joiner.store().repair_step(lists, 6 * extent, RepairWindow{extent});
  (void)joiner.store().repair_drain(30s);
  for (const auto& id : live) EXPECT_TRUE(joiner.data().has(id));
  // One fetch an object, every one from the one step.
  EXPECT_EQ(holder.calls_of(message::get_objects), live.size());
  EXPECT_EQ(holder.calls_of(message::get_object), 0u);
}

// The credit still bounds it: what the step's credit does not cover waits for the next
// step, as before. Credit for two extents: the step and its drain bring two.
TEST(StoreRepair, WhatAFetchOfManyLeavesPastTheStepsCreditWaitsForTheNextStep) {
  Cluster cluster;
  auto& holder = cluster.add("holder", options(2));
  auto& joiner = cluster.add("joiner", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto live = local_objects(holder, 6, 900 * 1024, 120);
  for (const auto& id : live) ASSERT_TRUE(joiner.store().should_own(id));
  const RepairLists lists{inventory_of(live), list_of(live), lacks_of({{holder.id(), {}}}), {}, holds_of({{holder.id(), live}})};
  (void)joiner.store().repair_step(lists, 2 * extent, RepairWindow{extent});
  (void)joiner.store().repair_drain(30s);
  const auto held = std::count_if(live.begin(), live.end(), [&](const ObjectId& id) { return joiner.data().has(id); });
  EXPECT_EQ(held, 2);
  EXPECT_EQ(holder.calls_of(message::get_objects), 2u);
}

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

// Macha's test_a_stop_during_the_diff_is_not_a_failed_peer (test_availability.cpp): a
// stop asked for while a peer's diff runs ends the survey with the stop, not with the
// peer counted as failed or put on the retry timer; the next survey asks it as any
// other.
TEST(StoreRepair, AStopDuringTheDiffIsNotAFailedPeer) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  // Enough on b for the diff to take several rounds, so a pause comes between them.
  const auto held = local_objects(b, 300, 1024, 7);
  auto provider = referring_to(held);
  a.publish_inventory(provider);
  const auto stop_once_asked = [&] {
    if (b.calls_of(message::trie_diff) > 0) throw MaintenanceStopping();
  };
  EXPECT_THROW((void)a.availability().refresh(stop_once_asked), MaintenanceStopping);
  EXPECT_GT(b.calls_of(message::trie_diff), 0u);
  EXPECT_EQ(a.availability().snapshot(), nullptr);
  EXPECT_FALSE(a.availability().due().has_value()) << "a stopped diff put the peer on the retry timer";
  ASSERT_TRUE(a.availability().refresh());
  ASSERT_NE(a.availability().snapshot(), nullptr);
  EXPECT_EQ(a.availability().snapshot()->survey.peers_failed, 0u);
  EXPECT_FALSE(a.availability().due().has_value());
}

// Macha's test_a_failed_diff_is_asked_again_on_a_timer (test_availability.cpp): a peer
// whose diff failed is asked again after the backoff with no new event, the backoff
// doubling from its floor while it keeps failing; an answer clears it, so nothing
// asks the peer again until something changes.
TEST(StoreRepair, AFailedDiffIsAskedAgainOnATimer) {
  dannan::test::SteppedTime time;
  Cluster cluster(std::chrono::hours(24));
  auto surveyor = options(2, 2);
  surveyor.time = &time;
  auto& a = cluster.add("a", surveyor);
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto held = local_objects(b, 20, 1024, 8);
  auto provider = referring_to(held);
  a.publish_inventory(provider);
  std::atomic_bool refusing{true};
  b.interpose([&](MessageType seen, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (seen == message::trie_diff && refusing.load()) return Handled::failed(500, "unavailable", "injected");
    return std::nullopt;
  });
  const AvailabilityOptions defaults;

  ASSERT_TRUE(a.availability().refresh());
  EXPECT_EQ(a.availability().snapshot()->survey.peers_failed, 1u);
  ASSERT_TRUE(a.availability().due().has_value());
  EXPECT_EQ(*a.availability().due(), time.now() + defaults.retry_floor);
  // Not before its time: nothing has changed, so nobody is asked.
  auto asked = b.calls_of(message::trie_diff);
  EXPECT_FALSE(a.availability().refresh());
  EXPECT_EQ(b.calls_of(message::trie_diff), asked);

  // Due: asked again, still failing, and the backoff doubles.
  time.advance(defaults.retry_floor);
  ASSERT_TRUE(a.availability().refresh());
  EXPECT_GT(b.calls_of(message::trie_diff), asked);
  ASSERT_TRUE(a.availability().due().has_value());
  EXPECT_EQ(*a.availability().due(), time.now() + 2 * defaults.retry_floor);

  // Answered: the backoff is cleared, and with nothing changed nobody is asked again.
  refusing = false;
  time.advance(2 * defaults.retry_floor);
  ASSERT_TRUE(a.availability().refresh());
  EXPECT_EQ(a.availability().snapshot()->survey.peers_failed, 0u);
  asked = b.calls_of(message::trie_diff);
  time.advance(std::chrono::hours(1));
  EXPECT_FALSE(a.availability().refresh());
  EXPECT_EQ(b.calls_of(message::trie_diff), asked);
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

// A served request's refusal, as status and code; none for an answer.
std::optional<std::pair<uint16_t, std::string>> refused(const CallOutcome& outcome) {
  EXPECT_TRUE(outcome.reply.has_value());
  if (!outcome.reply) return std::pair<uint16_t, std::string>{0, "no reply"};
  const auto refusal = dannan::net::refusal_of(*outcome.reply);
  if (!refusal) return std::nullopt;
  return std::pair{refusal->status, refusal->code};
}

// L7: at most four snapshots pinned (Macha's availability_service.hpp:177 at
// 98b816d, pinned_max): a fifth root question drops the oldest, which is then
// 410 snapshot_gone, while the four newer still answer.
TEST(StoreRepair, ATrieDiffPinsAtMostFourSnapshots) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2));
  auto& b = cluster.add("b", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto peer = record_of(a, b.id());
  const auto ask = [&](const ledger::TrieQuestion& question) {
    return a.context().routes().call_and_wait(peer, message::trie_diff, ledger::encode_trie_question(question), FrameType::speculative, 5s);
  };
  std::vector<decltype(ledger::ObjectTrie::Summary{}.hash)> roots;
  for (uint8_t round = 0; round < 5; ++round) {
    (void)local_objects(b, 1, 1024, static_cast<uint8_t>(90 + round));
    // The held trie takes the object when its ledger does: ask until the root moves.
    ledger::ObjectTrie::Summary summary;
    ASSERT_TRUE(wait_until([&] {
      const auto root = ask(ledger::TrieQuestion{});
      if (!root.reply || root.reply->type != message::trie_diff_reply) return false;
      summary = ledger::decode_trie_root(root.reply->payload);
      return summary.count == round + 1u;
    }));
    roots.push_back(summary.hash);
  }
  ledger::TrieQuestion children;
  children.kind = ledger::TrieQuestionKind::children;
  children.prefixes = {ledger::ObjectTrie::Prefix{}};
  children.at = roots.front();
  const auto oldest = refused(ask(children));
  ASSERT_TRUE(oldest.has_value());
  EXPECT_EQ(oldest->first, 410);
  EXPECT_EQ(oldest->second, "snapshot_gone");
  for (size_t i = 1; i < roots.size(); ++i) {
    children.at = roots[i];
    EXPECT_FALSE(refused(ask(children)).has_value()) << "pin " << i;
  }
}

// L7: a pin is dropped after two minutes unused (Macha's pinned_for), and one
// in use is kept. Macha (98b816d availability_service.cpp:335) reads the real
// clock for it; the server reads the node's time source, so stepping reaches it.
TEST(StoreRepair, ATrieDiffPinIsDroppedAfterTwoMinutesUnused) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto& a = cluster.add("a", options(2));
  auto serving = options(2);
  serving.time = &time;
  auto& b = cluster.add("b", serving);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  (void)local_objects(b, 10, 1024, 97);
  const auto peer = record_of(a, b.id());
  // b's liveness probes run on the stepped clock too, so a step past their budget closes the
  // route a call is on at once (Dannan's Client::await_ping), and that call fails with no
  // reply: CI's flake, 124 ms for the whole case. Asked until answered, the question reaches
  // b at the stepped time as one call would; a pin is touched only by a question it answers.
  const auto ask = [&](const ledger::TrieQuestion& question) {
    CallOutcome answered;
    EXPECT_TRUE(wait_until([&] {
      answered = a.context().routes().call_and_wait(peer, message::trie_diff, ledger::encode_trie_question(question), FrameType::speculative, 5s);
      return answered.reply.has_value();
    }));
    return answered;
  };
  const auto root = ask(ledger::TrieQuestion{});
  ASSERT_TRUE(root.reply && root.reply->type == message::trie_diff_reply);
  ledger::TrieQuestion children;
  children.kind = ledger::TrieQuestionKind::children;
  children.prefixes = {ledger::ObjectTrie::Prefix{}};
  children.at = ledger::decode_trie_root(root.reply->payload).hash;
  // Used within the two minutes, it stays; each use restarts them.
  time.advance(119s);
  EXPECT_FALSE(refused(ask(children)).has_value());
  time.advance(119s);
  EXPECT_FALSE(refused(ask(children)).has_value());
  // Unused for longer, it is gone.
  time.advance(121s);
  const auto gone = refused(ask(children));
  ASSERT_TRUE(gone.has_value());
  EXPECT_EQ(gone->first, 410);
}

// L7: while a node is still listing what it holds, a root question is 503
// holdings_seeding (Macha's availability_service.cpp:339). Its DATA ledger is
// removed while it is down and its objects directory holds one it cannot
// read, so the walk that would seed the ledger fails and retries.
TEST(StoreRepair, ARootQuestionWhileHoldingsSeedIsRefused) {
  if (::geteuid() == 0) GTEST_SKIP() << "running as root, which reads a directory whatever its permissions";
  Cluster cluster;
  auto& a = cluster.add("a", options(2));
  auto& b = cluster.add("b", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  (void)local_objects(b, 3, 1024, 98);
  b.stop();
  for (const auto& entry : std::filesystem::directory_iterator(b.state() / "ledger"))
    if (entry.path().filename().string().rfind("data-", 0) == 0) std::filesystem::remove_all(entry.path());
  const auto unreadable = b.backend() / "objects" / "zz";
  std::filesystem::create_directories(unreadable / "zz");
  std::filesystem::permissions(unreadable, std::filesystem::perms::none);
  struct Restore {
    std::filesystem::path path;
    ~Restore() { std::filesystem::permissions(path, std::filesystem::perms::owner_all); }
  } restore{unreadable};
  b.start();
  ASSERT_TRUE(cluster.converged());
  const auto answer = a.context().routes().call_and_wait(record_of(a, b.id()), message::trie_diff, ledger::encode_trie_question(ledger::TrieQuestion{}),
                                                         FrameType::speculative, 5s);
  const auto seeding = refused(answer);
  ASSERT_TRUE(seeding.has_value());
  EXPECT_EQ(seeding->first, 503);
  EXPECT_EQ(seeding->second, "holdings_seeding");
  b.stop();
}

// S30: the serving side refuses in Macha's codes (storage_server.cpp at
// 98b816d, :204-205, :236, :247, :395): a DATA object past the extent size is
// 413 too_large; a put to a full store 507 storage_full; a delete of a claimed
// object 409 claimed, the object kept, and of an unclaimed one done; a get of
// what is not held 404 with every backend online and 503 storage_unavailable
// with one offline, since that one could hold it.
TEST(StoreRepair, TheServerRefusesInMachasCodes) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2));
  auto& b = cluster.add("b", options(2, 1, 4 * extent));
  const auto offline = cluster.dir() / "c-offline";
  std::filesystem::create_directories(offline);
  std::ofstream(offline / backend_marker_name) << "not a marker\n";
  auto with_offline = options(2);
  with_offline.more_backends = {offline};
  auto& c = cluster.add("c", with_offline);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_FALSE(c.data().every_backend_online());
  const auto call = [&](ClusterNode& to, MessageType type, const Bytes& payload) {
    return refused(a.context().routes().call_and_wait(record_of(a, to.id()), type, payload, FrameType::foreground, 10s));
  };

  const auto large = pattern(extent + 1, 81);
  const auto too_large = call(b, message::put_object, message::encode_object(object_id(large), large));
  ASSERT_TRUE(too_large.has_value());
  EXPECT_EQ(*too_large, (std::pair<uint16_t, std::string>{413, "too_large"}));
  EXPECT_FALSE(b.data().has(object_id(large)));

  const auto kept = pattern(64 * 1024, 82);
  const auto loose = pattern(64 * 1024, 83);
  ASSERT_TRUE(b.data().put(object_id(kept), kept));
  ASSERT_TRUE(b.data().put(object_id(loose), loose));
  EXPECT_TRUE(b.store().retain_data_here({object_id(kept)}, contract::RetentionDot{b.id(), 1}).empty());
  const auto claimed = call(b, message::delete_object, message::encode_id(object_id(kept)));
  ASSERT_TRUE(claimed.has_value());
  EXPECT_EQ(*claimed, (std::pair<uint16_t, std::string>{409, "claimed"}));
  EXPECT_TRUE(b.data().has(object_id(kept)));
  EXPECT_FALSE(call(b, message::delete_object, message::encode_id(object_id(loose))).has_value());
  EXPECT_FALSE(b.data().has(object_id(loose)));

  const auto nowhere = object_id(pattern(4096, 84));
  const auto not_found = call(b, message::get_object, message::encode_id(nowhere));
  ASSERT_TRUE(not_found.has_value());
  EXPECT_EQ(*not_found, (std::pair<uint16_t, std::string>{404, "not_found"}));
  const auto unavailable = call(c, message::get_object, message::encode_id(nowhere));
  ASSERT_TRUE(unavailable.has_value());
  EXPECT_EQ(*unavailable, (std::pair<uint16_t, std::string>{503, "storage_unavailable"}));

  // Full: objects until the pool refuses one, then a put over the wire.
  for (unsigned i = 0; i < 256; ++i) {
    auto bytes = pattern(256 * 1024, static_cast<uint8_t>(i));
    bytes[1] = static_cast<uint8_t>(i >> 8);
    bytes[2] = 0xf5;
    if (!b.data().put(object_id(bytes), bytes)) break;
    ASSERT_LT(i, 255u) << "b never filled";
  }
  const auto more = pattern(256 * 1024, 85);
  const auto full = call(b, message::put_object, message::encode_object(object_id(more), more));
  ASSERT_TRUE(full.has_value());
  EXPECT_EQ(*full, (std::pair<uint16_t, std::string>{507, "storage_full"}));
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
  write.ack_level = Durability::replicated;
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
  write.ack_level = Durability::replicated;
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
      write.ack_level = Durability::persisted;
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
