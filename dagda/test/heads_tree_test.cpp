//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement H18: the Merkle tree over keys, over a neutral owner's values. Each
// case names the Macha case it moved from (tests/test_namespace_tree.cpp and
// tests/test_namespace_entries.cpp at 9a08846), its assertions kept but for the
// map form, which is Macha's working form and not Dagda's.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/heads/tree.h"
#include "dagda/test/fixtures/tree.h"
#include "dannan/contract/time_source.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda::heads {
namespace {

using dannan::sched::Budget;
using dannan::sched::Cursor;
using dannan::sched::Stop;
using test::fake_object;
using test::make_group;
using test::make_item;
using test::neutral_tree;
using test::NeutralEntries;
using test::NeutralItem;
using test::NeutralRef;
using test::NeutralTree;
using test::shaped_entries;

const dannan::contract::TimeSource& steady() { return dannan::contract::steady_time_source(); }

NeutralItem page_item(uint64_t n) {
  NeutralItem item;
  item.kind = 2;
  item.weight = n;
  NeutralRef ref;
  ref.span = static_cast<uint32_t>(n + 1);
  ref.id.bytes[0] = static_cast<uint8_t>(n);
  ref.id.bytes[1] = static_cast<uint8_t>(n >> 8);
  item.refs.push_back(ref);
  return item;
}

// `count` keys in a few groups, so keys share prefixes.
NeutralEntries paged_entries(size_t count) {
  NeutralEntries out;
  for (size_t i = 0; i < count; ++i) out["d" + std::to_string(i % 7) + "/f" + std::to_string(i)] = page_item(i);
  return out;
}

// Small fanouts, so a few hundred entries make a tree several levels deep.
TreeLimits small_limits() {
  TreeLimits limits;
  limits.entry_target_fanout = 3;
  limits.entry_max_fanout = 6;
  limits.branch_target_fanout = 3;
  limits.branch_max_fanout = 6;
  return limits;
}

std::vector<NeutralTree::Item> by_callback(const NeutralTree& tree, const ObjectId& root, const TreeNodeStore& store) {
  std::vector<NeutralTree::Item> out;
  tree.walk(root, store, [&](const std::string& key, const NeutralItem& item) { out.emplace_back(key, item); });
  return out;
}

std::vector<NeutralTree::Item> by_pages(const NeutralTree& tree, const ObjectId& root, const TreeNodeStore& store, size_t bound, size_t& pages) {
  std::vector<NeutralTree::Item> out;
  Cursor<std::string> cursor;
  pages = 0;
  for (;;) {
    Budget budget;
    budget.operations(bound);
    auto page = tree.entries(root, &store, cursor, budget, steady());
    ++pages;
    EXPECT_LE(page.items.size(), bound);
    for (auto& item : page.items) out.push_back(std::move(item));
    if (page.complete()) {
      EXPECT_FALSE(page.next.after.has_value());
      return out;
    }
    EXPECT_EQ(page.stopped, Stop::budget);
    EXPECT_TRUE(page.next.after.has_value());
    cursor = page.next;
    if (pages > 2000) {
      ADD_FAILURE() << "the walk never completed";
      return out;
    }
  }
}

// Macha: test_the_tree_round_trips_every_namespace_it_is_given (NT:78).
TEST(HeadsTree, TheTreeRoundTripsEveryKeySetItIsGiven) {
  const auto tree = neutral_tree();
  MemoryTreeNodeStore store;

  // The empty set is a tree like any other.
  const auto empty_root = tree.build({}, store);
  EXPECT_TRUE(tree.read(empty_root, store).empty());

  const auto entries = shaped_entries(12, 9);
  const auto root = tree.build(entries, store);
  const auto read_back = tree.read(root, store);
  ASSERT_EQ(read_back.size(), entries.size());
  EXPECT_TRUE(read_back == entries);

  // A child sequence comes back whole and in order.
  const auto& original = entries.at("a/all/group 0/part 1/item 0");
  ASSERT_EQ(original.refs.size(), 600u);
  EXPECT_TRUE(read_back.at("a/all/group 0/part 1/item 0").refs == original.refs);
}

// Macha: test_the_root_is_a_function_of_the_entry_set_and_nothing_else (NT:136).
// History independence: the same set reached by different insertions and
// deletions gives the same root.
TEST(HeadsTree, TheRootIsAFunctionOfTheEntrySetAndNothingElse) {
  const auto tree = neutral_tree();
  const auto entries = shaped_entries(8, 7);

  MemoryTreeNodeStore direct;
  const auto direct_root = tree.build(entries, direct);

  // The same set, reached by adding a key and taking it away again.
  auto detour = entries;
  detour["a/all/group 3/part 2/item 99"] = make_item(999, 40);
  MemoryTreeNodeStore scratch;
  (void)tree.build(detour, scratch);
  detour.erase("a/all/group 3/part 2/item 99");
  const auto detour_root = tree.build(detour, scratch);

  EXPECT_EQ(direct_root, detour_root);

  // A different store, built from scratch, agrees.
  MemoryTreeNodeStore elsewhere;
  EXPECT_EQ(tree.build(detour, elsewhere), direct_root);
}

// Macha: test_changing_one_file_rewrites_a_path_to_the_root_and_not_the_library
// (NT:159).
TEST(HeadsTree, ChangingOneValueRewritesAPathToTheRootAndNotTheTree) {
  const auto tree = neutral_tree();
  auto entries = shaped_entries(40, 12);
  MemoryTreeNodeStore store;
  const auto before_root = tree.build(entries, store);
  const auto whole_nodes = store.written().size();
  const auto whole_bytes = store.bytes();
  store.forget_written();

  auto& entry = entries.at("a/all/group 17/part 2/item 5");
  entry.stamp += 1000;
  entry.weight += 4096;
  const auto after_root = tree.build(entries, store);

  EXPECT_NE(after_root, before_root);
  const auto rewritten = store.written().size();
  ASSERT_GT(rewritten, 0u);

  // A write costs the tree's depth, not its size; the exact count depends on
  // boundary hashes, so the order of magnitude is what is asserted.
  EXPECT_LE(rewritten, 12u);
  EXPECT_LT(rewritten * 20, whole_nodes);
  EXPECT_GT(whole_bytes, 0u);

  // The changed value is what comes back.
  const auto read_back = tree.read(after_root, store);
  EXPECT_EQ(read_back.at("a/all/group 17/part 2/item 5").stamp, entry.stamp);
}

// Macha: test_a_lookup_reads_a_path_to_the_root_rather_than_the_namespace (NT:218).
TEST(HeadsTree, ALookupReadsAPathToTheRootRatherThanTheTree) {
  const auto tree = neutral_tree();
  const auto entries = shaped_entries(20, 10);
  MemoryTreeNodeStore store;
  const auto root = tree.build(entries, store);

  for (const auto& [key, entry] : entries) {
    const auto found = tree.lookup(root, key, store);
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(*found == entry);
  }

  // Absent keys sorting before everything, between leaves, and after everything.
  EXPECT_FALSE(tree.lookup(root, "", store).has_value());
  EXPECT_FALSE(tree.lookup(root, "/AAA", store).has_value());
  EXPECT_FALSE(tree.lookup(root, "a/all/group 3/part 1/item 999", store).has_value());
  EXPECT_FALSE(tree.lookup(root, "zzz", store).has_value());

  // The name's claim, which Macha's case leaves to its stat-only sibling: a path,
  // and no child node, is what a stat-only lookup costs.
  const auto stats = tree.stats(root, store);
  store.forget_reads();
  EXPECT_TRUE(tree.lookup(root, "a/all/group 3/part 1/item 0", store, false).has_value());
  EXPECT_LE(store.reads(), stats.depth);
}

// C7, and Macha: test_a_corrupt_node_is_refused_rather_than_trusted (NT:261), whose
// assertions are kept: nodes come from the control store, so a corrupt or forged
// one must not be trusted. Macha's asks only for survival. Here every bit of every
// node is flipped: what the tree reader does with bytes the store hands it is a
// named refusal or an answer that re-encodes to another root, and an answer that
// differs under the same root does not occur. That the id itself refuses them at
// the store is the other half (HeadsControlNodeStore.ACorruptObjectIsRefused...).
TEST(HeadsTree, ACorruptNodeIsRefused) {
  // Small fanouts, so branches, leaves, child leaves and child branches are all here
  // and every bit of every one can be flipped in a test's time.
  const auto tree = neutral_tree(TreeLimits{4, 8, 4, 8, 2, 4, 8});
  NeutralEntries entries;
  entries["a"] = make_group(0);
  entries["a/one"] = make_item(1, 3);
  entries["a/two"] = make_item(2, 30);
  for (int i = 0; i < 12; ++i) entries["a/set/f" + std::to_string(i)] = make_item(10 + i, 2);

  MemoryTreeNodeStore store;
  const auto root = tree.build(entries, store);
  const auto clean = tree.read(root, store);
  ASSERT_EQ(clean.size(), entries.size());

  // Only the damaged node differs from the clean store.
  class Overlay final : public TreeNodeStore {
   public:
    explicit Overlay(const MemoryTreeNodeStore& base) : _base(base) {}
    ObjectId put(std::span<const uint8_t>) override { throw std::logic_error("read only"); }
    std::optional<Bytes> get(const ObjectId& id) const override {
      if (id == damaged_id) return damaged;
      return _base.get(id);
    }
    ObjectId damaged_id{};
    Bytes damaged;

   private:
    const MemoryTreeNodeStore& _base;
  };
  Overlay broken(store);

  size_t nodes_tried = 0, refused = 0, same_answer = 0, other_root = 0, wrong = 0;
  for (const auto& id : store.written()) {
    const auto body = store.get(id);
    ASSERT_TRUE(body);
    ++nodes_tried;
    for (size_t at = 0; at < body->size(); ++at) {
      for (int bit = 0; bit < 8; ++bit) {
        auto damaged = *body;
        damaged[at] = static_cast<uint8_t>(damaged[at] ^ (1u << bit));
        // A flipped node is never the node its id names.
        ASSERT_NE(store::object_id(damaged), id);
        broken.damaged_id = id;
        broken.damaged = damaged;
        try {
          const auto out = tree.read(root, broken);
          if (out == clean) {
            ++same_answer;
          } else {
            MemoryTreeNodeStore fresh;
            if (tree.build(out, fresh) != root) {
              ++other_root;
            } else {
              ++wrong;
            }
          }
        } catch (const std::exception&) {
          ++refused;
        }
        try {
          (void)tree.lookup(root, "a/one", broken);
        } catch (const std::exception&) {
        }
      }
    }
  }
  EXPECT_GT(nodes_tried, 0u);
  EXPECT_GT(refused, 0u);
  // Both happen: a field a reader passes over gives the same answer, and a flip in a
  // value is seen only as another root. Damage in a value is not the reader's to
  // catch: the store's id is (below, in the case this comment names).
  EXPECT_GT(same_answer, 0u);
  EXPECT_GT(other_root, 0u);
  EXPECT_EQ(wrong, 0u);
}

// Macha: test_an_incremental_update_produces_the_tree_a_rebuild_would (NT:629).
TEST(HeadsTree, AnIncrementalUpdateProducesTheTreeARebuildWould) {
  const auto tree = neutral_tree();
  auto entries = shaped_entries(8, 6);
  MemoryTreeNodeStore store;
  auto root = tree.build(entries, store);

  uint64_t seed = 99;
  const auto next = [&] {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return seed >> 33;
  };

  for (int round = 0; round < 60; ++round) {
    NeutralTree::Changes changes;
    const size_t count = 1 + next() % 6;
    for (size_t i = 0; i < count; ++i) {
      const auto pick = next() % 3;
      if (pick == 0 && !entries.empty()) {
        // Delete an existing key, but never the first.
        auto it = entries.begin();
        std::advance(it, static_cast<ptrdiff_t>(next() % entries.size()));
        if (it->first == "a") continue;
        changes[it->first] = std::nullopt;
      } else if (pick == 1 && !entries.empty()) {
        // Change a value in place.
        auto it = entries.begin();
        std::advance(it, static_cast<ptrdiff_t>(next() % entries.size()));
        auto updated = it->second;
        updated.stamp += 1000;
        updated.weight += 4096;
        changes[it->first] = updated;
      } else {
        // Insert a new key, sometimes long enough to force a child sequence.
        const auto key = "a/all/new " + std::to_string(next() % 10000);
        changes[key] = make_item(next(), next() % 4 == 0 ? 700 : 2);
      }
    }
    if (changes.empty()) continue;

    for (const auto& [key, value] : changes) {
      if (value) {
        entries[key] = *value;
      } else {
        entries.erase(key);
      }
    }

    MemoryTreeNodeStore fresh;
    const auto rebuilt = tree.build(entries, fresh);
    root = tree.update(root, store, changes);
    EXPECT_EQ(root, rebuilt);
    if (root != rebuilt) break;
  }

  // The tree is what the changes said it should be.
  EXPECT_TRUE(tree.read(root, store) == entries);
}

// Macha: test_a_write_rewrites_a_path_rather_than_the_library (NT:692).
TEST(HeadsTree, AWriteRewritesAPathRatherThanTheTree) {
  const auto tree = neutral_tree();
  auto entries = shaped_entries(120, 20);
  MemoryTreeNodeStore store;
  auto root = tree.build(entries, store);
  const auto shape = tree.stats(root, store);

  auto changed = entries.at("a/all/group 7/part 2/item 5");
  changed.stamp += 1000;
  changed.weight += 4096;

  store.forget_written();
  root = tree.update(root, store, {{"a/all/group 7/part 2/item 5", changed}});
  const auto written = store.written().size();

  // New nodes are exactly the path from the root.
  EXPECT_EQ(written, shape.depth);
  EXPECT_LT(written, shape.leaves);
  EXPECT_TRUE(tree.read(root, store).at("a/all/group 7/part 2/item 5") == changed);

  // Every other entry is unchanged.
  const auto after = tree.read(root, store);
  EXPECT_EQ(after.size(), entries.size());
  entries["a/all/group 7/part 2/item 5"] = changed;
  EXPECT_TRUE(after == entries);
}

// Macha: test_a_delta_applied_to_the_tree_lands_where_the_map_lands (NT:721). The
// append's own rule (the base count, the times it stamps) is the owner's, here the
// neutral one's: Macha keeps its form of it.
TEST(HeadsTree, ADeltaAppliedToTheTreeLandsWhereAMapLands) {
  const auto tree = neutral_tree();
  auto entries = shaped_entries(10, 6);
  MemoryTreeNodeStore store;
  auto root = tree.build(entries, store);

  // A value with a child sequence.
  std::string appended;
  for (const auto& [key, entry] : entries)
    if (entry.refs.size() == 600) {
      appended = key;
      break;
    }
  ASSERT_FALSE(appended.empty());

  const auto append_two = [](size_t base) {
    return [base](NeutralItem& item) {
      if (item.refs.size() != base) throw DecodeError("append base mismatch");
      item.refs.push_back(make_item(77, 2).refs[0]);
      item.refs.push_back(make_item(78, 2).refs[1]);
      item.stamp = 12345;
    };
  };

  // An upsert, an erase, a create, and an append onto a child sequence.
  NeutralTree::Delta delta;
  auto touched = entries.at("a/all/group 1/part 2/item 5");
  touched.stamp += 5000;
  delta.upsert["a/all/group 1/part 2/item 5"] = touched;
  delta.upsert["a/all/group 9/part 4/new"] = make_item(4242, 3);
  delta.erase.push_back("a/all/group 2/part 3/item 2");
  delta.append[appended] = append_two(600);

  const auto updated = tree.apply(root, store, delta);

  entries["a/all/group 1/part 2/item 5"] = touched;
  entries["a/all/group 9/part 4/new"] = make_item(4242, 3);
  entries.erase("a/all/group 2/part 3/item 2");
  append_two(600)(entries.at(appended));
  MemoryTreeNodeStore fresh;
  const auto rebuilt = tree.build(entries, fresh);

  EXPECT_EQ(updated, rebuilt);
  EXPECT_TRUE(tree.read(updated, store) == entries);
  EXPECT_EQ(tree.read(updated, store).at(appended).refs.size(), 602u);

  // A delta whose append base does not match is refused.
  NeutralTree::Delta wrong;
  wrong.append[appended] = append_two(599);
  EXPECT_THROW((void)tree.apply(updated, store, wrong), DecodeError);
  // As is an append onto nothing, or onto what the same delta erased.
  NeutralTree::Delta missing;
  missing.append["absent"] = append_two(0);
  EXPECT_THROW((void)tree.apply(updated, store, missing), DecodeError);
  NeutralTree::Delta erased;
  erased.erase.push_back(appended);
  erased.append[appended] = append_two(602);
  EXPECT_THROW((void)tree.apply(updated, store, erased), DecodeError);
}

// Macha: test_a_prefix_scan_descends_rather_than_walking_the_library (NT:862).
TEST(HeadsTree, APrefixScanDescendsRatherThanWalkingTheTree) {
  const auto tree = neutral_tree();
  const auto entries = shaped_entries(30, 8);
  MemoryTreeNodeStore store;
  const auto root = tree.build(entries, store);

  const std::string prefix = "a/all/group 7/";
  NeutralEntries expected;
  for (const auto& [key, entry] : entries)
    if (key.starts_with(prefix)) expected[key] = entry;
  ASSERT_GT(expected.size(), 4u);
  ASSERT_LT(expected.size() * 4, entries.size());

  // The same answer as filtering a full walk.
  NeutralEntries found;
  store.forget_reads();
  tree.for_each_with_prefix(root, store, prefix, [&](const std::string& key, const NeutralItem& item) { found[key] = item; });
  const auto prefix_reads = store.reads();
  EXPECT_TRUE(found == expected);

  // It reads a fraction of what a full walk of the same tree reads.
  store.forget_reads();
  size_t walked = 0;
  tree.walk(root, store, [&](const std::string&, const NeutralItem&) { ++walked; });
  const auto full_reads = store.reads();
  EXPECT_EQ(walked, entries.size());
  EXPECT_LT(prefix_reads * 4, full_reads);

  // An empty prefix is every entry; an unmatched prefix costs almost nothing.
  size_t all = 0;
  tree.for_each_with_prefix(root, store, "", [&](const std::string&, const NeutralItem&) { ++all; });
  EXPECT_EQ(all, entries.size());

  store.forget_reads();
  size_t none = 0;
  tree.for_each_with_prefix(root, store, "zzz-nothing/", [&](const std::string&, const NeutralItem&) { ++none; });
  EXPECT_EQ(none, 0u);
  EXPECT_LT(store.reads() * 4, full_reads);
}

// Macha: test_a_scan_seeks_and_stops_where_it_is_told (NT:991). A scan can jump: the
// entries between where it was and where it seeks to are never visited, and it ends
// when told to.
TEST(HeadsTree, AScanSeeksAndStopsWhereItIsTold) {
  const auto tree = neutral_tree();
  const auto entries = shaped_entries(20, 8);
  MemoryTreeNodeStore store;
  const auto root = tree.build(entries, store);

  std::vector<std::string> seen;
  tree.scan(root, store, "a/all/group 1/", [&](const std::string& key, const NeutralItem&) {
    seen.push_back(key);
    if (seen.size() == 1) return TreeScanStep::seek("a/all/group 5");
    if (seen.size() == 4) return TreeScanStep::stop();
    return TreeScanStep::next();
  });
  ASSERT_EQ(seen.size(), 4u);
  auto expect = entries.lower_bound("a/all/group 1/");
  EXPECT_EQ(seen[0], expect->first);
  expect = entries.lower_bound("a/all/group 5");
  for (size_t i = 1; i < 4; ++i, ++expect) EXPECT_EQ(seen[i], expect->first);
  // A scan from past the last key visits nothing.
  size_t none = 0;
  tree.scan(root, store, "zzz", [&](const std::string&, const NeutralItem&) {
    ++none;
    return TreeScanStep::next();
  });
  EXPECT_EQ(none, 0u);
  // Seeking backwards is a caller's error.
  EXPECT_THROW(tree.scan(root, store, "a/all", [&](const std::string&, const NeutralItem&) { return TreeScanStep::seek("a"); }), std::logic_error);
}

// Macha: test_an_update_reaches_the_root_a_full_build_reaches (NT:1038). Whatever
// is changed and however the tree is shaped: single entries, runs, the first and
// last keys, whole subtrees removed, everything removed. Small fanouts make deep
// trees and the levels a full build regroups by count.
TEST(HeadsTree, AnUpdateReachesTheRootAFullBuildReaches) {
  uint64_t state = 0x9e3779b97f4a7c15ULL;
  const auto next = [&] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  const auto key_of = [](uint64_t n) { return "k/" + std::to_string(n % 97) + "/" + std::to_string(n % 100003); };
  for (const auto& limits : {TreeLimits{}, TreeLimits{4, 8, 4, 8, 2, 4, 8}, TreeLimits{2, 3, 2, 3, 1, 2, 3}}) {
    const auto tree = neutral_tree(limits);
    for (const size_t size : {size_t{0}, size_t{1}, size_t{3}, size_t{40}, size_t{900}, size_t{5000}}) {
      NeutralEntries entries;
      while (entries.size() < size) entries[key_of(next())] = make_item(next() % 1000, next() % 5);
      MemoryTreeNodeStore store;
      auto root = tree.build(entries, store);
      // Fewer rounds on the largest tree: each is checked against a full build.
      const int rounds = size > 1000 ? 12 : 40;
      for (int round = 0; round < rounds; ++round) {
        NeutralTree::Changes changes;
        const auto kind = next() % 6;
        const auto count = kind == 0 ? 1 : 1 + next() % 12;
        for (uint64_t i = 0; i < count; ++i) {
          if (kind <= 2 || entries.empty()) {
            changes[key_of(next())] = make_item(next() % 1000, next() % 20);
          } else {
            // An existing key: changed, removed, or with its neighbours removed
            // in a run.
            auto it = entries.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(next() % entries.size()));
            if (kind == 3) {
              changes[it->first] = make_item(next() % 1000, next() % 3);
            } else if (kind == 4) {
              changes[it->first] = std::nullopt;
            } else {
              for (int run = 0; run < 30 && it != entries.end(); ++run, ++it) changes[it->first] = std::nullopt;
            }
          }
        }
        if (round == 7 && !entries.empty()) {
          changes[entries.begin()->first] = std::nullopt;
          changes[std::prev(entries.end())->first] = make_item(7, 9);
          changes["/"] = make_group(1);
          changes["zzzz"] = make_group(2);
        }
        if (round == rounds - 1)
          for (const auto& [key, _] : entries) changes[key] = std::nullopt;
        NeutralTree::Differences expected;
        for (const auto& [key, value] : changes) {
          NeutralTree::Difference difference;
          if (const auto found = entries.find(key); found != entries.end()) difference.before = found->second;
          difference.after = value;
          if (difference.before != difference.after) expected.emplace(key, std::move(difference));
          if (value) {
            entries[key] = *value;
          } else {
            entries.erase(key);
          }
        }
        const auto previous = root;
        root = tree.update(root, store, changes);
        MemoryTreeNodeStore fresh;
        ASSERT_EQ(root, tree.build(entries, fresh));
        // The diff of the two trees is exactly what changed.
        const auto differing = tree.diff(previous, root, store);
        ASSERT_EQ(differing.size(), expected.size());
        for (const auto& [key, difference] : expected) {
          const auto found = differing.find(key);
          ASSERT_NE(found, differing.end());
          EXPECT_TRUE(found->second.before == difference.before);
          EXPECT_TRUE(found->second.after == difference.after);
        }
      }
    }
  }
}

