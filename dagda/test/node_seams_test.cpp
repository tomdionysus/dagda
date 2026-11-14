//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// What Macha's own loop needs of a node while it stays Macha's (until stage 8c):
// its adverts as the port expects them, its parts on the pass's clocks, its local
// stages one by one, and its own collector.

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/domain/wrap.h"
#include "dagda/node.h"
#include "dagda/store/messages.h"
#include "dagda/store/slow_stage.h"
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
  // A disk mounted there: a node never makes a backend's directory.
  std::filesystem::create_directories(dir.path() / "data");
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

// What the decision trace was told, by kind.
class TraceKinds {
 public:
  ledger::Collector::Trace sink() {
    return [this](std::string_view kind, std::string_view) {
      const std::scoped_lock lock(_mutex);
      _kinds.emplace(kind);
    };
  }
  bool saw(const std::string& kind) {
    const std::scoped_lock lock(_mutex);
    return _kinds.contains(kind);
  }

 private:
  std::mutex _mutex;
  std::set<std::string> _kinds;
};

// Macha's loop drives the node's own collector, to its trace.
TEST(NodeSeams, AnOwnersPassDrivesTheNodesCollectorToItsTrace) {
  TraceKinds trace;
  Cluster cluster;
  ClusterNodeOptions options;
  options.maintenance.trace = trace.sink();
  auto& node = cluster.add("node", options);
  cluster.start_all();
  auto& dagda = node.dagda_node();
  EXPECT_EQ(dagda.collector(), nullptr);

  test::TestProvider owner;
  owner.answer.complete = true;
  dagda.register_reachability(domain::domain_v1, owner);
  auto* collector = dagda.collector();
  ASSERT_NE(collector, nullptr);
  EXPECT_EQ(dagda.collector(), collector);
  const auto inventory = collector->inventory_step();
  EXPECT_TRUE(inventory.rebuilt);
  EXPECT_EQ(node.ledger().inventory(), inventory.inventory);
  EXPECT_TRUE(trace.saw("inventory"));

  // The node's own pass uses the same collector, and so the same trace.
  EXPECT_FALSE(trace.saw("gate.tombstones"));
  (void)node.run_maintenance_once();
  EXPECT_TRUE(trace.saw("gate.tombstones"));

  dagda.unregister_reachability(domain::domain_v1);
  EXPECT_EQ(dagda.collector(), nullptr);
}

// One hook is the whole of Macha's trace: repair's claim walk is on it too.
TEST(NodeSeams, TheTraceReachesRepairsClaimWalk) {
  TraceKinds trace;
  Cluster cluster;
  ClusterNodeOptions options;
  options.maintenance.trace = trace.sink();
  options.repair.initial_bandwidth = 1ULL << 42;
  auto& node = cluster.add("node", options);
  cluster.start_all();
  dannan::identity::NodeId origin;
  origin.bytes.fill(7);
  node.claims().retain(contract::RetentionClass::data, store::object_id(test::numbered(1)), {origin, 1});
  auto& repair = node.repair();
  const auto now = node.context().time().source().now();
  (void)repair.earn_credit(now, std::chrono::seconds(60));
  auto turn = repair.begin_turn(now, 0);
  (void)repair.claim_walks(turn);
  EXPECT_TRUE(trace.saw("claim-walk"));
}

// Two owners would race: the steps are single owner.
TEST(NodeSeams, TheCollectorIsNotHandedOutWhileTheNodesThreadRuns) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.maintenance.automatic = true;
  auto& node = cluster.add("node", options);
  cluster.start_all();
  EXPECT_THROW((void)node.dagda_node().collector(), std::logic_error);
}

// Macha's slow-stage line (maintenance.cpp log_slow_stage): 50 ms or more, at the
// log's most verbose level.
TEST(NodeSeams, ASlowStageIsLoggedInMachasWords) {
  auto sink = std::make_shared<test::RecordingSink>();
  const dannan::status::ScopedLogger logger(sink, "test");
  const auto now = dannan::contract::Clock::now();
  store::log_slow_stage(logger, "quick", now);
  EXPECT_FALSE(sink->saw("quick"));
  store::log_slow_stage(logger, "network-repair", now - std::chrono::milliseconds(60), "bytes=7");
  EXPECT_TRUE(sink->saw("DIAG maintenance-stage stage=network-repair elapsed_ms="));
  EXPECT_TRUE(sink->saw(" bytes=7"));
}

