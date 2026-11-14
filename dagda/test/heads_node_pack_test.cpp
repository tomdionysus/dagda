//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A node-local space's tree nodes (slice 1, D3; the three-systems design, "How slice 1
// builds node_local"): its own sealed file, never the shared control store, so nothing
// of it is offered, served or repaired, and collected by the space against what it
// still reaches.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "dagda/heads/node_pack.h"
#include "dagda/store/object_id.h"
#include "dagda/test/fixtures/heads.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::heads {
namespace {

using test::key_of;

Bytes node_bytes(const std::string& text) { return Bytes(text.begin(), text.end()); }

Bytes aad() { return node_bytes("dagda/test/node-pack"); }

TEST(HeadsNodePack, APutIsReadBackHereAndAfterReopening) {
  dannan::test::TempDir dir;
  ObjectId id{};
  {
    NodePack pack(dir.path(), key_of(7), aad(), test::quiet_logger());
    id = pack.put(node_bytes("a node"));
    EXPECT_EQ(id, store::object_id(node_bytes("a node")));
    EXPECT_EQ(pack.get(id), node_bytes("a node"));
  }
  NodePack reopened(dir.path(), key_of(7), aad(), test::quiet_logger());
  EXPECT_EQ(reopened.get(id), node_bytes("a node"));
  EXPECT_EQ(reopened.nodes(), 1U);
}

TEST(HeadsNodePack, HeldBytesAreNotWrittenTwiceAndAnUnknownIdIsEmpty) {
  dannan::test::TempDir dir;
  NodePack pack(dir.path(), key_of(7), aad(), test::quiet_logger());
  const auto id = pack.put(node_bytes("once"));
  const auto bytes = pack.bytes();
  EXPECT_EQ(pack.put(node_bytes("once")), id);
  EXPECT_EQ(pack.bytes(), bytes);
  EXPECT_FALSE(pack.get(store::object_id(node_bytes("never put"))));
}

// Sealed: the plain node is nowhere in the file, and another key, or another space's
// associated data, does not open it.
TEST(HeadsNodePack, ItsFileIsSealedAndOpensOnlyUnderItsKeyAndSpace) {
  dannan::test::TempDir dir;
  {
    NodePack pack(dir.path(), key_of(7), aad(), test::quiet_logger());
    pack.put(node_bytes("secret grant"));
  }
  std::ifstream in(NodePack::file(dir.path()), std::ios::binary);
  const std::string on_disk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(on_disk.find("secret grant"), std::string::npos);
  EXPECT_THROW(NodePack(dir.path(), key_of(8), aad(), test::quiet_logger()), std::runtime_error);
  EXPECT_THROW(NodePack(dir.path(), key_of(7), node_bytes("another space"), test::quiet_logger()), std::runtime_error);
}

// A put cut short by a crash leaves a partial frame at the end: it was never
// acknowledged, so it is cut at open and what came before stands.
TEST(HeadsNodePack, APartialLastFrameIsCutAtOpen) {
  dannan::test::TempDir dir;
  ObjectId kept{};
  {
    NodePack pack(dir.path(), key_of(7), aad(), test::quiet_logger());
    kept = pack.put(node_bytes("kept"));
    pack.put(node_bytes("cut short by a crash"));
  }
  const auto path = NodePack::file(dir.path());
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 5);
  NodePack reopened(dir.path(), key_of(7), aad(), test::quiet_logger());
  EXPECT_EQ(reopened.get(kept), node_bytes("kept"));
  EXPECT_EQ(reopened.nodes(), 1U);
  // And it writes on after the cut.
  const auto again = reopened.put(node_bytes("after"));
  EXPECT_EQ(NodePack(dir.path(), key_of(7), aad(), test::quiet_logger()).get(again), node_bytes("after"));
}

// Damage before the end is not a crash's: the pack refuses to open rather than drop
// what lies after it.
TEST(HeadsNodePack, DamageBeforeTheEndIsRefused) {
  dannan::test::TempDir dir;
  {
    NodePack pack(dir.path(), key_of(7), aad(), test::quiet_logger());
    pack.put(node_bytes("first"));
    pack.put(node_bytes("second"));
  }
  const auto path = NodePack::file(dir.path());
  {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(10);
    file.put('\x5a');
  }
  EXPECT_THROW(NodePack(dir.path(), key_of(7), aad(), test::quiet_logger()), std::runtime_error);
}

// Compaction keeps exactly the live nodes, the file shrinks to them, and a reopen sees
// the same.
TEST(HeadsNodePack, CompactionKeepsOnlyTheLiveNodes) {
  dannan::test::TempDir dir;
  NodePack pack(dir.path(), key_of(7), aad(), test::quiet_logger());
  std::vector<ObjectId> ids;
  for (int i = 0; i < 100; ++i) ids.push_back(pack.put(node_bytes("node " + std::to_string(i) + std::string(200, '.'))));
  const auto before = pack.bytes();
  const std::set<ObjectId> live{ids[3], ids[50], ids[99]};
  const auto result = pack.compact(live);
  EXPECT_EQ(result.kept, 3U);
  EXPECT_EQ(result.dropped, 97U);
  EXPECT_LT(pack.bytes(), before / 10);
  EXPECT_EQ(std::filesystem::file_size(NodePack::file(dir.path())), pack.bytes());
  for (const auto& id : ids) EXPECT_EQ(pack.get(id).has_value(), live.contains(id));
  NodePack reopened(dir.path(), key_of(7), aad(), test::quiet_logger());
  EXPECT_EQ(reopened.nodes(), 3U);
  EXPECT_EQ(reopened.get(ids[50]), node_bytes("node 50" + std::string(200, '.')));
}

// The pack as a tree's node store: a tree built into it is read back through it.
TEST(HeadsNodePack, ATreeIsBuiltAndReadThroughIt) {
  dannan::test::TempDir dir;
  NodePack pack(dir.path(), key_of(7), aad(), test::quiet_logger());
  test::NeutralEntries entries;
  for (int i = 0; i < 500; ++i) entries["k" + std::to_string(1000 + i)] = test::make_group(i);
  const auto writer = pack.factory()();
  const auto root = test::neutral_tree().build(entries, *writer);
  const auto reader = pack.factory()();
  const auto found = test::neutral_tree().lookup(root, "k1234", *reader);
  ASSERT_TRUE(found);
  EXPECT_EQ(*found, test::make_group(234));
}

}  // namespace
}  // namespace dagda::heads