// Macha: test_an_update_reads_the_path_it_changes_not_the_tree (NT:1125).
TEST(HeadsTree, AnUpdateReadsThePathItChangesNotTheTree) {
  const auto tree = neutral_tree();
  auto entries = shaped_entries(60, 200);
  MemoryTreeNodeStore store;
  auto root = tree.build(entries, store);
  const auto stats = tree.stats(root, store);
  ASSERT_GT(stats.leaves, 300u);
  ASSERT_GE(stats.depth, 3u);

  const std::string key = "a/all/group 31/part 2/item 77";
  ASSERT_TRUE(entries.contains(key));
  auto changed = entries.at(key);
  changed.tag = 0600;
  entries[key] = changed;
  store.forget_reads();
  store.forget_written();
  const auto before_change = root;
  root = tree.update(root, store, {{key, changed}});
  // The path down, the leaf, and at most a neighbour or two taken in.
  EXPECT_LE(store.reads(), 4 * stats.depth);
  EXPECT_LE(store.written().size(), 2 * stats.depth);
  MemoryTreeNodeStore fresh;
  EXPECT_EQ(root, tree.build(entries, fresh));

  // So does telling the two trees apart.
  store.forget_reads();
  const auto differing = tree.diff(before_change, root, store);
  ASSERT_EQ(differing.size(), 1u);
  EXPECT_EQ(differing.begin()->first, key);
  EXPECT_LE(store.reads(), 8 * stats.depth);

  // Removing a group reads its leaves and the path, not the tree.
  NeutralTree::Changes removal;
  for (const auto& [name, _] : entries)
    if (name == "a/all/group 9" || name.starts_with("a/all/group 9/")) removal[name] = std::nullopt;
  ASSERT_GT(removal.size(), 200u);
  for (const auto& [name, _] : removal) entries.erase(name);
  store.forget_reads();
  root = tree.update(root, store, removal);
  EXPECT_LT(store.reads() * 4, stats.leaves);
  MemoryTreeNodeStore again;
  EXPECT_EQ(root, tree.build(entries, again));
}

