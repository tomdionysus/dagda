//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Session S5, the exchange, on real multi-node clusters over the neutral owner. Each case
// names Macha's case at 9a08846 it comes from and follows it assertion for assertion; where
// one of Macha's assertions is about its filesystem, its transport's fault injection or its
// fixture, the comment says what stands in its place. A case keeps its writes sequential
// where Macha's could fork them: the forks are heads_reconcile_test.cpp's.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/heads/control_node_store.h"
#include "dagda/heads/convergence_demand.h"
#include "dagda/heads/exchange.h"
#include "dagda/heads/messages.h"
#include "dagda/node.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/heads_space.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda {
namespace {

using namespace std::chrono_literals;
using heads::Exchange;
using heads::HeadsNotReady;
using heads::Space;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::make_group;
using test::neutral_mutation;
using test::neutral_tree;
using test::NeutralCodec;
using test::NeutralEditing;
using test::NeutralHead;
using test::NeutralItem;
using test::NeutralRef;
using test::wait_until;

const std::string space_name = "test/neutral";

const NeutralHead& head_of(const heads::SpaceView& view) { return *NeutralCodec::as_head(view.state); }

Space& space_on(ClusterNode& node) {
  auto* found = node.dagda_node().spaces()->find(node.api().heads().id_of(space_name));
  EXPECT_NE(found, nullptr);
  return *found;
}

Exchange& exchange_on(ClusterNode& node) {
  auto* found = node.dagda_node().spaces()->exchange(node.api().heads().id_of(space_name));
  EXPECT_NE(found, nullptr);
  return *found;
}

heads::Replica& replica_on(ClusterNode& node) { return space_on(node).replica(); }

// What a node of a bootstrapped cluster runs with: joiners all, as Macha's mutual peers are.
ClusterNodeOptions joined(size_t write_copies = 2) {
  ClusterNodeOptions out;
  out.spaces = {test::founded_spec(space_name)};
  out.exchange.joiner = true;
  out.exchange.write_copies = write_copies;
  return out;
}

// The same with its maintenance thread running, on short intervals, so repair converges it
// as Macha's pass does.
ClusterNodeOptions live(size_t write_copies = 2) {
  auto out = joined(write_copies);
  out.maintenance.automatic = true;
  out.maintenance.interval = 25ms;
  out.maintenance.gc_interval = 1h;
  return out;
}

void write_key(ClusterNode& node, const std::string& key, uint64_t seed = 1) {
  (void)space_on(node).mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys[key] = make_group(seed); }));
}

// Macha's retry_while_not_ready: a node still forming says so, and a write waits for it.
bool writes(ClusterNode& node, const std::string& key, uint64_t seed = 1, std::chrono::milliseconds within = 15s) {
  return wait_until(
      [&] {
        try {
          write_key(node, key, seed);
          return true;
        } catch (const HeadsNotReady&) {
          return false;
        }
      },
      within);
}

std::unique_ptr<heads::TreeNodeStore> reader_on(ClusterNode& node) {
  return std::make_unique<heads::ControlTreeNodeStore>(heads::ControlTreeNodeStore::for_reading(node.control(), node.store()));
}

std::optional<NeutralItem> item_at(ClusterNode& node, const heads::SpaceView& view, const std::string& key) {
  const auto& root = head_of(view).root;
  if (!root) return std::nullopt;
  const auto reader = reader_on(node);
  return neutral_tree().lookup(*root, key, *reader);
}

// `key` reads back through the node's converged view.
bool holds(ClusterNode& node, const std::string& key) {
  try {
    return item_at(node, space_on(node).converged(), key).has_value();
  } catch (const std::exception&) {
    return false;
  }
}

bool sees(ClusterNode& node, const std::string& key, std::chrono::milliseconds within = 20s) {
  return wait_until([&] { return holds(node, key); }, within);
}

bool one_shared_head(ClusterNode& a, ClusterNode& b, std::chrono::milliseconds within = 30s) {
  return wait_until(
      [&] {
        const auto mine = replica_on(a).accepted_heads();
        const auto theirs = replica_on(b).accepted_heads();
        return mine.size() == 1 && theirs.size() == 1 && mine.front().hash == theirs.front().hash;
      },
      within);
}

// Holds every request of `type` on a node, in front of its handlers, until the gate opens:
// a peer that has stopped answering, as Macha's FaultyLinks stalls a route. Macha's holds the
// call on the sender's side; this holds it on the receiver's, which the sender cannot tell.
void stall(ClusterNode& node, dannan::net::MessageType type, dannan::test::Gate& gate) {
  node.interpose(
      [type, &gate](dannan::net::MessageType asked, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<dannan::net::Handled> {
        if (asked == type) gate.enter_and_wait();
        return std::nullopt;
      });
}

// ---- Founding ---------------------------------------------------------------

// Macha's test_joiner_cannot_form_genesis (RPC:2978). A pristine bootstrap node holds genesis
// only as local codec material, never advertised as authority.
TEST(HeadsFounding, AJoinerCannotFormGenesis) {
  Cluster cluster;
  auto& a = cluster.add("joiner", joined(1));
  a.start();
  EXPECT_TRUE(replica_on(a).accepted_heads().empty());
  bool rejected = false;
  try {
    (void)space_on(a).converged();
  } catch (const std::exception& error) {
    rejected = std::string(error.what()).find("waiting for bootstrap peer") != std::string::npos;
  }
  EXPECT_TRUE(rejected);
  // And what Macha's filesystem read comes to, in the contract's evidence.
  const auto read = a.api().heads().read(a.api().heads().id_of(space_name), HeadRead::converged, 10s);
  EXPECT_FALSE(read.finished);
  EXPECT_NE(read.why.find("waiting for bootstrap peer"), std::string::npos) << read.why;
}

// Macha's test_bootstrap_joiner_requires_complete_checkpoint_survey (RPC:2999). An unreachable
// peer in active membership: both surveys fail, and the joiner must not form an empty space.
// Macha's hunt for a phantom id that ranks this node first is placement, gone from the path
// (it is the case's old shape; `discover_or_form` reads no placement at 9a08846): here the
// peer is real, stopped, and still inside its dead_after.
TEST(HeadsFounding, AJoinerRequiresACompleteCheckpointSurvey) {
  Cluster cluster(60s);
  auto& a = cluster.add("a", joined(1));
  auto& b = cluster.add("b", joined(1));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() == 2; }));
  b.stop();
  ASSERT_EQ(a.context().nodes().active().size(), 2u);

  bool rejected = false;
  try {
    (void)space_on(a).converged();
  } catch (const std::exception& error) {
    rejected = std::string(error.what()).find("bootstrap checkpoint survey") != std::string::npos;
  }
  EXPECT_TRUE(rejected);
  EXPECT_LE(replica_on(a).current().generation, 1u);
  EXPECT_LE(replica_on(a).committed().generation, 1u);
}

// C16's first half. maintenance.cpp:467-472: exactly one deterministic founder; followers stay
// event driven. HeadsFounding.FoundersConfiguredDifferentlyMerge is the other half of C16.
TEST(HeadsFounding, TheLowestActiveNodeFoundsAndFollowersWait) {
  dannan::test::SteppedTime clock_a;
  dannan::test::SteppedTime clock_b;
  Cluster cluster;
  auto options_a = joined(2);
  options_a.maintenance.time = &clock_a;
  auto options_b = joined(2);
  options_b.maintenance.time = &clock_b;
  auto& a = cluster.add("a", options_a);
  auto& b = cluster.add("b", options_b);
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() == 2 && b.context().nodes().active().size() == 2; }));

  // While the formation settles nobody founds.
  for (auto* node : {&a, &b}) {
    const auto turns = node->dagda_node().metadata_repair();
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_TRUE(turns.front().waited);
    EXPECT_FALSE(turns.front().ran);
    EXPECT_LE(replica_on(*node).committed_generation(), 1u);
  }
  clock_a.advance(cluster.dead_after() + 1ms);
  clock_b.advance(cluster.dead_after() + 1ms);

  auto& founder = a.id() < b.id() ? a : b;
  auto& follower = a.id() < b.id() ? b : a;
  // The follower goes first and still waits.
  const auto waiting = follower.dagda_node().metadata_repair();
  EXPECT_TRUE(waiting.front().waited);
  EXPECT_FALSE(waiting.front().ran);
  EXPECT_LE(replica_on(follower).committed_generation(), 1u);

  const auto founded = founder.dagda_node().metadata_repair();
  EXPECT_TRUE(founded.front().ran);
  EXPECT_FALSE(founded.front().failed) << founded.front().why;
  EXPECT_EQ(replica_on(founder).committed_generation(), 2u);
  // A joiner never makes generation 1 its own authority; the record it adopts is the founder's.
  EXPECT_TRUE(wait_until([&] {
    (void)follower.dagda_node().metadata_repair();
    return replica_on(follower).committed_generation() == 2u;
  }));
  EXPECT_EQ(replica_on(follower).committed().hash, replica_on(founder).committed().hash);
}

// Macha's test_a_new_namespace_is_a_tree_from_its_first_commit (SM:2562), its neutral form
// (5.3). `entries.empty()` is Macha's map form, which the neutral owner does not have; the
// record's size stands for it: the keys are not in the payload. Macha's stays Macha's.
TEST(HeadsFounding, ASpaceIsATreeFromItsFirstCommit) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {test::founded_spec(space_name)};
  options.exchange.write_copies = 1;
  auto& a = cluster.add("a", options);
  a.start();
  const auto founded = space_on(a).converged();
  ASSERT_TRUE(head_of(founded).root.has_value());
  EXPECT_EQ(founded.generation, 2u);
  EXPECT_TRUE(item_at(a, founded, "a").has_value());
  const auto founded_payload = replica_on(a).committed().payload.size();

  write_key(a, "b");
  const auto written = space_on(a).converged();
  ASSERT_TRUE(head_of(written).root.has_value());
  EXPECT_NE(*head_of(written).root, *head_of(founded).root);
  EXPECT_TRUE(item_at(a, written, "b").has_value());
  // One clock entry more, and no key.
  EXPECT_LT(replica_on(a).committed().payload.size(), founded_payload + 64);
}

// ---- Convergence ------------------------------------------------------------

