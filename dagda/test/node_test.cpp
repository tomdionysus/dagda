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

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "dagda/migrate/migrate.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/legacy_v18.h"

namespace dagda {
namespace {

using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;

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
                                     "open availability",  "open repair",       "open storage-server", "open maintenance"};
// Step 7b: the maintenance thread stops first.
const std::vector<std::string> stops{"stop maintenance",      "stop storage-server", "stop repair",      "stop availability",  "stop distributed-store",
                                     "stop reference-counts", "stop claims",         "stop cache",       "stop control-store", "stop admission",
                                     "stop storage-pool",     "stop node-local-key", "stop state-layout"};

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
  for (const auto* plane : {"data_storage", "control_storage", "cache", "retention"}) EXPECT_TRUE(readiness.has(plane)) << plane;
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
  for (const auto* plane : {"data_storage", "control_storage", "cache", "retention"}) EXPECT_TRUE(readiness.has(plane)) << plane;
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

}  // namespace
}  // namespace dagda
