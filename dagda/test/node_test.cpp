//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Step 7a: dagda::Node, the assembly every cluster test now opens through
// the fixture. From the design (TODO/ACTIVE.md step 7a; requirements C4, C5;
// the maintainer, 2026-11-04 night: a node that finds a v18 disk refuses to start and
// names dagda-migrate): the order it opens and stops in, its refusals, the
// storage lock on every root against a second node and the migration, the
// message types it registers, its Status figures and readiness.

#include "dagda/node.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "dagda/migrate/migrate.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/legacy_v18.h"
#include "dannan/supervise/startup.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda {
namespace {

using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using namespace std::chrono_literals;

// The stages a node reports, in order.
struct Stages {
  std::mutex mutex;
  std::vector<std::string> seen;
  std::function<void(std::string_view)> sink() {
    return [this](std::string_view what) {
      std::lock_guard lock(mutex);
      seen.emplace_back(what);
    };
  }
  std::vector<std::string> take() {
    std::lock_guard lock(mutex);
    return std::exchange(seen, {});
  }
};

const std::vector<std::string> opens{"open message-types", "open state-layout", "open node-local-key", "open storage-pool",     "open admission",
                                     "open control-store", "open cache",        "open claims",         "open reference-counts", "open distributed-store",
                                     "open heads",         "open availability", "open repair",         "open storage-server",   "open maintenance"};
// Step 7b: the maintenance thread stops first.
const std::vector<std::string> stops{"stop maintenance",       "stop storage-server",   "stop repair",         "stop availability", "stop heads",
                                     "stop distributed-store", "stop reference-counts", "stop claims",         "stop cache",        "stop control-store",
                                     "stop admission",         "stop storage-pool",     "stop node-local-key", "stop state-layout"};

std::string refusal_of(ClusterNode& node) {
  try {
    node.start();
  } catch (const NodeRefused& refused) {
    return refused.what();
  }
  ADD_FAILURE() << node.name() << " was not refused";
  return {};
}

// It opens in the design's order on a fresh state and again on the state it
// left, stops in the reverse order, and what it held is still held.
TEST(Node, OpensAndStopsInOrderOnAFreshStateAndAReopenedOne) {
  Cluster cluster;
  Stages stages;
  ClusterNodeOptions options;
  options.trace = stages.sink();
  auto& a = cluster.add("a", options);
  a.start();
  EXPECT_EQ(stages.take(), opens);
  const auto data = test::numbered(7);
  const auto id = store::object_id(data);
  store::WriteOptions write;
  write.level = store::Durability::persisted;
  ASSERT_TRUE(a.store().write(id, data, write).met);
  a.stop();
  EXPECT_EQ(stages.take(), stops);
  a.start();
  EXPECT_EQ(stages.take(), opens);
  EXPECT_TRUE(a.store().has(id));
  a.stop();
  EXPECT_EQ(stages.take(), stops);
}

// A v18 state, and a v18 backend under a v19 state, are refused with the
// migration's command and the state path; nothing on either disk changes.
TEST(Node, AV18DiskIsRefusedNamingTheMigrationAndNothingChanges) {
  Cluster cluster;
  auto& a = cluster.add("a");
  test::legacy::write_state_layout_v18(a.state());
  test::legacy::write_backend_marker_v18(a.state(), a.backend(), dannan::identity::NodeId::random(), dannan::identity::NodeId::random());
  const auto state_before = test::legacy::tree_digest(a.state());
  const auto backend_before = test::legacy::tree_digest(a.backend());
  auto why = refusal_of(a);
  EXPECT_NE(why.find("dagda-migrate --state " + a.state().string()), std::string::npos) << why;
  EXPECT_FALSE(a.running());
  EXPECT_EQ(test::legacy::tree_digest(a.state()), state_before);
  EXPECT_EQ(test::legacy::tree_digest(a.backend()), backend_before);

  // A v19 node whose backend is put back as Macha left it.
  auto& b = cluster.add("b");
  b.start();
  b.stop();
  test::legacy::write_backend_marker_v18(b.state(), b.backend(), b.id(), dannan::identity::NodeId::random(), false);
  const auto b_before = test::legacy::tree_digest(b.state());
  why = refusal_of(b);
  EXPECT_NE(why.find("backend " + b.backend().string()), std::string::npos) << why;
  EXPECT_NE(why.find("dagda-migrate"), std::string::npos) << why;
  EXPECT_EQ(test::legacy::tree_digest(b.state()), b_before);
}

// No custody, no node-local key: the ledgers would have no key, so the
// node is refused and says why.
TEST(Node, ANodeWithoutCustodyIsRefused) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.custody = false;
  auto& a = cluster.add("a", options);
  const auto why = refusal_of(a);
  EXPECT_NE(why.find("no key custody"), std::string::npos) << why;
  EXPECT_FALSE(a.running());
}

