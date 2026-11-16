//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Step 7b: maintenance in the node and the per-domain wait (the maintainer, 2026-11-04
// night, "Claims and space": the collector and the GC and repair loop move
// into the node, the owner supplying only its reachability provider; the
// collector's wait is settable per domain). From the design: with no
// provider the collector removes nothing and says so; the domain's grace
// decides when an unreferenced, unclaimed object goes, on the node's own
// clock; the node's thread runs both passes on their own and stops first.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <variant>

#include "dagda/node.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/reachability.h"
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

std::string text(ClusterNode& node, const std::string& name) {
  node.dagda_node().publish_status();
  const auto snapshot = node.context().status().snapshot();
  const auto found = snapshot.values.find(name);
  if (found == snapshot.values.end()) {
    ADD_FAILURE() << name << " not published";
    return {};
  }
  return std::get<std::string>(found->second);
}

// An object in the node's own pool that nothing refers to or claims.
store::ObjectId orphan(ClusterNode& node, uint64_t n) {
  const auto bytes = test::numbered(n);
  const auto id = store::object_id(bytes);
  EXPECT_TRUE(node.data().put(id, bytes));
  return id;
}

// An owner whose state refers to nothing, at a head that has observed no
// claim write.
void refer_to_nothing(test::TestProvider& provider) {
  provider.answer.complete = true;
  contract::Hash256 head{};
  head.bytes[0] = 1;
  provider.head = head;
  contract::Reachability release;
  release.complete = true;
  provider.heads[head] = release;
}

// With no provider registered the collector does nothing destructive, and
// Status says so; repair still runs.
TEST(NodeMaintenance, WithNoProviderTheCollectorRemovesNothingAndSaysSo) {
  dannan::test::SteppedWall wall(dannan::contract::system_wall_clock().unix_ms());
  Cluster cluster;
  ClusterNodeOptions options;
  options.wall = &wall;
  options.garbage_grace = 1min;
  auto& a = cluster.add("a", options);
  a.start();
  const auto id = orphan(a, 1);
  EXPECT_NE(text(a, "dagda.gc.state").find("no reachability provider registered"), std::string::npos);
  for (int pass = 0; pass < 3; ++pass) {
    const auto report = a.run_maintenance_once();
    EXPECT_FALSE(report.gc);
    EXPECT_NE(report.gc_skipped.find("no reachability provider registered for domain"), std::string::npos) << report.gc_skipped;
    wall.advance(1h);
  }
  EXPECT_TRUE(a.data().has(id));
  EXPECT_EQ(figure(a, "dagda.gc.skipped"), 3u);
  EXPECT_EQ(figure(a, "dagda.gc.passes"), 0u);
  EXPECT_NE(text(a, "dagda.gc.state").find("the collector removes nothing"), std::string::npos);
  EXPECT_GT(a.repair().gates().ran + a.repair().gates().share + a.repair().gates().quiescent + a.repair().gates().credit, 0u);
}