// Macha's test_convergence_demand_coalesces_burst_and_keeps_same_generation_event
// (test_invariants.cpp:113), every assertion.
TEST(HeadsDemand, CoalescesABurstAndKeepsASameGenerationEvent) {
  heads::ConvergenceDemand demand;
  EXPECT_FALSE(demand.pending());

  demand.request(10);
  const auto first = demand.begin();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->generation, 10u);

  // Notices arriving during a run advance the high-water mark without scheduling a run per
  // intermediate generation.
  for (uint64_t generation = 11; generation <= 210; ++generation) demand.request(generation);
  demand.request(210);  // same-generation sibling or topology change

  const auto during = demand.diagnostics();
  EXPECT_EQ(during.events_received, 202u);
  EXPECT_EQ(during.runs_scheduled, 1u);
  EXPECT_EQ(during.latest_generation, 210u);
  EXPECT_TRUE(demand.complete(*first));

  const auto second = demand.begin();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->generation, 210u);
  EXPECT_FALSE(demand.complete(*second));

  const auto settled = demand.diagnostics();
  EXPECT_EQ(settled.runs_scheduled, 2u);
  EXPECT_EQ(settled.runs_completed, 2u);
  EXPECT_EQ(settled.completed_epoch, settled.requested_epoch);
  EXPECT_FALSE(settled.scheduled);

  // A sibling notice at an already processed generation still schedules a run: generation
  // alone is not the identity.
  demand.request(210);
  const auto sibling = demand.begin();
  ASSERT_TRUE(sibling.has_value());
  EXPECT_GT(sibling->epoch, second->epoch);
  EXPECT_FALSE(demand.complete(*sibling));
  EXPECT_EQ(demand.diagnostics().runs_scheduled, 3u);
}

// Macha's test_service_metadata_repair_coalesces_real_generation_burst (RPC:3212).
TEST(HeadsExchange, RepairCoalescesARealGenerationBurst) {
  dannan::test::Gate repair_gate;
  std::atomic_bool gate_repair{false};
  std::atomic_bool gate_once{false};
  Cluster cluster;
  dannan::test::GateOpener open_on_exit(repair_gate);
  auto options_a = live(1);
  options_a.maintenance.stage = [&](std::string_view stage) {
    if (stage == "metadata-repair-begin" && gate_repair.load(std::memory_order_acquire) && !gate_once.exchange(true, std::memory_order_acq_rel))
      repair_gate.enter_and_wait();
  };
  auto& a = cluster.add("a", options_a);
  auto& b = cluster.add("b", live(1));
  cluster.start_all();
  ASSERT_TRUE(writes(a, "first"));
  // Equal generation is not convergence: one shared head, so later reconciliation does not
  // pollute the burst's counters.
  ASSERT_TRUE(wait_until(
      [&] {
        const auto d1 = exchange_on(a).demand();
        const auto d2 = exchange_on(b).demand();
        return replica_on(a).committed_generation() > 1 && replica_on(a).committed().hash == replica_on(b).committed().hash &&
               replica_on(a).accepted_heads().size() == 1 && replica_on(b).accepted_heads().size() == 1 && !d1.scheduled &&
               d1.runs_scheduled == d1.runs_completed && !d2.scheduled && d2.runs_scheduled == d2.runs_completed;
      },
      15s));

  const auto before = exchange_on(a).demand();
  const auto baseline_generation = replica_on(b).committed_generation();
  const auto announcements_before = exchange_on(b).announcements();
  gate_repair.store(true, std::memory_order_release);

  write_key(b, "coalesced-0");
  ASSERT_TRUE(repair_gate.wait_for_entries(1, 5s));
  const auto claimed = exchange_on(a).demand();
  EXPECT_EQ(claimed.runs_scheduled, before.runs_scheduled + 1);
  EXPECT_EQ(claimed.runs_completed, before.runs_completed);
  const auto metadata_events = [&] {
    return a.context().events().count(a.context().events().kind("metadata")) + a.context().events().count(dannan::status::NodeEvents::membership);
  };
  const auto events_claimed = metadata_events();

  constexpr size_t burst = 32;
  for (size_t i = 1; i <= burst; ++i) write_key(b, "coalesced-" + std::to_string(i));
  const auto final_generation = replica_on(b).committed_generation();
  EXPECT_EQ(final_generation, baseline_generation + burst + 1);
  ASSERT_TRUE(wait_until([&] { return exchange_on(a).known_generation() >= final_generation; }, 5s));
  EXPECT_EQ(exchange_on(b).announcements(), announcements_before + burst + 1);

  // While the run is held the node counts the burst; the pass turns it into demand when it
  // next looks, so nothing more is scheduled meanwhile.
  EXPECT_GT(metadata_events(), events_claimed);
  const auto gated = exchange_on(a).demand();
  EXPECT_EQ(gated.runs_scheduled, before.runs_scheduled + 1);
  EXPECT_EQ(gated.runs_completed, before.runs_completed);

  // Macha's opens here with the replicator perhaps still delivering, so a straggler can make
  // a third run; this waits for it first, and the bound holds with no race.
  ASSERT_TRUE(exchange_on(b).wait_replicated(10s));
  repair_gate.open();
  ASSERT_TRUE(wait_until(
      [&] {
        const auto diagnostics = exchange_on(a).demand();
        return !diagnostics.scheduled && diagnostics.runs_completed == before.runs_completed + 2 && replica_on(a).committed_generation() == final_generation;
      },
      10s));

  const auto settled = exchange_on(a).demand();
  EXPECT_EQ(settled.runs_scheduled, before.runs_scheduled + 2);
  EXPECT_EQ(settled.runs_completed, before.runs_completed + 2);
  EXPECT_EQ(settled.completed_epoch, settled.requested_epoch);
  EXPECT_GE(settled.latest_generation, final_generation);
  EXPECT_FALSE(settled.scheduled);
  EXPECT_TRUE(holds(a, "coalesced-32"));
}

// Macha's test_failed_convergence_retries_once_per_burst_of_events (RPC:3328). Macha's runs five
// real seconds of 200 ms sleeps against real time; here the pass's clock is stepped by the same
// amounts, so the settle and the backoff are decided by the time the case says, and each step
// waits for the node to have seen the change rather than sleeping.
TEST(HeadsExchange, FailedConvergenceRetriesOncePerBurstOfEvents) {
  dannan::test::SteppedTime clock;
  std::atomic_bool failing{false};
  std::atomic_int failed{0};
  Cluster cluster;
  auto options_a = live(1);
  options_a.maintenance.time = &clock;
  // A failed run's own backoff, longer than the whole case.
  ASSERT_GE(options_a.exchange.retry_backoff, 30s);
  options_a.exchange.convergence_settle = 1s;
  options_a.exchange.convergence_max_delay = 20s;
  options_a.maintenance.stage = [&](std::string_view stage) {
    if (stage == "metadata-repair-begin" && failing.load()) {
      failed.fetch_add(1);
      throw std::runtime_error("injected convergence failure");
    }
  };
  auto& a = cluster.add("a", options_a);
  auto& b = cluster.add("b", live(1));
  cluster.start_all();
  ASSERT_TRUE(writes(b, "first"));
  ASSERT_TRUE(wait_until(
      [&] {
        clock.advance(10ms);
        const auto d1 = exchange_on(a).demand();
        return a.context().nodes().active().size() >= 2 && replica_on(a).committed_generation() > 1 &&
               replica_on(a).committed().hash == replica_on(b).committed().hash && !d1.scheduled && d1.runs_scheduled == d1.runs_completed;
      },
      15s));

  // A stream of the peer's changes while runs fail: the first change runs and fails; the rest
  // are one burst, retried after it, whatever else wakes the pass meanwhile.
  failing.store(true);
  for (int i = 0; i < 25; ++i) {
    const auto events = exchange_on(a).demand().events_received;
    write_key(b, "retry-burst-" + std::to_string(i));
    // The pass has absorbed this change.
    ASSERT_TRUE(wait_until([&] { return exchange_on(a).demand().events_received > events; }, 10s));
    clock.advance(200ms);
  }
  EXPECT_EQ(failed.load(), 1);

  // After the last change the retry runs once the settle has passed, not when the 30 s
  // backoff ends.
  failing.store(false);
  ASSERT_TRUE(exchange_on(b).wait_replicated(10s));
  const auto head = replica_on(b).committed();
  clock.advance(1s + 1ms);
  EXPECT_TRUE(wait_until(
      [&] {
        const auto d1 = exchange_on(a).demand();
        return !d1.scheduled && replica_on(a).committed().hash == head.hash;
      },
      10s));
  EXPECT_EQ(failed.load(), 1);
}

// Macha's test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs (RPC:4037).
TEST(HeadsExchange, ALaggingThirdReplicaCatchesUpALinearBurstInBoundedRuns) {
  Cluster cluster;
  auto& a = cluster.add("a", live(2));
  auto& b = cluster.add("b", live(2));
  auto& c = cluster.add("c", live(2));
  cluster.start_all();
  ASSERT_TRUE(wait_until(
      [&] { return a.context().nodes().active().size() == 3 && b.context().nodes().active().size() == 3 && c.context().nodes().active().size() == 3; }));

  ASSERT_TRUE(writes(a, "lagging-base"));
  ASSERT_TRUE(sees(c, "lagging-base", 10s));
  const auto base = replica_on(c).committed();

  c.stop();
  ASSERT_TRUE(cluster.dead_to(a, c.id()));
  ASSERT_TRUE(cluster.dead_to(b, c.id()));

  constexpr size_t burst = 64;
  for (size_t index = 0; index < burst; ++index) write_key(a, "lagging-burst-" + std::to_string(index));
  const auto final_head = replica_on(a).committed();
  EXPECT_EQ(final_head.generation, base.generation + burst);
  ASSERT_TRUE(wait_until([&] { return replica_on(b).committed().hash == final_head.hash; }, 10s));

  c.start();
  ASSERT_TRUE(wait_until(
      [&] {
        const auto diagnostics = exchange_on(c).demand();
        return replica_on(c).committed().hash == final_head.hash && holds(c, "lagging-burst-63") && !diagnostics.scheduled &&
               diagnostics.runs_scheduled == diagnostics.runs_completed;
      },
      15s));

  const auto diagnostics = exchange_on(c).demand();
  EXPECT_LE(diagnostics.runs_scheduled, 4u);
  EXPECT_LE(diagnostics.runs_completed, 4u);
  EXPECT_LT(diagnostics.runs_completed, burst);
  const auto held = replica_on(c).accepted_heads();
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held.front().hash, final_head.hash);
  EXPECT_TRUE(replica_on(c).history_contains(final_head.hash));

  EXPECT_LE(std::max(exchange_on(a).transfers().peak_in_flight, exchange_on(b).transfers().peak_in_flight), 8u);
}