std::optional<dannan::status::Surface::Value> published(ClusterNode& node, const std::string& name) {
  node.dagda_node().publish_status();
  const auto snapshot = node.context().status().snapshot();
  const auto found = snapshot.values.find("dagda." + name);
  if (found == snapshot.values.end()) return std::nullopt;
  return found->second;
}

// Macha's diagnostics.repair.last_credit_bytes: the credit when the pass noted its
// gate, which is what the pass reported, and the classes that paced it.
TEST(NodeSeams, TheLastCreditAndWhatPacedRepairArePublished) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  ClusterNodeOptions options;
  options.maintenance.time = &time;
  auto& node = cluster.add("node", options);
  cluster.start_all();
  (void)node.repair().pass();
  time.advance(std::chrono::seconds(1));
  const auto pass = node.repair().pass();
  ASSERT_GT(pass.credit, 0u);
  EXPECT_EQ(node.repair().gates().last_credit_bytes, pass.credit);
  ASSERT_TRUE(published(node, "repair.last_credit_bytes").has_value());
  EXPECT_EQ(std::get<uint64_t>(*published(node, "repair.last_credit_bytes")), pass.credit);

  node.dagda_node().activity().note(dannan::sched::FrameType::foreground, 64 * 1024);
  (void)node.repair().pass();
  ASSERT_TRUE(published(node, "repair.paced_by").has_value());
  EXPECT_EQ(std::get<uint64_t>(*published(node, "repair.paced_by")) & store::paced_by_foreground, uint64_t{store::paced_by_foreground});
}

// Every figure of Macha's diagnostics the store holds and did not publish
// (status_api.cpp:1009-1038, :1082-1105, :1317-1368; its gauges,
// node_services.cpp), under Dagda's words for its classes.
TEST(NodeSeams, MachasOtherMaintenanceFiguresArePublished) {
  Cluster cluster;
  auto& node = cluster.add("node");
  cluster.start_all();
  for (const auto* name : {"repair.push_examined",
                           "repair.pull_examined",
                           "repair.push_phase_complete",
                           "repair.unsourceable_sample",
                           "repair.estimated_network_bps",
                           "activity.foreground_idle_ms",
                           "activity.loader_idle_ms",
                           "cache.capacity_bytes",
                           "cache.used_bytes",
                           "admission.capacity_bytes",
                           "admission.foreground_reserve_bytes",
                           "admission.peak_used_bytes",
                           "admission.foreground_admissions",
                           "admission.loader_admissions",
                           "admission.speculative_admissions",
                           "admission.foreground_waits",
                           "admission.loader_waits",
                           "admission.speculative_waits",
                           "admission.cancelled_waits",
                           "admission.background_limit",
                           "admission.background_active",
                           "admission.peak_background_active",
                           "admission.device_service_us",
                           "admission.device_worst_us",
                           "admission.device_slowdown_percent",
                           "admission.device_pressure_onsets",
                           "admission.no_progress_failures",
                           "data_store.loose_reaffirmation_fast_paths",
                           "data_store.loose_reaffirmation_full_validations",
                           "data_store.presence_index_entries",
                           "data_store.pack_recovery_truncated_tails",
                           "data_store.pack_recovery_skipped_regions",
                           "data_store.pack_recovery_skipped_bytes"}) {
    EXPECT_TRUE(published(node, name).has_value()) << name;
  }
  if (::testing::Test::HasFailure()) return;
  const auto stats = node.credit()->stats();
  EXPECT_EQ(std::get<uint64_t>(*published(node, "admission.capacity_bytes")), stats.capacity_bytes);
  EXPECT_EQ(std::get<uint64_t>(*published(node, "admission.background_limit")), stats.background_limit);
  EXPECT_EQ(std::get<uint64_t>(*published(node, "cache.capacity_bytes")), node.options().cache.max_blocks * node.options().store.extent_size);
}

