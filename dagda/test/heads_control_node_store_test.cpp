//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement H19: tree nodes are CONTROL objects. A commit names a root only once
// this node holds every node it wrote; replay writes every node locally and fetches
// what it reads. Macha's control store at 9a08846 (metadata/namespace_control_store),
// which no Macha case tests alone; its gates are S3's and S4's.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#include "dagda/heads/control_node_store.h"
#include "dagda/test/fixtures/store.h"
#include "dagda/test/fixtures/tree.h"

namespace dagda::heads {
namespace {

using test::make_group;
using test::make_item;
using test::neutral_tree;
using test::NeutralEntries;
using test::shaped_entries;

// Puts into the control store what a peer would send, counting what it is asked for.
class PeerSource final : public store::ControlObjectSource {
 public:
  explicit PeerSource(store::LocalStore& control) : _control(control) {}
  bool ensure_control_local(const ObjectId& id) override {
    ++asks;
    const auto found = held.find(id);
    if (found == held.end()) return false;
    return _control.put(id, found->second);
  }
  std::map<ObjectId, Bytes> held;
  size_t asks = 0;

 private:
  store::LocalStore& _control;
};

struct Bed {
  test::StoreBed bed;
  std::unique_ptr<store::LocalStore> control = bed.open(bed.dir.path() / "metadata-objects", store::LocalStoreOptions::loose_only(1ULL << 30));
  PeerSource peers{*control};
};

TEST(HeadsControlNodeStore, AReaderRefusesAWriteAndAskedForWhatItLacksFetchesIt) {
  Bed bed;
  const auto tree = neutral_tree();
  const auto entries = shaped_entries(2, 4);
  auto reader = ControlTreeNodeStore::for_reading(*bed.control, bed.peers);
  EXPECT_THROW((void)reader.put(Bytes{1, 2, 3}), std::logic_error);
  EXPECT_TRUE(reader.written().empty());
  EXPECT_FALSE(bed.control->has(store::object_id(Bytes{1, 2, 3})));

  MemoryTreeNodeStore elsewhere;
  const auto root = tree.build(entries, elsewhere);
  // Nowhere: empty, not a throw, and the peers were asked.
  EXPECT_FALSE(reader.get(root).has_value());
  EXPECT_EQ(bed.peers.asks, 1u);
  // Not here but on a peer: the tree reads through, leaving each node local.
  for (const auto& id : elsewhere.written()) bed.peers.held[id] = *elsewhere.get(id);
  EXPECT_TRUE(tree.read(root, reader) == entries);
  for (const auto& id : elsewhere.written()) EXPECT_TRUE(bed.control->has(id));
  // Local now: a second read asks nobody.
  const auto asked = bed.peers.asks;
  EXPECT_TRUE(tree.read(root, reader) == entries);
  EXPECT_EQ(bed.peers.asks, asked);
}

TEST(HeadsControlNodeStore, ACommitHoldsEveryNodeItsRootNamesAndWritesOnlyWhatIsNew) {
  Bed bed;
  const auto tree = neutral_tree();
  auto entries = shaped_entries(6, 8);
  auto commit = ControlTreeNodeStore::for_commit(*bed.control, bed.peers);
  const auto root = tree.build(entries, commit);
  std::vector<ObjectId> nodes;
  tree.collect_nodes(root, commit, nodes);
  for (const auto& id : nodes) EXPECT_TRUE(bed.control->has(id));
  auto written = commit.written();
  std::sort(written.begin(), written.end());
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  EXPECT_EQ(written, nodes);
  // No peer was asked for anything.
  EXPECT_EQ(bed.peers.asks, 0u);

  // A node already held is not written again, and a further commit writes its path.
  auto again = ControlTreeNodeStore::for_commit(*bed.control, bed.peers);
  EXPECT_EQ(tree.build(entries, again), root);
  EXPECT_TRUE(again.written().empty());
  entries.at("a/all/group 1/part 2/item 1").stamp += 1;
  EXPECT_NE(tree.update(root, again, {{"a/all/group 1/part 2/item 1", entries.at("a/all/group 1/part 2/item 1")}}), root);
  EXPECT_EQ(again.written().size(), tree.stats(tree.build(entries, again), again).depth);
}

TEST(HeadsControlNodeStore, ACommitThatCannotHoldANodeThrowsSoNoRootIsPublished) {
  test::StoreBed bed;
  auto options = store::LocalStoreOptions::loose_only(16);
  auto control = bed.open(bed.dir.path() / "metadata-objects", options);
  PeerSource peers(*control);
  ASSERT_TRUE(test::wait_until([&] { return control->scan_complete(); }));
  const auto tree = neutral_tree();
  auto commit = ControlTreeNodeStore::for_commit(*control, peers);
  auto replay = ControlTreeNodeStore::for_replay(*control, peers);
  EXPECT_THROW((void)tree.build(shaped_entries(2, 2), commit), std::runtime_error);
  EXPECT_THROW((void)tree.build(shaped_entries(2, 2), replay), std::runtime_error);
}

TEST(HeadsControlNodeStore, ReplayWritesEveryNodeItIsGivenAndReachesTheCommittedRoot) {
  Bed committed;
  Bed replayed;
  const auto tree = neutral_tree();
  auto entries = shaped_entries(4, 6);
  auto commit = ControlTreeNodeStore::for_commit(*committed.control, committed.peers);
  const auto first = tree.build(entries, commit);
  auto changed = entries.at("a/all/group 2/part 1/item 4");
  changed.stamp += 1;
  entries["a/all/group 2/part 1/item 4"] = changed;
  const auto second = tree.update(first, commit, {{"a/all/group 2/part 1/item 4", changed}});

  auto replay = ControlTreeNodeStore::for_replay(*replayed.control, replayed.peers);
  EXPECT_EQ(tree.build(entries, replay), second);
  // Replay writes what it is given, whether or not it is held: twice over, each put
  // a write.
  const auto once = replay.written().size();
  EXPECT_GT(once, 0u);
  EXPECT_EQ(tree.build(entries, replay), second);
  EXPECT_EQ(replay.written().size(), 2 * once);
  // Identical nodes to the committed ones.
  for (const auto& id : replay.written()) EXPECT_EQ(replayed.control->get(id), committed.control->get(id));

  // A delta replay reads what it does not have from a peer.
  Bed cold;
  for (const auto& id : commit.written()) cold.peers.held[id] = *committed.control->get(id);
  auto cold_replay = ControlTreeNodeStore::for_replay(*cold.control, cold.peers);
  auto later = changed;
  later.stamp += 1;
  const auto third = tree.update(second, cold_replay, {{"a/all/group 2/part 1/item 4", later}});
  EXPECT_GT(cold.peers.asks, 0u);
  entries["a/all/group 2/part 1/item 4"] = later;
  MemoryTreeNodeStore fresh;
  EXPECT_EQ(third, tree.build(entries, fresh));
}

TEST(HeadsControlNodeStore, TheLocalStoreTouchesNoPeer) {
  Bed bed;
  const auto tree = neutral_tree();
  NeutralEntries entries;
  entries["a"] = make_group(0);
  entries["a/one"] = make_item(1, 30);
  LocalTreeNodeStore local(*bed.control);
  const auto root = tree.build(entries, local);
  EXPECT_FALSE(local.written().empty());
  EXPECT_GT(local.bytes_written(), 0u);
  EXPECT_TRUE(tree.read(root, local) == entries);
  EXPECT_FALSE(local.get(test::fake_object(1)).has_value());
  EXPECT_EQ(bed.peers.asks, 0u);
}

// C7's other half: damage on the disk is the store's refusal, named, and reaches the
// tree's caller as a throw and never as a wrong answer. What a flip of a node's own
// bytes does to its id is in HeadsTree.ACorruptNodeIsRefused.
TEST(HeadsControlNodeStore, ACorruptObjectIsRefusedAndThatPassesThroughTheTree) {
  Bed bed;
  const auto tree = neutral_tree();
  const auto entries = shaped_entries(2, 4);
  auto commit = ControlTreeNodeStore::for_commit(*bed.control, bed.peers);
  const auto root = tree.build(entries, commit);
  for (const auto& id : commit.written()) {
    Bed fresh;
    auto refill = ControlTreeNodeStore::for_commit(*fresh.control, fresh.peers);
    EXPECT_EQ(tree.build(entries, refill), root);
    test::corrupt_object(fresh.bed.dir.path() / "metadata-objects", id);
    auto reader = ControlTreeNodeStore::for_reading(*fresh.control, fresh.peers);
    EXPECT_THROW((void)tree.read(root, reader), std::runtime_error) << store::to_string(id);
  }
}

}  // namespace
}  // namespace dagda::heads