// What H15's window of 8 asks for, which Macha's only bounds from above: a history longer than
// the window is pushed through exactly the window.
TEST(HeadsExchange, AHistoryPushFillsItsWindowOfEightAndNoMore) {
  Cluster cluster;
  auto& a = cluster.add("a", joined(1));
  auto& b = cluster.add("b", joined(1));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() == 2; }));
  ASSERT_TRUE(writes(a, "base"));
  ASSERT_TRUE(sees(b, "base"));
  // No replicator's offers: a peer away, and back only for the repair below.
  b.stop();
  ASSERT_TRUE(cluster.dead_to(a, b.id()));
  for (int i = 0; i < 20; ++i) write_key(a, "chain-" + std::to_string(i));
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));
  const auto before = exchange_on(a).transfers();
  b.start();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() == 2; }));
  exchange_on(a).repair_once();
  EXPECT_EQ(replica_on(b).committed().hash, replica_on(a).committed().hash);
  const auto after = exchange_on(a).transfers();
  EXPECT_EQ(after.peak_in_flight, 8u);
  EXPECT_GE(after.entries_submitted - before.entries_submitted, 20u);
  EXPECT_GT(after.transfers, before.transfers);
}

// Macha's test_a_file_touch_commits_while_a_holder_is_away (RPC:4214). chmod on a file becomes an
// upsert of a key naming the same object: a change that carries it.
TEST(HeadsExchange, ATouchCommitsWhileAHolderIsAway) {
  Cluster cluster;
  auto with_store = [] {
    auto out = live(2);
    out.store.replication = 3;
    out.store.write_copies = 3;
    return out;
  };
  auto& a = cluster.add("a", with_store());
  auto& b = cluster.add("b", with_store());
  auto& c = cluster.add("c", with_store());
  cluster.start_all();
  // The write needs three extent-hosting members; membership alone arrives before them.
  ASSERT_TRUE(cluster.converged());

  ASSERT_TRUE(writes(a, "seeded"));
  const auto input = test::pattern(128 * 1024);
  const auto extent = store::object_id(input);
  store::WriteOptions write;
  write.level = store::Durability::durable;
  ASSERT_TRUE(a.store().write(extent, input, write).met);
  const auto naming = [&](uint32_t mode) {
    NeutralItem item;
    item.kind = 2;
    item.tag = mode;
    NeutralRef ref;
    ref.span = static_cast<uint32_t>(input.size());
    ref.id = extent;
    item.refs.push_back(ref);
    return item;
  };
  (void)space_on(a).mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["retained.bin"] = naming(0644); }));
  ASSERT_TRUE(wait_until([&] { return a.data().has(extent) && b.data().has(extent) && c.data().has(extent); }));
  // The accepted reference installed a DATA claim here at once, and on the holders present
  // once the commit reached them.
  EXPECT_TRUE(a.claims().retained(contract::RetentionClass::data, extent));
  ASSERT_TRUE(exchange_on(a).wait_replicated(20s));
  EXPECT_TRUE(b.claims().retained(contract::RetentionClass::data, extent));
  EXPECT_TRUE(c.claims().retained(contract::RetentionClass::data, extent));

  const auto before = replica_on(a).committed();
  c.stop();
  ASSERT_TRUE(cluster.dead_to(a, c.id()));
  ASSERT_TRUE(cluster.dead_to(b, c.id()));

  // A holder is away: the touch still commits, with a fresh claim on the holders present.
  (void)space_on(a).mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["retained.bin"] = naming(0600); }));
  EXPECT_EQ(item_at(a, space_on(a).converged(), "retained.bin")->tag, 0600u);
  EXPECT_NE(replica_on(a).committed().hash, before.hash);
  const auto dot = [&](ClusterNode& node) {
    const auto claims = node.claims().claims(contract::RetentionClass::data, extent);
    const auto found = claims.adds.find(a.id());
    return found == claims.adds.end() ? uint64_t{0} : found->second;
  };
  const auto touched = dot(a);
  EXPECT_GT(touched, 0u);
  ASSERT_TRUE(exchange_on(a).wait_replicated(20s));
  EXPECT_EQ(dot(b), touched);

  // The returning holder learns the change from the head.
  c.start();
  EXPECT_TRUE(wait_until(
      [&] {
        try {
          const auto item = item_at(c, space_on(c).converged(), "retained.bin");
          return item && item->tag == 0600u;
        } catch (const std::exception&) {
          return false;
        }
      },
      30s));
}

// Macha's test_commit_replicas_ordered_local_then_nearest (RPC:4317), every assertion.
TEST(HeadsExchange, CommitReplicasAreOrderedLocalThenNearest) {
  dannan::membership::NodeRecord local, lan, wan, unknown;
  local.id.bytes.fill(0x50);
  lan.id.bytes.fill(0x70);
  wan.id.bytes.fill(0x10);  // the lowest id: first under an ordering by id
  unknown.id.bytes.fill(0x90);
  local.host = "local";
  lan.host = "lan";
  wan.host = "wan";
  unknown.host = "unknown";
  const auto latency = [&](const heads::NodeId& id) -> std::optional<std::chrono::milliseconds> {
    if (id == lan.id) return 2ms;
    if (id == wan.id) return 120ms;
    return std::nullopt;
  };
  const auto ordered = heads::order_commit_replicas({wan, unknown, lan, local}, local.id, latency);
  ASSERT_EQ(ordered.size(), 4u);
  EXPECT_EQ(ordered[0].host, "local");
  EXPECT_EQ(ordered[1].host, "lan");
  EXPECT_EQ(ordered[2].host, "wan");
  EXPECT_EQ(ordered[3].host, "unknown");
  // No measurements: local first, the rest in their given order.
  const auto cold = heads::order_commit_replicas({wan, lan, local}, local.id, {});
  EXPECT_EQ(cold[0].host, "local");
  EXPECT_EQ(cold[1].host, "wan");
  EXPECT_EQ(cold[2].host, "lan");
}

// Macha's test_nodes_seeking_different_copy_counts_share_one_namespace (RPC:3060). Macha's two
// mkdirs may fork, and a merge then joins them; these wait for each other, and
// HeadsReconcile.NodesSeekingDifferentCopyCountsForkAndMerge is the fork.
TEST(HeadsExchange, NodesSeekingDifferentCopyCountsShareOneSpace) {
  Cluster cluster;
  auto& a = cluster.add("a", live(1));
  auto& b = cluster.add("b", live(2));
  ASSERT_NO_THROW(cluster.start_all());
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "from-one"));
  EXPECT_TRUE(sees(b, "from-one"));
  ASSERT_TRUE(writes(b, "from-two"));
  EXPECT_TRUE(sees(a, "from-two"));

  auto& c = cluster.add("c", live(3));
  c.start();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 3; }));
  EXPECT_TRUE(sees(c, "from-one"));
  ASSERT_TRUE(writes(c, "from-three"));
  EXPECT_TRUE(sees(a, "from-three"));
  EXPECT_TRUE(sees(b, "from-three"));
}

// M6's re-ask and H28: a peer's write-copies advert changing marks membership changed, which is
// what Dannan's own membership event leaves out, and fails nothing.
TEST(HeadsExchange, AWriteCopiesAdvertChangeIsAMembershipEvent) {
  Cluster cluster;
  auto& a = cluster.add("a", joined(1));
  auto& b = cluster.add("b", joined(2));
  cluster.start_all();
  const auto key = "dagda/heads/write-copies/" + dannan::identity::hex(a.api().heads().id_of(space_name).bytes);
  ASSERT_TRUE(wait_until([&] {
    for (const auto& record : a.context().nodes().active())
      if (record.id == b.id() && record.advert_u64(key) == 2u) return true;
    return false;
  }));
  const auto& events = a.context().events();
  const auto before = events.count(dannan::status::NodeEvents::membership);
  b.context().nodes().advertise(key, dannan::membership::u64_advert(3));
  EXPECT_TRUE(wait_until([&] { return events.count(dannan::status::NodeEvents::membership) > before; }));
  ASSERT_TRUE(writes(a, "still-writes"));
  EXPECT_TRUE(sees(b, "still-writes"));
}

// Macha's test_metadata_commits_with_whichever_nodes_remain (RPC:5148), over Macha's DurableTrio
// (2, 1): seeking two copies, and asking no node to hold any.
TEST(HeadsExchange, CommitsWithWhicheverNodesRemain) {
  Cluster cluster;
  auto& a = cluster.add("a", live(2));
  auto& b = cluster.add("b", live(2));
  auto& c = cluster.add("c", live(2));
  cluster.start_all();
  ASSERT_TRUE(wait_until(
      [&] { return a.context().nodes().active().size() == 3 && b.context().nodes().active().size() == 3 && c.context().nodes().active().size() == 3; }));
  ASSERT_TRUE(writes(a, "media"));
  ASSERT_TRUE(sees(b, "media"));
  c.stop();
  write_key(b, "after-one-loss");
  EXPECT_TRUE(holds(b, "after-one-loss"));
  EXPECT_TRUE(sees(a, "after-one-loss"));
  b.stop();
  write_key(a, "alone");
  EXPECT_TRUE(holds(a, "alone"));
}