// Macha: test_every_tree_node_is_reachable_for_the_collector (NT:1170). The
// collector's set is exactly the nodes the build wrote.
TEST(HeadsTree, EveryTreeNodeIsReachableForTheCollector) {
  const auto tree = neutral_tree();
  const auto entries = shaped_entries(12, 8);
  MemoryTreeNodeStore store;
  const auto root = tree.build(entries, store);

  std::vector<ObjectId> reachable;
  tree.collect_nodes(root, store, reachable);
  std::sort(reachable.begin(), reachable.end());
  reachable.erase(std::unique(reachable.begin(), reachable.end()), reachable.end());

  auto written = store.written();
  std::sort(written.begin(), written.end());
  EXPECT_EQ(reachable, written);
  EXPECT_EQ(reachable.size(), store.nodes());

  const auto shape = tree.stats(root, store);
  EXPECT_EQ(reachable.size(), shape.leaves + shape.branches + shape.child_nodes);
}

// Macha: test_a_diff_is_the_paths_that_differ_and_reads_only_their_leaves (NT:1436).
TEST(HeadsTree, ADiffIsTheKeysThatDifferAndReadsOnlyTheirLeaves) {
  const auto tree = neutral_tree();
  MemoryTreeNodeStore store;
  auto before = shaped_entries(200, 20);
  const auto root_before = tree.build(before, store);
  EXPECT_TRUE(tree.diff(root_before, root_before, store).empty());

  auto after = before;
  after.erase("a/all/group 3/part 1/item 0");
  after.at("a/all/group 3/part 1/item 4").stamp += 1;
  after["a/all/group 40/part 2/added"] = make_item(77, 700);
  // A child sequence one record longer.
  auto& grown = after.at("a/all/group 0/part 1/item 0");
  grown.refs.push_back(make_item(5, 1).refs.front());
  const auto root_after = tree.build(after, store);

  store.forget_reads();
  const auto differences = tree.diff(root_before, root_after, store);
  const auto reads = store.reads();
  ASSERT_EQ(differences.size(), 4u);
  for (const auto& [key, difference] : differences) {
    const auto was = before.find(key);
    const auto is = after.find(key);
    EXPECT_TRUE(difference.before == (was == before.end() ? std::optional<NeutralItem>() : std::optional<NeutralItem>(was->second)));
    EXPECT_TRUE(difference.after == (is == after.end() ? std::optional<NeutralItem>() : std::optional<NeutralItem>(is->second)));
  }
  // Both ways round, and against the empty tree.
  const auto reversed = tree.diff(root_after, root_before, store);
  ASSERT_EQ(reversed.size(), 4u);
  EXPECT_FALSE(reversed.at("a/all/group 40/part 2/added").after.has_value());
  const auto empty = tree.build({}, store);
  EXPECT_EQ(tree.diff(empty, root_before, store).size(), before.size());

  // Four changes among over a hundred leaves: both trees' few branch nodes, the
  // leaves that differ, and the changed values' children.
  const auto stats = tree.stats(root_before, store);
  EXPECT_GT(stats.leaves, 100u);
  EXPECT_LT(reads, stats.leaves / 2);
}