// The storage lock on the state and every root: a second node on the same
// state, or on the same backend, is refused while the first runs, and so is
// the migration given a running node's backend; a root the migration holds
// refuses the node. The first node runs on throughout.
TEST(Node, TheStorageLockRefusesASecondNodeAndTheMigration) {
  Cluster cluster;
  auto& a = cluster.add("a");
  a.start();

  ClusterNodeOptions same_state;
  same_state.state = a.state();
  same_state.hosts_extents = false;
  auto& b = cluster.add("b", same_state);
  auto why = refusal_of(b);
  EXPECT_NE(why.find(a.state().string() + " is in use"), std::string::npos) << why;

  ClusterNodeOptions same_backend;
  same_backend.data = a.backend();
  auto& c = cluster.add("c", same_backend);
  why = refusal_of(c);
  EXPECT_NE(why.find(a.backend().string() + " is in use"), std::string::npos) << why;

  // The migration's library entry, given a v18 state and the running node's
  // backend: refused at the backend's lock, before any key or write.
  const auto scratch = cluster.dir() / "migration";
  test::legacy::write_state_layout_v18(scratch / "state");
  (void)dannan::identity::KeyCustody::create(scratch / "custody", Cluster::custody_source());
  const auto custody = dannan::identity::KeyCustody::open(scratch / "custody", Cluster::custody_source());
  const auto backend_before = test::legacy::tree_digest(a.backend());
  try {
    (void)migrate::migrate_state(scratch / "state", {a.backend()}, cluster.key(), custody);
    ADD_FAILURE() << "the migration ran under a running node";
  } catch (const migrate::MigrationRefused& refused) {
    why = refused.what();
    EXPECT_NE(why.find(a.backend().string() + " is in use (is the node running?)"), std::string::npos) << why;
  }
  EXPECT_EQ(test::legacy::tree_digest(a.backend()), backend_before);

  const auto data = test::numbered(11);
  store::WriteOptions write;
  write.level = store::Durability::persisted;
  EXPECT_TRUE(a.store().write(store::object_id(data), data, write).met);

  // The other way: the migration holds the backend (as it does while it
  // runs), and the node is refused.
  a.stop();
  {
    store::StorageLock held(a.backend());
    why = refusal_of(a);
    EXPECT_NE(why.find(a.backend().string() + " is in use"), std::string::npos) << why;
  }
  a.start();
  EXPECT_TRUE(a.store().has(store::object_id(data)));
}

// The node registers Dagda's types before the Dannan node starts (C5): a
// second registration clashes, so a second store on the same Dannan node is
// refused, and a store opened after the Dannan node started is refused too.
TEST(Node, RegistersDagdasMessageTypesBeforeTheDannanNodeStarts) {
  Cluster cluster;
  auto& a = cluster.add("a");
  a.open();
  EXPECT_THROW(store::message::register_types(a.context().routes()), std::invalid_argument);
  ClusterNodeOptions other;
  other.state = cluster.dir() / "other-state";
  other.hosts_extents = false;
  auto& b = cluster.add("b", other);
  const auto second_store = [&] {
    try {
      Node second(a.context(), b.node_options());
      ADD_FAILURE() << "a second store opened on one Dannan node";
    } catch (const NodeRefused& refused) {
      return std::string(refused.what());
    }
    return std::string();
  };
  auto why = second_store();
  EXPECT_NE(why.find("registered already"), std::string::npos) << why;
  a.node().start();
  why = second_store();
  EXPECT_NE(why.find("before the Dannan node starts"), std::string::npos) << why;
  a.stop();
}

uint64_t figure(const dannan::status::Surface::Snapshot& snapshot, const std::string& name) {
  const auto found = snapshot.values.find(name);
  if (found == snapshot.values.end()) {
    ADD_FAILURE() << name << " not published";
    return 0;
  }
  const auto* value = std::get_if<uint64_t>(&found->second);
  if (!value) {
    ADD_FAILURE() << name << " is not a count";
    return 0;
  }
  return *value;
}

