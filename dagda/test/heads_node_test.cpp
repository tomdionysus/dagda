//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Session S4 on a real node: the "metadata" plane from real replay and its off switch
// (the plan's 2.1), the Context's forms, a commit's real claims on the store, and
// retirement through the provider and the collector. Over the neutral owner; Macha's
// cases are named where they are converted.

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dagda/context.h"
#include "dagda/contract/stopping.h"
#include "dagda/heads/control_node_store.h"
#include "dagda/heads/retirement.h"
#include "dagda/node.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/heads_space.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/encoding/base.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda {
namespace {

using namespace std::chrono_literals;
using heads::CommitOutcome;
using heads::Dot;
using heads::Space;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::make_group;
using test::neutral_batches;
using test::neutral_mutation;
using test::neutral_spec;
using test::neutral_tree;
using test::NeutralCodec;
using test::NeutralEditing;
using test::NeutralEntries;
using test::NeutralHead;
using test::NeutralItem;
using test::NeutralRef;

constexpr auto deadline = 10s;

const NeutralHead& head_of(const heads::SpaceView& view) { return *NeutralCodec::as_head(view.state); }

Space& space_on(ClusterNode& node, const std::string& name = "test/neutral") {
  auto* found = node.dagda_node().spaces()->find(node.api().heads().id_of(name));
  EXPECT_NE(found, nullptr) << name;
  return *found;
}

std::unique_ptr<heads::TreeNodeStore> commit_nodes(ClusterNode& node) {
  return std::make_unique<heads::ControlTreeNodeStore>(heads::ControlTreeNodeStore::for_commit(node.control(), node.store()));
}

std::function<std::unique_ptr<heads::TreeNodeStore>()> reader_of(ClusterNode& node) {
  return [&node]() -> std::unique_ptr<heads::TreeNodeStore> {
    return std::make_unique<heads::ControlTreeNodeStore>(heads::ControlTreeNodeStore::for_reading(node.control(), node.store()));
  };
}

// What S5's founding will make: a generation 2 tree head with no dot, by hand.
void found(ClusterNode& node, Space& space, const NeutralEntries& entries = {{"a", make_group(1)}}) {
  NeutralHead head;
  head.floor = 1;
  const auto writer = commit_nodes(node);
  head.root = neutral_tree().build(entries, *writer);
  auto record = test::record_of(2, space.replica().committed().hash, head);
  ASSERT_TRUE(space.replica().store_commit(record));
  ASSERT_TRUE(space.replica().accept_commit({record.generation, record.hash, 1, {node.id()}}));
}

// A DATA object on this node, as a write leaves it.
store::ObjectId write_object(ClusterNode& node, uint64_t n) {
  const auto data = test::numbered(n);
  const auto id = store::object_id(data);
  store::WriteOptions write;
  write.ack_level = store::Durability::persisted;
  EXPECT_TRUE(node.store().write(id, data, write).met);
  return id;
}

NeutralItem item_naming(const std::vector<store::ObjectId>& ids, bool with_gap) {
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
  if (with_gap) {
    NeutralRef gap;
    gap.start = start;
    gap.span = 8192;
    gap.gap = true;
    item.refs.push_back(gap);
  }
  return item;
}

// Holds in its constructor call of `genesis()`, which replay starts from.
class HeldCodec final : public heads::Codec {
 public:
  explicit HeldCodec(dannan::test::Gate& gate) : _gate(gate) {}
  heads::Bytes genesis() const override {
    _gate.enter_and_wait();
    return _inner.genesis();
  }
  heads::State decode_full(std::span<const uint8_t> payload) const override { return _inner.decode_full(payload); }
  heads::Bytes encode_full(const heads::State& state) const override { return _inner.encode_full(state); }
  std::optional<heads::Bytes> encode_delta(const heads::State& before, const heads::State& after) const override { return _inner.encode_delta(before, after); }
  void check_delta(std::span<const uint8_t> delta) const override { _inner.check_delta(delta); }
  heads::State apply_delta(const heads::State& parent, std::span<const uint8_t> delta, heads::TreeNodeStore* tree) const override {
    return _inner.apply_delta(parent, delta, tree);
  }
  heads::Bytes encode_after_delta(std::span<const uint8_t> delta, const heads::State& state) const override { return _inner.encode_after_delta(delta, state); }
  heads::HeadFields head_fields(const heads::State& state) const override { return _inner.head_fields(state); }
  uint64_t resident_bytes(const heads::State& state) const override { return _inner.resident_bytes(state); }

