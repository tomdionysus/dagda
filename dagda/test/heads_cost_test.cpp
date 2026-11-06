//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The cost baselines S3, S4 and S6 close (the plan's section 2.6): verify, replay, accept,
// commit and merge. They assert counts, never time: nothing here grows with the keys of the
// tree. A million-key run stays with the scale pass. S6 closes merge.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

#include "dagda/heads/replica.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/heads_merge.h"
#include "dagda/test/fixtures/heads_space.h"

namespace dagda::heads {
namespace {

using test::author;
using test::neutral_payload;
using test::NeutralEntries;
using test::NeutralHead;
using test::record_after;
using test::record_of;
using test::ReplicaBed;
using test::SharedTreeStore;

// A tree of `keys` keys, built once: the numbers below are what the path costs after.
struct KeyedBed {
  CostCounters costs;
  SharedTreeStore store{&costs};
  ReplicaBed bed;
  NeutralEntries entries;
  NeutralHead head;
  Record parent;

  explicit KeyedBed(size_t keys) {
    bed.codec = std::make_shared<test::NeutralCodec>(&costs);
    for (size_t i = 0; i < keys; ++i) entries["key/" + std::to_string(i * 7919 % keys) + "/" + std::to_string(i)] = test::make_item(i, 1);
    const auto writer = store.factory()();
    head.root = test::neutral_tree().build(entries, *writer);
    parent = record_of(2, bed.genesis().hash, head);
  }

  ReplicaOptions options() {
    auto out = bed.options();
    out.tree_store = store.factory();
    out.costs = &costs;
    return out;
  }

  void reset() {
    costs.tree_nodes_read = 0;
    costs.tree_nodes_written = 0;
    costs.record_bytes_encoded = 0;
    costs.record_bytes_hashed = 0;
    costs.full_decodes = 0;
    costs.parent_states_copied = 0;
    costs.history_frames_written = 0;
    costs.syncs = 0;
  }