// The store's figures are in Dannan's Status surface under "dagda.", and the
// node's readiness planes are ready once it has opened (C4).
TEST(Node, PublishesTheStoresFiguresAndGoesReady) {
  Cluster cluster;
  auto& a = cluster.add("a");
  a.start();
  auto& readiness = a.context().readiness();
  for (const auto* plane : {"data_storage", "control_storage", "cache", "retention", "metadata"}) EXPECT_TRUE(readiness.has(plane)) << plane;
  EXPECT_TRUE(readiness.complete());
  EXPECT_FALSE(readiness.failed());

  auto snapshot = a.context().status().snapshot();
  EXPECT_EQ(figure(snapshot, "dagda.context_version"), context_version);
  EXPECT_EQ(figure(snapshot, "dagda.storage.limit"), 64ULL * 1024 * 1024);
  EXPECT_EQ(figure(snapshot, "dagda.storage.backends_online"), 1u);
  EXPECT_EQ(figure(snapshot, "dagda.storage.used"), 0u);
  EXPECT_EQ(std::get<bool>(snapshot.values.at("dagda.storage.degraded")), false);
  EXPECT_EQ(std::get<bool>(snapshot.values.at("dagda.cache.enabled")), false);
  EXPECT_EQ(std::get<std::string>(snapshot.values.at("dagda.domain.name")), "macha");
  EXPECT_EQ(std::get<std::string>(snapshot.values.at("dagda.repair.gate.last")), "none");
  EXPECT_EQ(figure(snapshot, "dagda.repair.passes"), 0u);
  EXPECT_TRUE(snapshot.values.contains("dagda.admission.credit_used"));

  const auto data = test::numbered(3);
  store::WriteOptions write;
  write.level = store::Durability::persisted;
  ASSERT_TRUE(a.store().write(store::object_id(data), data, write).met);
  a.dagda_node().publish_status();
  snapshot = a.context().status().snapshot();
  EXPECT_GT(figure(snapshot, "dagda.storage.used"), 0u);
  a.stop();
}

// The node declares its own readiness planes (Dannan 0.4.1's
// Readiness::declare), so an owner names none of Dagda's in its
// NodeConfig: the node still reports each and goes ready.
TEST(Node, DeclaresItsOwnReadinessPlanes) {
  Cluster cluster;
  auto& a = cluster.add("a");
  a.start();
  auto& readiness = a.context().readiness();
  for (const auto* plane : {"data_storage", "control_storage", "cache", "retention", "metadata"}) EXPECT_TRUE(readiness.has(plane)) << plane;
  EXPECT_TRUE(readiness.complete());
  EXPECT_FALSE(readiness.failed());
  a.stop();
}

// A node that opens with every backend offline is ready but degraded, and says
// so: it must not crash-loop on a startup timeout (the maintainer, 2026-11-05). A backend
// coming online clears it.
TEST(Node, NoBackendOnlineIsReadyButDegraded) {
  Cluster cluster;
  const auto bad = cluster.dir() / "bad";
  std::filesystem::create_directories(bad);
  std::ofstream(bad / store::backend_marker_name) << "not a marker\n";
  ClusterNodeOptions options;
  options.data = bad;
  auto& a = cluster.add("a", options);
  a.start();
  auto& node = a.dagda_node();
  const auto degraded = [&] {
    node.publish_status();
    return std::get<bool>(a.context().status().snapshot().values.at("dagda.storage.degraded"));
  };
  EXPECT_TRUE(a.data().degraded());
  EXPECT_TRUE(degraded());
  EXPECT_TRUE(a.context().readiness().complete());
  auto backends = node.options().pool.backends;
  backends[0].path = cluster.dir() / "good";
  node.reconfigure({backends, {}});
  EXPECT_FALSE(degraded());
  EXPECT_TRUE(a.context().readiness().complete());
  a.stop();
}