// Macha's test_metadata_repair_stalled_on_a_silent_peer_does_not_block_local_writes (RPC:5588).
TEST(HeadsExchange, RepairStalledOnASilentPeerDoesNotBlockLocalWrites) {
  dannan::test::Gate silent;
  Cluster cluster;
  dannan::test::GateOpener open_on_exit(silent);
  auto& a = cluster.add("a", joined(1));
  auto& b = cluster.add("b", joined(1));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  // Past the virgin generation, so repair takes the survey and fan-out path.
  ASSERT_TRUE(writes(a, "warm"));
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));

  stall(b, a.options().exchange.numbers.has_history_entry, silent);
  std::atomic_bool repair_done{false};
  std::jthread repair([&] {
    try {
      exchange_on(a).repair_once();
    } catch (const std::exception&) {
      // A pass abandoned on the stalled peer is fine; it retries.
    }
    repair_done = true;
  });
  // Repair is now inside the fan-out, holding the probe to the silent peer.
  ASSERT_TRUE(silent.wait_for_entries(1, 15s));
  EXPECT_FALSE(repair_done.load());

  const auto started = std::chrono::steady_clock::now();
  write_key(a, "during-stall");
  const auto elapsed = std::chrono::steady_clock::now() - started;
  // Well inside the 30 s deadline on the call.
  EXPECT_LT(elapsed, 3s);
  EXPECT_FALSE(repair_done.load());

  silent.open();
  ASSERT_TRUE(wait_until([&] { return repair_done.load(); }, 60s));
  repair.join();

  bool converged = false;
  try {
    exchange_on(a).repair_once();
    converged = true;
  } catch (const std::exception&) {
  }
  EXPECT_TRUE(converged);
}

// Macha's port (`call_deadline.hpp`, Tom 2026-11-11): a metadata call names five minutes,
// and one waited on synchronously ends sooner when it makes no progress for the window.
// Repair's probe to a silent peer ends at the window, not at the 30 s it was.
TEST(HeadsExchange, AProbeToASilentPeerEndsAtTheNoProgressWindow) {
  dannan::test::SteppedTime time;
  dannan::test::Gate silent;
  Cluster cluster(std::chrono::hours(24));
  dannan::test::GateOpener open_on_exit(silent);
  auto stepped = joined(1);
  stepped.time = &time;
  stepped.exchange.call_no_progress = 60s;
  auto& a = cluster.add("a", stepped);
  auto& b = cluster.add("b", joined(1));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "warm"));
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));

  stall(b, a.options().exchange.numbers.has_history_entry, silent);
  auto repaired = std::async(std::launch::async, [&] {
    try {
      exchange_on(a).repair_once();
    } catch (const std::exception&) {
      // A pass abandoned on the silent peer is fine; it retries.
    }
  });
  // Opens the gate before the future waits on its thread, so a failure ends the test.
  dannan::test::GateOpener release(silent);
  ASSERT_TRUE(silent.wait_for_entries(1, 15s));
  // Each probe the pass sends the peer is held, so the first one ending shows as the
  // pass going on to the next, or ending.
  const auto first_ended = [&](std::chrono::seconds within) {
    return silent.wait_for_entries(2, within) || repaired.wait_for(0s) == std::future_status::ready;
  };
  time.advance(31s);
  EXPECT_FALSE(first_ended(2s));
  time.advance(30s);
  EXPECT_TRUE(first_ended(30s));
}

// Macha's test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim (RPC:5660). A
// catalogue root in Macha's is the head's tree here: what a commit names and claims. wait_replicated
// returning with the peer's claim unanswered is the store's own deadline on the claim.
TEST(HeadsExchange, ACommitIsClaimedHereWhenThePeerNeverAnswersTheClaim) {
  dannan::test::Gate silent;
  Cluster cluster;
  dannan::test::GateOpener open_on_exit(silent);
  auto options = joined(2);
  options.store.control_call = 500ms;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "established"));
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));

  stall(b, store::message::retain_objects, silent);
  write_key(a, "claimed");
  // The commit is claimed here before the caller is answered; the peer's claim is the
  // replicator's, and it gives up on a peer that never answers.
  const auto alone = head_of(space_on(a).local()).root;
  ASSERT_TRUE(alone.has_value());
  EXPECT_TRUE(a.claims().retained(contract::RetentionClass::control, *alone));
  ASSERT_TRUE(exchange_on(a).wait_replicated(30s));
  EXPECT_GE(silent.entered(), 1u);
  EXPECT_FALSE(b.claims().retained(contract::RetentionClass::control, *alone));

  silent.open();
  b.interpose({});
  write_key(a, "claimed-twice");
  const auto root = head_of(space_on(a).local()).root;
  ASSERT_TRUE(root.has_value());
  EXPECT_NE(*root, *alone);
  EXPECT_TRUE(a.claims().retained(contract::RetentionClass::control, *root));
  ASSERT_TRUE(exchange_on(a).wait_replicated(30s));
  EXPECT_TRUE(b.claims().retained(contract::RetentionClass::control, *root));
}

// Macha's test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again (RPC:5758).
TEST(HeadsExchange, APeerThatStallsOnACommitIsNotWaitedForAgain) {
  dannan::test::Gate silent;
  Cluster cluster(1500ms);
  dannan::test::GateOpener open_on_exit(silent);
  auto options = joined(2);
  options.exchange.write_stall = 150ms;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "established"));
  ASSERT_TRUE(sees(b, "established"));
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));

  std::atomic_size_t asked{0};
  const auto accept_commit = a.options().exchange.numbers.accept_commit;
  b.interpose([&](dannan::net::MessageType type, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<dannan::net::Handled> {
    if (type == accept_commit) {
      asked.fetch_add(1);
      silent.enter_and_wait();
    }
    return std::nullopt;
  });
  write_key(a, "while-stalled");
  // Answered while the peer's call has not returned.
  EXPECT_TRUE(holds(a, "while-stalled"));
  ASSERT_TRUE(exchange_on(a).wait_replicated(5s));
  const auto stalled_calls = asked.load();
  EXPECT_GE(stalled_calls, 1u);
  EXPECT_TRUE(exchange_on(a).stalled(b.id()));
  write_key(a, "not-asked");
  EXPECT_TRUE(holds(a, "not-asked"));
  ASSERT_TRUE(exchange_on(a).wait_replicated(5s));
  EXPECT_EQ(asked.load(), stalled_calls);

  silent.open();
  EXPECT_TRUE(sees(b, "while-stalled"));
  EXPECT_TRUE(sees(b, "not-asked"));
}

// Macha's test_a_node_truncates_its_history_with_its_peer_away_and_the_peer_converges (RPC:6293).
TEST(HeadsExchange, ANodeTruncatesItsHistoryWithItsPeerAwayAndThePeerConverges) {
  Cluster cluster;
  auto& a = cluster.add("a", live(2));
  auto& b = cluster.add("b", live(2));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "a"));
  ASSERT_TRUE(sees(b, "a"));
  ASSERT_TRUE(writes(b, "b"));
  ASSERT_TRUE(sees(a, "b"));
  ASSERT_TRUE(one_shared_head(a, b));
  const auto before = replica_on(a).accepted_heads().front().hash;
  const auto local_key = std::make_shared<const domain::StorageKey>(b.node_local_key());

  b.stop();

  // Two commits the peer never sees, so that no record it holds is the previous of the one this
  // node keeps.
  auto& r1 = replica_on(a);
  ASSERT_TRUE(writes(a, "c"));
  ASSERT_TRUE(writes(a, "d"));
  ASSERT_EQ(r1.accepted_heads().size(), 1u);
  ASSERT_GT(r1.diagnostics().history_records, 1u);
  exchange_on(a).truncate_history(1, 1);
  EXPECT_EQ(r1.diagnostics().history_records, 1u);
  EXPECT_FALSE(r1.history_contains(before));
  for (const auto* key : {"a", "b", "c", "d"}) EXPECT_TRUE(holds(a, key)) << key;

  ASSERT_TRUE(writes(a, "e"));
  EXPECT_EQ(r1.diagnostics().history_records, 2u);
  const auto ahead = r1.committed().hash;

  {
    // The stopped node's state, as it stands: its head is the one it left with.
    heads::ReplicaOptions opened;
    opened.directory = heads::space_directory(b.state(), a.api().heads().id_of(space_name));
    opened.content_key = test::known_answer_key();
    opened.local_key = local_key;
    opened.magics = test::neutral_magics();
    opened.aad = heads::replica_aad(a.dagda_node().options().domain.id, a.api().heads().id_of(space_name));
    opened.codec = std::make_shared<NeutralCodec>();
    opened.accept_pristine_genesis_authority = false;
    opened.logger = test::quiet_logger();
    const heads::Replica stopped(std::move(opened));
    const auto held = stopped.accepted_heads();
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(held.front().hash, before);
  }
  b.start();
  ASSERT_TRUE(sees(b, "e"));
  ASSERT_TRUE(one_shared_head(a, b));
  // The peer took this node's head as it stood: nothing was merged.
  EXPECT_EQ(replica_on(b).accepted_heads().front().hash, ahead);
  for (auto* node : {&a, &b})
    for (const auto* key : {"a", "b", "c", "d", "e"}) EXPECT_TRUE(holds(*node, key)) << node->name() << " " << key;

  ASSERT_TRUE(writes(b, "f"));
  ASSERT_TRUE(sees(a, "f"));
  ASSERT_TRUE(one_shared_head(a, b));
}

// Macha's test_unreconstructable_accepted_head_is_repaired_live_from_a_peer (RPC:6448). A head the
// local replica cannot replay is repaired from a peer while the node runs.
TEST(HeadsExchange, AnUnreconstructableHeadIsRepairedLiveFromAPeer) {
  Cluster cluster;
  auto& a = cluster.add("a", joined(2));
  auto& b = cluster.add("b", joined(2));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "a"));
  ASSERT_TRUE(sees(b, "a"));

  const auto certificates = replica_on(b).accepted_head_certificates();
  ASSERT_EQ(certificates.size(), 1u);
  const auto head = certificates.front().hash;
  EXPECT_TRUE(replica_on(a).history_contains(head));

  // b loses its checkpoint, journal and history while stopped; its acceptance certificate
  // survives. It comes back holding a head it cannot replay, which it keeps, flags and excludes
  // from reads.
  const auto directory = heads::space_directory(b.state(), a.api().heads().id_of(space_name));
  b.stop();
  for (const auto* name : {"checkpoint.meta", "journal.log", "history.log"}) std::filesystem::remove(directory / name);
  b.start();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_EQ(replica_on(b).unreconstructable_heads(), std::vector<heads::Hash256>{head});

  // The flag's 30 s cooldown means only repair can clear it here.
  auto& replica = replica_on(b);
  ASSERT_TRUE(wait_until([&] {
    (void)exchange_on(b).repair_unreconstructable_heads(dannan::sched::FrameType::control);
    return replica.unreconstructable_heads().empty();
  }));
  EXPECT_EQ(replica.accepted_heads().size(), 1u);
  EXPECT_EQ(replica.accepted_heads().front().hash, head);
  EXPECT_TRUE(holds(b, "a"));

  // The wire call itself, independently of the driver.
  dannan::membership::NodeRecord peer;
  for (const auto& record : b.context().nodes().active())
    if (record.id == a.id()) peer = record;
  const auto& numbers = b.options().exchange.numbers;
  const auto reply = b.context().routes().call_and_wait(peer, numbers.get_history_record,
                                                        heads::message::with_space(heads::message::encode_hash(head), a.api().heads().id_of(space_name)),
                                                        dannan::sched::FrameType::control, 10s);
  ASSERT_TRUE(reply.reply.has_value()) << reply.failure;
  ASSERT_EQ(reply.reply->type, numbers.history_entry_reply);
  const auto served = heads::decode_history_entry(reply.reply->payload);
  EXPECT_EQ(served.hash, head);
  EXPECT_EQ(served.body, heads::HistoryEntry::Body::full);
}