 private:
  dannan::test::Gate& _gate;
  NeutralCodec _inner;
};

// C21. Replay runs the owner's codec, so the plane is marked honestly only after it.
TEST(NodeHeads, TheMetadataPlaneIsNotReadyBeforeReplayReturns) {
  dannan::test::Gate held;
  dannan::test::GateOpener opener(held);
  Cluster cluster;
  ClusterNodeOptions options;
  options.open_later = true;
  options.spaces = {neutral_spec("test/neutral", std::make_shared<HeldCodec>(held))};
  auto& a = cluster.add("a", options);
  a.start();
  auto& readiness = a.context().readiness();
  std::thread opening([&] { a.dagda_node().open_store(); });
  ASSERT_TRUE(held.wait_for_entries(1));
  // Everything before it is ready, and it is not.
  EXPECT_TRUE(readiness.has("retention"));
  EXPECT_FALSE(readiness.has("metadata"));
  EXPECT_FALSE(readiness.complete());
  held.open();
  opening.join();
  EXPECT_TRUE(readiness.has("metadata"));
  EXPECT_TRUE(readiness.complete());
  EXPECT_FALSE(readiness.failed());
}

// C22.
TEST(NodeHeads, WithNoSpacesThePlaneIsReadyAtOnce) {
  Cluster cluster;
  auto& a = cluster.add("a");
  a.start();
  EXPECT_TRUE(a.context().readiness().has("metadata"));
  EXPECT_TRUE(a.context().readiness().complete());
  EXPECT_NE(a.dagda_node().spaces(), nullptr);
}

// C23. Primary state bad and no valid seed (md.cpp:2000-2004, :2033-2035): the node is
// refused with the reason, and readiness fails naming it.
TEST(NodeHeads, ASpaceThatCannotOpenFailsReadinessAndNamesIt) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {neutral_spec()};
  auto& a = cluster.add("a", options);
  a.start();
  auto& space = space_on(a);
  found(a, space);
  const auto directory = heads::space_directory(a.state(), a.api().heads().id_of("test/neutral"));
  a.stop();
  {
    std::ofstream damage(directory / "checkpoint.meta", std::ios::binary | std::ios::trunc);
    damage << "not a checkpoint";
  }
  a.options().open_later = true;
  a.start();
  try {
    a.dagda_node().open_store();
    ADD_FAILURE() << "not refused";
  } catch (const NodeRefused& refused) {
    const std::string why = refused.what();
    EXPECT_NE(why.find("test/neutral"), std::string::npos) << why;
    EXPECT_NE(why.find("checkpoint.meta"), std::string::npos) << why;
  }
  EXPECT_TRUE(a.context().readiness().failed());
  EXPECT_NE(a.context().readiness().error().find("test/neutral"), std::string::npos) << a.context().readiness().error();
  EXPECT_FALSE(a.context().readiness().has("metadata"));
}