// Macha's node answers while its store recovers: the Dannan node starts first, so
// Status and its roster work and the store's planes read as not ready, then the
// store opens with the node already running.
TEST(Node, OpensInTwoPartsWithTheDannanNodeAlreadyRunning) {
  Cluster cluster;
  auto& peer = cluster.add("peer");
  ClusterNodeOptions options;
  options.open_later = true;
  auto& late = cluster.add("late", options);
  cluster.start_all();
  EXPECT_FALSE(late.dagda_node().running());
  EXPECT_FALSE(late.context().readiness().has("data_storage"));
  EXPECT_FALSE(late.context().readiness().complete());
  EXPECT_TRUE(test::wait_until([&] { return late.context().nodes().active().size() == 2u; }));

  late.dagda_node().open_store();
  EXPECT_TRUE(late.dagda_node().running());
  EXPECT_TRUE(late.context().readiness().has("data_storage"));
  EXPECT_TRUE(late.context().readiness().complete());
  EXPECT_THROW(late.dagda_node().open_store(), std::logic_error);
  ASSERT_TRUE(cluster.converged());
  const auto bytes = test::pattern(4096, 61);
  EXPECT_TRUE(peer.store().put(store::object_id(bytes), bytes));
  EXPECT_TRUE(late.store().get(store::object_id(bytes)).has_value());
}

// A store that cannot open is refused at open_store, the node stays up to say why,
// and stopping it is safe. Stopping one that never opened is too.
TEST(Node, ALateOpenCanBeRefusedOrNeverHappen) {
  Cluster cluster;
  ClusterNodeOptions never;
  never.open_later = true;
  auto& a = cluster.add("never", never);
  a.start();
  EXPECT_FALSE(a.dagda_node().running());
  a.stop();

  ClusterNodeOptions refused;
  refused.open_later = true;
  auto& b = cluster.add("refused", refused);
  b.start();
  // Another process holds the state.
  {
    store::StorageLock held(b.state());
    EXPECT_THROW(b.dagda_node().open_store(), NodeRefused);
  }
  EXPECT_TRUE(b.context().readiness().failed());
  EXPECT_FALSE(b.dagda_node().running());
  b.stop();
}

// Macha (tests/test_storage_v18.cpp): test_unversioned_state_is_refused, over a
// whole node. A state holding files and no marker is refused, saying a store
// begins only in an empty directory, and left as it was. A DATA backend holding an
// old layout's files leaves the node up with nothing online: ready but degraded
// (the maintainer, 2026-11-05).
TEST(Node, AnUnversionedStateIsRefusedAndAnUnversionedBackendIsReadyButDegraded) {
  Cluster cluster;
  auto& old_state = cluster.add("old-state");
  std::filesystem::create_directories(old_state.state());
  std::ofstream(old_state.state() / "metadata.bin") << "old namespace";
  const auto why = refusal_of(old_state);
  EXPECT_NE(why.find("no storage-layout marker"), std::string::npos) << why;
  EXPECT_NE(why.find("begins only in an empty state directory"), std::string::npos) << why;
  EXPECT_FALSE(old_state.running());
  EXPECT_FALSE(std::filesystem::exists(old_state.state() / "storage-layout"));

  auto& boundary = cluster.add("fresh-boundary");
  std::filesystem::create_directories(boundary.backend() / "objects");
  std::ofstream(boundary.backend() / "objects" / "legacy") << "old namespace";
  boundary.start();
  EXPECT_TRUE(boundary.context().readiness().control_plane_online());
  EXPECT_TRUE(boundary.data().degraded());
  EXPECT_TRUE(boundary.context().readiness().complete());
  EXPECT_FALSE(boundary.context().readiness().failed());
  EXPECT_EQ(boundary.data().online_backends(), 0u);
  EXPECT_EQ(boundary.data().limit(), 0u);
  boundary.stop();
}