// The domain's grace decides: on one stepped clock, a node whose domain
// waits two hours frees an unreferenced, unclaimed object once it has seen
// it so for two hours, and a node whose domain waits Macha's 30 days keeps
// its own.
TEST(NodeMaintenance, TheDomainsGraceDecidesWhenAnUnreferencedObjectGoes) {
  test::TestProvider short_owner;
  test::TestProvider long_owner;
  refer_to_nothing(short_owner);
  refer_to_nothing(long_owner);
  dannan::test::SteppedWall wall(dannan::contract::system_wall_clock().unix_ms());
  Cluster cluster;
  ClusterNodeOptions short_options;
  short_options.wall = &wall;
  short_options.garbage_grace = 2h;
  ClusterNodeOptions long_options;
  long_options.wall = &wall;
  auto& quick = cluster.add("quick", short_options);
  auto& slow = cluster.add("slow", long_options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  EXPECT_EQ(figure(quick, "dagda.gc.grace_ms"), 2u * 3600 * 1000);
  EXPECT_EQ(figure(slow, "dagda.gc.grace_ms"), 30ULL * 24 * 3600 * 1000);
  const auto quick_id = orphan(quick, 10);
  const auto slow_id = orphan(slow, 11);
  quick.dagda_node().register_reachability(domain::domain_v1, short_owner);
  slow.dagda_node().register_reachability(domain::domain_v1, long_owner);

  // The first pass builds the inventory and uses it for nothing; the second
  // sweeps and first sees each object unreferenced.
  for (auto* node : {&quick, &slow}) {
    const auto built = node->run_maintenance_once();
    ASSERT_TRUE(built.gc) << built.gc_skipped;
    EXPECT_TRUE(built.gc->rebuilt_inventory);
    const auto swept = node->run_maintenance_once();
    ASSERT_TRUE(swept.gc && swept.gc->data);
    EXPECT_TRUE(swept.gc->data->permitted) << swept.gc->data->reason;
  }
  EXPECT_EQ(text(quick, "dagda.gc.data_gate"), "open");
  wall.advance(119min);
  (void)quick.run_maintenance_once();
  EXPECT_TRUE(quick.data().has(quick_id)) << "freed before the domain's wait";
  wall.advance(2min);
  (void)quick.run_maintenance_once();
  (void)slow.run_maintenance_once();
  EXPECT_FALSE(quick.data().has(quick_id));
  EXPECT_TRUE(slow.data().has(slow_id));
  EXPECT_GT(figure(quick, "dagda.gc.reclaimed_bytes"), 0u);
  EXPECT_EQ(figure(slow, "dagda.gc.reclaimed_bytes"), 0u);
  wall.advance(24h * 29);
  (void)slow.run_maintenance_once();
  EXPECT_TRUE(slow.data().has(slow_id));
  wall.advance(24h + 1min);
  (void)slow.run_maintenance_once();
  EXPECT_FALSE(slow.data().has(slow_id));
  quick.dagda_node().unregister_reachability(domain::domain_v1);
  slow.dagda_node().unregister_reachability(domain::domain_v1);
}

// With maintenance on, the node's own thread runs GC and repair passes with
// nothing driving them, publishes their figures, frees what the domain's
// wait allows, and stops before anything it reaches.
TEST(NodeMaintenance, TheNodesThreadRunsThePassesOnItsOwn) {
  test::TestProvider owner;
  refer_to_nothing(owner);
  Cluster cluster;
  ClusterNodeOptions options;
  options.maintenance.automatic = true;
  options.maintenance.interval = 20ms;
  options.maintenance.gc_interval = 40ms;
  options.maintenance.collector.retry_window = 0ms;
  options.garbage_grace = 0ms;
  auto& a = cluster.add("a", options);
  a.start();
  auto bytes = test::numbered(20);
  const auto id = store::object_id(bytes);
  ASSERT_TRUE(a.data().put(id, bytes));
  a.dagda_node().register_reachability(domain::domain_v1, owner);
  EXPECT_TRUE(test::wait_until([&] { return !a.data().has(id); }, 30s)) << text(a, "dagda.gc.state") << " " << text(a, "dagda.gc.data_gate");
  // One pass to build the inventory, one to use it; its figures land as the
  // pass that removed the object ends.
  EXPECT_TRUE(test::wait_until([&] { return figure(a, "dagda.gc.reclaimed_bytes") > 0; }));
  EXPECT_GE(figure(a, "dagda.gc.passes"), 2u);
  EXPECT_GE(figure(a, "dagda.gc.inventories"), 1u);
  EXPECT_TRUE(test::wait_until([&] {
    const auto gates = a.repair().gates();
    return gates.ran + gates.share + gates.quiescent + gates.credit >= 3;
  }));
  // Unregistered, the provider is no longer read: the thread's pass in
  // progress is waited for.
  a.dagda_node().unregister_reachability(domain::domain_v1);
  const auto asked = owner.inventories + owner.releases;
  EXPECT_TRUE(test::wait_until([&] { return figure(a, "dagda.gc.skipped") >= 2; }));
  EXPECT_EQ(owner.inventories + owner.releases, asked);
  a.stop();
}

// A provider registered once the node runs is read at once, not when the collector's
// rest ends: the loop's first pass found no provider, which rests the collector as a
// settled sweep does. Until it is read nothing is referenced, so repair has nothing to
// work towards (seen as five minutes on the harness's repair workload, 2026-11-16).
TEST(NodeMaintenance, AProviderRegisteredLaterIsReadWhileTheCollectorRests) {
  test::TestProvider owner;
  refer_to_nothing(owner);
  Cluster cluster;
  ClusterNodeOptions options;
  options.maintenance.automatic = true;
  options.maintenance.interval = 20ms;
  options.maintenance.gc_interval = 1h;
  auto& a = cluster.add("a", options);
  a.start();
  // The loop has passed with no provider.
  ASSERT_TRUE(test::wait_until([&] { return figure(a, "dagda.gc.skipped") >= 1; }));
  a.dagda_node().register_reachability(domain::domain_v1, owner);
  EXPECT_TRUE(test::wait_until([&] { return a.ledger().inventory() != nullptr; }, 10s));
  a.dagda_node().unregister_reachability(domain::domain_v1);
  a.stop();
}

// A new reference reaches the inventory in the pass that sees the owner's generation
// move, not when the collector's rest ends: the survey and repair work from it, so a
// copy a write could not make would otherwise wait out the rest. Macha's pass runs its
// inventory stage whenever repair is due and rebuilds on a generation change
// (maintenance.cpp:611-627 at f0597a4). Here the collector rests an hour. (Before the
// fix this case stopped in its setup, the case above's defect: no GC pass ever ran.)
TEST(NodeMaintenance, ANewGenerationIsReadWhileTheCollectorRests) {
  test::TestProvider owner;
  refer_to_nothing(owner);
  Cluster cluster;
  ClusterNodeOptions options;
  options.maintenance.automatic = true;
  options.maintenance.interval = 20ms;
  options.maintenance.gc_interval = 1h;
  options.maintenance.collector.retry_window = 0ms;
  auto& a = cluster.add("a", options);
  a.start();
  a.dagda_node().register_reachability(domain::domain_v1, owner);
  // The sweep settles and the collector rests: repair's turns go by with no GC pass.
  uint64_t passes = 0;
  uint64_t turns_then = 0;
  ASSERT_TRUE(test::wait_until(
      [&] {
        const auto now = figure(a, "dagda.gc.passes");
        const auto gates = a.repair().gates();
        const auto turns = gates.ran + gates.share + gates.quiescent + gates.credit;
        if (now != passes) {
          passes = now;
          turns_then = turns;
          return false;
        }
        return passes >= 2 && turns >= turns_then + 10;
      },
      30s));
  ASSERT_NE(a.ledger().inventory(), nullptr);
  ASSERT_EQ(a.ledger().inventory()->generation(), 1u);

  // The owner's state moves on: it refers to one object more. Re-registered, so the
  // provider is never changed under a pass.
  a.dagda_node().unregister_reachability(domain::domain_v1);
  owner.answer.data = {store::object_id(test::numbered(30))};
  owner.current_generation = 2;
  owner.known = 2;
  a.dagda_node().register_reachability(domain::domain_v1, owner);
  EXPECT_TRUE(test::wait_until(
      [&] {
        const auto inventory = a.ledger().inventory();
        return inventory && inventory->generation() == 2;
      },
      10s))
      << "gc passes " << figure(a, "dagda.gc.passes");
  a.dagda_node().unregister_reachability(domain::domain_v1);
  a.stop();
}

// A stop during the owner's inventory walk ends it, rather than waiting for the walk
// (Macha 0.93.4). The walk here never finishes on its own, within the bound.
TEST(NodeMaintenance, AStopEndsTheOwnersInventoryWalk) {
  test::TestProvider owner;
  refer_to_nothing(owner);
  std::atomic<bool> walking{false};
  std::atomic<bool> ran_out{false};
  owner.walk = [&](const std::function<void()>& pause) {
    const auto until = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < until) {
      walking = true;
      if (pause) pause();
      std::this_thread::sleep_for(1ms);
    }
    ran_out = true;
  };
  ClusterNodeOptions options;
  options.maintenance.automatic = true;
  options.maintenance.interval = 5ms;
  options.maintenance.gc_interval = 5ms;
  options.repair.foreground_quiet = 1ms;
  Cluster cluster;
  auto& node = cluster.add("node", options);
  cluster.start_all();
  node.dagda_node().register_reachability(domain::domain_v1, owner);
  ASSERT_TRUE(test::wait_until([&] { return walking.load(); }));
  node.stop();
  EXPECT_FALSE(ran_out.load());
}

}  // namespace
}  // namespace dagda