// Macha: test_distant_changes_read_no_leaf_between_them (NT:1477).
TEST(HeadsTree, DistantChangesReadNoLeafBetweenThem) {
  const auto tree = neutral_tree();
  MemoryTreeNodeStore store;
  auto entries = shaped_entries(200, 20);
  const auto root = tree.build(entries, store);
  const auto stats = tree.stats(root, store);

  NeutralTree::Changes changes;
  auto first = entries.at("a/all/group 0/part 1/item 0");
  first.stamp += 1;
  changes["a/all/group 0/part 1/item 0"] = first;
  changes["a/all/group 50/part 4/item 19"] = std::nullopt;
  changes["zz-last"] = make_item(3, 2);
  for (const auto& [key, value] : changes) {
    if (value) {
      entries[key] = *value;
    } else {
      entries.erase(key);
    }
  }

  store.forget_reads();
  const auto updated = tree.update(root, store, changes);
  const auto reads = store.reads();
  MemoryTreeNodeStore fresh;
  EXPECT_EQ(updated, tree.build(entries, fresh));
  // The branch nodes, the three windows' leaves and those leaves' children; no leaf
  // between the windows.
  EXPECT_LT(reads, stats.leaves);
}

// Macha: test_pages_visit_what_the_callback_walk_visits_on_every_shape (NE:88).
TEST(HeadsTree, PagesVisitWhatTheCallbackWalkVisitsOnEveryShape) {
  const auto tree = neutral_tree(small_limits());
  for (size_t count : {0U, 1U, 2U, 3U, 7U, 31U, 100U, 257U}) {
    MemoryTreeNodeStore store;
    const auto root = tree.build(paged_entries(count), store);
    const auto expected = by_callback(tree, root, store);
    EXPECT_EQ(expected.size(), count);
    for (size_t bound : {1U, 2U, 3U, 5U, 8U, 64U, 1000U}) {
      size_t pages = 0;
      EXPECT_TRUE(by_pages(tree, root, store, bound, pages) == expected);
      // The page that takes the last entry is complete; there is no empty final page.
      EXPECT_EQ(pages, std::max<size_t>(1, (count + bound - 1) / bound));
    }
  }
}