// An object no peer would supply is in the sample, as Macha's list has it.
TEST(NodeSeams, TheUnsourceableSampleNamesItsObjects) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.store.replication = 2;
  auto& node = cluster.add("node", options);
  (void)cluster.add("peer", options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto lost = store::object_id(test::numbered(1));
  const std::vector<store::ObjectId> live{lost};
  store::RepairLists lists;
  lists.inventory =
      std::make_shared<const contract::InventoryHorizon>(uint64_t{1}, true, live, std::vector<store::ObjectId>{}, std::vector<contract::Garbage>{});
  lists.pull = std::make_shared<const std::vector<store::ObjectId>>(live);
  ASSERT_TRUE(test::wait_until([&] {
    (void)node.store().repair_step(lists, 1ULL << 30, 16, [] { return false; });
    return !node.store().repair_diagnostics().unsourceable_sample.empty();
  }));
  const auto text = published(node, "repair.unsourceable_sample");
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(std::get<std::string>(*text).find(store::to_string(lost)), std::string::npos);
}

// A log a test can act from, on whichever thread writes the line: a line that tells
// it something, and a line held until released, so a test can act while the work
// that wrote it is provably in progress.
class WatchingSink final : public dannan::status::LogSink {
 public:
  bool enabled(dannan::status::LogLevel) const noexcept override { return true; }
  void write(dannan::status::LogLevel, std::string_view, std::string_view message) override {
    std::function<void()> told;
    bool hold = false;
    {
      const std::scoped_lock lock(_mutex);
      if (!_told_by.empty() && message.find(_told_by) != std::string_view::npos) told = _told;
      if (!_held_by.empty() && message.find(_held_by) != std::string_view::npos) {
        hold = true;
        _held_by.clear();
      }
    }
    if (told) told();
    if (hold) {
      _entered.set_value();
      _released.wait();
    }
  }
  void tell(std::string text, std::function<void()> told) {
    const std::scoped_lock lock(_mutex);
    _told_by = std::move(text);
    _told = std::move(told);
  }
  void hold(std::string text) {
    const std::scoped_lock lock(_mutex);
    _held_by = std::move(text);
  }
  bool entered() { return _entered_future.wait_for(std::chrono::seconds(10)) == std::future_status::ready; }
  void release() {
    if (!_released_once.exchange(true)) _release.set_value();
  }

 private:
  std::mutex _mutex;
  std::string _told_by;
  std::function<void()> _told;
  std::string _held_by;
  std::promise<void> _entered;
  std::future<void> _entered_future = _entered.get_future();
  std::promise<void> _release;
  std::shared_future<void> _released = _release.get_future().share();
  std::atomic_bool _released_once{false};
};

// A node on a Dannan node built here, for what the cluster fixture does not carry.
struct HandBuiltNode {
  dannan::test::TempDir dir;
  test::ClusterIo io;
  std::shared_ptr<WatchingSink> sink = std::make_shared<WatchingSink>();
  std::function<void(NodeOptions&)> configure;
  std::unique_ptr<dannan::Node> host;
  std::unique_ptr<Node> node;

  explicit HandBuiltNode(std::function<void(NodeOptions&)> configured) : configure(std::move(configured)) {
    std::ofstream(dir.path() / "cluster.key") << "seams test";
    open();
  }
  explicit HandBuiltNode(AdmissionOptions admission) : HandBuiltNode([admission](NodeOptions& options) { options.admission = admission; }) {}
  ~HandBuiltNode() {
    sink->release();
    close();
  }
  void open() {
    const auto source = dannan::identity::KeySource::typed_passphrase(
        [] {
          const std::string phrase = "node";
          return dannan::identity::Secret(std::span(reinterpret_cast<const uint8_t*>(phrase.data()), phrase.size()));
        },
        dannan::identity::Argon2Costs{1024, 1, 1});
    if (!dannan::identity::KeyCustody::exists(dir.path() / "node")) (void)dannan::identity::KeyCustody::create(dir.path() / "node", source);
    dannan::NodeConfig config;
    config.state_path = dir.path() / "node";
    config.key = dannan::identity::load_cluster_key(dir.path() / "cluster.key");
    config.listen_host = "127.0.0.1";
    config.advertise_host = "127.0.0.1";
    config.port = 0;
    config.version = "dagda-test";
    config.custody = source;
    config.log = sink;
    host = std::make_unique<dannan::Node>(io.context, config);
    NodeOptions options;
    options.state = state();
    options.key = test::known_answer_key();
    // A disk mounted there: a node never makes a backend's directory.
    std::filesystem::create_directories(data());
    options.pool.backends.push_back({data(), 64ULL * 1024 * 1024, 0});
    options.maintenance.automatic = false;
    if (configure) configure(options);
    node = std::make_unique<Node>(host->context(), options);
  }
  void close() {
    if (node) node->stop();
    node.reset();
    host.reset();
  }
  std::filesystem::path state() const { return dir.path() / "state"; }
  std::filesystem::path data() const { return dir.path() / "data"; }
};