// C24. Off: no plane, no stage, and a Heads noun refuses; an owner declares and marks its
// own plane of that name. `startup.metadata` is proved by Macha's facade at the port.
// No message types are registered either way until S5.
TEST(NodeHeads, OffTheNodeDeclaresNothingAndAnOwnersPlaneWorks) {
  std::vector<std::string> stages;
  Cluster cluster;
  ClusterNodeOptions options;
  options.heads = false;
  options.trace = [&](std::string_view what) { stages.emplace_back(what); };
  auto& a = cluster.add("a", options);
  a.open();
  auto& readiness = a.context().readiness();
  EXPECT_FALSE(readiness.has("metadata"));
  readiness.declare("metadata");
  a.start();
  EXPECT_EQ(std::count(stages.begin(), stages.end(), "open heads"), 0);
  EXPECT_EQ(a.dagda_node().spaces(), nullptr);
  // Every plane of the node's is ready and the owner's is not.
  for (const auto* plane : {"data_storage", "control_storage", "cache", "retention"}) EXPECT_TRUE(readiness.has(plane));
  EXPECT_FALSE(readiness.complete());
  readiness.ready("metadata");
  EXPECT_TRUE(readiness.complete());

  auto& heads = a.api().heads();
  const auto opened = heads.open(neutral_spec(), deadline);
  EXPECT_FALSE(opened.opened);
  EXPECT_NE(opened.why.find("heads off"), std::string::npos) << opened.why;
  const auto id = heads.id_of("test/neutral");
  EXPECT_FALSE(heads.current(id).finished);
  EXPECT_FALSE(heads.heads(id).known);
  EXPECT_FALSE(heads.mutate_delta(id, neutral_mutation([](NeutralEditing&) {}), deadline).finished);
  a.stop();
  EXPECT_EQ(std::count(stages.begin(), stages.end(), "stop heads"), 0);
}

// C9 at the contract: refused with the reason, and every-node opens at any time, gating
// nothing. A second open of a name says so.
TEST(NodeHeads, TheContractOpensAnEveryNodeSpaceAndRefusesReplicas) {
  Cluster cluster;
  auto& a = cluster.add("a");
  a.start();
  auto& heads = a.api().heads();
  auto spec = neutral_spec();
  spec.policy = heads::Policy::replicas_of(2);
  const auto refused = heads.open(spec, deadline);
  EXPECT_FALSE(refused.opened);
  EXPECT_NE(refused.why.find("L2"), std::string::npos) << refused.why;
  EXPECT_FALSE(heads.heads(heads.id_of("test/neutral")).known);

  const auto opened = heads.open(neutral_spec(), deadline);
  EXPECT_TRUE(opened.opened) << opened.why;
  EXPECT_EQ(opened.generation, 1U);
  EXPECT_EQ(opened.space, heads.id_of("test/neutral"));
  EXPECT_TRUE(std::filesystem::exists(heads::space_directory(a.state(), opened.space) / "history.log"));
  const auto again = heads.open(neutral_spec(), deadline);
  EXPECT_FALSE(again.opened);
  EXPECT_NE(again.why.find("open already"), std::string::npos) << again.why;
}

