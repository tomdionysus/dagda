//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement L24, cut at seam 7: the horizon builder's two builds from an
// owner's ReachabilityProvider.
//
// Moved from Macha's tests/test_horizon_builder.cpp (develop 5a06d3c):
// test_inventory_is_both_live_sets_and_the_catalogue_control_set,
// test_inventory_control_set_holds_the_namespace_tree,
// test_release_over_a_tree_matches_the_pass_build,
// test_an_unreadable_catalogue_root_leaves_the_release_incomplete,
// test_an_unreadable_tree_node_leaves_the_release_incomplete,
// test_an_empty_head_releases_against_nothing. Macha built these from its
// filesystem and catalogue; here what those answered is the provider's
// answer, and every assertion about the horizon is kept. What the cases
// proved of the FS's own walk (its tree reaching every node, extents and
// holes, conflict alternatives, the catalogue's root set) is the owner's and
// stays Macha's; each case below says what it gives the ledger instead.

#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>

#include "dagda/ledger/node_horizon_builder.h"
#include "dagda/ledger/object_trie.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/reachability.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

using contract::Garbage;
using contract::Reachability;
using contract::RetentionClass;

constexpr auto data = RetentionClass::data;
constexpr auto control = RetentionClass::control;

ObjectId id(uint8_t n) {
  ObjectId out{};
  out.bytes[0] = n;
  return out;
}

std::vector<ObjectId> ids_of(std::span<const ObjectId> span) { return {span.begin(), span.end()}; }

// A frozen view of a counted trie holding `ids`.
ObjectTrie::Snapshot view_of(const std::filesystem::path& dir, const std::vector<ObjectId>& ids) {
  ObjectTrie trie(dir, test::key_of(0), {}, test::quiet_logger());
  std::vector<ObjectTrie::Change> changes;
  for (const auto& object : ids) changes.push_back({object, Bytes(8, 1)});
  trie.apply(changes);
  return trie.snapshot();
}

// Macha: test_inventory_is_both_live_sets_and_the_catalogue_control_set. The
// owner's counted DATA, its other DATA and its CONTROL make the inventory; a
// retirement whose object any of them refers to again is stale.
TEST(LedgerHorizonBuilder, TheInventoryIsBothLiveSetsAndTheControlSet) {
  dannan::test::TempDir dir;
  Reachability answer;
  answer.views.data = view_of(dir.path() / "extents", {id(3), id(1)});
  answer.generation = 9;
  answer.garbage = {Garbage{id(2), 5, {}}, Garbage{id(8), 6, {}}};
  answer.data = {id(2), id(3)};
  answer.control = {id(7), id(6)};
  answer.complete = false;

  const auto inventory = build_inventory(answer);
  EXPECT_EQ(inventory->generation(), 9u);
  EXPECT_FALSE(inventory->complete());
  EXPECT_EQ(ids_of(inventory->all(data)), (std::vector<ObjectId>{id(1), id(2), id(3)}));
  EXPECT_EQ(ids_of(inventory->all(control)), (std::vector<ObjectId>{id(6), id(7)}));
  // A retirement revived by other DATA is stale, as one the counts revive is.
  EXPECT_TRUE(inventory->stale_garbage().size() == 1 && inventory->stale_garbage()[0].id == id(2));
  EXPECT_TRUE(inventory->garbage().size() == 1 && inventory->garbage()[0].id == id(8));

  answer.complete = true;
  EXPECT_TRUE(build_inventory(answer)->complete());
}

// Macha: test_inventory_control_set_holds_the_namespace_tree. The owner's
// counted CONTROL nodes join its other CONTROL in the inventory, and its
// batches are referenced CONTROL too; an owner that cannot read a node
// throws, and no inventory is built rather than a shorter one.
TEST(LedgerHorizonBuilder, TheInventoryControlSetHoldsTheOwnersNodes) {
  dannan::test::TempDir dir;
  std::vector<ObjectId> walked;
  for (uint8_t n = 100; n < 140; ++n) walked.push_back(id(n));
  test::TestProvider provider;
  provider.answer.views.control = view_of(dir.path() / "nodes", walked);
  provider.answer.control = {id(7)};
  provider.answer.batches = {contract::GarbageBatch{id(9), 1, 3}};
  provider.answer.complete = true;
  NodeHorizonBuilder builder(provider, test::quiet_logger());
  const auto inventory = builder.inventory();
  EXPECT_EQ(inventory->size(control), walked.size() + 2);
  EXPECT_TRUE(inventory->referenced(control, id(7)));
  EXPECT_TRUE(inventory->referenced(control, id(9)));
  for (const auto& node : walked) EXPECT_TRUE(inventory->referenced(control, node));
  EXPECT_EQ(inventory->batches().size(), 1u);

  provider.inventory_throws = true;
  EXPECT_THROW(builder.inventory(), std::runtime_error);
}

