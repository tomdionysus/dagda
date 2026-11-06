//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The cost baselines S3 and S4 close (the plan's section 2.6): verify, replay, accept
// and commit. They assert counts, never time: nothing here grows with the keys of the
// tree. A million-key run stays with the scale pass; merge is S6's.

#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <string>

#include "dagda/heads/replica.h"
#include "dagda/test/fixtures/heads.h"
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