// Macha's test_metadata_identity_rpc (SM:4183), and R11: the reply is the committed identity,
// and Macha's case compares it with current_identity(), equal with no journal mid-replay.
TEST(HeadsExchange, APeerIsAskedItsIdentity) {
  Cluster cluster;
  auto& a = cluster.add("a", joined(1));
  auto& b = cluster.add("b", joined(1));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2; }, 5s));
  dannan::membership::NodeRecord peer;
  bool found = false;
  for (const auto& record : a.context().nodes().active())
    if (record.id == b.id()) {
      peer = record;
      found = true;
    }
  ASSERT_TRUE(found);
  const auto& numbers = a.options().exchange.numbers;
  const auto reply = a.context().routes().call_and_wait(peer, numbers.get_identity, heads::message::with_space({}, a.api().heads().id_of(space_name)),
                                                        dannan::sched::FrameType::speculative, 10s);
  ASSERT_TRUE(reply.reply.has_value()) << reply.failure;
  ASSERT_EQ(reply.reply->type, numbers.identity_reply);
  EXPECT_EQ(reply.reply->payload.size(), sizeof(uint64_t) + 32);
  const auto observed = heads::message::decode_identity(reply.reply->payload);
  EXPECT_EQ(observed, replica_on(b).current_identity());
  EXPECT_EQ(observed, replica_on(b).committed_identity());
}

// Macha's test_metadata_decoded_cache_ttl_recovers_missed_notice (test_hydration_catalogue.cpp:4177),
// H31. A valid newer head is installed on the peer's replica directly, to simulate a notice this
// node missed; the view's lifetime must still discover it.
TEST(HeadsExchange, ADecodedViewsLifetimeRecoversAMissedNotice) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto options_a = joined(1);
  options_a.exchange.advert_interval = 1h;
  auto options_b = options_a;
  // Node two's decoded view lives by a clock the test steps.
  options_b.exchange.view_time = &time;
  auto& a = cluster.add("a", options_a);
  auto& b = cluster.add("b", options_b);
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "a"));
  const auto initial1 = space_on(a).converged();
  std::optional<heads::SpaceView> initial2;
  ASSERT_TRUE(wait_until([&] {
    try {
      auto view = space_on(b).converged();
      if (view.generation != initial1.generation) return false;
      initial2 = std::move(view);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }));
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));
  // The notice of what b just took is a's to absorb; nothing further is on its way.
  ASSERT_TRUE(exchange_on(b).wait_replicated(10s));
  time.advance(a.options().exchange.view_ttl);
  ASSERT_EQ(space_on(b).converged().generation, initial2->generation);

  const auto base = replica_on(a).current();
  // The same tree under the next generation: what changed does not matter here.
  auto same = *NeutralCodec::as_head(initial1.state);
  ASSERT_TRUE(same.root.has_value());
  const auto next = test::record_after(base, test::neutral_payload(same));
  ASSERT_TRUE(replica_on(a).store_commit(next));
  ASSERT_TRUE(replica_on(a).accept_commit({next.generation, next.hash, 1, {a.id()}}));

  // With no notice the decoded view is legitimately reused until the lifetime expires.
  EXPECT_LT(exchange_on(b).known_generation(), next.generation);
  EXPECT_EQ(space_on(b).converged().generation, initial2->generation);
  time.advance(a.options().exchange.view_ttl - 1ms);
  EXPECT_EQ(space_on(b).converged().generation, initial2->generation);
  time.advance(1ms);
  ASSERT_LT(exchange_on(b).known_generation(), next.generation);

  // Expiry must force a real read, discover the newer head and replace the view, even with no
  // notice.
  const auto refreshed = space_on(b).converged();
  EXPECT_EQ(refreshed.generation, next.generation);
  EXPECT_GE(exchange_on(b).known_generation(), next.generation);
}

// A notice alone moves the view at once: the epoch half of H31, and H24.
TEST(HeadsExchange, ANoticeMakesAViewStaleBeforeItsLifetimeEnds) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto options = joined(1);
  options.exchange.view_time = &time;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "a"));
  ASSERT_TRUE(sees(b, "a"));
  const auto epoch = exchange_on(b).remote_epoch();
  write_key(a, "b");
  // The clock stands still, and the view moves.
  ASSERT_TRUE(wait_until([&] { return exchange_on(b).remote_epoch() > epoch; }));
  EXPECT_TRUE(sees(b, "b"));
}

// A claim barrier whose peers' share a case holds, naming what each commit brought.
class HeldPeersBarrier final : public heads::ClaimBarrier {
 public:
  explicit HeldPeersBarrier(dannan::test::Gate& peers) : _peers(peers) {}
  std::vector<store::ObjectId> retain_data_here(const std::vector<store::ObjectId>&, const heads::Dot&) override { return {}; }
  std::vector<store::ObjectId> retain_data(const std::vector<store::ObjectId>&, const heads::Dot&) override { return {}; }
  bool retain_control_here(const std::vector<store::ObjectId>&, const heads::Dot&) override { return true; }
  void retain_on_peers(const heads::Dot&, const heads::CommitClaims&) override { _peers.enter_and_wait(); }

 private:
  dannan::test::Gate& _peers;
};

// A mutation that says it brought in `count` objects, as Macha's fixture's claims callback does.
class ManyClaims final : public heads::Mutation {
 public:
  ManyClaims(const heads::State& parent, std::string key, std::atomic_uint64_t& made, size_t count)
      : _inner(parent, [key = std::move(key)](NeutralEditing& editing) { editing.delta.upsert_keys[key] = make_group(1); }), _made(made), _count(count) {}
  void edit(const heads::MutationContext& context) override { _inner.edit(context); }
  void stamp(const heads::MutationContext& context) override { _inner.stamp(context); }
  void advance_clock(const heads::MutationContext& context) override { _inner.advance_clock(context); }
  void apply(const heads::MutationContext& context) override { _inner.apply(context); }
  heads::State state() const override { return _inner.state(); }
  heads::Bytes delta() const override { return _inner.delta(); }
  heads::Introduced introduced(const heads::MutationContext&) const override {
    heads::Introduced out;
    out.data.reserve(_count);
    const auto base = _made.fetch_add(1) * _count;
    for (size_t i = 0; i < _count; ++i) {
      store::ObjectId id{};
      const uint64_t value = base + i;
      std::memcpy(id.bytes.data(), &value, sizeof(value));
      out.data.push_back(id);
    }
    return out;
  }

 private:
  test::NeutralMutation _inner;
  std::atomic_uint64_t& _made;
  const size_t _count;
};

// Macha's test_claims_owed_to_slow_peers_stay_within_their_bound (test_invariants.cpp:3030), every
// assertion. H15. The replicator's second half, past the claims, is RPC:5758 above.
TEST(HeadsExchange, ClaimsOwedToSlowPeersStayWithinTheirBound) {
  // 1.6 MB of claim ids a commit: twenty commits are past the 32 MB bound.
  constexpr size_t ids_per_commit = 50000;
  dannan::test::Gate peers;
  HeldPeersBarrier barrier(peers);
  std::atomic_uint64_t made{0};
  Cluster cluster;
  dannan::test::GateOpener open_on_exit(peers);
  ClusterNodeOptions options;
  options.spaces = {test::founded_spec(space_name)};
  options.exchange.write_copies = 1;
  options.exchange.claims = &barrier;
  auto& a = cluster.add("a", options);
  a.start();
  auto& space = space_on(a);
  const auto commit = [&](const std::string& key) {
    (void)space.mutate_delta([&, key](const heads::State& parent) { return std::make_unique<ManyClaims>(parent, key, made, ids_per_commit); });
  };
  commit("first");
  ASSERT_TRUE(peers.wait_for_entries(1, 10s));
  for (int i = 0; i < 40; ++i) commit("burst-" + std::to_string(i));

  const auto backlog = space.owed_backlog();
  const uint64_t bound = 32ULL * 1024 * 1024;
  EXPECT_LE(backlog.claim_bytes, bound);
  EXPECT_GT(backlog.claim_bytes + ids_per_commit * sizeof(store::ObjectId), bound);
  EXPECT_GT(backlog.dropped, 0u);
  EXPECT_LT(backlog.commits, 40u);

  peers.open();
  ASSERT_TRUE(exchange_on(a).wait_replicated(20s));
  const auto drained = space.owed_backlog();
  EXPECT_EQ(drained.commits, 0u);
  EXPECT_EQ(drained.claim_bytes, 0u);
}

// The waits an exchange declares, with the guard as their check: control work may not wait on
// the network or a device, whether it asks through the space or here, and maintenance's may.
TEST(HeadsExchange, RepairEntersTheWaitGuard) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {test::founded_spec(space_name)};
  options.exchange.write_copies = 1;
  auto& a = cluster.add("a", options);
  a.start();
  ASSERT_TRUE(writes(a, "x"));
  auto& guard = a.dagda_node().spaces()->guard();
  guard.set_mode(dannan::sched::WaitGuard::Mode::throw_on_violation);
  auto& exchange = exchange_on(a);
  const dannan::sched::WorkContext control(dannan::sched::FrameType::control);
  EXPECT_THROW(exchange.repair_once(control), std::logic_error);
  EXPECT_THROW((void)exchange.repair_unreconstructable_heads(control, dannan::sched::FrameType::control), std::logic_error);
  EXPECT_EQ(guard.violations(), 2u);
  // Truncation waits on the state device and locks, which control work may (Macha's
  // metadata_view.hpp:179-186).
  EXPECT_NO_THROW(exchange.truncate_history(control));
  EXPECT_EQ(guard.violations(), 2u);
  const dannan::sched::WorkContext background(dannan::sched::FrameType::speculative);
  EXPECT_NO_THROW(exchange.repair_once(background));
  EXPECT_NO_THROW((void)exchange.repair_unreconstructable_heads(background, dannan::sched::FrameType::speculative));
  EXPECT_NO_THROW(exchange.truncate_history(background));
  EXPECT_EQ(guard.violations(), 2u);
  // And the node's own passes under it, refused nothing.
  (void)a.run_maintenance_once();
  EXPECT_EQ(guard.violations(), 2u);
}