  // One key changed, one clock entry moved.
  test::NeutralDelta change(const NeutralHead& on, uint64_t sequence, NeutralHead& after) {
    test::NeutralDelta delta;
    const auto key = entries.begin()->first;
    auto item = entries.begin()->second;
    item.stamp = static_cast<int64_t>(sequence);
    delta.upsert_keys[key] = item;
    delta.clock[author(1)] = sequence;
    after = on;
    after.clock[author(1)] = sequence;
    const auto writer = store.factory()();
    test::NeutralTree::Delta changes;
    changes.upsert[key] = item;
    after.root = test::neutral_tree().apply(*after.root, *writer, changes);
    return delta;
  }
};

struct Spent {
  uint64_t nodes_read = 0;
  uint64_t nodes_written = 0;
  uint64_t encoded = 0;
  uint64_t hashed = 0;
  uint64_t decodes = 0;
  uint64_t copies = 0;
  uint64_t frames = 0;
  uint64_t syncs = 0;
};

Spent spent(const CostCounters& costs) {
  return {costs.tree_nodes_read.load(), costs.tree_nodes_written.load(),   costs.record_bytes_encoded.load(),   costs.record_bytes_hashed.load(),
          costs.full_decodes.load(),    costs.parent_states_copied.load(), costs.history_frames_written.load(), costs.syncs.load()};
}

// Verify keys: the same delta on the receiver, its parent materialised, store_commit's
// path. Fixed one-leaf change; 1k and 100k keys.
Spent verify_at(size_t keys) {
  KeyedBed bed(keys);
  Replica replica(bed.options());
  EXPECT_TRUE(replica.store_commit(bed.parent));
  EXPECT_TRUE(replica.materialized(bed.parent.hash));
  NeutralHead after;
  const auto delta = test::encode_neutral_delta(bed.change(bed.head, 1, after));
  const auto child = record_after(bed.parent, neutral_payload(after));
  bed.reset();
  EXPECT_TRUE(replica.store_commit(child, delta));
  EXPECT_EQ(replica.history_entry(child.hash)->body, HistoryEntry::Body::delta);
  return spent(bed.costs);
}

TEST(HeadsCost, VerifyDoesNotGrowWithKeys) {
  const auto small = verify_at(1000);
  const auto large = verify_at(100000);
  // Record bytes are equal at both sizes: the record names a root and no entries.
  EXPECT_EQ(small.encoded, large.encoded);
  EXPECT_EQ(small.hashed, large.hashed);
  // One decode of the arriving record for its parents, as Macha's store_commit makes;
  // none of the parent, which is materialised.
  EXPECT_EQ(small.decodes, 1U);
  EXPECT_EQ(large.decodes, 1U);
  EXPECT_EQ(small.copies, 0U);
  EXPECT_EQ(large.copies, 0U);
  EXPECT_EQ(large.frames, 1U);
  EXPECT_EQ(large.syncs, 1U);
  // Nodes: a path, proportional to depth. 100 times the keys adds at most a level or
  // two, against the 100-fold a walk would show.
  EXPECT_GT(small.nodes_read, 0U);
  EXPECT_LE(large.nodes_read, small.nodes_read * 4);
  EXPECT_LE(large.nodes_written, small.nodes_written * 4);
  EXPECT_LT(large.nodes_read, 64U);
  EXPECT_LT(large.nodes_written, 64U);
}

// Replay keys: a chain of L deltas, read cold. Bytes encoded follow L and not keys;
// one materialisation shared.
struct ReplaySpent {
  Spent cost;
  uint64_t deltas = 0;
  size_t cache_growth = 0;
};

ReplaySpent replay_at(size_t keys, size_t chain) {
  KeyedBed bed(keys);
  auto head = bed.parent;
  auto state = bed.head;
  {
    Replica writer(bed.options());
    EXPECT_TRUE(writer.store_commit(head));
    for (size_t i = 0; i < chain; ++i) {
      NeutralHead after;
      const auto delta = test::encode_neutral_delta(bed.change(state, i + 1, after));
      const auto child = record_after(head, neutral_payload(after));
      EXPECT_TRUE(writer.store_commit(child, delta));
      head = child;
      state = std::move(after);
    }
  }
  Replica replica(bed.options());
  const auto before = replica.diagnostics();
  bed.reset();
  const auto replayed = replica.materialized(head.hash);
  EXPECT_NE(replayed, nullptr);
  EXPECT_EQ(replayed->record.payload, head.payload);
  const auto after = replica.diagnostics();
  // Shared afterwards: no further work.
  const auto spent_once = spent(bed.costs);
  EXPECT_EQ(replica.materialized(head.hash), replayed);
  EXPECT_EQ(spent(bed.costs).encoded, spent_once.encoded);
  return {spent_once, after.historical_deltas_applied - before.historical_deltas_applied,
          after.materialization_cache_entries - before.materialization_cache_entries};
}

TEST(HeadsCost, ReplayFollowsTheChainAndNotTheKeys) {
  constexpr size_t chain = 32;
  const auto small = replay_at(1000, chain);
  const auto large = replay_at(100000, chain);
  EXPECT_EQ(small.deltas, chain);
  EXPECT_EQ(large.deltas, chain);
  // One materialisation, whatever the chain: the intermediates are not cached.
  EXPECT_LE(small.cache_growth, 1U);
  EXPECT_LE(large.cache_growth, 1U);
  // Independent of keys, proportional to L.
  EXPECT_EQ(small.cost.encoded, large.cost.encoded);
  EXPECT_EQ(small.cost.hashed, large.cost.hashed);
  const auto twice = replay_at(1000, chain * 2);
  EXPECT_GE(twice.cost.encoded, small.cost.encoded * 2);
  EXPECT_LE(twice.cost.encoded, small.cost.encoded * 2 + chain * 64);
  // One anchor decoded, no parent state copied.
  EXPECT_EQ(large.cost.decodes, 1U);
  EXPECT_EQ(large.cost.copies, 0U);
  // Nodes per delta: a path, not the tree.
  EXPECT_LE(large.cost.nodes_read, small.cost.nodes_read * 4);
  EXPECT_LT(large.cost.nodes_read, chain * 64);
}

// Accept repeat: a held head costs a lookup, no materialisation, as SM:2492.
TEST(HeadsCost, AcceptingAHeldHeadAgainCostsNothing) {
  KeyedBed bed(1000);
  const Acceptance certificate{bed.parent.generation, bed.parent.hash, 1, {author(1)}};
  {
    Replica replica(bed.options());
    ASSERT_TRUE(replica.store_commit(bed.parent));
    ASSERT_TRUE(replica.accept_commit(certificate));
  }
  Replica replica(bed.options());
  const auto before = replica.diagnostics();
  bed.reset();
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(replica.accept_commit(certificate));
  const auto after = replica.diagnostics();
  const auto cost = spent(bed.costs);
  EXPECT_EQ(cost.nodes_read, 0U);
  EXPECT_EQ(cost.nodes_written, 0U);
  EXPECT_EQ(cost.encoded, 0U);
  EXPECT_EQ(cost.hashed, 0U);
  EXPECT_EQ(cost.decodes, 0U);
  EXPECT_EQ(cost.syncs, 0U);
  EXPECT_EQ(after.historical_requests, before.historical_requests);
  EXPECT_EQ(after.accepted_head_persistence_writes, before.accepted_head_persistence_writes);
}

// Commit keys: a one-leaf change, one author, through the whole mutation path. Nodes
// written and read at most follow depth, record bytes equal at both sizes, no full decode
// for parents and no parent copied.
Spent commit_at(size_t keys) {
  test::SpaceBed bed;
  test::NeutralEntries entries;
  for (size_t i = 0; i < keys; ++i) entries["key/" + std::to_string(i * 7919 % keys) + "/" + std::to_string(i)] = test::make_item(i, 1);
  auto space = bed.open();
  bed.found(*space, entries);
  const auto key = entries.begin()->first;
  auto item = entries.begin()->second;
  item.stamp = 12345;
  // One commit, so the next is at the steady state, its parent materialised.
  (void)space->mutate_delta(test::neutral_mutation([&](test::NeutralEditing& editing) { editing.delta.upsert_keys[key] = item; }));
  for (auto* counter : {&bed.costs.tree_nodes_read, &bed.costs.tree_nodes_written, &bed.costs.record_bytes_encoded, &bed.costs.record_bytes_hashed,
                        &bed.costs.full_decodes, &bed.costs.parent_states_copied, &bed.costs.history_frames_written, &bed.costs.syncs})
    *counter = 0;
  item.stamp = 12346;
  const auto outcome = space->mutate_delta(test::neutral_mutation([&](test::NeutralEditing& editing) { editing.delta.upsert_keys[key] = item; }));
  EXPECT_EQ(outcome.body, HistoryEntry::Body::delta);
  return spent(bed.costs);
}

TEST(HeadsCost, CommitDoesNotGrowWithKeys) {
  const auto small = commit_at(1000);
  const auto large = commit_at(100000);
  // Record bytes equal at both sizes: the record names a root and no entries.
  EXPECT_EQ(small.encoded, large.encoded);
  EXPECT_EQ(small.hashed, large.hashed);
  // The replica's own decode of the proposal for its parents, as Macha's store_commit
  // makes, and its one on acceptance; none for the parent, which is materialised.
  EXPECT_EQ(small.decodes, large.decodes);
  EXPECT_LE(large.decodes, 2U);
  EXPECT_EQ(large.copies, 0U);
  EXPECT_EQ(large.frames, 1U);
  // Nodes: a path, proportional to depth. 100 times the keys adds at most a level or
  // two, against the 100-fold a walk would show.
  EXPECT_GT(small.nodes_written, 0U);
  EXPECT_LE(large.nodes_read, small.nodes_read * 4);
  EXPECT_LE(large.nodes_written, small.nodes_written * 4);
  EXPECT_LT(large.nodes_read, 128U);
  EXPECT_LT(large.nodes_written, 64U);
  std::cout << "BENCH name=heads.commit.keys keys=100000 nodes_read=" << large.nodes_read << " nodes_written=" << large.nodes_written
            << " record_bytes_encoded=" << large.encoded << " record_bytes_hashed=" << large.hashed << " decodes=" << large.decodes << '\n';
}

// Merge keys, and Macha's test_baseline_reconcile_materialised_against_tree (NT:1628): k keys
// differ between two heads, far apart among 1k and 100k. Reads follow k, as NT:1477; the owner's
// merge is called on what differs and nothing else. Macha's sizes (8,700 paths, 900,000 extents) and
// its rename and directory outcomes are its filesystem's; its comparison with the materialising merge
// is kept, reported here in its BENCH form, with the root the oracle gives asserted.
struct MergeSpent {
  Spent cost;
  uint64_t keys_examined = 0;
  uint64_t leaves = 0;
};

MergeSpent merge_at(size_t keys, size_t differing) {
  CostCounters costs;
  SharedTreeStore store(&costs);
  test::NeutralEntries entries;
  for (size_t i = 0; i < keys; ++i) entries["key/" + std::to_string(i * 7919 % keys) + "/" + std::to_string(i)] = test::make_item(i, 1);
  const auto writer = store.factory()();
  NeutralHead origin;
  origin.notes.set("padding", std::string(1024, '.'));
  origin.root = test::neutral_tree().build(entries, *writer);

  // A key every `step`, each changed on one side or the other, plus one added on each.
  auto left = origin;
  auto right = origin;
  auto left_entries = entries;
  auto right_entries = entries;
  const Dot left_dot{author(1), 1};
  const Dot right_dot{author(2), 1};
  left.clock[left_dot.author] = left_dot.sequence;
  right.clock[right_dot.author] = right_dot.sequence;
  test::NeutralTree::Delta left_changes;
  test::NeutralTree::Delta right_changes;
  const size_t step = keys / differing;
  size_t at = 0;
  size_t index = 0;
  for (const auto& [key, item] : entries) {
    if (at++ % step) continue;
    auto changed = item;
    changed.weight += 4096;
    test::stamp_item(changed, index % 2 ? left_dot : right_dot);
    (index % 2 ? left_changes : right_changes).upsert[key] = changed;
    (index % 2 ? left_entries : right_entries)[key] = changed;
    ++index;
  }
  left.root = test::neutral_tree().apply(*left.root, *writer, left_changes);
  right.root = test::neutral_tree().apply(*right.root, *writer, right_changes);
  const auto left_record = record_of(5, Hash256{}, left);
  const auto right_record = record_of(5, Hash256{}, right);
  const bool left_primary = left_record.hash < right_record.hash;
  const auto& primary = left_primary ? left_record : right_record;
  const auto& secondary = left_primary ? right_record : left_record;
  const State primary_state = std::make_shared<const NeutralHead>(left_primary ? left : right);
  const State secondary_state = std::make_shared<const NeutralHead>(left_primary ? right : left);

  const auto report = [](const char* name, std::chrono::steady_clock::duration elapsed) {
    std::cout << "BENCH name=" << name << " ns_per_op=" << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() << " ops=1\n";
  };
  auto started = std::chrono::steady_clock::now();
  const auto materialised = test::neutral_merge_over(left, right, left_entries, right_entries, left_record.hash, right_record.hash);
  heads::MemoryTreeNodeStore rebuilt;
  const auto expected_root = test::neutral_tree().build(materialised.entries, rebuilt);
  report("heads.reconcile.materialised", std::chrono::steady_clock::now() - started);

  for (auto* counter : {&costs.tree_nodes_read, &costs.tree_nodes_written}) *counter = 0;
  test::NeutralMerge merge;
  const auto reader = store.factory()();
  const heads::TreePause no_pause;
  started = std::chrono::steady_clock::now();
  const auto result = merge.merge(std::nullopt, heads::MergeHeads{primary, secondary, primary_state, secondary_state, *reader, *writer, no_pause});
  report("heads.reconcile.tree", std::chrono::steady_clock::now() - started);
  std::cout << "BENCH name=heads.reconcile.tree.node_reads ns_per_op=0 ops=" << costs.tree_nodes_read.load() << " keys=" << keys << '\n';
  EXPECT_EQ(*test::NeutralCodec::as_head(result.state)->root, expected_root);
  EXPECT_TRUE(test::neutral_conflicts(*test::NeutralCodec::as_head(result.state)).empty());

  MergeSpent out;
  out.cost = spent(costs);
  out.keys_examined = merge.keys_examined.load();
  out.leaves = test::neutral_tree().stats(origin.root.value(), *reader).leaves;
  return out;
}

TEST(HeadsCost, MergeFollowsWhatDiffersAndNotTheKeys) {
  constexpr size_t differing = 8;
  const auto small = merge_at(1000, differing);
  const auto large = merge_at(100000, differing);
  // The owner's merge sees what differs.
  EXPECT_EQ(small.keys_examined, differing);
  EXPECT_EQ(large.keys_examined, differing);
  // Reads follow k: far fewer than the leaves, and not growing with the keys beyond a level or two.
  EXPECT_GT(large.leaves, 1000u);
  EXPECT_LT(large.cost.nodes_read, large.leaves / 4);
  EXPECT_LE(large.cost.nodes_read, small.cost.nodes_read * 8);
  EXPECT_LE(large.cost.nodes_written, small.cost.nodes_written * 8);
  std::cout << "BENCH name=heads.merge.keys keys=100000 differing=" << differing << " nodes_read=" << large.cost.nodes_read
            << " nodes_written=" << large.cost.nodes_written << " leaves=" << large.leaves << '\n';
}

// The record's axis reports only (section 2.6): what grows the record is its clock.
TEST(HeadsCost, TheRecordAxisReportsOnly) {
  for (size_t authors : {size_t{1}, size_t{64}, size_t{4096}}) {
    KeyedBed bed(1000);
    for (size_t i = 0; i < authors; ++i) bed.head.clock[NodeId::random()] = i + 1;
    bed.parent = record_of(2, bed.bed.genesis().hash, bed.head);
    Replica replica(bed.options());
    EXPECT_TRUE(replica.store_commit(bed.parent));
    NeutralHead after;
    const auto delta = test::encode_neutral_delta(bed.change(bed.head, 1, after));
    const auto child = record_after(bed.parent, neutral_payload(after));
    bed.reset();
    EXPECT_TRUE(replica.store_commit(child, delta));
    const auto cost = spent(bed.costs);
    std::cout << "BENCH name=heads.verify.record_axis authors=" << authors << " record_bytes_encoded=" << cost.encoded << " record_bytes_hashed=" << cost.hashed
              << '\n';
  }
}

}  // namespace
}  // namespace dagda::heads