// The executor rule and a deadline: a mutation's completion is posted to the caller's
// executor, never inline, and the deadline answers a mutation still held.
TEST(NodeHeads, TheContractCompletesOnTheCallersExecutorAndByTheDeadline) {
  dannan::test::Gate held;
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {neutral_spec()};
  auto& a = cluster.add("a", options);
  a.start();
  auto& space = space_on(a);
  found(a, space);
  auto& heads = a.api().heads();
  const auto id = heads.id_of("test/neutral");

  boost::asio::io_context executor;
  const auto keep_running = boost::asio::make_work_guard(executor);
  std::optional<MutationEvidence> evidence;
  std::thread::id completed_on;
  heads.mutate_delta(id, neutral_mutation([](NeutralEditing& editing) { editing.set_note("k", "v"); }), deadline, executor.get_executor(),
                     [&](MutationEvidence got) {
                       completed_on = std::this_thread::get_id();
                       evidence = std::move(got);
                     });
  // Not answered until the executor runs it, here on this thread.
  EXPECT_FALSE(evidence);
  for (int i = 0; i < 2000 && !evidence; ++i) (void)executor.run_one_for(10ms);
  ASSERT_TRUE(evidence);
  EXPECT_EQ(completed_on, std::this_thread::get_id());
  ASSERT_TRUE(evidence->finished) << evidence->why;
  EXPECT_EQ(evidence->outcome->kind, CommitOutcome::Kind::committed);
  EXPECT_EQ(evidence->outcome->record.generation, 3U);

  EXPECT_THROW(heads.mutate_delta(id, neutral_mutation([](NeutralEditing&) {}), 0ms), std::invalid_argument);
  EXPECT_THROW(heads.read(id, HeadRead::local, -1ms), std::invalid_argument);

  // Held in its edit, a mutation answers at its deadline and may yet land.
  dannan::test::GateOpener opener(held);
  const auto late = heads.mutate_delta(id, neutral_mutation([&](NeutralEditing&) { held.enter_and_wait(); }), 100ms);
  EXPECT_FALSE(late.finished);
  EXPECT_NE(late.why.find("deadline passed"), std::string::npos) << late.why;
  EXPECT_NE(late.why.find("may yet land"), std::string::npos) << late.why;
  // And a read queued behind it finds the same: no call waits past its deadline.
  const auto read = heads.read(id, HeadRead::local, 50ms);
  EXPECT_FALSE(read.finished);
  held.open();
  // Then it lands.
  for (int i = 0; i < 1000 && space.replica().committed().generation < 4; ++i) (void)heads.read(id, HeadRead::local, deadline);
  EXPECT_EQ(space.replica().committed().generation, 4U);
}

// A stop reaches the caller as what it is: the async form cannot throw it, and its evidence
// says so rather than calling it a refusal.
TEST(NodeHeads, AStopInAMutationIsEvidenceOfAStop) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {neutral_spec()};
  auto& a = cluster.add("a", options);
  a.start();
  auto& space = space_on(a);
  found(a, space);
  const auto before = space.replica().committed();
  const auto stopped = a.api().heads().mutate_delta(a.api().heads().id_of("test/neutral"),
                                                    neutral_mutation([](NeutralEditing&) { throw contract::MaintenanceStopping(); }), deadline);
  EXPECT_FALSE(stopped.finished);
  EXPECT_TRUE(stopped.stopped);
  EXPECT_EQ(space.replica().committed().hash, before.hash);
}

// The reads through the contract: finding nothing is not a failure, and a space not
// open says so.
TEST(NodeHeads, TheContractsReadsAndHeadsAreEvidence) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {neutral_spec()};
  auto& a = cluster.add("a", options);
  a.start();
  auto& heads = a.api().heads();
  const auto id = heads.id_of("test/neutral");

  const auto none = heads.current(heads.id_of("no such space"));
  EXPECT_FALSE(none.finished);
  EXPECT_NE(none.why.find("no such head space"), std::string::npos) << none.why;
  // Open, with no head decoded yet: nothing found, and that is a success.
  const auto empty = heads.current(id);
  EXPECT_TRUE(empty.finished);
  EXPECT_FALSE(empty.view);

  auto& space = space_on(a);
  found(a, space);
  const auto noted = heads.mutate(id, neutral_mutation([](NeutralEditing& editing) { editing.set_note("k", "v"); }), deadline);
  ASSERT_TRUE(noted.finished) << noted.why;
  const auto hash = noted.outcome->record.hash;
  EXPECT_EQ(heads.current(id).view->hash, hash);
  for (const auto how : {HeadRead::current, HeadRead::local, HeadRead::converged}) {
    const auto read = heads.read(id, how, deadline);
    ASSERT_TRUE(read.finished) << read.why;
    EXPECT_EQ(read.view->hash, hash);
    EXPECT_EQ(head_of(*read.view).notes.get().at("k"), "v");
  }
  const auto report = heads.heads(id);
  ASSERT_TRUE(report.known);
  ASSERT_EQ(report.report.certificates.size(), 1U);
  EXPECT_EQ(report.report.certificates.front().hash, hash);
  EXPECT_EQ(report.report.certificates.front().replicas, std::vector<heads::NodeId>{a.id()});
  EXPECT_EQ(report.report.preferred.hash, hash);
  EXPECT_FALSE(report.report.recovery_required);
}