// ---- The every-node space ---------------------------------------------------

// C10, H20. A commit on one of three reaches all; a node away gets it on return; the node asked
// first is not preferred: each writes on from what it holds.
TEST(HeadsSpace, AnEveryNodeSpaceIsOfferedToEveryActiveNode) {
  Cluster cluster;
  auto& a = cluster.add("a", live(1));
  auto& b = cluster.add("b", live(1));
  auto& c = cluster.add("c", live(1));
  cluster.start_all();
  ASSERT_TRUE(wait_until(
      [&] { return a.context().nodes().active().size() == 3 && b.context().nodes().active().size() == 3 && c.context().nodes().active().size() == 3; }));
  ASSERT_TRUE(writes(c, "reaches-all"));
  const auto head = replica_on(c).committed();
  for (auto* node : {&a, &b}) EXPECT_TRUE(wait_until([&] { return replica_on(*node).committed().hash == head.hash; })) << node->name();

  b.stop();
  ASSERT_TRUE(cluster.dead_to(a, b.id()));
  ASSERT_TRUE(cluster.dead_to(c, b.id()));
  write_key(a, "while-away");
  const auto later = replica_on(a).committed();
  EXPECT_TRUE(wait_until([&] { return replica_on(c).committed().hash == later.hash; }));
  b.start();
  EXPECT_TRUE(wait_until([&] { return replica_on(b).committed().hash == later.hash; }));

  // None is a primary: the last to learn writes next, and the others follow it.
  write_key(b, "from-the-last");
  const auto last = replica_on(b).committed();
  for (auto* node : {&a, &c}) EXPECT_TRUE(wait_until([&] { return replica_on(*node).committed().hash == last.hash; })) << node->name();
}

// ---- Policy -----------------------------------------------------------------

// The owner's policy commit: a note under "policy/" the owner's config names. As Macha's
// maybe_reconfigure changes data_replication, it clears the merge parents and leaves the clock.
std::function<std::optional<heads::State>(const heads::State&)> policy_of(std::shared_ptr<std::string> configured) {
  return [configured](const heads::State& state) -> std::optional<heads::State> {
    const auto& head = *NeutralCodec::as_head(state);
    const auto found = head.notes.get().find("policy/copies");
    if (found != head.notes.get().end() && found->second == *configured) return std::nullopt;
    auto next = std::make_shared<NeutralHead>(head);
    next->merge_parents.clear();
    next->notes.set("policy/copies", *configured);
    return next;
  };
}

ClusterNodeOptions with_policy(std::shared_ptr<std::string> configured, size_t write_copies = 1) {
  auto out = joined(write_copies);
  out.spaces.front().reconfigure = policy_of(std::move(configured));
  return out;
}

// C11, the neutral form of test_replication_policy_change_on_restart's policy half (RPC:4756).
// What repair does with the new replication is the store's and stays Macha's case.
TEST(HeadsPolicy, AChangedPolicyOnRestartIsCommittedAndWritesFollowIt) {
  auto policy = std::make_shared<std::string>("1");
  Cluster cluster;
  auto& a = cluster.add("a", with_policy(policy));
  auto& b = cluster.add("b", with_policy(policy));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "policy"));
  ASSERT_TRUE(sees(b, "policy"));
  EXPECT_EQ(head_of(space_on(a).local()).notes.get().at("policy/copies"), "1");
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));
  ASSERT_TRUE(exchange_on(b).wait_replicated(10s));

  // The replica policy changes while the cluster is stopped.
  b.stop();
  a.stop();
  *policy = "2";
  a.start();
  b.start();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));

  const auto before = replica_on(a).committed_generation();
  ASSERT_TRUE(writes(a, "after-grow"));
  const auto head = head_of(space_on(a).local());
  EXPECT_EQ(head.notes.get().at("policy/copies"), "2");
  // One policy commit, one mutation.
  EXPECT_EQ(replica_on(a).committed_generation(), before + 2);
  EXPECT_TRUE(sees(b, "after-grow"));
  EXPECT_EQ(head_of(space_on(b).local()).notes.get().at("policy/copies"), "2");
  const auto settled = replica_on(a).committed_generation();
  // Later commits carry it and make no other.
  write_key(b, "carried");
  ASSERT_TRUE(sees(a, "carried"));
  EXPECT_EQ(replica_on(a).committed_generation(), settled + 1);
  EXPECT_EQ(head_of(space_on(a).local()).notes.get().at("policy/copies"), "2");
}

// C37, R2. No dot, its clock its parent's, its merge parents cleared, published to the active set
// on the control frame, and it runs on reads. "Only a merge collapses it" asserted of what is here:
// with no edge left two heads a policy commit apart have equal clocks, and neither is dropped; the
// merge that does collapse them is HeadsReconcile.AMergeCollapsesWhatAPolicyCommitLeft.
TEST(HeadsPolicy, APolicyCommitCarriesNoDotAndOnlyAMergeCollapsesIt) {
  auto policy = std::make_shared<std::string>("1");
  Cluster cluster;
  // Seeking two, so the active set is asked; a mutation seeks none (R7).
  auto& a = cluster.add("a", with_policy(policy, 2));
  auto& b = cluster.add("b", with_policy(policy, 2));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "x"));
  ASSERT_TRUE(sees(b, "x"));
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));
  ASSERT_TRUE(exchange_on(b).wait_replicated(10s));

  const auto parent = replica_on(a).committed();
  *policy = "2";
  const auto frames_before = b.calls_of(a.options().exchange.numbers.put_commit);
  // A read commits it (R6). The cached view must pass first.
  ASSERT_TRUE(wait_until([&] { return replica_on(a).committed().generation > parent.generation || ((void)space_on(a).converged(), false); }, 5s));
  const auto committed = replica_on(a).committed();
  ASSERT_EQ(committed.generation, parent.generation + 1);
  ASSERT_EQ(committed.previous, parent.hash);
  const auto& fields_codec = *a.options().spaces.front().codec;
  const auto before_fields = fields_codec.head_fields(fields_codec.decode_full(parent.payload));
  const auto after_fields = fields_codec.head_fields(fields_codec.decode_full(committed.payload));
  // No dot.
  EXPECT_EQ(after_fields.clock, before_fields.clock);
  EXPECT_TRUE(after_fields.merge_parents.empty());
  // To the active set, on the control frame.
  ASSERT_GT(b.calls_of(a.options().exchange.numbers.put_commit), frames_before);
  EXPECT_TRUE(b.frames_of(a.options().exchange.numbers.put_commit).contains(dannan::sched::FrameType::control));
  EXPECT_EQ(replica_on(b).committed().hash, committed.hash);
  const auto certificate = replica_on(a).acceptance(committed.hash);
  ASSERT_TRUE(certificate.has_value());
  // R5: how many held it.
  EXPECT_EQ(certificate->required, 2u);

  // What its equal clock means once history is truncated (md.cpp:2802, `behind != ahead`): a
  // record's direct parent is still its ancestor by its hash (md.cpp's history_is_ancestor), but
  // one policy commit further on, a node holding the old head and handed the newer as a peer's
  // truncated root cannot tell, and both stand. A mutation's clock, by contrast, does say so.
  *policy = "3";
  ASSERT_TRUE(wait_until([&] { return replica_on(a).committed().generation > committed.generation || ((void)space_on(a).converged(), false); }, 5s));
  const auto further = replica_on(a).committed();
  ASSERT_EQ(further.previous, committed.hash);
  EXPECT_EQ(fields_codec.head_fields(fields_codec.decode_full(further.payload)).clock, before_fields.clock);
  test::ReplicaBed bed;
  const auto fresh = bed.open("truncated", std::nullopt, false);
  const auto certify = [&](const heads::Record& record) { return heads::Acceptance{record.generation, record.hash, 1, {a.id()}}; };
  ASSERT_TRUE(fresh->import_history(test::rootless_entry(parent)));
  ASSERT_TRUE(fresh->accept_commit(certify(parent)));
  ASSERT_TRUE(fresh->import_history(test::rootless_entry(further)));
  ASSERT_TRUE(fresh->accept_commit(certify(further)));
  EXPECT_EQ(fresh->accepted_heads().size(), 2u);

  write_key(a, "y");
  const auto mutated = replica_on(a).committed();
  const auto other = bed.open("by-contrast", std::nullopt, false);
  ASSERT_TRUE(other->import_history(test::rootless_entry(parent)));
  ASSERT_TRUE(other->accept_commit(certify(parent)));
  ASSERT_TRUE(other->import_history(test::rootless_entry(mutated)));
  ASSERT_TRUE(other->accept_commit(certify(mutated)));
  ASSERT_EQ(other->accepted_heads().size(), 1u);
  EXPECT_EQ(other->accepted_heads().front().hash, mutated.hash);
}

// C17, H28, R2. Macha's loop, kept (Tom's "left", Macha's TODO/ACTIVE.md:79-82): two nodes that
// disagree each commit on seeing the other's value, and neither fails.
TEST(HeadsPolicy, DisagreeingNodesKeepCommitting) {
  auto mine = std::make_shared<std::string>("1");
  auto theirs = std::make_shared<std::string>("2");
  Cluster cluster;
  auto& a = cluster.add("a", with_policy(mine));
  auto& b = cluster.add("b", with_policy(theirs));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "x"));
  ASSERT_TRUE(wait_until([&] { return replica_on(b).committed_generation() == replica_on(a).committed_generation(); }));
  const auto start = replica_on(a).committed_generation();
  // Each read after the other's commit is a commit.
  ASSERT_TRUE(wait_until(
      [&] {
        for (auto* node : {&a, &b}) {
          try {
            (void)exchange_on(*node).repair_once();
          } catch (const std::exception&) {
          }
        }
        return replica_on(a).committed_generation() >= start + 4;
      },
      20s));
  EXPECT_GE(replica_on(a).committed_generation(), start + 4);
}