// Macha: test_a_resumed_page_reads_only_its_own_path (NE:108).
TEST(HeadsTree, AResumedPageReadsOnlyItsOwnPath) {
  const auto tree = neutral_tree(small_limits());
  MemoryTreeNodeStore store;
  const auto root = tree.build(paged_entries(1000), store);
  const auto all = by_callback(tree, root, store);
  const size_t total_nodes = store.nodes();

  // Resuming before the last entry reads one root-to-leaf path (and its child
  // nodes), far fewer than the tree's nodes.
  store.forget_reads();
  Budget budget;
  budget.operations(10);
  const auto page = tree.entries(root, &store, Cursor<std::string>{all[all.size() - 2].first}, budget, steady());
  EXPECT_TRUE(page.complete());
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items.front().first, all.back().first);
  EXPECT_LT(store.reads(), total_nodes / 10);
  EXPECT_GE(store.reads(), 2u);
}

// Macha: test_a_cursor_between_paths_resumes_after_it (NE:129), the tree form.
TEST(HeadsTree, ACursorBetweenKeysResumesAfterIt) {
  const auto tree = neutral_tree(small_limits());
  MemoryTreeNodeStore store;
  const auto root = tree.build(paged_entries(20), store);
  const auto all = by_callback(tree, root, store);
  // From a key not in the tree, the walk resumes at the next one.
  const std::string between = all[4].first + "~";
  Budget budget;
  budget.operations(100);
  const auto page = tree.entries(root, &store, Cursor<std::string>{between}, budget, steady());
  ASSERT_FALSE(page.items.empty());
  EXPECT_GT(page.items.front().first, between);
  EXPECT_LE(page.items.front().first, all[5].first);
}