// Half of a run of small objects removed, so packs hold dead bytes beside live ones.
struct PackedObjects {
  std::vector<store::ObjectId> live;
  std::vector<store::ObjectId> dead;
  uint64_t live_bytes = 0;
};
PackedObjects dead_beside_live(store::StoragePool& data) {
  PackedObjects out;
  for (uint64_t n = 0; n < 128; ++n) {
    const auto bytes = test::pattern(16 * 1024 + n, static_cast<uint8_t>(n));
    const auto id = store::object_id(bytes);
    EXPECT_TRUE(data.put(id, bytes));
    if (n % 2) {
      out.live.push_back(id);
      out.live_bytes += bytes.size();
    } else {
      out.dead.push_back(id);
    }
  }
  (void)data.reassert_durable(out.dead.back());
  for (const auto& id : out.dead) EXPECT_TRUE(data.remove(id));
  return out;
}

void pack_options(NodeOptions& options) {
  options.pool.pack_threshold = 256 * 1024;
  options.pool.pack_target_size = 256 * 1024;
}

// Nothing was lost, and further runs of the stage reclaim what the stop left.
void expect_compaction_finishes(Node& node, const PackedObjects& objects, uint64_t used_before) {
  uint64_t used = node.data().used();
  for (int run = 0; run < 64; ++run) {
    auto turn = node.begin_local_stages();
    node.compact_packs(turn);
    EXPECT_TRUE(turn.stages.packs_ran);
    const auto now = node.data().used();
    if (run > 8 && now == used) break;
    used = now;
  }
  EXPECT_LT(used, used_before);
  // What is left is what lives, and the seals and headers around it.
  EXPECT_LT(used, objects.live_bytes + objects.live_bytes / 8);
  for (const auto& id : objects.live) EXPECT_TRUE(node.data().get(id).has_value());
  for (const auto& id : objects.dead) EXPECT_FALSE(node.data().has(id));
}