// ---- Status -----------------------------------------------------------------

// C38, Dagda's half of test_metadata_availability_logs_only_transitions. Validation describes
// convergence, not write authority: writable through a failed validation, and stable says which;
// each change of availability is one log line.
TEST(HeadsStatus, StaysWritableThroughPendingReconciliationAndReportsStable) {
  Cluster cluster;
  auto options = joined(1);
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  auto& exchange = exchange_on(a);
  // No view yet: not available, and validating says nothing of it.
  exchange.note_validation(false, "no head yet");
  EXPECT_EQ(exchange.status().availability, heads::Availability::unavailable);
  EXPECT_FALSE(exchange.status().write_available);
  EXPECT_EQ(exchange.status().replicas, 2u);

  ASSERT_TRUE(writes(a, "x"));
  exchange.note_validation(false, "reconciliation pending");
  auto status = exchange.status();
  EXPECT_EQ(status.availability, heads::Availability::writable);
  EXPECT_TRUE(status.write_available);
  EXPECT_FALSE(status.stable);
  EXPECT_EQ(status.write_replicas_required, 1u);
  EXPECT_EQ(status.generation, space_on(a).current_generation());
  const auto lines = [&](const std::string& text) {
    size_t count = 0;
    for (const auto& line : a.log().lines())
      if (line.find(text) != std::string::npos) ++count;
    return count;
  };
  EXPECT_EQ(lines("metadata availability changed state=writable"), 1u);
  EXPECT_EQ(lines("reconciliation pending\""), 1u);

  // A validation is news to Status and not to the log.
  exchange.note_validation(true);
  status = exchange.status();
  EXPECT_TRUE(status.stable);
  EXPECT_EQ(status.availability, heads::Availability::writable);
  exchange.note_validation(false, "reconciliation pending");
  exchange.note_validation(true);
  EXPECT_EQ(lines("metadata availability changed state=writable"), 1u);
  EXPECT_GT(status.observed_unix_ms, 0u);

  // And in dagda.heads.*, every figure the facade names (second cutover 8c.2).
  a.dagda_node().publish_status();
  const auto snapshot = a.context().status().snapshot();
  for (const auto* figure : {"generation",
                             "history_records",
                             "history_file_bytes",
                             "history_resident_payload_bytes",
                             "historical_requests",
                             "historical_reconstructions",
                             "historical_deltas_applied",
                             "materialization_cache_hits",
                             "materialization_cache_misses",
                             "materialization_cache_evictions",
                             "materialization_cache_entries",
                             "materialization_cache_bytes",
                             "materialization_cache_limit_bytes",
                             "accepted_head_persistence_writes",
                             "accepted_head_persistence_bytes",
                             "accepted_head_persistence_failures",
                             "head_holders",
                             "head_holders_present",
                             "heads_set_aside",
                             "reconciliations",
                             "conflicts_created",
                             "conflicts_superseded",
                             "mutations",
                             "mutation_retention_ms_total",
                             "mutation_retention_ms_max",
                             "mutation_publish_ms_total",
                             "mutation_publish_ms_max",
                             "availability",
                             "write_available",
                             "stable",
                             "replicas",
                             "replicas_online",
                             "write_replicas_required",
                             "observed_unix_ms",
                             "convergence.events_received",
                             "convergence.runs_scheduled",
                             "convergence.runs_completed",
                             "convergence.requested_epoch",
                             "convergence.completed_epoch",
                             "convergence.latest_generation",
                             "convergence.scheduled"})
    EXPECT_TRUE(snapshot.values.contains("dagda.heads." + space_name + "." + figure)) << figure;
  EXPECT_EQ(std::get<std::string>(snapshot.values.at("dagda.heads." + space_name + ".availability")), "writable");
}

// ---- The scope check --------------------------------------------------------

// A directory that refuses a node's writes to one space, at a generation it names.
class RefusesASpace final : public dannan::contract::ScopeSource {
 public:
  explicit RefusesASpace(std::string space, dannan::identity::NodeId refused) : _space(std::move(space)), _refused(refused) {}
  dannan::contract::ScopeDecision may_write(const dannan::contract::Principal& principal, std::string_view prefix) const override {
    if (prefix == _space && principal.id == dannan::identity::to_string(_refused))
      return {false, 42, "no grant for " + principal.id + " under " + std::string(prefix)};
    return {true, 42, {}};
  }

 private:
  const std::string _space;
  const dannan::identity::NodeId _refused;
};

// C18, H29: the one that matters to Macha. With no source a node principal commits and accepts on
// a peer, and nothing is refused.
TEST(HeadsMessages, WithNoScopeSourceNodePrincipalsCommitAndAccept) {
  Cluster cluster;
  auto& a = cluster.add("a", joined(2));
  auto& b = cluster.add("b", joined(2));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "x"));
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));
  EXPECT_EQ(replica_on(b).committed().hash, replica_on(a).committed().hash);
  EXPECT_TRUE(exchange_on(a).refusals().empty());
  EXPECT_TRUE(exchange_on(b).refusals().empty());
}

// C19. With a source, on put and on accept: nothing durable on the refusing replica, and the
// evidence names why. Pattern: store_scope_test.cpp.
TEST(HeadsMessages, ARefusedScopeStoresNothingAndSaysWhy) {
  Cluster cluster;
  auto& a = cluster.add("a", joined(2));
  auto& b = cluster.add("b", joined(2));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "x"));
  ASSERT_TRUE(sees(b, "x"));
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));
  ASSERT_TRUE(exchange_on(b).wait_replicated(10s));
  const auto held_before = replica_on(b).committed();
  const auto frames_before = replica_on(b).diagnostics().history_records;

  // b's directory refuses a's writes under this space.
  b.context().identity().install_scope_source(std::make_shared<RefusesASpace>(space_name, a.id()));
  write_key(a, "refused");
  const auto head = replica_on(a).committed();
  ASSERT_NE(head.hash, held_before.hash);

  // The commit, sought on b too: stored only here, and the evidence says who and why.
  const auto published = exchange_on(a).publish_commit(a.context().nodes().active(), head, {}, dannan::sched::FrameType::control);
  EXPECT_EQ(published.acceptance.replicas, std::vector<heads::NodeId>{a.id()});
  EXPECT_EQ(published.accepted_on, std::vector<heads::NodeId>{a.id()});
  ASSERT_EQ(published.refused.size(), 1u);
  EXPECT_EQ(published.refused.front().peer, b.id());
  EXPECT_NE(published.refused.front().why.find("403 scope_refused"), std::string::npos) << published.refused.front().why;
  EXPECT_NE(published.refused.front().why.find("no grant for"), std::string::npos) << published.refused.front().why;
  EXPECT_NE(published.refused.front().why.find("scope generation 42"), std::string::npos) << published.refused.front().why;

  // Accept alone, addressed straight at b: refused the same way, nothing moved.
  dannan::membership::NodeRecord peer;
  for (const auto& record : a.context().nodes().active())
    if (record.id == b.id()) peer = record;
  const auto& numbers = a.options().exchange.numbers;
  const heads::Acceptance certificate{head.generation, head.hash, 1, {a.id()}};
  const auto refused = a.context().routes().call_and_wait(peer, numbers.accept_commit,
                                                          heads::message::with_space(heads::encode_acceptance(certificate), a.api().heads().id_of(space_name)),
                                                          dannan::sched::FrameType::control, 10s);
  ASSERT_TRUE(refused.reply.has_value()) << refused.failure;
  const auto refusal = dannan::net::refusal_of(*refused.reply);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->status, 403);
  EXPECT_EQ(refusal->code, "scope_refused");

  // Nothing durable on b.
  EXPECT_EQ(replica_on(b).committed().hash, held_before.hash);
  EXPECT_FALSE(replica_on(b).history_contains(head.hash));
  EXPECT_EQ(replica_on(b).diagnostics().history_records, frames_before);

  // A refused node refuses itself the same way: a commit here is refused before any dot.
  a.context().identity().install_scope_source(std::make_shared<RefusesASpace>(space_name, a.id()));
  const auto generation = replica_on(a).committed_generation();
  try {
    write_key(a, "refused-here");
    ADD_FAILURE() << "not refused";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("no grant for"), std::string::npos) << error.what();
  }
  EXPECT_EQ(replica_on(a).committed_generation(), generation);
}

// ---- Stages -----------------------------------------------------------------

struct StageRecord {
  std::mutex mutex;
  std::vector<std::string> stages;
  std::function<void(std::string_view)> hook() {
    return [this](std::string_view stage) {
      std::lock_guard lock(mutex);
      stages.emplace_back(stage);
    };
  }
  std::vector<std::string> taken() {
    std::lock_guard lock(mutex);
    return stages;
  }
};

size_t position_of(const std::vector<std::string>& stages, const std::string& stage) {
  const auto found = std::find(stages.begin(), stages.end(), stage);
  return found == stages.end() ? stages.size() : static_cast<size_t>(found - stages.begin());
}

ClusterNodeOptions one_founder() {
  ClusterNodeOptions out;
  out.spaces = {test::founded_spec(space_name)};
  out.exchange.write_copies = 1;
  return out;
}

// C35. The plan's 2.7, Macha's enter_stage order at maintenance.cpp:489, :1135, :1144.
TEST(NodeHeads, StagesRunInMachasOrder) {
  StageRecord record;
  Cluster cluster;
  auto options = one_founder();
  options.maintenance.stage = record.hook();
  auto& a = cluster.add("a", options);
  a.start();
  const auto report = a.run_maintenance_once();
  ASSERT_EQ(report.metadata.size(), 1u);
  EXPECT_TRUE(report.metadata.front().ran) << report.metadata.front().why;
  const auto stages = record.taken();
  EXPECT_LT(position_of(stages, "metadata-repair"), position_of(stages, "retention-compact"));
  EXPECT_LT(position_of(stages, "retention-compact"), position_of(stages, "head-repair"));
  EXPECT_LT(position_of(stages, "head-repair"), position_of(stages, "history-truncate"));
  EXPECT_LT(position_of(stages, "history-truncate"), position_of(stages, "compact-packs"));
  EXPECT_LT(position_of(stages, "compact-packs"), stages.size());
  EXPECT_TRUE(report.local.heads_repair_ran);
  EXPECT_TRUE(report.local.history_truncate_ran);
}

