//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The local stages of a node's maintenance, Macha's: rebalance, the claims'
// compaction, DATA pack compaction, the scrub and the ledger verify, on its
// gates. Busy holds everything but the verify, which is paced and never stopped.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <variant>

#include "dagda/node.h"
#include "dagda/store/cpu_pacing.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda {
namespace {

using namespace std::chrono_literals;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;

uint64_t figure(ClusterNode& node, const std::string& name) {
  node.dagda_node().publish_status();
  const auto snapshot = node.context().status().snapshot();
  const auto found = snapshot.values.find(name);
  if (found == snapshot.values.end()) {
    ADD_FAILURE() << name << " not published";
    return 0;
  }
  return std::get<uint64_t>(found->second);
}

store::ObjectId put_one(ClusterNode& node, uint64_t n) {
  const auto bytes = test::numbered(n);
  const auto id = store::object_id(bytes);
  EXPECT_TRUE(node.data().put(id, bytes));
  return id;
}

std::string read_all(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Enough of a link that a millisecond earns an extent's credit.
ClusterNodeOptions fast_link() {
  ClusterNodeOptions options;
  options.repair.initial_bandwidth = 1ULL << 42;
  return options;
}

TEST(NodeLocalMaintenance, TheCpuScaleIsMachas) {
  EXPECT_EQ(store::background_cpu_scale(5, 0, 0, 0.1), 1.0);
  EXPECT_EQ(store::background_cpu_scale(0.05, 0, 1, 0.1), 1.0);
  EXPECT_DOUBLE_EQ(store::background_cpu_scale(1.0, 0, 1, 0.1), 0.1);
  // The stage's own CPU does not pace it.
  EXPECT_EQ(store::background_cpu_scale(1.0, 1.0, 1, 0.1), 1.0);
}

TEST(NodeLocalMaintenance, TheVerifysRateIsMachas) {
  EXPECT_DOUBLE_EQ(ledger_verify_credit(0, 1, false), 4.0);
  EXPECT_DOUBLE_EQ(ledger_verify_credit(0, 1, true), 0.25);
  EXPECT_DOUBLE_EQ(ledger_verify_credit(250, 1000, false), 256.0);
}

// A backend added later: copies that prefer it move there, every object still
// reads, and rebalance then waits for an event.
TEST(NodeLocalMaintenance, RebalanceMovesCopiesOntoTheirPreferredBackend) {
  Cluster cluster;
  auto& node = cluster.add("node", fast_link());
  cluster.start_all();
  std::vector<store::ObjectId> ids;
  for (uint64_t n = 0; n < 48; ++n) ids.push_back(put_one(node, n));
  node.add_backend(cluster.dir() / "second");
  node.restart();
  ASSERT_EQ(node.data().online_backends(), 2u);
  uint64_t moved = 0;
  bool settled = false;
  for (int pass = 0; pass < 32 && !settled; ++pass) {
    const auto report = node.run_maintenance_once();
    ASSERT_TRUE(report.local.rebalance_ran);
    moved += report.local.rebalance.objects;
    settled = report.local.rebalance.complete && !report.local.rebalance.bytes;
  }
  EXPECT_TRUE(settled);
  EXPECT_GT(moved, 0u);
  EXPECT_EQ(figure(node, "dagda.maintenance.rebalance_objects"), moved);
  for (const auto& id : ids) EXPECT_TRUE(node.data().has(id));
  EXPECT_FALSE(node.run_maintenance_once().local.rebalance_ran);
}

// Past 4096 records the claims' journal is compacted by the pass.
TEST(NodeLocalMaintenance, TheClaimsAreCompactedPast4096Records) {
  Cluster cluster;
  auto& node = cluster.add("node");
  cluster.start_all();
  EXPECT_FALSE(node.run_maintenance_once().local.retention_compacted);
  dannan::identity::NodeId origin;
  origin.bytes.fill(7);
  // A frame each, which is what the threshold counts.
  std::vector<store::ObjectId> ids;
  for (uint64_t n = 0; n < 4100; ++n) {
    ids.push_back(store::object_id(test::numbered(n)));
    node.claims().retain(contract::RetentionClass::data, ids.back(), {origin, n + 1});
  }
  EXPECT_TRUE(node.run_maintenance_once().local.retention_compacted);
  EXPECT_EQ(figure(node, "dagda.maintenance.retention_compactions"), 1u);
  EXPECT_FALSE(node.run_maintenance_once().local.retention_compacted);
  EXPECT_TRUE(node.claims().retained(contract::RetentionClass::data, ids.back()));
}

// Dead pack bytes are reclaimed, and the live objects survive it.
TEST(NodeLocalMaintenance, DeadPackBytesAreReclaimed) {
  ClusterNodeOptions options;
  options.packed = true;
  options.pack_target_size = 256 * 1024;
  Cluster cluster;
  auto& node = cluster.add("node", options);
  cluster.start_all();
  std::vector<store::ObjectId> live;
  std::vector<store::ObjectId> dead;
  for (uint64_t n = 0; n < 128; ++n) {
    const auto bytes = test::pattern(16 * 1024 + n, static_cast<uint8_t>(n));
    const auto id = store::object_id(bytes);
    ASSERT_TRUE(node.data().put(id, bytes));
    (n % 2 ? live : dead).push_back(id);
  }
  (void)node.data().reassert_durable(dead.back());
  for (const auto& id : dead) ASSERT_TRUE(node.data().remove(id));
  const auto before = node.data().used();
  const auto report = node.run_maintenance_once();
  ASSERT_TRUE(report.local.packs_ran);
  EXPECT_GT(report.local.packs_compacted, 0u);
  EXPECT_LT(node.data().used(), before);
  EXPECT_GT(figure(node, "dagda.maintenance.packs_compacted"), 0u);
  for (const auto& id : live) EXPECT_TRUE(node.data().get(id).has_value());
}

// The scrub waits for its campaign; once due it finds what rotted and sets the
// next date, which a restart keeps.
TEST(NodeLocalMaintenance, TheScrubRunsWhenDueAndItsDateSurvivesARestart) {
  dannan::test::SteppedWall wall(dannan::contract::system_wall_clock().unix_ms());
  auto options = fast_link();
  options.wall = &wall;
  Cluster cluster;
  auto& node = cluster.add("node", options);
  cluster.start_all();
  const auto schedule = node.state() / "maintenance" / "scrub.next";
  const auto first = read_all(schedule);
  EXPECT_EQ(first, std::to_string(wall.unix_ms() + 30ULL * 24 * 3600 * 1000) + "\n");
  node.restart();
  EXPECT_EQ(read_all(schedule), first);

  const auto rotten = put_one(node, 1);
  const auto sound = put_one(node, 2);
  (void)node.data().reassert_durable(sound);
  {
    const auto path = store::local_format::loose_path(node.backend() / "objects", rotten);
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    // Flipped, not set: the byte might already be any given value.
    file.seekg(-1, std::ios::end);
    const char last = static_cast<char>(file.get());
    file.seekp(-1, std::ios::end);
    file.put(static_cast<char>(last ^ 0x01));
  }
  EXPECT_FALSE(node.run_maintenance_once().local.scrub_ran);
  EXPECT_TRUE(node.data().has(rotten));

  wall.advance(std::chrono::hours(24 * 30) + 1s);
  bool complete = false;
  for (int pass = 0; pass < 16 && !complete; ++pass) {
    const auto report = node.run_maintenance_once();
    ASSERT_TRUE(report.local.scrub_ran);
    complete = report.local.scrub.complete;
  }
  EXPECT_TRUE(complete);
  EXPECT_FALSE(node.data().has(rotten));
  EXPECT_TRUE(node.data().has(sound));
  EXPECT_EQ(figure(node, "dagda.maintenance.scrub_campaigns"), 1u);
  const auto next = read_all(schedule);
  EXPECT_EQ(next, std::to_string(wall.unix_ms() + 30ULL * 24 * 3600 * 1000) + "\n");
  EXPECT_EQ(figure(node, "dagda.maintenance.scrub_due_unix_ms"), wall.unix_ms() + 30ULL * 24 * 3600 * 1000);
  EXPECT_FALSE(node.run_maintenance_once().local.scrub_ran);
}

// A date Macha's maintenance left is kept across the move.
TEST(NodeLocalMaintenance, AScheduleMachaLeftIsHonoured) {
  dannan::test::SteppedWall wall(dannan::contract::system_wall_clock().unix_ms());
  auto options = fast_link();
  options.wall = &wall;
  Cluster cluster;
  auto& node = cluster.add("node", options);
  cluster.start_all();
  node.stop();
  std::ofstream(node.state() / "maintenance" / "scrub.next") << wall.unix_ms() - 1 << "\n";
  node.start();
  EXPECT_TRUE(node.run_maintenance_once().local.scrub_ran);
}

// The verify finds a file gone behind the store, then rests for a day.
TEST(NodeLocalMaintenance, TheVerifyFindsWhatWentAndThenRests) {
  Cluster cluster;
  auto& node = cluster.add("node");
  cluster.start_all();
  const auto gone = put_one(node, 1);
  (void)node.data().reassert_durable(gone);
  ASSERT_TRUE(test::wait_until([&] { return node.data().held_view().identity.complete; }, 10s));
  std::filesystem::remove(store::local_format::loose_path(node.backend() / "objects", gone));
  const auto report = node.run_maintenance_once();
  ASSERT_TRUE(report.local.verify_ran);
  EXPECT_TRUE(report.local.verify.complete);
  EXPECT_EQ(report.local.verify.lost, 1u);
  EXPECT_EQ(figure(node, "dagda.maintenance.verify_passes"), 1u);
  EXPECT_FALSE(node.run_maintenance_once().local.verify_ran);
}

// Loader work holds every gated stage, whatever the credit and the schedule;
// when it stops they run. The same goes for foreground work.
TEST(NodeLocalMaintenance, BusyHoldsTheGatedStages) {
  dannan::test::SteppedWall wall(dannan::contract::system_wall_clock().unix_ms());
  auto options = fast_link();
  options.wall = &wall;
  options.repair.foreground_quiet = 300ms;
  Cluster cluster;
  auto& node = cluster.add("node", options);
  cluster.start_all();
  wall.advance(std::chrono::hours(24 * 31));
  (void)node.dagda_node().run_local_stages();
  for (const auto frame : {dannan::sched::FrameType::loader, dannan::sched::FrameType::foreground}) {
    node.context().activity().note(frame, 64 * 1024);
    const auto held = node.dagda_node().run_local_stages();
    EXPECT_FALSE(held.rebalance_ran);
    EXPECT_FALSE(held.retention_ran);
    EXPECT_FALSE(held.packs_ran);
    EXPECT_FALSE(held.scrub_ran);
    bool ran = false;
    ASSERT_TRUE(test::wait_until(
        [&] {
          const auto stages = node.dagda_node().run_local_stages();
          ran = stages.retention_ran && stages.packs_ran;
          return ran;
        },
        10s));
  }
}

// The rest of the process's CPU takes rebalance's and repair's credit away.
TEST(NodeLocalMaintenance, CpuElsewhereShrinksTheCredit) {
  auto burning = std::make_shared<std::atomic<double>>(0.0);
  const store::CpuSource hot = [burning] { return store::CpuSample{burning->fetch_add(1000.0), 0.0}; };
  auto options = fast_link();
  options.repair.initial_bandwidth = 1ULL << 30;
  options.maintenance.cpu = hot;
  options.repair.cpu = hot;
  Cluster cluster;
  auto& hot_node = cluster.add("hot", options);
  auto& cool_node = cluster.add("cool", fast_link());
  cluster.start_all();
  (void)hot_node.dagda_node().run_local_stages();
  (void)cool_node.dagda_node().run_local_stages();
  ASSERT_TRUE(test::wait_until([&] { return cool_node.dagda_node().run_local_stages().rebalance_ran; }, 10s));
  EXPECT_FALSE(hot_node.dagda_node().run_local_stages().rebalance_ran);
  (void)hot_node.repair().pass();
  std::this_thread::sleep_for(20ms);
  const auto pass = hot_node.repair().pass();
  EXPECT_LT(pass.credit, 1024u * 1024u);
}

// The node's own thread runs the stages.
TEST(NodeLocalMaintenance, TheNodesThreadRunsThem) {
  auto options = fast_link();
  options.maintenance.automatic = true;
  options.maintenance.interval = 5ms;
  Cluster cluster;
  auto& node = cluster.add("node", options);
  cluster.start_all();
  EXPECT_TRUE(test::wait_until([&] { return figure(node, "dagda.maintenance.verify_directories") > 0; }, 15s));
}

}  // namespace
}  // namespace dagda