// The wait guard on the node's path, with its mode a test's: a control read that would
// wait on the network is refused as evidence.
TEST(NodeHeads, TheContractEntersTheWaitGuard) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {neutral_spec()};
  auto& a = cluster.add("a", options);
  a.start();
  auto& space = space_on(a);
  found(a, space);
  auto* spaces = a.dagda_node().spaces();
  spaces->guard().set_mode(dannan::sched::WaitGuard::Mode::throw_on_violation);
  const auto id = a.api().heads().id_of("test/neutral");
  const auto refused = a.api().heads().read(id, HeadRead::converged, deadline, dannan::sched::WorkContext(dannan::sched::FrameType::control));
  EXPECT_FALSE(refused.finished);
  EXPECT_EQ(spaces->guard().violations(), 1U);
  EXPECT_TRUE(a.api().heads().read(id, HeadRead::local, deadline, dannan::sched::WorkContext(dannan::sched::FrameType::control)).finished);
  EXPECT_EQ(spaces->guard().violations(), 1U);
}

// test_a_commit_is_published_only_once_its_objects_are_claimed
// (test_node_services.cpp:362), on the node's real claims. Every object the commit refers
// to is claimed under its dot; a gap (Macha's hole) is not an object and claims nothing;
// an object nobody holds refuses the commit in Macha's words, unpublished. Macha's file
// and its truncate are owner's: an item with an object and a gap stands in, and "a hole
// is not an object" stays Macha's too, through FileSystem.
TEST(NodeHeads, ACommitIsPublishedOnlyOnceItsObjectsAreClaimed) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {neutral_spec()};
  auto& a = cluster.add("a", options);
  a.start();
  auto& space = space_on(a);
  found(a, space);
  auto& claims = a.claims();
  const auto id = write_object(a, 41);

  const auto sparse = space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["sparse"] = item_naming({id}, true); }));
  ASSERT_EQ(sparse.kind, CommitOutcome::Kind::committed);
  const auto reader = reader_of(a)();
  const auto stored = neutral_tree().lookup(*head_of(*space.current()).root, "sparse", *reader);
  ASSERT_TRUE(stored);
  size_t gaps = 0;
  size_t data = 0;
  const auto sequence = head_of(*space.current()).clock.at(sparse.dot->author);
  for (const auto& ref : stored->refs) {
    if (ref.gap) {
      ++gaps;
      continue;
    }
    ++data;
    EXPECT_EQ(claims.claims(contract::RetentionClass::data, ref.id).adds.at(sparse.dot->author), sequence);
  }
  EXPECT_GE(gaps, 1U);
  EXPECT_EQ(data, 1U);
  EXPECT_EQ(claims.claim_objects(contract::RetentionClass::data), 1U);
  // The tree nodes it wrote are claimed here under the same dot.
  ASSERT_FALSE(sparse.claims.control.empty());
  for (const auto& node : sparse.claims.control) {
    EXPECT_TRUE(a.control().has(node));
    EXPECT_EQ(claims.claims(contract::RetentionClass::control, node).adds.at(sparse.dot->author), sequence);
  }

  // An entry bringing in an object nobody holds is refused and unpublished, and the
  // barrier says why.
  const auto phantom = store::object_id(test::numbered(42));
  const auto head = space.replica().committed();
  std::string refusal;
  try {
    space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["phantom"] = item_naming({phantom}, false); }));
  } catch (const heads::HeadsNotReady& error) {
    refusal = error.what();
  }
  EXPECT_EQ(refusal, "DATA object is held by no node present before metadata publication");
  EXPECT_EQ(space.replica().committed().hash, head.hash);
  EXPECT_FALSE(neutral_tree().lookup(*head_of(*space.current()).root, "phantom", *reader));
  EXPECT_EQ(claims.claim_objects(contract::RetentionClass::data), 1U);
  EXPECT_TRUE(test::saw(a.log(), "metadata retention barrier"));
  EXPECT_TRUE(test::saw(a.log(), "data_objects=1"));
  EXPECT_TRUE(test::saw(a.log(), "outcome=data-unheld"));
}

