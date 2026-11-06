//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// What Macha's own loop needs of a node while it stays Macha's (until stage 8c):
// its adverts as the port expects them, and its parts on the pass's clocks.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/domain/wrap.h"
#include "dagda/node.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/reachability.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda {
namespace {

using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;

// Custody and no certificate, as a Macha node under its cluster key has: the
// advert is signed by a certificate's key, so there is none to publish, and the
// node opens without it. The cluster fixture always issues a certificate, so this
// builds its Dannan node by hand.
TEST(NodeSeams, ANodeWithoutACertificateOpensAndAdvertisesNoWrapKey) {
  dannan::test::TempDir dir;
  test::ClusterIo io;
  std::ofstream(dir.path() / "cluster.key") << "seams test";
  const auto source = dannan::identity::KeySource::typed_passphrase(
      [] {
        const std::string phrase = "node";
        return dannan::identity::Secret(std::span(reinterpret_cast<const uint8_t*>(phrase.data()), phrase.size()));
      },
      dannan::identity::Argon2Costs{1024, 1, 1});
  const auto dannan_state = dir.path() / "node";
  (void)dannan::identity::KeyCustody::create(dannan_state, source);
  dannan::NodeConfig config;
  config.state_path = dannan_state;
  config.key = dannan::identity::load_cluster_key(dir.path() / "cluster.key");
  config.listen_host = "127.0.0.1";
  config.advertise_host = "127.0.0.1";
  config.port = 0;
  config.version = "dagda-test";
  config.custody = source;
  config.log = std::make_shared<test::RecordingSink>();
  dannan::Node host(io.context, config);
  ASSERT_FALSE(host.context().identity().certificate().has_value());

  NodeOptions options;
  options.state = dir.path() / "state";
  options.key = test::known_answer_key();
  options.pool.backends.push_back({dir.path() / "data", 64ULL * 1024 * 1024, 0});
  options.maintenance.automatic = false;
  Node node(host.context(), options);
  EXPECT_TRUE(node.running());
  const auto self = host.context().nodes().self();
  EXPECT_FALSE(self.advert(domain::wrap_advert).has_value());
  EXPECT_EQ(domain::verify_wrap_advert(self, std::nullopt, 0).why, "no wrap advert");
  // The key itself is custody's, and is still there for what wraps with it.
  EXPECT_TRUE(std::ranges::equal(node.wrap_key().public_key(), domain::WrapKey::derive(host.context().identity())->public_key()));
  // The store's own adverts are as they are on any node.
  EXPECT_TRUE(self.advert(store::extents_advert).has_value());
  node.stop();
}

// With a certificate the node still publishes it, and it verifies.
TEST(NodeSeams, ANodeWithACertificateStillAdvertisesItsWrapKey) {
  Cluster cluster;
  auto& node = cluster.add("node");
  cluster.start_all();
  const auto now = node.context().time().unix_ms();
  const auto checked = domain::verify_wrap_advert(node.context().nodes().self(), cluster.ca().certificate(), now);
  EXPECT_TRUE(checked.ok) << checked.why;
}

// Macha's refresh takes its pass's now, so a fixture's manual clock moves the
// survey's retries and pacing. A peer that cannot answer is asked again after the
// back-off's floor on that clock, and real time has no say.
TEST(NodeSeams, TheSurveyRunsOnTheInjectedClock) {
  const auto survey_on = [](const std::function<void(ClusterNodeOptions&, const dannan::contract::TimeSource&)>& inject) {
    dannan::test::SteppedTime time;
    Cluster cluster;
    ClusterNodeOptions options;
    options.store.replication = 2;
    auto& peer = cluster.add("peer", options);
    inject(options, time);
    auto& node = cluster.add("node", options);
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    ASSERT_TRUE(test::wait_until([&] { return node.data().held_view().identity.complete; }));
    peer.interpose(
        [](dannan::net::MessageType type, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<dannan::net::Handled> {
          if (type == store::message::trie_diff) return dannan::net::Handled::processed();
          return std::nullopt;
        });
    test::TestProvider provider;
    provider.answer.complete = true;
    node.publish_inventory(provider);

    ASSERT_TRUE(node.availability().refresh());
    ASSERT_TRUE(node.availability().due().has_value());
    EXPECT_EQ(*node.availability().due(), time.now() + store::AvailabilityOptions{}.retry_floor);
    const auto asked = peer.calls_of(store::message::trie_diff);
    ASSERT_GT(asked, 0u);
    time.advance(store::AvailabilityOptions{}.retry_floor);
    EXPECT_TRUE(node.availability().refresh());
    EXPECT_GT(peer.calls_of(store::message::trie_diff), asked);
    peer.interpose({});
  };
  survey_on([](ClusterNodeOptions& options, const dannan::contract::TimeSource& time) { options.availability.time = &time; });
  survey_on([](ClusterNodeOptions& options, const dannan::contract::TimeSource& time) { options.maintenance.time = &time; });
}

// Macha's idle_for runs on its pass's clock: past the quiet window on that clock
// the pass is idle, and real time has no say.
TEST(NodeSeams, ActivityRunsOnThePassesClock) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  ClusterNodeOptions options;
  options.maintenance.time = &time;
  auto& node = cluster.add("node", options);
  cluster.start_all();
  auto& activity = node.dagda_node().activity();
  activity.note(dannan::sched::FrameType::foreground, 64 * 1024);
  const auto quiet = options.repair.foreground_quiet;
  EXPECT_TRUE(activity.recently_active(dannan::sched::WorkClass::foreground, quiet));
  time.advance(quiet + std::chrono::milliseconds(1));
  EXPECT_FALSE(activity.recently_active(dannan::sched::WorkClass::foreground, quiet));
}

// The verify's day of rest is on the pass's clock, and the scrub's due date on its
// wall clock.
TEST(NodeSeams, TheLocalStagesRunOnThePassesClocks) {
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  Cluster cluster;
  ClusterNodeOptions options;
  options.maintenance.time = &time;
  options.maintenance.wall = &wall;
  options.maintenance.scrub_interval = std::chrono::hours(1);
  auto& node = cluster.add("node", options);
  cluster.start_all();
  ASSERT_TRUE(test::wait_until([&] { return node.data().held_view().identity.complete && node.control().held_view().identity.complete; }));
  EXPECT_TRUE(node.run_maintenance_once().local.verify_ran);
  EXPECT_FALSE(node.run_maintenance_once().local.verify_ran);
  time.advance(std::chrono::hours(24));
  EXPECT_TRUE(node.run_maintenance_once().local.verify_ran);

  EXPECT_FALSE(node.run_maintenance_once().local.scrub_ran);
  wall.advance(std::chrono::hours(2));
  EXPECT_TRUE(node.run_maintenance_once().local.scrub_ran);
}

// A packed node with dead bytes for the compaction to reclaim.
class LocalStagesBed : public testing::Test {
 protected:
  void SetUp() override {
    ClusterNodeOptions options;
    options.packed = true;
    options.pack_target_size = 256 * 1024;
    options.repair.initial_bandwidth = 1ULL << 42;
    options.maintenance.time = &time;
    node = &cluster.add("node", options);
    cluster.start_all();
    std::vector<store::ObjectId> dead;
    for (uint64_t n = 0; n < 128; ++n) {
      const auto bytes = test::pattern(16 * 1024 + n, static_cast<uint8_t>(n));
      const auto id = store::object_id(bytes);
      ASSERT_TRUE(node->data().put(id, bytes));
      if (n % 2 == 0) dead.push_back(id);
    }
    (void)node->data().reassert_durable(dead.back());
    for (const auto& id : dead) ASSERT_TRUE(node->data().remove(id));
    used = node->data().used();
  }
  Node& dagda() { return node->dagda_node(); }