// Macha: test_release_over_a_tree_matches_the_pass_build. The release at a
// head: the DATA and CONTROL it refers to (Macha: its files' extents but not
// holes, conflict alternatives, both catalogue roots' objects, every tree
// node), stamped with the head and its clock.
TEST(LedgerHorizonBuilder, TheReleaseAtAHeadIsWhatItRefersTo) {
  contract::Hash256 head{};
  head.bytes[0] = 0x51;
  contract::NodeId writer{};
  writer.bytes[0] = 1;
  std::vector<ObjectId> nodes{id(200), id(201), id(202)};
  test::TestProvider provider;
  Reachability at;
  // Extents 1, 3, 3, 4; conflict alternatives 6 and 1; catalogue DATA 8, 1
  // and 10.
  at.data = {id(1), id(3), id(3), id(4), id(6), id(1), id(8), id(1), id(10)};
  // Catalogue CONTROL 9, 11, 9, and the tree's nodes.
  at.control = {id(9), id(11), id(9)};
  at.control.insert(at.control.end(), nodes.begin(), nodes.end());
  at.clock = {{writer, 17}};
  at.complete = true;
  provider.heads[head] = at;
  NodeHorizonBuilder builder(provider, test::quiet_logger());
  const auto built = builder.release(head);
  EXPECT_TRUE(built.complete);
  EXPECT_EQ(ids_of(built.horizon->all(data)), (std::vector<ObjectId>{id(1), id(3), id(4), id(6), id(8), id(10)}));
  EXPECT_TRUE(built.horizon->referenced(control, id(9)));
  EXPECT_TRUE(built.horizon->referenced(control, id(11)));
  EXPECT_EQ(built.horizon->size(control), 2 + nodes.size());
  EXPECT_EQ(built.horizon->head(), head);
  EXPECT_EQ(built.horizon->clock(), (contract::RetentionClock{{writer, 17}}));
}

// Macha: test_an_unreadable_catalogue_root_leaves_the_release_incomplete.
// What could be read is still in the horizon, and it is incomplete.
TEST(LedgerHorizonBuilder, AnUnreadablePartLeavesTheReleaseIncomplete) {
  contract::Hash256 head{};
  head.bytes[0] = 0x52;
  test::TestProvider provider;
  Reachability at;
  at.data = {id(1), id(8)};
  at.control = {id(9)};
  at.complete = false;
  provider.heads[head] = at;
  NodeHorizonBuilder builder(provider, test::quiet_logger());
  const auto built = builder.release(head);
  EXPECT_FALSE(built.complete);
  EXPECT_EQ(ids_of(built.horizon->all(data)), (std::vector<ObjectId>{id(1), id(8)}));
  EXPECT_EQ(ids_of(built.horizon->all(control)), (std::vector<ObjectId>{id(9)}));
}

// Macha: test_an_unreadable_tree_node_leaves_the_release_incomplete. An
// owner that cannot answer at all gives an incomplete build at that head,
// never an exception and never a complete empty one.
TEST(LedgerHorizonBuilder, AHeadThatCannotBeReadLeavesTheReleaseIncomplete) {
  contract::Hash256 head{};
  head.bytes[0] = 0x53;
  test::TestProvider provider;
  NodeHorizonBuilder builder(provider, test::quiet_logger());
  const auto built = builder.release(head);
  EXPECT_FALSE(built.complete);
  EXPECT_EQ(built.horizon->head(), head);
  EXPECT_EQ(built.horizon->size(data), 0u);
}

// Macha: test_an_empty_head_releases_against_nothing.
TEST(LedgerHorizonBuilder, AnEmptyHeadReleasesAgainstNothing) {
  contract::Hash256 head{};
  head.bytes[0] = 0x54;
  test::TestProvider provider;
  Reachability empty;
  empty.complete = true;
  provider.heads[head] = empty;
  NodeHorizonBuilder builder(provider, test::quiet_logger());
  const auto built = builder.release(head);
  EXPECT_TRUE(built.complete);
  EXPECT_EQ(built.horizon->size(data), 0u);
  EXPECT_EQ(built.horizon->size(control), 0u);
}

}  // namespace
}  // namespace dagda::ledger