// Macha: test_cancellation_stops_before_an_entry (NE:148), on a tree: Macha's walks
// the map form.
TEST(HeadsTree, CancellationStopsBeforeAnEntry) {
  const auto tree = neutral_tree(small_limits());
  MemoryTreeNodeStore store;
  const auto root = tree.build(paged_entries(5), store);
  std::atomic_bool cancelled{true};
  Budget budget(dannan::sched::WorkContext(dannan::sched::FrameType::control, {}, &cancelled));
  budget.operations(100);
  const auto page = tree.entries(root, &store, {}, budget, steady());
  EXPECT_TRUE(page.items.empty());
  EXPECT_EQ(page.stopped, Stop::cancelled);
}

// Macha: test_a_missing_node_or_store_throws (NE:159).
TEST(HeadsTree, AMissingNodeOrStoreThrows) {
  const auto tree = neutral_tree(small_limits());
  MemoryTreeNodeStore store;
  const auto root = tree.build(paged_entries(50), store);
  MemoryTreeNodeStore empty;
  Budget budget;
  EXPECT_THROW((void)tree.entries(root, &empty, {}, budget, steady()), std::exception);
  EXPECT_THROW((void)tree.entries(root, nullptr, {}, budget, steady()), std::exception);
}

// Macha: test_a_node_that_is_not_a_tree_node_throws (NE:225).
TEST(HeadsTree, ANodeThatIsNotATreeNodeThrows) {
  const auto tree = neutral_tree(small_limits());
  MemoryTreeNodeStore store;
  const auto root = tree.build(paged_entries(40), store);
  // The root's address now holds bytes that are no tree node at all.
  store.put_at(root, Bytes{'n', 'o', 'p', 'e', 0, 0, 0, 0});
  Budget budget;
  EXPECT_THROW((void)tree.entries(root, &store, {}, budget, steady()), std::exception);
}