  dannan::test::SteppedTime time;
  Cluster cluster;
  ClusterNode* node = nullptr;
  uint64_t used = 0;
};

// Macha's compact_packs(stop) through the composite: the stop comes out as
// MaintenanceStopping, past the catch that logs a failed stage, and nothing was
// compacted. Without a stop the composite behaves as it did.
TEST_F(LocalStagesBed, TheCompositeTakesTheStopAndNothingSwallowsIt) {
  std::stop_source stop;
  stop.request_stop();
  EXPECT_THROW((void)dagda().run_local_stages(stop.get_token()), contract::MaintenanceStopping);
  EXPECT_EQ(node->data().used(), used);
  const auto stages = dagda().run_local_stages();
  EXPECT_TRUE(stages.packs_ran);
  EXPECT_GT(stages.packs_compacted, 0u);
  EXPECT_LT(node->data().used(), used);
}

// A stop after the turn began reaches the stage on its own, and each stage throws
// it whichever it is.
TEST_F(LocalStagesBed, EachStageThrowsTheStop) {
  const std::vector<std::function<void(LocalTurn&)>> stages{
      [&](LocalTurn& turn) { dagda().local_rebalance(turn); }, [&](LocalTurn& turn) { dagda().retention_compact(turn); },
      [&](LocalTurn& turn) { dagda().compact_packs(turn); },   [&](LocalTurn& turn) { dagda().local_scrub(turn); },
      [&](LocalTurn& turn) { dagda().ledger_verify(turn); },
  };
  for (size_t index = 0; index < stages.size(); ++index) {
    SCOPED_TRACE(index);
    std::stop_source stop;
    LocalTurnOptions options;
    options.stop = stop.get_token();
    auto turn = dagda().begin_local_stages(options);
    stop.request_stop();
    EXPECT_THROW(stages[index](turn), contract::MaintenanceStopping);
  }
  EXPECT_EQ(node->data().used(), used);
}

// Macha's order is reachable: each stage does its own work and no other's, and
// what falls between them can take the node's own locks, as head repair and history
// truncation between the claims' compaction and the packs' may.
TEST_F(LocalStagesBed, TheStagesRunOneByOneWithAnOwnersBetweenThem) {
  dannan::identity::NodeId origin;
  origin.bytes.fill(7);
  for (uint64_t n = 0; n < 4100; ++n) node->claims().retain(contract::RetentionClass::data, store::object_id(test::numbered(n)), {origin, n + 1});
  LocalTurnOptions options;
  options.when_no_time_passed = std::chrono::seconds(1);
  auto turn = dagda().begin_local_stages(options);
  EXPECT_FALSE(turn.busy);

  dagda().local_rebalance(turn);
  EXPECT_TRUE(turn.stages.rebalance_ran);
  EXPECT_FALSE(turn.stages.retention_ran);
  dagda().retention_compact(turn);
  EXPECT_TRUE(turn.stages.retention_compacted);
  EXPECT_FALSE(turn.stages.packs_ran);
  EXPECT_EQ(node->data().used(), used);

  test::TestProvider owner;
  dagda().register_reachability(domain::domain_v1, owner);
  dagda().unregister_reachability(domain::domain_v1);

  dagda().compact_packs(turn);
  EXPECT_TRUE(turn.stages.packs_ran);
  EXPECT_LT(node->data().used(), used);
  EXPECT_FALSE(turn.stages.verify_ran);
  dagda().local_scrub(turn);
  EXPECT_FALSE(turn.stages.scrub_ran);
  dagda().ledger_verify(turn);
  EXPECT_TRUE(turn.stages.verify_ran);
}

// Each behind its gate: the node's activity holds them, and an owner's pass that
// has decided for itself is taken at its word.
TEST_F(LocalStagesBed, TheStagesKeepTheirGates) {
  dagda().activity().note(dannan::sched::FrameType::foreground, 64 * 1024);
  auto held = dagda().begin_local_stages();
  EXPECT_TRUE(held.busy);
  dagda().local_rebalance(held);
  dagda().retention_compact(held);
  dagda().compact_packs(held);
  dagda().local_scrub(held);
  EXPECT_FALSE(held.stages.rebalance_ran);
  EXPECT_FALSE(held.stages.retention_ran);
  EXPECT_FALSE(held.stages.packs_ran);
  EXPECT_FALSE(held.stages.scrub_ran);
  EXPECT_EQ(node->data().used(), used);

  LocalTurnOptions idle;
  idle.busy = false;
  auto taken = dagda().begin_local_stages(idle);
  EXPECT_FALSE(taken.busy);
  dagda().compact_packs(taken);
  EXPECT_TRUE(taken.stages.packs_ran);
}

// Macha earns a whole interval when no time has passed; the composite earns nothing.
TEST_F(LocalStagesBed, CreditEarnsAnIntervalOnlyWhenAsked) {
  auto first = dagda().begin_local_stages();
  dagda().local_rebalance(first);
  EXPECT_FALSE(first.stages.rebalance_ran);
  LocalTurnOptions options;
  options.when_no_time_passed = std::chrono::seconds(1);
  auto second = dagda().begin_local_stages(options);
  dagda().local_rebalance(second);
  EXPECT_TRUE(second.stages.rebalance_ran);
}

}  // namespace
}  // namespace dagda