// Macha's stop inside compact_packs where it can fall: while the compaction waits for
// the accounting walk (local_store.cpp wait_for_accounting at 9a08846). The walk is
// held by the line it writes as it begins; the stop wakes the wait, comes out of the
// stage, and the next run, the walk done, finishes the work.
TEST(NodeSeams, AStopWhileCompactionWaitsForTheWalkComesThroughAndTheNextRunFinishes) {
  HandBuiltNode built(pack_options);
  const auto objects = dead_beside_live(built.node->data());
  const auto used = built.node->data().used();
  ASSERT_GT(used, objects.live_bytes);
  built.close();
  // No accounting to trust: the next open walks the disk.
  std::filesystem::remove(built.data() / ".macha.accounting");
  built.sink->hold("storage accounting scan begin path=" + built.data().string());
  built.open();
  ASSERT_TRUE(built.sink->entered());

  std::stop_source stop;
  LocalTurnOptions options;
  options.stop = stop.get_token();
  auto turn = built.node->begin_local_stages(options);
  auto compaction = std::async(std::launch::async, [&] { built.node->compact_packs(turn); });
  // Still in it: a stage that returned or threw here never waited for the walk.
  ASSERT_EQ(compaction.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  stop.request_stop();
  EXPECT_THROW(compaction.get(), contract::MaintenanceStopping);
  EXPECT_TRUE(turn.stages.packs_ran);

  built.sink->release();
  ASSERT_TRUE(test::wait_until([&] {
    auto next = built.node->begin_local_stages();
    built.node->compact_packs(next);
    return built.node->data().used() < used;
  }));
  expect_compaction_finishes(*built.node, objects, used);
}

// Macha's other place in compact_packs: between one backend and the next. The first
// backend's deferred compaction is what lands the stop. The second is not visited,
// the stop comes out of the stage, and the next runs finish both.
TEST(NodeSeams, AStopBetweenBackendsComesThroughAndTheNextRunFinishes) {
  if (::geteuid() == 0) GTEST_SKIP() << "running as root, which writes a pack directory whatever its permissions";
  std::atomic<int> deferred{0};
  std::stop_source stop;
  HandBuiltNode built([&](NodeOptions& options) {
    pack_options(options);
    std::filesystem::create_directories(options.state.parent_path() / "second");
    options.pool.backends.push_back({options.state.parent_path() / "second", 64ULL * 1024 * 1024, 0});
  });
  const auto second = built.dir.path() / "second";
  ASSERT_EQ(built.node->data().online_backends(), 2u);
  const auto objects = dead_beside_live(built.node->data());
  const auto used = built.node->data().used();
  ASSERT_TRUE(std::filesystem::exists(built.data() / "packs"));
  ASSERT_TRUE(std::filesystem::exists(second / "packs"));
  built.sink->tell("DATA pack compaction deferred", [&] {
    ++deferred;
    stop.request_stop();
  });
  const auto read_only = std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec;
  std::filesystem::permissions(built.data() / "packs", read_only);
  std::filesystem::permissions(second / "packs", read_only);

  LocalTurnOptions options;
  options.stop = stop.get_token();
  auto turn = built.node->begin_local_stages(options);
  EXPECT_THROW(built.node->compact_packs(turn), contract::MaintenanceStopping);
  std::filesystem::permissions(built.data() / "packs", std::filesystem::perms::owner_all);
  std::filesystem::permissions(second / "packs", std::filesystem::perms::owner_all);
  EXPECT_EQ(deferred.load(), 1);
  EXPECT_EQ(built.node->data().used(), used);

  expect_compaction_finishes(*built.node, objects, used);
}

// The claims' compaction takes no stop, here or in Macha (compact_if_needed(4096) at
// maintenance.cpp:1128): a stop landing inside it waits for its end. The stage then
// says so, and what it compacted stands: the next run has nothing left to do, and a
// reopened node holds every claim.
TEST(NodeSeams, AStopInsideTheClaimsCompactionWaitsForItsEndThenComesThrough) {
  if (::geteuid() == 0) GTEST_SKIP() << "running as root, which removes a checkpoint file whatever its directory's permissions";
  std::stop_source stop;
  HandBuiltNode built(nullptr);
  const dannan::identity::NodeId origin = [] {
    dannan::identity::NodeId out;
    out.bytes.fill(7);
    return out;
  }();
  std::vector<store::ObjectId> ids;
  for (uint64_t n = 0; n < 4100; ++n) {
    ids.push_back(store::object_id(test::numbered(n)));
    built.node->claims().retain(contract::RetentionClass::data, ids.back(), {origin, n + 1});
  }
  // The compaction's last act tidies files Macha's older layouts kept. One that cannot
  // be removed writes a line from inside it, and that lands the stop.
  const auto held = built.state() / "retention" / "checkpoints" / "held";
  std::filesystem::create_directories(held);
  std::ofstream(held / "file") << "x";
  std::filesystem::permissions(held, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
  built.sink->tell("cannot remove retention checkpoint files", [&] { stop.request_stop(); });

  LocalTurnOptions options;
  options.stop = stop.get_token();
  auto turn = built.node->begin_local_stages(options);
  EXPECT_THROW(built.node->retention_compact(turn), contract::MaintenanceStopping);
  std::filesystem::permissions(held, std::filesystem::perms::owner_all);
  std::filesystem::remove_all(held.parent_path());
  EXPECT_TRUE(stop.stop_requested());
  EXPECT_TRUE(turn.stages.retention_compacted);

  auto next = built.node->begin_local_stages();
  built.node->retention_compact(next);
  EXPECT_TRUE(next.stages.retention_ran);
  EXPECT_FALSE(next.stages.retention_compacted);
  built.close();
  built.open();
  for (const auto& id : ids) EXPECT_TRUE(built.node->claims().retained(contract::RetentionClass::data, id));
}

// Macha's dht.io_pressure_*: the figures reach the monitor, and work is judged by
// them.
TEST(NodeSeams, TheDevicePressureFiguresReachTheMonitor) {
  AdmissionOptions admission;
  admission.device_pressure.overhead = std::chrono::milliseconds(11);
  admission.device_pressure.per_mib = std::chrono::milliseconds(22);
  admission.device_pressure.slowdown_percent = 333;
  admission.device_pressure.release_percent = 44;
  admission.device_pressure.outlier_percent = 5555;
  HandBuiltNode built(admission);
  const auto& thresholds = built.node->data().service_monitor().thresholds();
  EXPECT_EQ(thresholds.overhead, std::chrono::milliseconds(11));
  EXPECT_EQ(thresholds.per_mib, std::chrono::milliseconds(22));
  EXPECT_EQ(thresholds.slowdown_percent, 333u);
  EXPECT_EQ(thresholds.release_percent, 44u);
  EXPECT_EQ(thresholds.outlier_percent, 5555u);
  built.node->data().service_monitor().note(std::chrono::seconds(60), 4096);
  EXPECT_TRUE(built.node->credit()->stats().device_pressured);
}

// Macha's zero: admission does not watch the device however slow it is
// (local_services.cpp:23-25).
TEST(NodeSeams, AZeroSlowdownTurnsTheDeviceGateOff) {
  AdmissionOptions admission;
  admission.device_pressure.slowdown_percent = 0;
  HandBuiltNode built(admission);
  built.node->data().service_monitor().note(std::chrono::seconds(60), 4096);
  EXPECT_FALSE(built.node->credit()->stats().device_pressured);
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
  // A turn of the steps, earning enough credit, and whether rebalance ran.
  bool rebalance_runs() {
    LocalTurnOptions options;
    options.when_no_time_passed = std::chrono::seconds(1);
    auto turn = dagda().begin_local_stages(options);
    dagda().local_rebalance(turn);
    return turn.stages.rebalance_ran;
  }
  // With credit to spare, a turn that does not run is one that rests.
  bool rebalance_rests() {
    return test::wait_until([&] { return !rebalance_runs(); });
  }

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

// Macha's rest ends on its four events and no others, so a busy cluster's adverts
// cost a Macha node no rebalance steps: a peer's storage figures, or a kind of event
// of some other hosted system's, leave it resting.
TEST_F(LocalStagesBed, OnlyMachasFourEventsEndRebalancesRest) {
  auto& events = node->context().events();
  ASSERT_TRUE(rebalance_rests());
  const auto before = dagda().maintenance_events();

  events.notify(dannan::status::NodeEvents::topology);
  EXPECT_FALSE(rebalance_runs());
  node->context().nodes().advertise("seams/advert", store::Bytes{1, 2, 3});
  EXPECT_FALSE(rebalance_runs());
  events.notify(events.kind("another-system"));
  EXPECT_FALSE(rebalance_runs());
  EXPECT_EQ(dagda().maintenance_events().total(), before.total());

  for (const auto kind : {events.kind("storage"), events.kind("metadata"), events.kind("claims"), dannan::status::NodeEvents::membership}) {
    SCOPED_TRACE(kind);
    events.notify(kind);
    EXPECT_TRUE(rebalance_runs());
    ASSERT_TRUE(rebalance_rests());
  }
  const auto after = dagda().maintenance_events();
  EXPECT_EQ(after.storage, before.storage + 1);
  EXPECT_EQ(after.metadata, before.metadata + 1);
  EXPECT_EQ(after.claims, before.claims + 1);
  EXPECT_EQ(after.topology, before.topology + 1);
}

// Dagda's own composite still ends its rest on any event, until its loop is 8c's.
TEST_F(LocalStagesBed, TheCompositesRestStillEndsOnAnyEvent) {
  const auto run = [&] {
    time.advance(std::chrono::seconds(1));
    return dagda().run_local_stages();
  };
  ASSERT_TRUE(test::wait_until([&] {
    const auto stages = run();
    return stages.rebalance_ran && stages.rebalance.complete && !stages.rebalance.bytes && !stages.rebalance.yielded && !run().rebalance_ran;
  }));
  node->context().events().notify(dannan::status::NodeEvents::topology);
  EXPECT_TRUE(run().rebalance_ran);
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