// Not Macha's case but its behaviour, kept (namespace_tree.cpp:328-334 at 9a08846):
// "load children for this key" is said by a non-empty key, so the empty key's full
// lookup comes back with no child records. Macha's key_bytes vector holds that key.
TEST(HeadsTree, TheEmptyKeysLookupFetchesNoChildren) {
  const auto tree = neutral_tree();
  NeutralEntries entries;
  entries[""] = make_item(1, 30);
  entries["a"] = make_item(2, 30);
  MemoryTreeNodeStore store;
  const auto root = tree.build(entries, store);
  ASSERT_TRUE(tree.lookup(root, "", store).has_value());
  EXPECT_TRUE(tree.lookup(root, "", store)->refs.empty());
  EXPECT_EQ(tree.lookup(root, "a", store)->refs.size(), 30u);
  // A walk loads them.
  EXPECT_EQ(tree.read(root, store).at("").refs.size(), 30u);
}

// The stop (Macha's contract/stopping.hpp at 9a08846): a pause that throws goes
// through every long walk untouched, and is not treated as the walk's own failure.
TEST(HeadsTree, APauseThatStopsPassesThroughEveryLongWalk) {
  const auto tree = neutral_tree(small_limits());
  MemoryTreeNodeStore store;
  auto entries = paged_entries(200);
  const auto root = tree.build(entries, store);
  entries["d1/changed"] = page_item(9);
  const auto other = tree.build(entries, store);
  const TreePause stop = [] { throw contract::MaintenanceStopping(); };
  std::vector<ObjectId> a, b;
  const auto nothing = [](const std::string&, const NeutralItem&) {};
  EXPECT_THROW(tree.walk(root, store, nothing, stop), contract::MaintenanceStopping);
  EXPECT_THROW(tree.collect_nodes(root, store, a, stop), contract::MaintenanceStopping);
  EXPECT_THROW(tree.collect_changes(root, other, store, a, stop), contract::MaintenanceStopping);
  EXPECT_THROW(tree.diff_nodes(root, other, store, a, b, stop), contract::MaintenanceStopping);
  EXPECT_THROW((void)tree.diff(root, other, store, stop), contract::MaintenanceStopping);
  // Given once per entry on a walk, and not at all when none is given.
  size_t pauses = 0;
  tree.walk(root, store, nothing, [&] { ++pauses; });
  EXPECT_EQ(pauses, entries.size() - 1);
}

}  // namespace
}  // namespace dagda::heads