// H22: DATA "on a node present": a peer holds the object, so the commit stands, and
// its claim lands there.
TEST(NodeHeads, AnObjectHeldByANodePresentIsClaimedThere) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {neutral_spec()};
  options.store.write_copies = 1;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b");
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  auto& space = space_on(a);
  found(a, space);
  const auto data = test::numbered(50);
  const auto id = store::object_id(data);
  ASSERT_TRUE(b.data().put(id, data));
  ASSERT_FALSE(a.data().has(id));
  const auto outcome = space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["there"] = item_naming({id}, false); }));
  EXPECT_EQ(outcome.kind, CommitOutcome::Kind::committed);
  EXPECT_TRUE(b.claims().retained(contract::RetentionClass::data, id));
}

// H19, H25 through the node's stores: a restart replays the deltas through the control
// store; a damaged checkpoint recovers from the cache seed and is not authority.
TEST(NodeHeads, ARestartReplaysThroughTheControlStoreAndTheSeedRecovers) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {neutral_spec()};
  auto& a = cluster.add("a", options);
  a.options().cache.path = a.state().parent_path() / "cache";
  a.options().cache.max_blocks = 16;
  a.start();
  auto& space = space_on(a);
  found(a, space);
  heads::Record head;
  for (int i = 0; i < 3; ++i)
    head = space
               .mutate_delta(neutral_mutation(
                   [i](NeutralEditing& editing) { editing.delta.upsert_keys["k" + std::to_string(i)] = make_group(static_cast<uint64_t>(i)); }))
               .record;
  const auto id = a.api().heads().id_of("test/neutral");
  const auto directory = heads::space_directory(a.state(), id);
  const auto seed_file = a.options().cache.path / "metadata" / (dannan::encoding::hex(id.bytes) + ".meta");
  EXPECT_TRUE(std::filesystem::exists(seed_file));
  // Each commit that moves the committed record is remembered as it happens.
  heads::CacheSeed seed(a.cache(), a.dagda_node().options().key, a.dagda_node().options().domain.id, test::neutral_seed_magic, test::quiet_logger());
  const auto remembered = seed.load(id);
  ASSERT_TRUE(remembered);
  EXPECT_EQ(remembered->hash, head.hash);
  const auto seeded = std::filesystem::last_write_time(seed_file);
  a.stop();
  // Written at open from the committed record, as well as on each commit that moves it.
  std::filesystem::remove(seed_file);
  a.start();
  EXPECT_TRUE(std::filesystem::exists(seed_file));
  EXPECT_GE(std::filesystem::last_write_time(seed_file), seeded);
  {
    auto& reopened = space_on(a);
    EXPECT_EQ(reopened.replica().committed().hash, head.hash);
    const auto view = reopened.local();
    EXPECT_EQ(view.hash, head.hash);
    const auto reader = reader_of(a)();
    EXPECT_TRUE(neutral_tree().lookup(*head_of(view).root, "k2", *reader));
    EXPECT_TRUE(a.context().readiness().has("metadata"));
  }

  a.stop();
  {
    std::ofstream damage(directory / "checkpoint.meta", std::ios::binary | std::ios::trunc);
    damage << "not a checkpoint";
  }
  a.start();
  auto& recovered = space_on(a);
  EXPECT_TRUE(recovered.replica().recovery_required());
  EXPECT_FALSE(recovered.replica().acceptance(head.hash));
}

