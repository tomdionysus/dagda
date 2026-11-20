//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// C34, the plan's section 2.5: heads::ReferenceCensus, moved from Macha's FS census
// (filesystem.cpp:2498-2576 at 9a08846), counted by the node store. Macha's gates are
// test_the_inventory_and_the_media_index_follow_the_tree (NS:198) and NT:1436, NT:1170.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/heads/reference_census.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/tree.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::heads {
namespace {

using ledger::ObjectTrie;
using ledger::ReferenceCounts;
using test::key_of;
using test::logger_into;
using test::make_group;
using test::make_item;
using test::neutral_tree;
using test::NeutralEntries;
using test::NeutralItem;
using test::NeutralTree;
using test::RecordingSink;
using test::shaped_entries;

std::map<ObjectId, Bytes> contents(const ObjectTrie::Snapshot& view) {
  std::map<ObjectId, Bytes> out;
  std::optional<ObjectId> after;
  for (;;) {
    const auto page = view.next(after, 97);
    for (const auto& [id, value] : page) out.emplace(id, value);
    if (page.size() < 97) return out;
    after = page.back().first;
  }
}

// One set of counts in its own directory, and a census over it.
struct Bed {
  explicit Bed(const NeutralTree& tree) : counts(dir.path() / "referenced", key_of(9), 1 << 20, logger_into(sink)), census(counts, tree, logger_into(sink)) {}
  dannan::test::TempDir dir;
  std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
  ReferenceCounts counts;
  ReferenceCensus<NeutralItem> census;
};

// What a first count at `root` makes of it: the whole walk, the oracle.
void expect_as_walked(const Bed& bed, const NeutralTree& tree, const ObjectId& root, const TreeNodeStore& store) {
  Bed fresh(tree);
  fresh.census.count(root, store);
  EXPECT_EQ(bed.counts.root(), std::optional(root));
  EXPECT_EQ(bed.counts.totals().entries, fresh.counts.totals().entries);
  EXPECT_EQ(bed.counts.totals().extent_count, fresh.counts.totals().extent_count);
  const auto views = bed.counts.views();
  const auto oracle = fresh.counts.views();
  EXPECT_TRUE(contents(views.extents) == contents(oracle.extents));
  EXPECT_TRUE(contents(views.nodes) == contents(oracle.nodes));
}

// Whether a count equals what the state refers to, computed apart from the census.
void expect_counts_entries(const Bed& bed, const NeutralEntries& entries) {
  uint64_t items = 0;
  std::map<ObjectId, uint64_t> data;
  for (const auto& [_, item] : entries) {
    items += item.refs.size();
    for (const auto& ref : item.refs)
      if (!ref.gap) ++data[ref.id];
  }
  EXPECT_EQ(bed.counts.totals().entries, entries.size());
  EXPECT_EQ(bed.counts.totals().extent_count, items);
  const auto counted = contents(bed.counts.views().extents);
  EXPECT_EQ(counted.size(), data.size());
  for (const auto& [id, count] : data) {
    const auto found = counted.find(id);
    ASSERT_NE(found, counted.end());
    dannan::net::Reader reader(found->second);
    EXPECT_EQ(reader.u64(), count);
  }
}

// C34: follow equals walk after any sequence: inserts, deletes, a value with its
// child sequence, a gap, one shared by two keys.
TEST(HeadsCensus, FollowEqualsWalkAfterAnySequence) {
  const auto tree = neutral_tree();
  auto entries = shaped_entries(6, 6);
  auto gapped = make_item(4242, 12);
  gapped.refs[3].gap = true;
  entries["a/gapped"] = gapped;
  entries["a/shared one"] = make_item(777, 40);
  entries["a/shared two"] = make_item(777, 40);
  MemoryTreeNodeStore store;
  auto root = tree.build(entries, store);
  Bed bed(tree);
  bed.census.count(root, store);
  expect_counts_entries(bed, entries);
  expect_as_walked(bed, tree, root, store);

  std::mt19937_64 random(34);
  for (int round = 0; round < 40; ++round) {
    NeutralTree::Changes changes;
    const auto count = 1 + random() % 8;
    for (uint64_t i = 0; i < count; ++i) {
      auto it = entries.begin();
      std::advance(it, static_cast<std::ptrdiff_t>(random() % entries.size()));
      switch (random() % 4) {
        case 0:
          changes["a/new " + std::to_string(random() % 200)] = make_item(random() % 50, random() % 4 == 0 ? 300 : 2);
          break;
        case 1:
          changes[it->first] = std::nullopt;
          break;
        case 2:
          changes[it->first] = make_item(random() % 20, 12);
          break;
        default:
          changes[it->first] = make_group(random() % 3);
          break;
      }
    }
    for (const auto& [key, value] : changes) {
      if (value) {
        entries[key] = *value;
      } else {
        entries.erase(key);
      }
    }
    root = tree.update(root, store, changes);
    bed.census.count(root, store);
    expect_counts_entries(bed, entries);
    expect_as_walked(bed, tree, root, store);
  }
  EXPECT_FALSE(test::saw(*bed.sink, "walking it again"));
}

// C34: counts that do not match the trees refuse the change, and the census walks.
TEST(HeadsCensus, ARefusedChangeWalks) {
  const auto tree = neutral_tree();
  auto entries = shaped_entries(2, 4);
  MemoryTreeNodeStore store;
  const auto before = tree.build(entries, store);
  Bed bed(tree);
  // Counts at the right root that hold nothing: the first thing taken out refuses.
  bed.counts.reset(before, {}, {}, {});
  auto touched = entries.at("a/all/group 0/part 2/item 1");
  touched.refs.clear();
  entries["a/all/group 0/part 2/item 1"] = touched;
  const auto after = tree.build(entries, store);
  bed.census.count(after, store);
  EXPECT_TRUE(test::saw(*bed.sink, "census did not match the tree"));
  expect_counts_entries(bed, entries);
  expect_as_walked(bed, tree, after, store);
}

// C34: a tree the counts stand at that cannot be read is no reason to stop.
TEST(HeadsCensus, AnUnreadableTreeWalks) {
  const auto tree = neutral_tree();
  const auto entries = shaped_entries(2, 4);
  MemoryTreeNodeStore store;
  const auto root = tree.build(entries, store);
  Bed bed(tree);
  bed.counts.reset(test::fake_object(1), {}, {}, {});
  bed.census.count(root, store);
  EXPECT_TRUE(test::saw(*bed.sink, "census cannot follow the tree"));
  expect_counts_entries(bed, entries);

  // But a tree that cannot be walked either throws, and installs nothing.
  Bed second(tree);
  MemoryTreeNodeStore empty;
  EXPECT_THROW(second.census.count(root, empty), DecodeError);
  EXPECT_FALSE(second.counts.root());
}

// C34: a change interrupted between its state writes leaves no root, so a restart's
// next count walks, whatever the tries were left holding.
TEST(HeadsCensus, ACrashMidChangeWalks) {
  const auto tree = neutral_tree();
  const auto entries = shaped_entries(3, 4);
  MemoryTreeNodeStore store;
  const auto root = tree.build(entries, store);
  dannan::test::TempDir dir;
  const auto sink = std::make_shared<RecordingSink>();
  {
    ReferenceCounts counts(dir.path() / "referenced", key_of(9), 1 << 20, logger_into(sink));
    ReferenceCensus<NeutralItem> census(counts, tree, logger_into(sink));
    census.count(root, store);
    EXPECT_EQ(counts.root(), std::optional(root));
  }
  // Across a clean restart the root stands, and nothing is read.
  {
    ReferenceCounts counts(dir.path() / "referenced", key_of(9), 1 << 20, logger_into(sink));
    ReferenceCensus<NeutralItem> census(counts, tree, logger_into(sink));
    EXPECT_EQ(counts.root(), std::optional(root));
    store.forget_reads();
    census.count(root, store);
    EXPECT_EQ(store.reads(), 0u);
  }
  // `state` as an interrupted change leaves it: not clean.
  {
    std::ofstream state(dir.path() / "referenced" / "state", std::ios::binary | std::ios::in | std::ios::out);
    state.seekp(8);
    state.put(0);
  }
  ReferenceCounts counts(dir.path() / "referenced", key_of(9), 1 << 20, logger_into(sink));
  ReferenceCensus<NeutralItem> census(counts, tree, logger_into(sink));
  EXPECT_FALSE(counts.root());
  census.count(root, store);
  EXPECT_EQ(counts.root(), std::optional(root));
  EXPECT_EQ(counts.totals().entries, entries.size());
}

// C34: counted by the node store, a follow costs what differs; a walk costs the tree.
TEST(HeadsCensus, AFollowReadsOnlyWhatDiffers) {
  const auto tree = neutral_tree();
  auto entries = shaped_entries(60, 20);
  MemoryTreeNodeStore store;
  const auto before = tree.build(entries, store);
  Bed bed(tree);
  store.forget_reads();
  bed.census.count(before, store);
  const auto walk_reads = store.reads();
  const auto stats = tree.stats(before, store);
  EXPECT_GE(walk_reads, stats.leaves + stats.branches + stats.child_nodes);

  entries.at("a/all/group 7/part 2/item 5").stamp += 1;
  const auto after = tree.build(entries, store);
  store.forget_reads();
  bed.census.count(after, store);
  EXPECT_LT(store.reads() * 8, walk_reads);
  EXPECT_LE(store.reads(), 8 * stats.depth);
  EXPECT_FALSE(test::saw(*bed.sink, "walking it again"));
  expect_as_walked(bed, tree, after, store);
}

// C34: a child sequence and DATA two keys share go with their last reference, Macha's
// "the same content at a second path" (NS:198).
TEST(HeadsCensus, ASharedChildGoesWithItsLastReference) {
  const auto tree = neutral_tree();
  NeutralEntries entries;
  entries["a"] = make_group(0);
  entries["a/one"] = make_item(5, 20);
  entries["a/two"] = make_item(5, 20);
  MemoryTreeNodeStore store;
  auto root = tree.build(entries, store);
  const auto references = tree.references(entries.at("a/one"));
  ASSERT_FALSE(references.children.empty());
  ASSERT_EQ(references.data.size(), 20u);
  EXPECT_EQ(references.items, 20u);

  Bed bed(tree);
  bed.census.count(root, store);
  const auto count_of = [&](const ObjectTrie::Snapshot& view, const ObjectId& id) -> uint64_t {
    const auto value = view.get(id);
    if (!value) return 0;
    dannan::net::Reader reader(*value);
    return reader.u64();
  };
  EXPECT_EQ(count_of(bed.counts.views().nodes, references.children.front()), 2u);
  EXPECT_EQ(count_of(bed.counts.views().extents, references.data.front()), 2u);

  root = tree.update(root, store, {{"a/one", std::nullopt}});
  bed.census.count(root, store);
  EXPECT_EQ(count_of(bed.counts.views().nodes, references.children.front()), 1u);
  EXPECT_EQ(count_of(bed.counts.views().extents, references.data.front()), 1u);

  root = tree.update(root, store, {{"a/two", std::nullopt}});
  bed.census.count(root, store);
  EXPECT_EQ(count_of(bed.counts.views().nodes, references.children.front()), 0u);
  EXPECT_EQ(count_of(bed.counts.views().extents, references.data.front()), 0u);
  EXPECT_FALSE(test::saw(*bed.sink, "walking it again"));
}

// The stop: as Macha's census_count rethrows it (filesystem.cpp at 9a08846), neither a
// follow nor a walk takes it for its own failure, and neither changes the counts.
TEST(HeadsCensus, AStopPassesThroughAndLeavesTheCountsWhereTheyWere) {
  const auto tree = neutral_tree();
  auto entries = shaped_entries(4, 4);
  MemoryTreeNodeStore store;
  const auto before = tree.build(entries, store);
  Bed bed(tree);
  const TreePause stop = [] { throw contract::MaintenanceStopping(); };
  // A walk.
  EXPECT_THROW(bed.census.count(before, store, stop), contract::MaintenanceStopping);
  EXPECT_FALSE(bed.counts.root());
  // A pause per entry, as Macha's walk has it.
  size_t pauses = 0;
  bed.census.count(before, store, [&] { ++pauses; });
  EXPECT_EQ(pauses, entries.size());
  // A follow: no warning, no walk behind its back.
  entries.at("a/all/group 1/part 2/item 1").stamp += 1;
  const auto after = tree.build(entries, store);
  EXPECT_THROW(bed.census.count(after, store, stop), contract::MaintenanceStopping);
  EXPECT_EQ(bed.counts.root(), std::optional(before));
  EXPECT_FALSE(test::saw(*bed.sink, "walking it again"));
  // A pause per difference.
  pauses = 0;
  bed.census.count(after, store, [&] { ++pauses; });
  EXPECT_EQ(pauses, 1u);
  EXPECT_EQ(bed.counts.root(), std::optional(after));
}

}  // namespace
}  // namespace dagda::heads