// Macha (tests/test_root_parts.cpp):
// test_node_resources_yield_the_loader_to_a_recent_viewer_under_pressure. Law 3
// as the node wires it: under device pressure the loader yields only while a
// foreground user has been active within the quiet window, read from the node's
// activity clock on the node's time. Stopped, it admits nothing.
TEST(Node, TheLoaderYieldsToARecentForegroundUserUnderPressure) {
  using dannan::sched::FrameType;
  dannan::test::SteppedTime time;
  const store::DataWorkContext loader(FrameType::loader);
  std::optional<store::DataResourceArbiter::Lease> first;
  store::DataResourceArbiter* credit = nullptr;
  std::optional<bool> admitted_once_stopped;
  Cluster cluster;
  ClusterNodeOptions options;
  options.time = &time;
  options.background_concurrency = 8;
  options.repair.foreground_quiet = 2s;
  options.trace = [&](std::string_view stage) {
    // Admission stopped first, and the credit lives until after the server.
    if (stage != "stop storage-server" || !credit) return;
    first.reset();
    admitted_once_stopped = credit->try_acquire(loader, 4096).has_value();
  };
  auto& a = cluster.add("a", options);
  a.open();
  credit = a.credit();
  ASSERT_NE(credit, nullptr);

  a.data().service_monitor().note(2000ms, 4096);
  ASSERT_TRUE(a.data().service_monitor().pressured());

  // One loader lease is always admitted; the pressure gate judges the next.
  first = credit->try_acquire(loader, 4096);
  ASSERT_TRUE(first.has_value());

  // No foreground user: pressure alone does not hold an import back.
  EXPECT_TRUE(credit->try_acquire(loader, 4096).has_value());
  EXPECT_EQ(credit->stats().pressure_refusals, 0u);

  // A foreground read within the window: the loader is refused.
  a.dagda_node().activity().note(FrameType::foreground, 1);
  time.advance(1999ms);
  EXPECT_FALSE(credit->try_acquire(loader, 4096).has_value());
  EXPECT_EQ(credit->stats().pressure_refusals, 1u);

  // The window passes with no more activity: admitted again.
  time.advance(1ms);
  EXPECT_TRUE(credit->try_acquire(loader, 4096).has_value());
  EXPECT_EQ(credit->stats().pressure_refusals, 1u);

  a.stop();
  EXPECT_EQ(admitted_once_stopped, std::optional<bool>{false});
}

// Macha (tests/test_rpc_cluster.cpp): test_service_startup_gate_spares_progress_
// and_ends_a_stall, what the owner needs of Node::open (the Macha session's
// answers, 2026-11-05: StartupStageHook). The gate itself is the owner's, over
// dannan::supervise::StartupStallGate. A stage may hold the open; the owner's
// progress spares it through three windows while recovery ticks; silence for
// the window ends it; let go, the open goes on. A real open's recovery ticks the
// same progress.
TEST(Node, AStageHoldsTheOpenAndTheOwnersProgressTellsAStallFromRecovery) {
  dannan::test::SteppedTime time;
  dannan::test::Gate gate;
  dannan::supervise::StartupProgress progress;
  Cluster cluster;
  ClusterNodeOptions options;
  options.startup_progress = &progress;
  options.trace = [&](std::string_view stage) {
    if (stage == "open storage-pool") gate.enter_and_wait();
  };
  auto& a = cluster.add("a", options);

  dannan::supervise::StartupStallGate stall(400ms, 0ms, time.now(), progress.value());
  auto opening = std::async(std::launch::async, [&] { a.open(); });
  ASSERT_TRUE(gate.wait_for_entries(1, 10s));

  // Recovery ticks for three windows: the gate holds.
  for (int tick = 0; tick < 24; ++tick) {
    progress.note();
    time.advance(50ms);
    EXPECT_FALSE(stall.stalled(time.now(), progress.value()));
  }
  // Silence: the gate fires at the window and no sooner.
  EXPECT_FALSE(stall.stalled(time.now(), progress.value()));
  time.advance(399ms);
  EXPECT_FALSE(stall.stalled(time.now(), progress.value()));
  time.advance(1ms);
  EXPECT_TRUE(stall.stalled(time.now(), progress.value()));
  EXPECT_EQ(opening.wait_for(0ms), std::future_status::timeout);

  gate.open();
  ASSERT_EQ(opening.wait_for(30s), std::future_status::ready);
  opening.get();
  ASSERT_TRUE(a.running());

  const auto data = test::numbered(11);
  store::WriteOptions write;
  write.level = store::Durability::persisted;
  ASSERT_TRUE(a.store().write(store::object_id(data), data, write).met);
  a.stop();
  // No clean checkpoint to trust: the open's accounting scan walks the store,
  // on its own thread.
  std::filesystem::remove(a.backend() / ".macha.accounting");
  const auto before = progress.value();
  a.open();
  EXPECT_TRUE(test::wait_until([&] { return progress.value() > before; }));
  a.stop();
}

// Macha has no device reserve; its facade turns Dagda's off, and DATA work then goes
// uncapped by it.
TEST(Node, TheDeviceReserveCanBeOff) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.device_reserve = false;
  auto& a = cluster.add("a", options);
  cluster.start_all();
  EXPECT_EQ(a.dagda_node().io(), nullptr);
  const auto bytes = test::pattern(4096, 91);
  EXPECT_TRUE(a.store().put(store::object_id(bytes), bytes));
  EXPECT_TRUE(a.store().get(store::object_id(bytes)).has_value());
  a.dagda_node().publish_status();
}

}  // namespace
}  // namespace dagda