// C35, maintenance.cpp:1134-1150. Head repair is always; truncation only when idle.
TEST(NodeHeads, RepairRunsWhileBusyAndTruncationDoesNot) {
  Cluster cluster;
  auto& a = cluster.add("a", one_founder());
  a.start();
  ASSERT_TRUE(writes(a, "x"));
  auto& node = a.dagda_node();
  LocalTurnOptions busy;
  busy.busy = true;
  auto turn = node.begin_local_stages(busy);
  node.head_repair(turn);
  node.history_truncate(turn);
  EXPECT_TRUE(turn.stages.heads_repair_ran);
  EXPECT_FALSE(turn.stages.history_truncate_ran);
  LocalTurnOptions idle;
  idle.busy = false;
  auto quiet = node.begin_local_stages(idle);
  node.head_repair(quiet);
  node.history_truncate(quiet);
  EXPECT_TRUE(quiet.stages.heads_repair_ran);
  EXPECT_TRUE(quiet.stages.history_truncate_ran);
}

// C35. Each in its own try, as Macha's are: a throw in either leaves what follows running.
TEST(NodeHeads, AThrowInEitherLeavesTheRestRunning) {
  for (const auto* failing : {"head-repair", "history-truncate"}) {
    Cluster cluster;
    auto options = one_founder();
    const std::string which = failing;
    options.maintenance.stage = [which](std::string_view stage) {
      if (stage == which) throw std::runtime_error("injected " + which + " failure");
    };
    auto& a = cluster.add("a", options);
    a.start();
    const auto report = a.run_maintenance_once();
    EXPECT_TRUE(report.local.packs_ran) << which;
    EXPECT_TRUE(report.local.verify_ran) << which;
    if (which == "head-repair") {
      EXPECT_TRUE(report.local.history_truncate_ran);
    }
  }
}

// C35. A stop goes through each: between flagged heads and peers, in a run, and between stages,
// nothing swallowing it.
TEST(NodeHeads, StopIsRespected) {
  Cluster cluster;
  auto& a = cluster.add("a", joined(1));
  auto& b = cluster.add("b", joined(1));
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  ASSERT_TRUE(writes(a, "x"));
  ASSERT_TRUE(sees(b, "x"));
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));
  ASSERT_TRUE(exchange_on(b).wait_replicated(10s));

  std::stop_source stopping;
  stopping.request_stop();

  // In a run of metadata repair, with demand standing.
  exchange_on(b).request_demand();
  EXPECT_THROW((void)b.dagda_node().metadata_repair(stopping.get_token()), contract::MaintenanceStopping);

  // In head repair, whether or not a head is flagged.
  LocalTurnOptions stop;
  stop.stop = stopping.get_token();
  auto turn = b.dagda_node().begin_local_stages(stop);
  EXPECT_THROW(b.dagda_node().head_repair(turn), contract::MaintenanceStopping);
  auto other = b.dagda_node().begin_local_stages(stop);
  EXPECT_THROW(b.dagda_node().history_truncate(other), contract::MaintenanceStopping);
  EXPECT_THROW((void)b.dagda_node().run_local_stages(stopping.get_token()), contract::MaintenanceStopping);

  // Between a flagged head's peers.
  const auto directory = heads::space_directory(b.state(), a.api().heads().id_of(space_name));
  b.stop();
  for (const auto* name : {"checkpoint.meta", "journal.log", "history.log"}) std::filesystem::remove(directory / name);
  b.start();
  ASSERT_TRUE(wait_until([&] { return b.context().nodes().active().size() >= 2; }));
  ASSERT_FALSE(replica_on(b).unreconstructable_heads().empty());
  EXPECT_THROW((void)exchange_on(b).repair_unreconstructable_heads(dannan::sched::FrameType::speculative, [] { throw contract::MaintenanceStopping{}; }),
               contract::MaintenanceStopping);
  EXPECT_FALSE(replica_on(b).unreconstructable_heads().empty());
}

// C35. `forced` makes busy false: run_maintenance_once runs them under activity.
TEST(NodeHeads, ForcedRunsThem) {
  Cluster cluster;
  auto& a = cluster.add("a", one_founder());
  a.start();
  ASSERT_TRUE(writes(a, "x"));
  a.dagda_node().activity().note(dannan::sched::FrameType::foreground, 64 * 1024);
  const auto held = a.dagda_node().run_local_stages();
  EXPECT_TRUE(held.heads_repair_ran);
  EXPECT_FALSE(held.history_truncate_ran);
  a.dagda_node().activity().note(dannan::sched::FrameType::foreground, 64 * 1024);
  const auto forced = a.run_maintenance_once();
  EXPECT_TRUE(forced.local.heads_repair_ran);
  EXPECT_TRUE(forced.local.history_truncate_ran);
}

// C35, maintenance.cpp:459-542. A run goes before anything that depends on the head; a virgin
// follower waits; and a failed run with no head yet holds the pass.
TEST(NodeHeads, MetadataRepairRunsFirstAndAVirginFollowerWaits) {
  {
    StageRecord record;
    Cluster cluster;
    auto options = one_founder();
    options.maintenance.stage = record.hook();
    auto& a = cluster.add("a", options);
    a.start();
    (void)a.run_maintenance_once();
    EXPECT_EQ(position_of(record.taken(), "metadata-repair"), 0u);
    EXPECT_EQ(position_of(record.taken(), "metadata-repair-begin"), 1u);
  }
  {
    // The higher id, its founder's first look not yet taken: a run never starts.
    StageRecord record;
    Cluster cluster;
    auto options_a = joined(1);
    auto options_b = joined(1);
    options_a.maintenance.stage = record.hook();
    options_b.maintenance.stage = record.hook();
    auto& a = cluster.add("a", options_a);
    auto& b = cluster.add("b", options_b);
    cluster.start_all();
    ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
    auto& follower = a.id() < b.id() ? b : a;
    const auto report = follower.run_maintenance_once();
    ASSERT_EQ(report.metadata.size(), 1u);
    EXPECT_TRUE(report.metadata.front().waited);
    EXPECT_FALSE(report.metadata.front().ran);
    EXPECT_EQ(position_of(record.taken(), "metadata-repair"), record.taken().size());
    // A follower waiting is no failure: the rest of the pass goes on.
    EXPECT_TRUE(report.local.packs_ran);
  }
  {
    // Alone with bootstrap peers configured: the run fails, generation 1, and nothing depending
    // on the head proceeds (maintenance.cpp:514-517).
    Cluster cluster;
    auto& a = cluster.add("a", joined(1));
    a.start();
    const auto report = a.run_maintenance_once();
    ASSERT_EQ(report.metadata.size(), 1u);
    EXPECT_TRUE(report.metadata.front().hold_pass);
    EXPECT_NE(report.metadata.front().why.find("waiting for bootstrap peer"), std::string::npos) << report.metadata.front().why;
    EXPECT_FALSE(report.local.packs_ran);
    EXPECT_NE(report.gc_skipped.find("waiting for bootstrap peer"), std::string::npos) << report.gc_skipped;
    EXPECT_EQ(exchange_on(a).status().availability, heads::Availability::unavailable);
  }
}

// NS:421's loop half (test_a_stale_tombstone_in_a_batch_is_erased_once, S4 took the rest): on the
// node's own maintenance thread, through the provider, with a peer: erased once, and the peer
// follows the head.
TEST(NodeHeads, AStaleRetirementIsErasedOnceByTheLoopAndThePeerFollows) {
  dannan::test::SteppedWall wall(dannan::contract::system_wall_clock().unix_ms());
  Cluster cluster;
  auto options = live(1);
  options.wall = &wall;
  // Nothing matures: only staleness erases a retirement here.
  options.garbage_grace = 1h;
  options.maintenance.gc_interval = 25ms;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(writes(a, "seeded"));
  auto& space = space_on(a);

  const auto write_object = [&](uint64_t n) {
    const auto data = test::numbered(n);
    const auto id = store::object_id(data);
    store::WriteOptions write;
    write.level = store::Durability::persisted;
    EXPECT_TRUE(a.store().write(id, data, write).met);
    return id;
  };
  const auto item_naming = [](const std::vector<store::ObjectId>& ids) {
    NeutralItem item;
    item.kind = 2;
    uint64_t start = 0;
    for (const auto& id : ids) {
      NeutralRef ref;
      ref.start = start;
      ref.span = 4096;
      ref.id = id;
      item.refs.push_back(ref);
      start += 4096;
    }
    return item;
  };
  const auto revived = write_object(31);
  const auto other = write_object(32);
  (void)space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["gone"] = item_naming({revived, other}); }));
  (void)space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) {
    editing.delta.erase_keys.push_back("gone");
    editing.retire.push_back({true, 0, {revived, other}});
  }));
  const auto before = test::neutral_batches(head_of(*space.current()));
  ASSERT_FALSE(before.empty());
  (void)space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["again"] = item_naming({revived}); }));

  const auto reader = [&a]() -> std::unique_ptr<heads::TreeNodeStore> { return reader_on(a); };
  test::NeutralSpaceProvider provider(space, reader);
  a.dagda_node().register_reachability(domain::domain_v1, provider);
  const auto retired = [&](const store::ObjectId& id) {
    const auto view = space.local();
    const auto nodes = reader_on(a);
    const auto all = heads::retirements_of(test::neutral_batches(head_of(view)), *nodes, test::neutral_batch_magic);
    return std::any_of(all.begin(), all.end(), [&](const contract::Garbage& one) { return one.id == id; });
  };
  EXPECT_TRUE(wait_until([&] { return !retired(revived); }, 30s));
  EXPECT_TRUE(retired(other));
  const auto after = test::neutral_batches(head_of(space.local()));
  ASSERT_EQ(after.size(), 1u);
  EXPECT_EQ(after.front().retired_at_ns, before.front().retired_at_ns);
  a.dagda_node().unregister_reachability(domain::domain_v1);
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));
  const auto head = replica_on(a).committed();
  EXPECT_TRUE(wait_until([&] { return replica_on(b).committed().hash == head.hash; }, 20s));
}

}  // namespace
}  // namespace dagda