// test_tombstones_live_in_batches_in_a_tree_backed_head (NS:336). A retiring mutation
// writes one batch, a CONTROL object, which survives a restart. Maintenance reads every
// retirement from the batches, through the provider, and past their grace drops them
// whole. Macha's half through FileSystem::unlink stays Macha's: a mutation that erases a
// key and retires what it named stands in. Macha's `garbage` list is its owner's slot,
// and the neutral owner has none, so `head.garbage.empty()` has nothing to assert here.
TEST(NodeHeads, RetirementsLiveInBatchesInATreeBackedHead) {
  dannan::test::SteppedWall wall(dannan::contract::system_wall_clock().unix_ms());
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {neutral_spec()};
  options.wall = &wall;
  options.garbage_grace = 20s;
  auto& a = cluster.add("a", options);
  a.start();

  std::vector<store::ObjectId> before_restart;
  {
    auto& space = space_on(a);
    found(a, space);
    for (uint64_t i = 0; i < 3; ++i) before_restart.push_back(write_object(a, 100 + i));
    (void)space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["before"] = item_naming(before_restart, false); }));
    (void)space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) {
      editing.delta.erase_keys.push_back("before");
      editing.retire.push_back({true, 0, before_restart});
    }));
    EXPECT_FALSE(neutral_batches(head_of(*space.current())).empty());
  }
  a.restart();
  auto& space = space_on(a);
  ASSERT_TRUE(head_of(space.local()).root);
  ASSERT_FALSE(neutral_batches(head_of(space.local())).empty());

  // A retirement in the tree writes one batch naming its objects.
  std::vector<store::ObjectId> after_restart;
  for (uint64_t i = 0; i < 4; ++i) after_restart.push_back(write_object(a, 200 + i));
  (void)space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["after"] = item_naming(after_restart, false); }));
  auto sorted_after = after_restart;
  std::sort(sorted_after.begin(), sorted_after.end());
  const auto batches_before = neutral_batches(head_of(space.local()));
  const auto retired = space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) {
    editing.delta.erase_keys.push_back("after");
    editing.retire.push_back({true, 0, after_restart});
  }));
  const auto batches_after = neutral_batches(head_of(*space.current()));
  std::vector<heads::GarbageBatch> added;
  std::set_difference(batches_after.begin(), batches_after.end(), batches_before.begin(), batches_before.end(), std::back_inserter(added));
  ASSERT_EQ(added.size(), 1U);
  EXPECT_EQ(added.front().count, sorted_after.size());
  const auto object = a.control().get(added.front().id);
  ASSERT_TRUE(object);
  EXPECT_EQ(heads::decode_retirement_batch(test::neutral_batch_magic, *object).ids, sorted_after);
  // Claimed here as what the head names.
  EXPECT_TRUE(a.claims().retained(contract::RetentionClass::control, added.front().id));
  EXPECT_NE(std::find(retired.claims.control.begin(), retired.claims.control.end(), added.front().id), retired.claims.control.end());
  // The head stays small: batches, not retirements.
  const auto payload = space.replica().committed().payload;
  EXPECT_LT(payload.size(), 4096U);
  EXPECT_EQ(neutral_batches(test::neutral_head_from(payload)), batches_after);

  // Maintenance sees every retirement, from the batches.
  test::NeutralSpaceProvider provider(space, reader_of(a));
  a.dagda_node().register_reachability(domain::domain_v1, provider);
  auto report = a.run_maintenance_once();
  ASSERT_TRUE(report.gc) << report.gc_skipped;
  const auto has = [&](const store::ObjectId& id) {
    return std::any_of(provider.latest.garbage.begin(), provider.latest.garbage.end(), [&](const contract::Garbage& one) { return one.id == id; });
  };
  EXPECT_TRUE(std::all_of(after_restart.begin(), after_restart.end(), has));
  EXPECT_TRUE(std::all_of(before_restart.begin(), before_restart.end(), has));
  EXPECT_EQ(provider.latest.batches, batches_after);

  // Past their grace, maintenance drops the batches whole.
  wall.advance(21s);
  for (int pass = 0; pass < 8 && !neutral_batches(head_of(space.local())).empty(); ++pass) {
    report = a.run_maintenance_once();
    ASSERT_TRUE(report.gc) << report.gc_skipped;
  }
  EXPECT_TRUE(neutral_batches(head_of(space.local())).empty());
  a.dagda_node().unregister_reachability(domain::domain_v1);
}

// test_a_stale_tombstone_in_a_batch_is_erased_once (NS:421), the half through the
// provider (5.3). S5 runs it on the node's loop; FileSystem's half stays Macha's. Macha
// waits out real seconds to see the generation hold; here maintenance is stepped, so
// "no further commit" is said pass by pass.
TEST(NodeHeads, AStaleRetirementInABatchIsErasedOnce) {
  dannan::test::SteppedWall wall(dannan::contract::system_wall_clock().unix_ms());
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {neutral_spec()};
  options.wall = &wall;
  // Nothing matures: only staleness erases a retirement here.
  options.garbage_grace = 1h;
  auto& a = cluster.add("a", options);
  a.start();
  auto& space = space_on(a);
  found(a, space);
  const auto revived = write_object(a, 31);
  const auto other = write_object(a, 32);
  ASSERT_NE(revived, other);
  (void)space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["gone"] = item_naming({revived, other}, false); }));
  (void)space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) {
    editing.delta.erase_keys.push_back("gone");
    editing.retire.push_back({true, 0, {revived, other}});
  }));
  const auto batches_before = neutral_batches(head_of(*space.current()));
  ASSERT_FALSE(batches_before.empty());

  // The first object's content is live again: its retirement is stale.
  (void)space.mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["again"] = item_naming({revived}, false); }));

  test::NeutralSpaceProvider provider(space, reader_of(a));
  a.dagda_node().register_reachability(domain::domain_v1, provider);
  const auto retired_again = [&](const store::ObjectId& id) {
    const auto view = space.local();
    const auto reader = reader_of(a)();
    const auto all = heads::retirements_of(neutral_batches(head_of(view)), *reader, test::neutral_batch_magic);
    return std::any_of(all.begin(), all.end(), [&](const contract::Garbage& one) { return one.id == id; });
  };
  ASSERT_TRUE(retired_again(revived));
  for (int pass = 0; pass < 8 && retired_again(revived); ++pass) {
    const auto report = a.run_maintenance_once();
    ASSERT_TRUE(report.gc) << report.gc_skipped;
  }
  EXPECT_FALSE(retired_again(revived));
  // The rest of the batch was written again, not lost.
  EXPECT_TRUE(retired_again(other));
  const auto batches_after = neutral_batches(head_of(space.local()));
  EXPECT_NE(batches_after, batches_before);
  EXPECT_LT(space.replica().committed().payload.size(), 4096U);
  ASSERT_EQ(batches_after.size(), 1U);
  EXPECT_EQ(batches_after.front().retired_at_ns, batches_before.front().retired_at_ns);

  // With the stale retirement gone the pass has nothing to say: no commit follows.
  const auto settled = space.replica().committed().generation;
  for (int pass = 0; pass < 4; ++pass) {
    const auto report = a.run_maintenance_once();
    ASSERT_TRUE(report.gc) << report.gc_skipped;
  }
  EXPECT_EQ(space.replica().committed().generation, settled);
  EXPECT_FALSE(retired_again(revived));
  EXPECT_TRUE(retired_again(other));
  a.dagda_node().unregister_reachability(domain::domain_v1);
}

}  // namespace
}  // namespace dagda
