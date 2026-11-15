//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The replica on disk, over the neutral owner (the plan's session S3). Each case
// names Macha's it moved from (tests/test_storage_metadata.cpp at 9a08846, "SM"),
// assertion for assertion; where Macha asserted something of its own filesystem
// records the case says so. Macha's records named paths: a note stands in for each.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/heads/replica.h"
#include "dagda/test/fixtures/heads.h"
#include "dannan/identity/durable_file.h"

namespace dagda::heads {
namespace {

using test::accepted_by_two;
using test::append_journal_frame;
using test::author;
using test::commit_record;
using test::delta_between;
using test::flip_byte_at;
using test::flip_history_frame;
using test::hashes_of;
using test::head_with;
using test::journal_commit;
using test::journal_prepare_delta;
using test::journal_prepare_full;
using test::make_record;
using test::neutral_payload;
using test::NeutralCodec;
using test::NeutralEntries;
using test::NeutralHead;
using test::NeutralTree;
using test::record_after;
using test::record_of;
using test::record_with_clock;
using test::ReplicaBed;
using test::rootless_entry;
using test::SharedTreeStore;
using test::sorted_hashes;

std::vector<Hash256> just(const Record& record) { return {record.hash}; }

bool any_file_starts_with(const std::filesystem::path& directory, const std::string& prefix) {
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    if (entry.path().filename().string().rfind(prefix, 0) == 0) return true;
  return false;
}

NeutralHead with_note(NeutralHead head, const std::string& key) {
  head.notes.set(key, "");
  return head;
}

// test_metadata_branch_merge_history_roundtrip (SM:626). Macha's first half asserts
// its own merge (entries from both sides, the later file in place, two conflicts):
// owner's outcomes, which stay Macha's. The union below is this test's merge.
TEST(HeadsReplica, ABranchMergeRoundTripsThroughHistory) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto left_head = head_with({"left", "same-left"}, {{author(1), 1}});
  const auto right_head = head_with({"right", "same-right"}, {{author(2), 1}});
  const auto left = record_of(genesis.generation + 1, genesis.hash, left_head);
  const auto right = record_of(genesis.generation + 1, genesis.hash, right_head);
  ASSERT_NE(left.hash, right.hash);

  auto branch = head_with({"left", "same-left", "right", "same-right"}, {{author(1), 1}, {author(2), 1}});
  branch.merge_parents = {right.hash};
  const auto branch_encoded = neutral_payload(branch);
  const auto branch_decoded = test::neutral_head_from(branch_encoded);
  EXPECT_EQ(branch_decoded.merge_parents, branch.merge_parents);

  {
    auto left_replica = bed.open("branch-left");
    auto right_replica = bed.open("branch-right");
    commit_record(*left_replica, left);
    commit_record(*right_replica, right);

    const auto right_history = right_replica->history_entry(right.hash);
    ASSERT_TRUE(right_history.has_value());
    ASSERT_TRUE(left_replica->import_history(*right_history));
    const auto common = left_replica->history_common_ancestor(left.hash, right.hash);
    ASSERT_TRUE(common.has_value());
    EXPECT_EQ(*common, genesis.hash);

    const auto reconciliation = make_record(std::max(left.generation, right.generation) + 1, left.hash, branch_encoded);
    commit_record(*left_replica, reconciliation);
    // The right branch can accept the same merge because its head is an explicit
    // secondary parent, even though the primary parent is left.
    commit_record(*right_replica, reconciliation);
    EXPECT_TRUE(left_replica->history_is_ancestor(left.hash, reconciliation.hash));
    EXPECT_TRUE(left_replica->history_is_ancestor(right.hash, reconciliation.hash));
  }

  auto reopened_left = bed.open("branch-left");
  auto reopened_right = bed.open("branch-right");
  EXPECT_EQ(reopened_left->committed().hash, reopened_right->committed().hash);
  EXPECT_TRUE(reopened_left->history_is_ancestor(left.hash, reopened_left->committed().hash));
  EXPECT_TRUE(reopened_left->history_is_ancestor(right.hash, reopened_left->committed().hash));
  const auto historical_merge = reopened_left->historical(reopened_left->committed().hash);
  ASSERT_TRUE(historical_merge.has_value());
  const auto reopened_head = test::neutral_head_from(historical_merge->payload);
  EXPECT_EQ(reopened_head.notes, branch.notes);
  EXPECT_EQ(reopened_head.merge_parents, branch.merge_parents);
}

// test_metadata_commit_store_acceptance_heads_roundtrip (SM:739). Macha's merged
// entries are the owner's merge; the roster clause is its `metadata_participants`,
// a slot no generic field holds: a certificate naming any replica is accepted.
TEST(HeadsReplica, CommitStoreAcceptanceAndHeadsRoundTrip) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto make_child = [&](const char* name, uint8_t who) {
    return record_of(genesis.generation + 1, genesis.hash, head_with({name}, {{author(who), 1}}, 2));
  };
  const auto left = make_child("left", 1);
  const auto right = make_child("right", 2);
  ASSERT_NE(left.hash, right.hash);
  EXPECT_EQ(test::neutral_head_from(left.payload).floor, 2U);

  const auto a = author(1);
  const auto b = author(2);
  const auto c = author(3);
  const Acceptance left_accept{left.generation, left.hash, 2, {a, b}};
  const Acceptance right_accept{right.generation, right.hash, 2, {b, c}};

  {
    const auto rostered = record_of(genesis.generation + 1, genesis.hash, head_with({"roster-is-not-authority"}, {}, 2));
    auto replica = bed.open("roster-not-authority");
    ASSERT_TRUE(replica->store_commit(rostered));
    EXPECT_TRUE(replica->accept_commit({rostered.generation, rostered.hash, 2, {a, c}}));
  }

  {
    auto replica = bed.open("accepted-heads");
    ASSERT_TRUE(replica->store_commit(left));
    const auto before_acceptance = replica->diagnostics();
    ASSERT_TRUE(replica->accept_commit(left_accept));
    const auto after_left = replica->diagnostics();
    EXPECT_EQ(after_left.accepted_head_persistence_writes, before_acceptance.accepted_head_persistence_writes + 1);
    EXPECT_GT(after_left.accepted_head_persistence_bytes, before_acceptance.accepted_head_persistence_bytes);
    EXPECT_EQ(after_left.accepted_head_persistence_failures, 0U);
    ASSERT_TRUE(replica->store_commit(right));
    ASSERT_TRUE(replica->accept_commit(right_accept));
    const auto after_right = replica->diagnostics();
    EXPECT_EQ(after_right.accepted_head_persistence_writes, after_left.accepted_head_persistence_writes + 1);
    EXPECT_GT(after_right.accepted_head_persistence_bytes, after_left.accepted_head_persistence_bytes);
    EXPECT_EQ(after_right.accepted_head_persistence_failures, 0U);
    ASSERT_EQ(replica->accepted_heads().size(), 2U);
    EXPECT_TRUE(replica->history_is_ancestor(genesis.hash, left.hash));
    EXPECT_TRUE(replica->history_is_ancestor(genesis.hash, right.hash));
  }

  Record reconciliation;
  Acceptance merge_accept;
  {
    auto replica = bed.open("accepted-heads");
    ASSERT_EQ(replica->accepted_heads().size(), 2U);
    const auto common = replica->history_common_ancestor(left.hash, right.hash);
    ASSERT_TRUE(common.has_value());
    EXPECT_EQ(*common, genesis.hash);

    auto primary = left;
    auto secondary = right;
    if (secondary.hash < primary.hash) std::swap(primary, secondary);
    auto merged = head_with({"left", "right"}, {{author(1), 1}, {author(2), 1}}, 2);
    merged.merge_parents = {secondary.hash};
    reconciliation = record_of(std::max(left.generation, right.generation) + 1, primary.hash, merged);
    merge_accept = {reconciliation.generation, reconciliation.hash, 2, {a, c}};
    ASSERT_TRUE(replica->store_commit(reconciliation));
    ASSERT_TRUE(replica->accept_commit(merge_accept));
    const auto heads = replica->accepted_heads();
    ASSERT_EQ(heads.size(), 1U);
    EXPECT_EQ(heads.front().hash, reconciliation.hash);
    EXPECT_TRUE(replica->history_is_ancestor(left.hash, reconciliation.hash));
    EXPECT_TRUE(replica->history_is_ancestor(right.hash, reconciliation.hash));
  }

  auto reopened = bed.open("accepted-heads");
  const auto heads = reopened->accepted_heads();
  ASSERT_EQ(heads.size(), 1U);
  EXPECT_EQ(heads.front().hash, reconciliation.hash);
  const auto certificate = reopened->acceptance(reconciliation.hash);
  ASSERT_TRUE(certificate.has_value());
  EXPECT_EQ(*certificate, merge_accept);

  // With one head the history is truncated to it: the head stays reconstructable
  // and the branches behind it are dropped.
  const auto history_path = bed.path("accepted-heads") / "history.log";
  const auto history_before = std::filesystem::file_size(history_path);
  ASSERT_TRUE(reopened->compact_history_if_safe(1, 1));
  EXPECT_LT(std::filesystem::file_size(history_path), history_before);
  EXPECT_FALSE(reopened->historical(left.hash).has_value());
  EXPECT_FALSE(reopened->historical(right.hash).has_value());
  ASSERT_TRUE(reopened->historical(reconciliation.hash).has_value());

  auto rerooted = bed.open("accepted-heads");
  EXPECT_FALSE(rerooted->historical(left.hash).has_value());
  ASSERT_TRUE(rerooted->historical(reconciliation.hash).has_value());
  EXPECT_EQ(rerooted->historical(reconciliation.hash)->payload, reconciliation.payload);
}

// test_accept_commit_refreshes_checkpoint_on_both_repair_and_ordinary_paths (SM:877).
TEST(HeadsReplica, AcceptRefreshesTheCheckpointOnBothPaths) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto child_head = head_with({"child"});
  const auto child = record_of(genesis.generation + 1, genesis.hash, child_head);
  const auto grandchild = record_after(child, neutral_payload(with_note(child_head, "grandchild")));
  const auto a = author(1);

  auto replica = bed.open("checkpoint-refresh-paths");
  ASSERT_TRUE(replica->store_commit(child));
  ASSERT_TRUE(replica->accept_commit({child.generation, child.hash, 1, {a}}));
  EXPECT_EQ(replica->committed().hash, child.hash);
  {
    // Reopening loads the checkpoint on disk, proving it landed.
    auto reopened = bed.open("checkpoint-refresh-paths");
    EXPECT_EQ(reopened->committed().hash, child.hash);
  }

  ASSERT_TRUE(replica->store_commit(grandchild));
  // A direct descendant of the sole head prunes it and moves the checkpoint on.
  ASSERT_TRUE(replica->accept_commit({grandchild.generation, grandchild.hash, 1, {a}}));
  const auto heads = replica->accepted_heads();
  ASSERT_EQ(heads.size(), 1U);
  EXPECT_EQ(heads.front().hash, grandchild.hash);
  EXPECT_EQ(replica->committed().hash, grandchild.hash);
  {
    auto reopened = bed.open("checkpoint-refresh-paths");
    EXPECT_EQ(reopened->committed().hash, grandchild.hash);
  }
}

// What SM:933, SM:1007 and SM:1112 build: an older and a newer head from genesis,
// a merge numbered above the newer as a delta on the older (the lower hash), and a
// divergent head that overtakes it.
struct GenerationGap {
  NeutralHead older_head;
  NeutralHead newer_head;
  NeutralHead merged_head;
  Record older;
  Record newer;
  Record merge;
  Bytes merge_delta;
  Record divergent;

  explicit GenerationGap(const Record& genesis) {
    older_head = head_with({"older"}, {}, 2);
    older = record_of(10, genesis.hash, older_head);
    for (size_t attempt = 0; attempt < 256; ++attempt) {
      newer_head = head_with({}, {}, 2);
      newer_head.notes.set("newer-" + std::to_string(attempt), "");
      newer = record_of(20, genesis.hash, newer_head);
      if (older.hash < newer.hash) break;
    }
    merged_head = older_head;
    for (const auto& [key, value] : newer_head.notes.get()) merged_head.notes.set(key, value);
    merged_head.merge_parents = {newer.hash};
    // The deterministic lower hash, not generation - 1.
    merge = record_of(newer.generation + 1, older.hash, merged_head);
    merge_delta = delta_between(older_head, merged_head);
    divergent = record_of(30, newer.hash, with_note(newer_head, "divergent"));
  }
};

// test_merge_delta_primary_may_precede_merge_generation (SM:933).
TEST(HeadsReplica, AMergeDeltasPrimaryMayPrecedeItsGeneration) {
  ReplicaBed bed;
  const auto a = author(1);
  const auto b = author(2);
  const GenerationGap gap(bed.genesis());
  ASSERT_TRUE(gap.older.hash < gap.newer.hash);
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(gap.older));
    ASSERT_TRUE(replica->accept_commit({gap.older.generation, gap.older.hash, 2, {a, b}}));
    ASSERT_TRUE(replica->store_commit(gap.newer));
    ASSERT_TRUE(replica->accept_commit({gap.newer.generation, gap.newer.hash, 2, {a, b}}));
    ASSERT_TRUE(replica->store_commit(gap.merge, gap.merge_delta));
    ASSERT_TRUE(replica->accept_commit({gap.merge.generation, gap.merge.hash, 2, {a, b}}));
    const auto entry = replica->history_entry(gap.merge.hash);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->body, HistoryEntry::Body::delta);
    EXPECT_LT(gap.older.generation + 1, gap.merge.generation);
  }
  auto reopened = bed.open();
  const auto heads = reopened->accepted_heads();
  ASSERT_EQ(heads.size(), 1U);
  EXPECT_EQ(heads.front().hash, gap.merge.hash);
  EXPECT_TRUE(reopened->history_is_ancestor(gap.older.hash, gap.merge.hash));
  EXPECT_TRUE(reopened->history_is_ancestor(gap.newer.hash, gap.merge.hash));
}

// test_merge_delta_with_generation_gap_reconstructs_after_cache_eviction_and_reopen
// (SM:1007).
TEST(HeadsReplica, AGenerationGapDeltaReconstructsAfterEvictionAndReopen) {
  ReplicaBed bed;
  const auto a = author(1);
  const auto b = author(2);
  const GenerationGap gap(bed.genesis());
  {
    // A one-byte budget: only the pinned current and committed head stay cached, so
    // other reads reconstruct from history.log.
    auto replica = bed.open("space", {}, true, 1);
    ASSERT_TRUE(replica->store_commit(gap.older));
    ASSERT_TRUE(replica->accept_commit({gap.older.generation, gap.older.hash, 2, {a, b}}));
    ASSERT_TRUE(replica->store_commit(gap.newer));
    ASSERT_TRUE(replica->accept_commit({gap.newer.generation, gap.newer.hash, 2, {a, b}}));
    ASSERT_TRUE(replica->store_commit(gap.merge, gap.merge_delta));
    ASSERT_TRUE(replica->accept_commit({gap.merge.generation, gap.merge.hash, 2, {a, b}}));
    const auto entry = replica->history_entry(gap.merge.hash);
    ASSERT_TRUE(entry.has_value());
    ASSERT_EQ(entry->body, HistoryEntry::Body::delta);

    ASSERT_TRUE(replica->store_commit(gap.divergent));
    ASSERT_TRUE(replica->accept_commit({gap.divergent.generation, gap.divergent.hash, 2, {a, b}}));
    EXPECT_EQ(replica->committed().hash, gap.divergent.hash);
    // Any further materialisation evicts the now unpinned merge.
    ASSERT_TRUE(replica->materialized(gap.divergent.hash));

    const auto reconstructed = replica->materialized(gap.merge.hash);
    ASSERT_TRUE(reconstructed);
    EXPECT_EQ(reconstructed->record.hash, gap.merge.hash);
    EXPECT_EQ(reconstructed->record.payload, gap.merge.payload);
    EXPECT_EQ(replica->accepted_heads().size(), 2U);
  }

  // A restart does not throw and still sees both heads.
  auto reopened = bed.open("space", {}, true, 1);
  EXPECT_EQ(reopened->accepted_heads().size(), 2U);
  const auto reconstructed = reopened->materialized(gap.merge.hash);
  ASSERT_TRUE(reconstructed);
  EXPECT_EQ(reconstructed->record.payload, gap.merge.payload);
  EXPECT_TRUE(reopened->history_is_ancestor(gap.older.hash, gap.merge.hash));
  EXPECT_TRUE(reopened->history_is_ancestor(gap.newer.hash, gap.merge.hash));
}

// test_unreconstructable_accepted_head_is_kept_at_startup_and_reanchored_live
// (SM:1112).
TEST(HeadsReplica, AnUnreconstructableHeadIsKeptAtOpenAndReanchoredLive) {
  ReplicaBed bed;
  const auto a = author(1);
  const auto b = author(2);
  const GenerationGap gap(bed.genesis());
  const auto source_history = bed.path("source") / "history.log";

  // Source: a healthy replica holding both heads, the merge as a delta.
  auto source = bed.open("source", {}, true, 1);
  ASSERT_TRUE(source->store_commit(gap.older));
  ASSERT_TRUE(source->accept_commit({gap.older.generation, gap.older.hash, 2, {a, b}}));
  ASSERT_TRUE(source->store_commit(gap.newer));
  ASSERT_TRUE(source->accept_commit({gap.newer.generation, gap.newer.hash, 2, {a, b}}));
  ASSERT_TRUE(source->store_commit(gap.merge, gap.merge_delta));
  const auto merge_frame_end = std::filesystem::file_size(source_history);
  ASSERT_TRUE(source->accept_commit({gap.merge.generation, gap.merge.hash, 2, {a, b}}));
  ASSERT_TRUE(source->store_commit(gap.divergent));
  ASSERT_TRUE(source->accept_commit({gap.divergent.generation, gap.divergent.hash, 2, {a, b}}));
  ASSERT_EQ(source->accepted_heads().size(), 2U);

  // Target: identical except it never received the merge's frame, then finds itself
  // with the source's certificates naming the merge.
  {
    auto target = bed.open("target", {}, true, 1);
    ASSERT_TRUE(target->store_commit(gap.older));
    ASSERT_TRUE(target->accept_commit({gap.older.generation, gap.older.hash, 2, {a, b}}));
    ASSERT_TRUE(target->store_commit(gap.newer));
    ASSERT_TRUE(target->accept_commit({gap.newer.generation, gap.newer.hash, 2, {a, b}}));
    ASSERT_TRUE(target->store_commit(gap.divergent));
    ASSERT_TRUE(target->accept_commit({gap.divergent.generation, gap.divergent.hash, 2, {a, b}}));
    EXPECT_EQ(target->committed().hash, gap.divergent.hash);
  }
  std::filesystem::copy_file(bed.path("source") / "heads.meta", bed.path("target") / "heads.meta", std::filesystem::copy_options::overwrite_existing);

  // (a) Opening keeps the certificate and flags the head instead of throwing.
  auto target = bed.open("target", {}, true, 1);
  EXPECT_FALSE(target->recovery_required());
  ASSERT_EQ(target->accepted_head_certificates().size(), 2U);
  EXPECT_EQ(target->accepted_heads().size(), 1U);  // the merge excluded from reads
  const auto flagged = target->unreconstructable_heads();
  ASSERT_EQ(flagged.size(), 1U);
  EXPECT_EQ(flagged.front(), gap.merge.hash);
  EXPECT_FALSE(target->materialized(gap.merge.hash));

  // (c) A peer serves the record whole and the target re-anchors it in place.
  const auto served = source->full_history_record(gap.merge.hash);
  ASSERT_TRUE(served.has_value());
  EXPECT_EQ(served->body, HistoryEntry::Body::full);
  EXPECT_EQ(served->merge_parents, just(gap.newer));
  ASSERT_TRUE(target->reanchor_history(*served));
  EXPECT_TRUE(target->unreconstructable_heads().empty());
  EXPECT_EQ(target->accepted_heads().size(), 2U);
  const auto repaired = target->materialized(gap.merge.hash);
  ASSERT_TRUE(repaired);
  EXPECT_EQ(repaired->record.payload, gap.merge.payload);
  EXPECT_TRUE(target->history_is_ancestor(gap.older.hash, gap.merge.hash));

  // Garbage is refused: a whole body whose hash does not bind its content.
  auto forged = *served;
  forged.generation += 1;
  EXPECT_FALSE(target->reanchor_history(forged));

  // Re-anchoring over an indexed but unreplayable frame supersedes it in place, and
  // a restart prefers the whole frame over the older delta.
  {
    flip_history_frame(source_history, merge_frame_end);
    ASSERT_EQ(source->accepted_heads().size(), 1U);
    ASSERT_EQ(source->unreconstructable_heads(), just(gap.merge));
    HistoryEntry full;
    full.generation = gap.merge.generation;
    full.previous = gap.merge.previous;
    full.hash = gap.merge.hash;
    full.previous_known = true;
    full.merge_parents = {gap.newer.hash};
    full.body = HistoryEntry::Body::full;
    full.payload.assign(gap.merge.payload.begin(), gap.merge.payload.end());
    const auto before = source->diagnostics().history_records;
    ASSERT_TRUE(source->reanchor_history(full));
    EXPECT_EQ(source->diagnostics().history_records, before + 1);
    // The superseded delta frame is whole again for the restart below.
    flip_history_frame(source_history, merge_frame_end);
    const auto entry = source->history_entry(gap.merge.hash);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->body, HistoryEntry::Body::full);
    EXPECT_TRUE(source->unreconstructable_heads().empty());
    EXPECT_EQ(source->accepted_heads().size(), 2U);
  }
  auto reopened_source = bed.open("source", {}, true, 1);
  EXPECT_EQ(reopened_source->accepted_heads().size(), 2U);
  const auto reopened_entry = reopened_source->history_entry(gap.merge.hash);
  ASSERT_TRUE(reopened_entry.has_value());
  EXPECT_EQ(reopened_entry->body, HistoryEntry::Body::full);
  ASSERT_TRUE(reopened_source->materialized(gap.merge.hash));
  auto reopened_target = bed.open("target", {}, true, 1);
  EXPECT_EQ(reopened_target->accepted_heads().size(), 2U);
  EXPECT_TRUE(reopened_target->unreconstructable_heads().empty());
}

// test_full_fallback_boundary_heals_when_parent_arrives (SM:1273).
TEST(HeadsReplica, AFullFallbackBoundaryHealsWhenItsParentArrives) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto parent_head = head_with({"parent"}, {}, 2);
  const auto parent = record_of(49, genesis.hash, parent_head);
  const auto child_head = with_note(parent_head, "child");
  const auto child = record_of(50, parent.hash, child_head);
  const auto descendant = record_of(51, child.hash, with_note(child_head, "descendant"));
  const auto sibling = record_of(50, parent.hash, with_note(parent_head, "sibling"));

  auto source = bed.open("boundary-source");
  ASSERT_TRUE(source->store_commit(parent));
  const auto parent_entry = source->history_entry(parent.hash);
  ASSERT_TRUE(parent_entry.has_value());

  {
    auto target = bed.open("boundary-target");
    ASSERT_TRUE(target->store_commit(child));
    const auto boundary = target->history_entry(child.hash);
    ASSERT_TRUE(boundary.has_value());
    EXPECT_FALSE(boundary->previous_known);

    // The head's direct predecessor exists and the missing ancestry is one level
    // deeper, so healing the head alone does not reach the compacted boundary.
    ASSERT_TRUE(target->store_commit(descendant));
    ASSERT_TRUE(target->history_contains(descendant.previous));
    EXPECT_FALSE(target->history_contains(parent.hash));
    const auto resident_before = target->diagnostics().materialization_cache_bytes;
    const auto links = target->history_links(descendant.hash);
    ASSERT_TRUE(links.has_value());
    EXPECT_EQ(links->previous, child.hash);
    EXPECT_EQ(target->diagnostics().materialization_cache_bytes, resident_before);

    ASSERT_TRUE(target->import_history(*parent_entry));
    ASSERT_TRUE(target->store_commit(sibling));
    const auto common = target->history_common_ancestor(descendant.hash, sibling.hash);
    ASSERT_TRUE(common.has_value());
    EXPECT_EQ(*common, parent.hash);
  }

  auto reopened = bed.open("boundary-target");
  const auto common = reopened->history_common_ancestor(descendant.hash, sibling.hash);
  ASSERT_TRUE(common.has_value());
  EXPECT_EQ(*common, parent.hash);
}

// test_compacted_direct_predecessor_cannot_resurface_as_head (SM:1355).
TEST(HeadsReplica, ACompactedDirectPredecessorCannotResurfaceAsAHead) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto parent_head = head_with({"parent"}, {}, 2);
  const auto parent = record_of(genesis.generation + 1, genesis.hash, parent_head);
  const auto child = record_of(parent.generation + 1, parent.hash, with_note(parent_head, "child"));
  const Acceptance parent_accept = accepted_by_two(parent);
  const Acceptance child_accept = accepted_by_two(child);

  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(parent));
    ASSERT_TRUE(replica->accept_commit(parent_accept));
    ASSERT_TRUE(replica->store_commit(child));
    ASSERT_TRUE(replica->accept_commit(child_accept));
    auto heads = replica->accepted_heads();
    ASSERT_EQ(heads.size(), 1U);
    EXPECT_EQ(heads.front().hash, child.hash);

    const auto entry = replica->history_entry(parent.hash);
    ASSERT_TRUE(entry.has_value());
    const auto stale_parent = *entry;
    ASSERT_TRUE(replica->compact_history_if_safe(1, 1));
    EXPECT_FALSE(replica->historical(parent.hash).has_value());
    EXPECT_TRUE(replica->history_is_ancestor(parent.hash, child.hash));

    // A restarted peer advertising an old but valid head after this node compacted
    // away the predecessor's payload.
    ASSERT_TRUE(replica->import_history(stale_parent));
    ASSERT_TRUE(replica->accept_commit(parent_accept));
    heads = replica->accepted_heads();
    ASSERT_EQ(heads.size(), 1U);
    EXPECT_EQ(heads.front().hash, child.hash);

    const auto common = replica->history_common_ancestor(parent.hash, child.hash);
    ASSERT_TRUE(common.has_value());
    EXPECT_EQ(*common, parent.hash);
  }

  auto reopened = bed.open();
  const auto heads = reopened->accepted_heads();
  ASSERT_EQ(heads.size(), 1U);
  EXPECT_EQ(heads.front().hash, child.hash);
}

// test_unreconstructable_accepted_head_is_rate_limited_not_hammered (SM:1428), and
// past Macha's case: time is injected here, so the cooldown's end is asserted too.
TEST(HeadsReplica, AnUnreconstructableHeadIsRateLimited) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  // Siblings from genesis: each acceptance below is a real conflicting branch.
  const auto head_a = record_of(genesis.generation + 1, genesis.hash, head_with({"a"}));
  const auto head_b = record_of(genesis.generation + 1, genesis.hash, head_with({"b"}));
  const auto head_c = record_of(genesis.generation + 1, genesis.hash, head_with({"c"}));
  // Its higher generation makes it committed, so nothing pins head_a in the cache.
  const auto head_z = record_of(5, genesis.hash, head_with({"z"}));

  auto replica = bed.open("space", {}, true, 1);
  const auto witness = author(1);
  const auto history = bed.path() / "history.log";
  ASSERT_TRUE(replica->store_commit(head_a));
  const auto head_a_frame_end = std::filesystem::file_size(history);
  ASSERT_TRUE(replica->accept_commit({head_a.generation, head_a.hash, 1, {witness}}));
  ASSERT_TRUE(replica->store_commit(head_z));
  ASSERT_TRUE(replica->accept_commit({head_z.generation, head_z.hash, 1, {witness}}));
  ASSERT_EQ(replica->committed().hash, head_z.hash);
  ASSERT_EQ(replica->accepted_heads().size(), 2U);

  // head_a's frame stops authenticating.
  flip_history_frame(history, head_a_frame_end);

  ASSERT_TRUE(replica->store_commit(head_b));
  bool threw = false;
  try {
    replica->accept_commit({head_b.generation, head_b.hash, 1, {witness}});
  } catch (const std::exception& error) {
    threw = true;
    EXPECT_NE(std::string(error.what()).find("cannot be reconstructed"), std::string::npos);
  }
  EXPECT_TRUE(threw);  // the first occurrence still surfaces
  ASSERT_EQ(replica->unreconstructable_heads(), just(head_a));

  // The frame is whole again: from here a reconstruction would succeed, so only the
  // cooldown keeps it excluded.
  flip_history_frame(history, head_a_frame_end);

  // A second fork accepted shortly after neither tries head_a nor throws.
  ASSERT_TRUE(replica->store_commit(head_c));
  EXPECT_NO_THROW(EXPECT_TRUE(replica->accept_commit({head_c.generation, head_c.hash, 1, {witness}})));

  // accepted_heads(), the hottest path, stays quiet too and returns the rest.
  for (int i = 0; i < 5; ++i) {
    std::vector<Record> heads;
    EXPECT_NO_THROW(heads = replica->accepted_heads());
    ASSERT_EQ(heads.size(), 3U);
    for (const auto& head : heads) EXPECT_NE(head.hash, head_a.hash);
  }
  // Not retried within the cooldown.
  EXPECT_EQ(replica->unreconstructable_heads(), just(head_a));

  bed.time.advance(std::chrono::seconds(29));
  EXPECT_EQ(replica->accepted_heads().size(), 3U);
  bed.time.advance(std::chrono::seconds(1));
  EXPECT_EQ(replica->accepted_heads().size(), 4U);
  EXPECT_TRUE(replica->unreconstructable_heads().empty());
}

// test_pristine_joiner_adopts_compacted_cluster_head (SM:1544).
TEST(HeadsReplica, APristineJoinerAdoptsACompactedClusterHead) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto established = record_of(50, genesis.hash, head_with({"established"}, {{author(1), 7}}, 2));
  const auto established_accept = accepted_by_two(established);

  HistoryEntry compacted_head;
  {
    auto replica = bed.open("established");
    ASSERT_TRUE(replica->store_commit(established));
    ASSERT_TRUE(replica->accept_commit(established_accept));
    ASSERT_TRUE(replica->compact_history_if_safe(1, 1));
    const auto entry = replica->history_entry(established.hash);
    ASSERT_TRUE(entry.has_value());
    compacted_head = *entry;
    EXPECT_FALSE(compacted_head.previous_known);
    EXPECT_FALSE(replica->historical(genesis.hash).has_value());
  }

  HistoryEntry genesis_entry;
  {
    auto joiner = bed.open("joiner", {}, false);
    EXPECT_TRUE(joiner->accepted_heads().empty());
    const auto entry = joiner->history_entry(genesis.hash);
    ASSERT_TRUE(entry.has_value());
    genesis_entry = *entry;

    // A pristine node imports a valid head whose ancestry was compacted away;
    // genesis is a semantic ancestor, so adoption is immediate.
    ASSERT_TRUE(joiner->import_history(compacted_head));
    ASSERT_TRUE(joiner->accept_commit(established_accept));
    const auto heads = joiner->accepted_heads();
    ASSERT_EQ(heads.size(), 1U);
    EXPECT_EQ(heads.front().hash, established.hash);
    EXPECT_EQ(joiner->committed().hash, established.hash);
  }

  {
    auto replica = bed.open("established");
    // The opposite race: an established replica sees the joiner's genesis
    // certificate before the joiner has adopted the cluster head.
    ASSERT_TRUE(replica->import_history(genesis_entry));
    ASSERT_TRUE(replica->accept_commit({genesis.generation, genesis.hash, 0, {}}));
    const auto heads = replica->accepted_heads();
    ASSERT_EQ(heads.size(), 1U);
    EXPECT_EQ(heads.front().hash, established.hash);
  }

  auto reopened_joiner = bed.open("joiner", {}, false);
  const auto joiner_heads = reopened_joiner->accepted_heads();
  ASSERT_EQ(joiner_heads.size(), 1U);
  EXPECT_EQ(joiner_heads.front().hash, established.hash);

  auto reopened_established = bed.open("established");
  const auto established_heads = reopened_established->accepted_heads();
  ASSERT_EQ(established_heads.size(), 1U);
  EXPECT_EQ(established_heads.front().hash, established.hash);
}

// test_a_head_with_no_clock_is_nobodys_ancestor (SM:1683).
TEST(HeadsReplica, AHeadWithNoClockIsNobodysAncestor) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  Hash256 unseen{};
  unseen.bytes[0] = 0x55;
  const auto clockless = record_with_clock(genesis.generation + 4, unseen, {}, {"only-here"});
  const auto other = record_with_clock(genesis.generation + 2, genesis.hash, {{author(2), 1}}, {"other"});
  auto replica = bed.open();
  ASSERT_TRUE(replica->import_history(rootless_entry(clockless)));
  ASSERT_TRUE(replica->accept_commit(accepted_by_two(clockless)));
  ASSERT_TRUE(replica->import_history(rootless_entry(other)));
  ASSERT_TRUE(replica->accept_commit(accepted_by_two(other)));
  EXPECT_EQ(hashes_of(replica->accepted_heads()), sorted_hashes({&clockless, &other}));
}

// test_a_head_from_before_truncation_is_recognised_by_its_clock (SM:1702).
TEST(HeadsReplica, AHeadFromBeforeTruncationIsRecognisedByItsClock) {
  ReplicaBed bed;
  const auto a = author(1);
  const auto b = author(2);
  const auto genesis = bed.genesis();
  const auto old = record_with_clock(genesis.generation + 1, genesis.hash, {{a, 1}}, {"old"});
  const auto between = record_with_clock(old.generation + 1, old.hash, {{a, 2}}, {"old", "between"});
  const auto now = record_with_clock(between.generation + 1, between.hash, {{a, 3}}, {"old", "between", "now"});
  // Written on `old` by another author: `now` does not carry it.
  const auto side = record_with_clock(old.generation + 1, old.hash, {{a, 1}, {b, 1}}, {"old", "side"});

  {
    auto replica = bed.open();
    for (const auto* record : {&old, &between, &now}) {
      ASSERT_TRUE(replica->store_commit(*record));
      ASSERT_TRUE(replica->accept_commit(accepted_by_two(*record)));
    }
    ASSERT_TRUE(replica->compact_history_if_safe(1, 1));
    EXPECT_EQ(replica->diagnostics().history_records, 1U);
    EXPECT_FALSE(replica->historical(old.hash).has_value());

    // A peer still holds `old` and offers it. No edge in history joins the two; the
    // clock of `now` covers it.
    ASSERT_TRUE(replica->import_history(rootless_entry(old)));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(old)));
    EXPECT_FALSE(replica->history_is_ancestor(old.hash, now.hash));
    EXPECT_EQ(hashes_of(replica->accepted_heads()), just(now));
    EXPECT_EQ(replica->committed().hash, now.hash);

    // A head whose clock `now` does not cover stays beside it, to be merged.
    ASSERT_TRUE(replica->import_history(rootless_entry(side)));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(side)));
    EXPECT_EQ(hashes_of(replica->accepted_heads()), sorted_hashes({&now, &side}));

    // History is not truncated while two heads are held.
    const auto records = replica->diagnostics().history_records;
    EXPECT_FALSE(replica->compact_history_if_safe(1, 1));
    EXPECT_EQ(replica->diagnostics().history_records, records);
  }

  auto reopened = bed.open();
  EXPECT_EQ(hashes_of(reopened->accepted_heads()), sorted_hashes({&now, &side}));
}

// test_returning_replica_adopts_a_head_whose_ancestry_a_peer_truncated (SM:1754).
TEST(HeadsReplica, AReturningReplicaAdoptsAHeadWhoseAncestryAPeerTruncated) {
  ReplicaBed bed;
  const auto a = author(1);
  const auto b = author(2);
  const auto genesis = bed.genesis();
  const auto floor = record_with_clock(genesis.generation + 1, genesis.hash, {{a, 1}}, {"floor"});
  const auto between = record_with_clock(floor.generation + 1, floor.hash, {{a, 2}}, {"floor", "between"});
  const auto beyond = record_with_clock(between.generation + 1, between.hash, {{a, 2}, {b, 1}}, {"floor", "between", "beyond"});

  // The peer went on from `floor` and truncated: its head is all it serves.
  HistoryEntry served;
  {
    auto peer = bed.open("peer");
    for (const auto* record : {&floor, &between, &beyond}) {
      ASSERT_TRUE(peer->store_commit(*record));
      ASSERT_TRUE(peer->accept_commit(accepted_by_two(*record)));
    }
    ASSERT_TRUE(peer->compact_history_if_safe(1, 1));
    const auto entry = peer->history_entry(beyond.hash);
    ASSERT_TRUE(entry.has_value());
    served = *entry;
    EXPECT_FALSE(served.previous_known);
  }

  // The returning replica holds `floor` and never saw `between`.
  {
    auto replica = bed.open("returning");
    ASSERT_TRUE(replica->store_commit(floor));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(floor)));
    ASSERT_TRUE(replica->import_history(served));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(beyond)));
    EXPECT_FALSE(replica->history_is_ancestor(floor.hash, beyond.hash));
    EXPECT_EQ(hashes_of(replica->accepted_heads()), just(beyond));
    EXPECT_EQ(replica->committed().hash, beyond.hash);
  }

  // A later head that does not carry this replica's mutations is a second head,
  // whatever its generation.
  {
    Hash256 unseen{};
    unseen.bytes[0] = 0x77;
    const auto foreign = record_with_clock(floor.generation + 5, unseen, {{b, 4}}, {"foreign"});
    auto replica = bed.open("diverged");
    ASSERT_TRUE(replica->store_commit(floor));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(floor)));
    ASSERT_TRUE(replica->import_history(rootless_entry(foreign)));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(foreign)));
    EXPECT_EQ(hashes_of(replica->accepted_heads()), sorted_hashes({&floor, &foreign}));
  }
}

// Holds the write of one file until released: Macha's fsync_hold, at the seam
// Dagda has for it, so the case runs on every platform and not only Linux.
class WriteHold {
 public:
  explicit WriteHold(std::filesystem::path path) : _path(std::move(path)) {}

  std::function<void(const std::filesystem::path&, std::string_view)> writer() {
    return [this](const std::filesystem::path& path, std::string_view contents) {
      if (path == _path) {
        std::unique_lock lock(_mutex);
        if (_armed) {
          _holding = true;
          _changed.notify_all();
          _changed.wait(lock, [&] { return !_armed; });
        }
      }
      dannan::identity::durable_replace_file(path, contents);
    };
  }
  void arm() {
    std::lock_guard lock(_mutex);
    _armed = true;
  }
  bool wait_until_holding() {
    std::unique_lock lock(_mutex);
    return _changed.wait_for(lock, std::chrono::seconds(60), [&] { return _holding; });
  }
  void release() {
    {
      std::lock_guard lock(_mutex);
      _armed = false;
    }
    _changed.notify_all();
  }

 private:
  const std::filesystem::path _path;
  std::mutex _mutex;
  std::condition_variable _changed;
  bool _armed = false;
  bool _holding = false;
};

// test_a_checkpoint_write_holds_no_reader_of_the_replica (SM:1812).
TEST(HeadsReplica, ACheckpointWriteHoldsNoReader) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto mine = record_with_clock(genesis.generation + 1, genesis.hash, {{author(1), 1}}, {"mine"});
  const auto theirs = record_with_clock(genesis.generation + 3, genesis.hash, {{author(2), 1}}, {"theirs"});
  WriteHold hold(bed.path() / "checkpoint.meta");
  {
    auto options = bed.options();
    options.replace_file = hold.writer();
    Replica replica(std::move(options));
    ASSERT_TRUE(replica.store_commit(mine));
    ASSERT_TRUE(replica.accept_commit(accepted_by_two(mine)));
    ASSERT_TRUE(replica.import_history(rootless_entry(theirs)));
    ASSERT_TRUE(replica.accept_commit(accepted_by_two(theirs)));
    ASSERT_NE(replica.committed().hash, mine.hash);

    hold.arm();
    std::thread writer([&] { EXPECT_TRUE(replica.set_aside(theirs.hash, 10'000)); });
    EXPECT_TRUE(hold.wait_until_holding());

    auto reader = std::async(std::launch::async, [&] { return std::make_pair(replica.committed().hash, hashes_of(replica.usable_heads())); });
    const bool answered = reader.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
    hold.release();
    writer.join();
    EXPECT_TRUE(answered);
    const auto [committed, usable] = reader.get();
    EXPECT_EQ(committed, mine.hash);
    EXPECT_EQ(usable, just(mine));
  }
  // The checkpoint written off the lock opens; a set-aside is this run's alone, so
  // a restart holds both heads again.
  auto reopened = bed.open();
  EXPECT_EQ(hashes_of(reopened->accepted_heads()), sorted_hashes({&mine, &theirs}));
}

// test_a_journal_compaction_holds_no_reader_of_the_replica (SM:1872). It fills the
// journal by hand, as Macha's does: nothing writes one.
TEST(HeadsReplica, AJournalCompactionHoldsNoReader) {
  ReplicaBed bed;
  WriteHold hold(bed.path() / "checkpoint.meta");
  Record last;
  {
    auto opened = bed.open();
    for (size_t i = 0; i < 65; ++i) {
      NeutralHead head;
      head.notes.set("mtime", std::to_string(i + 1));
      commit_record(*opened, record_after(opened->current(), neutral_payload(head)));
    }
    last = opened->committed();
  }
  for (size_t i = 0; i < 130; ++i) append_journal_frame(bed, bed.path(), journal_commit, last);
  {
    auto options = bed.options();
    options.replace_file = hold.writer();
    Replica replica(std::move(options));
    const auto head = replica.committed().hash;

    hold.arm();
    std::thread compactor([&] { replica.compact(); });
    EXPECT_TRUE(hold.wait_until_holding());
    auto reader = std::async(std::launch::async, [&] { return replica.committed().hash; });
    const bool answered = reader.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
    hold.release();
    compactor.join();
    EXPECT_TRUE(answered);
    EXPECT_EQ(reader.get(), head);
  }
  EXPECT_LT(std::filesystem::file_size(bed.path() / "journal.log"), 4096U);
  auto reopened = bed.open();
  EXPECT_EQ(reopened->current().generation, 66U);
  EXPECT_EQ(reopened->current().hash, reopened->committed().hash);
}

// test_a_head_set_aside_for_the_horizon_is_dropped (SM:1928).
TEST(HeadsReplica, AHeadSetAsideForTheHorizonIsDropped) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto mine = record_with_clock(genesis.generation + 1, genesis.hash, {{author(1), 1}}, {"mine"});
  const auto theirs = record_with_clock(genesis.generation + 3, genesis.hash, {{author(2), 1}}, {"theirs"});
  const uint64_t first = 10'000;
  const auto horizon = std::chrono::milliseconds(1000);

  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(mine));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(mine)));

    // The only head is never set aside, and nothing expires.
    EXPECT_FALSE(replica->set_aside(mine.hash, first));
    EXPECT_EQ(replica->expire_set_aside(first + 100'000, horizon), 0U);
    EXPECT_EQ(hashes_of(replica->accepted_heads()), just(mine));

    ASSERT_TRUE(replica->import_history(rootless_entry(theirs)));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(theirs)));
    EXPECT_EQ(hashes_of(replica->usable_heads()), sorted_hashes({&mine, &theirs}));
    EXPECT_EQ(replica->set_aside_generation(), 0U);

    ASSERT_TRUE(replica->set_aside(theirs.hash, first));
    EXPECT_EQ(hashes_of(replica->usable_heads()), just(mine));
    EXPECT_EQ(replica->set_aside_generation(), theirs.generation);
    // A node keeps a head to work on.
    EXPECT_FALSE(replica->set_aside(mine.hash, first));

    EXPECT_EQ(replica->expire_set_aside(first + 999, horizon), 0U);
    EXPECT_EQ(hashes_of(replica->accepted_heads()), sorted_hashes({&mine, &theirs}));
    EXPECT_EQ(replica->set_aside_generation(), theirs.generation);
  }
  {
    // A reopened replica holds both heads as usable again, and still counts the wait
    // from the first time the head was set aside.
    auto replica = bed.open();
    EXPECT_EQ(hashes_of(replica->usable_heads()), sorted_hashes({&mine, &theirs}));
    EXPECT_EQ(replica->set_aside_generation(), 0U);
    ASSERT_TRUE(replica->set_aside(theirs.hash, first + 600));
    EXPECT_EQ(replica->expire_set_aside(first + 999, horizon), 0U);
    EXPECT_EQ(replica->expire_set_aside(first + 1000, horizon), 1U);
    EXPECT_EQ(hashes_of(replica->accepted_heads()), just(mine));
    EXPECT_EQ(hashes_of(replica->usable_heads()), just(mine));
    EXPECT_EQ(replica->set_aside_generation(), 0U);
    EXPECT_EQ(replica->committed().hash, mine.hash);
    EXPECT_EQ(replica->expire_set_aside(first + 100'000, horizon), 0U);
  }
  auto reopened = bed.open();
  EXPECT_EQ(hashes_of(reopened->accepted_heads()), just(mine));
}

// test_the_set_aside_time_of_a_head_no_longer_held_is_forgotten (SM:1987).
TEST(HeadsReplica, TheSetAsideTimeOfAHeadNoLongerHeldIsForgotten) {
  ReplicaBed bed;
  const auto a = author(1);
  const auto b = author(2);
  const auto c = author(3);
  const auto genesis = bed.genesis();
  const auto mine = record_with_clock(genesis.generation + 1, genesis.hash, {{a, 1}}, {"mine"});
  const auto theirs = record_with_clock(genesis.generation + 3, genesis.hash, {{b, 1}}, {"theirs"});
  // Carries both.
  const auto merged = record_with_clock(theirs.generation + 1, mine.hash, {{a, 1}, {b, 1}, {c, 1}}, {"mine", "theirs"});
  const uint64_t first = 10'000;
  const auto horizon = std::chrono::milliseconds(1000);
  const auto times = bed.path() / "set-aside.meta";

  auto replica = bed.open();
  ASSERT_TRUE(replica->store_commit(mine));
  ASSERT_TRUE(replica->accept_commit(accepted_by_two(mine)));
  ASSERT_TRUE(replica->import_history(rootless_entry(theirs)));
  ASSERT_TRUE(replica->accept_commit(accepted_by_two(theirs)));
  ASSERT_TRUE(replica->set_aside(theirs.hash, first));
  const auto recorded = std::filesystem::file_size(times);

  ASSERT_TRUE(replica->import_history(rootless_entry(merged)));
  ASSERT_TRUE(replica->accept_commit(accepted_by_two(merged)));
  EXPECT_EQ(hashes_of(replica->accepted_heads()), just(merged));
  EXPECT_EQ(replica->set_aside_generation(), 0U);

  EXPECT_EQ(replica->expire_set_aside(first + 100'000, horizon), 0U);
  EXPECT_EQ(hashes_of(replica->accepted_heads()), just(merged));
  EXPECT_LT(std::filesystem::file_size(times), recorded);
}

// test_metadata_journal_mid_frame_corruption_truncates_not_reseeds (SM:2069). The
// frames are built by the test helper, at the bytes Macha's reader accepts: no live
// code at 0.93.4 writes one.
TEST(HeadsReplica, AJournalMidFrameCorruptionTruncatesNotReseeds) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto first_head = head_with({"first"});
  const auto first = record_of(genesis.generation + 1, genesis.hash, first_head);
  const auto second = record_after(first, neutral_payload(with_note(first_head, "second")));

  const auto journal = bed.path() / "journal.log";
  {
    auto opened = bed.open();
  }
  append_journal_frame(bed, bed.path(), journal_prepare_full, first, first.payload);
  const auto first_frame_end = std::filesystem::file_size(journal);
  ASSERT_GT(first_frame_end, 4U);
  append_journal_frame(bed, bed.path(), journal_prepare_full, second, second.payload);
  {
    auto replayed = bed.open();
    EXPECT_EQ(replayed->current().hash, second.hash);
  }
  ASSERT_GT(std::filesystem::file_size(journal), first_frame_end);

  // A ciphertext byte inside the first frame; a whole valid frame follows it, so
  // this is not a torn tail.
  flip_byte_at(journal, first_frame_end / 2);

  auto reopened = bed.open();
  EXPECT_FALSE(reopened->recovery_required());
  EXPECT_EQ(reopened->current().hash, genesis.hash);
  EXPECT_EQ(reopened->committed().hash, genesis.hash);
  EXPECT_EQ(std::filesystem::file_size(journal), 0U);
  EXPECT_TRUE(std::filesystem::exists(bed.path() / "checkpoint.meta"));
  EXPECT_TRUE(any_file_starts_with(bed.path(), "journal.log.corrupt."));

  // The replica is usable: the same commit applies again.
  commit_record(*reopened, first);
  EXPECT_EQ(reopened->committed().hash, first.hash);
}

// test_protocol20_checkpoint_without_acceptance_never_self_promotes (SM:2189).
TEST(HeadsReplica, ACheckpointWithoutAcceptanceNeverSelfPromotes) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto record = record_of(genesis.generation + 1, genesis.hash, head_with({"accepted"}, {}, 2));
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(record));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(record)));
    EXPECT_EQ(replica->committed().hash, record.hash);
  }

  // The acceptance-proof file is lost while the checkpoint and history stay readable:
  // the checkpoint is recovery material, never authority.
  ASSERT_TRUE(std::filesystem::remove(bed.path() / "heads.meta"));
  auto reopened = bed.open();
  EXPECT_EQ(reopened->committed().hash, record.hash);
  EXPECT_TRUE(reopened->recovery_required());
  EXPECT_TRUE(reopened->accepted_heads().empty());
  EXPECT_FALSE(reopened->acceptance(record.hash).has_value());
}

// test_an_author_sequence_never_regresses (SM:2231).
TEST(HeadsReplica, AnAuthorSequenceNeverRegresses) {
  ReplicaBed bed;
  const auto node = NodeId::random();
  const auto other = NodeId::random();
  {
    auto replica = bed.open();
    EXPECT_EQ(replica->reserve_mutation_dot(node, {}), (Dot{node, 1}));
    // A head that has seen further (merged from a peer) moves it on.
    EXPECT_EQ(replica->reserve_mutation_dot(node, {{node, 5}, {other, 9}}), (Dot{node, 6}));
    // A head that has seen less does not move it back.
    EXPECT_EQ(replica->reserve_mutation_dot(node, {{node, 2}}), (Dot{node, 7}));
  }
  auto reopened = bed.open();
  EXPECT_EQ(reopened->reserve_mutation_dot(node, {{node, 1}}), (Dot{node, 8}));
  EXPECT_EQ(reopened->reserve_mutation_dot(node, {{node, 100}}), (Dot{node, 101}));
  EXPECT_EQ(reopened->author_ids(node, {}), std::vector<NodeId>{node});
}

// test_a_broken_author_chain_starts_a_new_author (SM:2257).
TEST(HeadsReplica, ABrokenAuthorChainStartsANewAuthor) {
  ReplicaBed bed;
  const auto node = NodeId::random();
  NodeId second{};
  {
    auto replica = bed.open();
    const auto first = replica->reserve_mutation_dot(node, {});
    replica->note_author_accepted(first);
    const auto next = replica->reserve_mutation_dot(node, {{node, 1}});
    EXPECT_EQ(next, (Dot{node, 2}));
    replica->note_author_accepted(next);

    // The head on offer carries only the first.
    const auto fresh = replica->reserve_mutation_dot(node, {{node, 1}});
    EXPECT_NE(fresh.author, node);
    EXPECT_EQ(fresh.sequence, 1U);
    second = fresh.author;
    EXPECT_EQ(replica->author_ids(node, {}), (std::vector<NodeId>{second, node}));
    // The new author continues normally.
    replica->note_author_accepted(fresh);
    EXPECT_EQ(replica->reserve_mutation_dot(node, {{node, 1}, {second, 1}}), (Dot{second, 2}));
  }
  auto reopened = bed.open();
  EXPECT_EQ(reopened->author_ids(node, {}), (std::vector<NodeId>{second, node}));
  EXPECT_EQ(reopened->reserve_mutation_dot(node, {{second, 2}}), (Dot{second, 3}));
}

// test_a_lost_author_record_starts_a_new_author (SM:2291).
TEST(HeadsReplica, ALostAuthorRecordStartsANewAuthor) {
  ReplicaBed bed;
  const auto node = NodeId::random();
  auto replica = bed.open();
  const auto dot = replica->reserve_mutation_dot(node, {{node, 41}});
  EXPECT_NE(dot.author, node);
  EXPECT_EQ(dot.sequence, 1U);
  EXPECT_EQ(replica->author_ids(node, {}), (std::vector<NodeId>{dot.author, node}));

  EXPECT_TRUE(clock_covers({{node, 41}}, Dot{node, 41}));
  EXPECT_FALSE(clock_covers({{node, 41}}, Dot{node, 42}));
  EXPECT_FALSE(clock_covers({{node, 41}}, dot));
  EXPECT_FALSE(static_cast<bool>(Dot{}));
}

// test_metadata_delta_chain_reuses_bounded_materialized_head (SM:2365).
TEST(HeadsReplica, ADeltaChainReusesABoundedMaterializedHead) {
  ReplicaBed bed;
  const auto origin = NodeId::random();
  auto state = head_with({});
  auto head = record_of(2, bed.genesis().hash, state);
  constexpr size_t chain_length = 200;
  std::optional<Record> early_record;
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(head));
    for (size_t i = 0; i < chain_length; ++i) {
      auto next = state;
      next.clock[origin] = i + 1;
      next.notes.set("delta-" + std::to_string(i), "");
      const auto child = record_after(head, neutral_payload(next));
      ASSERT_TRUE(replica->store_commit(child, delta_between(state, next)));
      if (i == 5) early_record = child;
      state = std::move(next);
      head = child;
    }
  }

  // Reopening drops what building the chain materialised. Concurrent misses share
  // one computation off the lock; later callers reuse the immutable result.
  auto replica = bed.open();
  const auto before = replica->diagnostics();
  constexpr size_t concurrent_readers = 8;
  std::array<std::shared_ptr<const Materialization>, concurrent_readers> results;
  std::atomic_bool start{false};
  std::vector<std::thread> readers;
  for (size_t index = 0; index < concurrent_readers; ++index) {
    readers.emplace_back([&, index] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      results[index] = replica->materialized(head.hash);
    });
  }
  start.store(true, std::memory_order_release);
  for (auto& reader : readers) reader.join();
  const auto middle = replica->diagnostics();
  const auto second = replica->historical(head.hash);
  const auto after = replica->diagnostics();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->payload, head.payload);
  for (const auto& result : results) {
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->record.payload, head.payload);
    EXPECT_EQ(result, results.front());
  }

  EXPECT_EQ(middle.historical_requests - before.historical_requests, concurrent_readers);
  EXPECT_EQ(after.historical_requests - middle.historical_requests, 1U);
  EXPECT_EQ(middle.historical_reconstructions - before.historical_reconstructions, 1U);
  EXPECT_EQ(after.historical_reconstructions - middle.historical_reconstructions, 0U);
  EXPECT_EQ(middle.historical_deltas_applied - before.historical_deltas_applied, chain_length);
  EXPECT_EQ(after.historical_deltas_applied - middle.historical_deltas_applied, 0U);
  EXPECT_EQ(middle.materialization_cache_hits - before.materialization_cache_hits, concurrent_readers - 1);
  EXPECT_EQ(after.materialization_cache_hits - middle.materialization_cache_hits, 1U);
  EXPECT_EQ(middle.materialization_cache_misses - before.materialization_cache_misses, 1U);
  EXPECT_LE(after.materialization_cache_entries, 64U);
  // Replaying a long chain caches only the requested result.
  EXPECT_LE(middle.materialization_cache_entries, before.materialization_cache_entries + 1);
  EXPECT_GE(middle.materialization_cache_bytes, results.front()->resident_bytes);

  const auto first_materialized = replica->materialized(head.hash);
  const auto second_materialized = replica->materialized(head.hash);
  ASSERT_NE(first_materialized, nullptr);
  ASSERT_NE(second_materialized, nullptr);
  EXPECT_EQ(first_materialized, second_materialized);
  EXPECT_EQ(first_materialized->state, second_materialized->state);
  EXPECT_EQ(bed.codec->encode_full(first_materialized->state), Bytes(head.payload.begin(), head.payload.end()));

  // Materialisation is an optimisation, never authority: stored, never certified.
  EXPECT_FALSE(replica->acceptance(head.hash).has_value());
  for (const auto& record : replica->accepted_heads()) EXPECT_NE(record.hash, head.hash);

  // The bounded cache evicts old, non-authoritative results. Such a record stays
  // reconstructable from history, byte identical.
  ASSERT_TRUE(early_record.has_value());
  const auto before_evicted_lookup = replica->diagnostics();
  const auto reconstructed_early = replica->historical(early_record->hash);
  const auto after_evicted_lookup = replica->diagnostics();
  ASSERT_TRUE(reconstructed_early.has_value());
  EXPECT_EQ(reconstructed_early->payload, early_record->payload);
  EXPECT_EQ(after_evicted_lookup.historical_reconstructions - before_evicted_lookup.historical_reconstructions, 1U);
  EXPECT_LE(after_evicted_lookup.materialization_cache_entries, 64U);
}

// test_accepting_a_held_head_again_rebuilds_nothing (SM:2492).
TEST(HeadsReplica, AcceptingAHeldHeadAgainRebuildsNothing) {
  ReplicaBed bed;
  const auto origin = NodeId::random();
  auto state = head_with({});
  auto head = record_of(2, bed.genesis().hash, state);
  Acceptance certificate;
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(head));
    for (size_t i = 0; i < 30; ++i) {
      auto next = state;
      next.clock[origin] = i + 1;
      next.notes.set("held-" + std::to_string(i), "");
      const auto child = record_after(head, neutral_payload(next));
      ASSERT_TRUE(replica->store_commit(child, delta_between(state, next)));
      state = std::move(next);
      head = child;
    }
    certificate = Acceptance{head.generation, head.hash, 1, {origin}};
    ASSERT_TRUE(replica->accept_commit(certificate));
  }

  // Reopened, so nothing it materialised while building the chain is cached.
  auto replica = bed.open();
  const auto before = replica->diagnostics();
  for (int i = 0; i < 5; ++i) {
    bool changed = true;
    EXPECT_TRUE(replica->accept_commit(certificate, &changed));
    EXPECT_FALSE(changed);
  }
  const auto after = replica->diagnostics();
  EXPECT_EQ(after.historical_requests, before.historical_requests);
  EXPECT_EQ(after.historical_reconstructions, before.historical_reconstructions);
  EXPECT_EQ(after.historical_deltas_applied, before.historical_deltas_applied);

  auto wider = certificate;
  wider.replicas.push_back(NodeId::random());
  std::sort(wider.replicas.begin(), wider.replicas.end());
  bool changed = false;
  EXPECT_TRUE(replica->accept_commit(wider, &changed));
  EXPECT_TRUE(changed);
  ASSERT_TRUE(replica->acceptance(head.hash).has_value());
  EXPECT_EQ(replica->acceptance(head.hash)->replicas.size(), 2U);
}

// test_metadata_history_payloads_are_disk_backed_and_byte_bounded (SM:2581). Macha's
// structural floor counts its entry map, which is the owner's: payload and the
// codec's own weight here.
TEST(HeadsReplica, HistoryPayloadsAreDiskBackedAndByteBounded) {
  ReplicaBed bed;
  const auto origin = NodeId::random();
  NeutralHead state;
  // A record's size is its clock here: make it outgrow the cache budget.
  for (size_t i = 0; i < 4000; ++i) state.clock[NodeId::random()] = i + 1;

  Record head = bed.genesis();
  {
    auto writer = bed.open();
    for (uint64_t generation = 2; generation <= 42; ++generation) {
      state.clock[origin] = generation;
      auto next = record_after(head, neutral_payload(state));
      ASSERT_TRUE(writer->store_commit(next));
      head = std::move(next);
    }
  }

  constexpr uint64_t cache_limit = 64 * 1024;
  EXPECT_GT(head.payload.size(), cache_limit);
  auto reopened = bed.open("space", {}, true, cache_limit);
  const auto cold = reopened->diagnostics();
  EXPECT_GE(cold.history_records, 41U);
  EXPECT_GT(cold.history_file_bytes, 1024U * 1024U);
  EXPECT_EQ(cold.history_resident_payload_bytes, 0U);
  EXPECT_EQ(cold.materialization_cache_limit_bytes, cache_limit);
  EXPECT_LE(cold.materialization_cache_bytes, cache_limit);

  const auto recovered = reopened->materialized(head.hash);
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->record.payload, head.payload);
  EXPECT_GE(recovered->resident_bytes, recovered->record.payload.size() + bed.codec->resident_bytes(recovered->state));
  const auto after = reopened->diagnostics();
  // Larger than the budget: returned to the caller, not retained.
  EXPECT_LE(after.materialization_cache_bytes, cache_limit);
  EXPECT_EQ(after.history_resident_payload_bytes, 0U);
}

// test_cold_accepted_head_validation_caches_only_owned_results (SM:2637).
TEST(HeadsReplica, ColdAcceptedHeadValidationCachesOnlyOwnedResults) {
  ReplicaBed bed;
  const auto witness = NodeId::random();
  auto state = head_with({});
  Record head = bed.genesis();
  constexpr size_t chain_length = 80;
  {
    auto writer = bed.open();
    auto policy_root = record_after(head, neutral_payload(state));
    ASSERT_TRUE(writer->store_commit(policy_root));
    head = std::move(policy_root);
    for (size_t i = 0; i < chain_length; ++i) {
      auto next = state;
      next.clock[witness] = i + 1;
      next.notes.set("owned-" + std::to_string(i), "");
      auto child = record_after(head, neutral_payload(next));
      ASSERT_TRUE(writer->store_commit(child, delta_between(state, next)));
      state = std::move(next);
      head = std::move(child);
    }
    ASSERT_TRUE(writer->accept_commit({head.generation, head.hash, 1, {witness}}));
  }

  // Validation at open may retain the accepted result and its parent, but never one
  // state for every delta traversed to obtain them.
  auto reopened = bed.open();
  EXPECT_LE(reopened->diagnostics().materialization_cache_entries, 2U);
  const auto accepted = reopened->accepted_heads();
  ASSERT_EQ(accepted.size(), 1U);
  EXPECT_EQ(accepted.front().hash, head.hash);
  const auto materialized = reopened->materialized(head.hash);
  ASSERT_NE(materialized, nullptr);
  EXPECT_EQ(materialized->record.payload, head.payload);
}

// test_metadata_materialization_cache_never_validates_corrupt_delta (SM:2699).
TEST(HeadsReplica, TheCacheNeverValidatesACorruptDelta) {
  ReplicaBed bed;
  const auto parent_head = head_with({});
  const auto parent = record_of(2, bed.genesis().hash, parent_head);

  auto replica = bed.open();
  ASSERT_TRUE(replica->store_commit(parent));
  const auto cached_parent = replica->materialized(parent.hash);
  ASSERT_NE(cached_parent, nullptr);
  const auto cache_after_parent = replica->diagnostics().materialization_cache_entries;

  const auto child_head = with_note(parent_head, "valid-child");
  const auto child = record_after(parent, neutral_payload(child_head));

  // Well-formed and applicable to the cached parent, but it reconstructs another
  // successor than the claimed hash. A cache hit for the parent must not turn that
  // false edge into authority.
  HistoryEntry corrupt;
  corrupt.generation = child.generation;
  corrupt.previous = parent.hash;
  corrupt.hash = child.hash;
  corrupt.previous_known = true;
  corrupt.body = HistoryEntry::Body::delta;
  corrupt.payload = delta_between(parent_head, with_note(parent_head, "wrong-child"));

  EXPECT_FALSE(replica->import_history(corrupt));
  EXPECT_FALSE(replica->history_contains(child.hash));
  EXPECT_EQ(replica->materialized(child.hash), nullptr);
  EXPECT_EQ(replica->diagnostics().materialization_cache_entries, cache_after_parent);
  EXPECT_FALSE(replica->accept_commit({child.generation, child.hash, 1, {NodeId::random()}}));

  auto valid = corrupt;
  valid.payload = delta_between(parent_head, child_head);
  ASSERT_TRUE(replica->import_history(valid));
  const auto materialized_child = replica->materialized(child.hash);
  ASSERT_NE(materialized_child, nullptr);
  EXPECT_EQ(materialized_child->record.payload, child.payload);
  EXPECT_EQ(bed.codec->encode_full(materialized_child->state), Bytes(child.payload.begin(), child.payload.end()));

  // A later corrupt duplicate of the same claimed identity is rejected before the
  // valid entry is consulted as success.
  EXPECT_FALSE(replica->import_history(corrupt));
  const auto after_duplicate = replica->materialized(child.hash);
  ASSERT_NE(after_duplicate, nullptr);
  EXPECT_EQ(after_duplicate, materialized_child);
  EXPECT_EQ(after_duplicate->record.payload, child.payload);
}

// test_history_replay_applies_a_delta_to_the_tree_not_the_map (NT:780). Macha's map
// form is its own: the rebuild from the entries in hand is what it compares with.
TEST(HeadsReplica, ReplayAppliesADeltaToTheTree) {
  ReplicaBed bed;
  auto entries = test::shaped_entries(6, 4);
  SharedTreeStore store;
  const auto factory = store.factory();
  const auto tree = test::neutral_tree();
  NeutralHead parent_head;
  {
    const auto writer = factory();
    parent_head.root = tree.build(entries, *writer);
  }
  const auto parent = record_of(2, bed.genesis().hash, parent_head);

  test::NeutralDelta delta;
  const std::string touched_key = "a/all/group 2/part 3/item 2";
  ASSERT_TRUE(entries.contains(touched_key));
  auto touched = entries.at(touched_key);
  touched.stamp += 7000;
  delta.upsert_keys[touched_key] = touched;
  delta.upsert_keys["a/all/group 0/late"] = test::make_item(31337, 5);
  delta.erase_keys.push_back("a/all/group 4/part 2/item 1");
  ASSERT_TRUE(entries.contains(delta.erase_keys.back()));
  delta.clock[author(1)] = 1;
  const auto delta_bytes = test::encode_neutral_delta(delta);

  // Without a node store the delta is refused, not applied to what was never read.
  const auto parent_state = bed.codec->decode_full(parent.payload);
  EXPECT_THROW((void)bed.codec->apply_delta(parent_state, delta_bytes, nullptr), DecodeError);

  auto mapped = entries;
  mapped[touched_key] = touched;
  mapped["a/all/group 0/late"] = test::make_item(31337, 5);
  mapped.erase("a/all/group 4/part 2/item 1");
  heads::MemoryTreeNodeStore fresh;
  auto child_head = parent_head;
  child_head.root = tree.build(mapped, fresh);
  child_head.clock[author(1)] = 1;
  const auto child = record_after(parent, neutral_payload(child_head));

  {
    // A replica with no node store refuses it the same way.
    auto storeless = bed.open("storeless");
    ASSERT_TRUE(storeless->store_commit(parent));
    EXPECT_FALSE(storeless->store_commit(child, delta_bytes));
    EXPECT_FALSE(storeless->history_contains(child.hash));
  }

  auto options = bed.options();
  options.tree_store = factory;
  {
    Replica replica(options);
    ASSERT_TRUE(replica.store_commit(parent));
    ASSERT_TRUE(replica.store_commit(child, delta_bytes));
    EXPECT_EQ(replica.history_entry(child.hash)->body, HistoryEntry::Body::delta);
  }
  // Replayed cold, the record stays tree-backed and matches a rebuild.
  Replica replica(options);
  const auto replayed = replica.materialized(child.hash);
  ASSERT_NE(replayed, nullptr);
  EXPECT_EQ(replayed->record.payload, child.payload);
  const auto head = NeutralCodec::as_head(replayed->state);
  ASSERT_TRUE(head->root.has_value());
  EXPECT_EQ(*head->root, *child_head.root);
  const auto reader = factory();
  EXPECT_EQ(tree.read(*head->root, *reader), mapped);
  // What is not the tree's moves as in the record.
  EXPECT_EQ(head->clock, child_head.clock);
}

// test_metadata_delta_state_model (tests/test_models.cpp:43), neutral. Each transition
// is encoded and decoded before it is applied, and, past Macha's, stored in a
// replica as a delta, which the hash holds byte for byte.
TEST(HeadsReplica, TheDeltaStateModel) {
  ReplicaBed bed;
  const NeutralCodec codec;
  const auto mutator = author(1);
  NeutralHead state;
  auto replica = bed.open();
  auto record = record_of(2, bed.genesis().hash, state);
  ASSERT_TRUE(replica->store_commit(record));

  for (uint32_t step = 1; step <= 96; ++step) {
    auto expected = state;
    expected.clock[mutator] = step;
    expected.notes.set("model", "group");
    expected.notes.set("file-" + std::to_string(step % 11), std::to_string(1000 + step));
    if (step > 12 && step % 4 == 0) expected.notes.erase("file-" + std::to_string((step + 3) % 11));
    if (step % 3 == 0) {
      expected.notes.set("root", std::to_string(2000 + step));
    } else if (step % 7 == 0) {
      expected.notes.erase("root");
    }
    if (step % 5 == 0) expected.notes.set("retired-" + std::to_string(step), std::to_string(1'000'000 + step));
    if (step % 8 == 0 && expected.notes.get().contains("retired-" + std::to_string(step - 3))) expected.notes.erase("retired-" + std::to_string(step - 3));

    const auto delta = test::neutral_delta(state, expected);
    ASSERT_TRUE(delta.has_value());
    const auto encoded = test::encode_neutral_delta(*delta);
    const auto parent_state = codec.decode_full(neutral_payload(state));
    const auto applied = codec.apply_delta(parent_state, encoded, nullptr);
    EXPECT_EQ(*NeutralCodec::as_head(applied), expected);

    // Serialisation must agree with the delta path at every state.
    EXPECT_EQ(test::neutral_head_from(codec.encode_full(applied)), expected);

    const auto next = record_after(record, neutral_payload(expected));
    ASSERT_TRUE(replica->store_commit(next, encoded));
    EXPECT_EQ(replica->history_entry(next.hash)->body, HistoryEntry::Body::delta);
    record = next;
    state = std::move(expected);
  }

  auto incompatible = state;
  ++incompatible.floor;
  EXPECT_FALSE(test::neutral_delta(state, incompatible).has_value());

  incompatible = state;
  incompatible.clock[mutator] = state.clock.at(mutator) - 1;
  EXPECT_FALSE(test::neutral_delta(state, incompatible).has_value());
}

// test_metadata_codec_and_replica (SM:3848), the replica half; the codec half is
// Macha's. Stored is not yet a head; reopen; a delta's growth in history.
TEST(HeadsReplica, AStoredCommitIsNotAHeadUntilAcceptedAndHistoryGrowsByTheDelta) {
  ReplicaBed bed;
  const auto a = author(1);
  auto replica = bed.open("node");
  const auto current = replica->current();
  EXPECT_EQ(replica->current_identity().generation, current.generation);
  EXPECT_EQ(replica->current_identity().hash, current.hash);
  EXPECT_EQ(replica->committed_identity().generation, replica->committed().generation);
  EXPECT_EQ(replica->committed_identity().hash, replica->committed().hash);
  EXPECT_EQ(replica->committed().hash, current.hash);

  auto head = head_with({"item"}, {{a, 7}});
  // A record padded well past a delta's size, as Macha's is by its own fields.
  head.notes.set("padding", std::string(2000, 'x'));
  const auto next = record_after(current, neutral_payload(head));
  // A stored commit is not yet a head: committed moves on acceptance.
  ASSERT_TRUE(replica->store_commit(next));
  EXPECT_EQ(replica->committed().hash, current.hash);
  ASSERT_TRUE(replica->accept_commit({next.generation, next.hash, 1, {a}}));
  EXPECT_EQ(replica->committed().hash, next.hash);

  auto reopened = bed.open("node");
  EXPECT_EQ(reopened->current().hash, next.hash);
  EXPECT_EQ(reopened->committed().hash, next.hash);
  EXPECT_TRUE(std::filesystem::exists(bed.path("node") / "checkpoint.meta"));
  EXPECT_TRUE(std::filesystem::exists(bed.path("node") / "journal.log"));

  // An ordinary mutation is a compact delta: history grows by the delta, not by the
  // successor's record, and it survives restart.
  auto after = head;
  after.notes.set("item", "456");
  after.clock[a] = 8;
  const auto delta_bytes = delta_between(head, after);
  const auto history_before_delta = std::filesystem::file_size(bed.path("node") / "history.log");
  const auto delta_next = record_after(reopened->current(), neutral_payload(after));
  commit_record(*reopened, delta_next, delta_bytes);
  const auto history_growth = std::filesystem::file_size(bed.path("node") / "history.log") - history_before_delta;
  EXPECT_LT(delta_bytes.size(), delta_next.payload.size());
  EXPECT_GE(history_growth, delta_bytes.size());
  EXPECT_LT(history_growth, delta_bytes.size() + 512);

  auto reopened_again = bed.open("node");
  EXPECT_EQ(reopened_again->current().hash, delta_next.hash);
  EXPECT_EQ(reopened_again->committed().hash, delta_next.hash);
  EXPECT_EQ(test::neutral_head_from(reopened_again->current().payload).notes.get().at("item"), "456");
}

// The same case's journal half: state accepted but not committed stays current after
// a restart, and a torn or forged tail is quarantined, keeping the prefix.
TEST(HeadsReplica, AnUncommittedProposalSurvivesAndATornJournalTailIsPreserved) {
  ReplicaBed bed;
  const auto directory = bed.path("uncommitted");
  Record uncommitted_next;
  Hash256 uncommitted_base_hash;
  {
    auto uncommitted = bed.open("uncommitted");
    commit_record(*uncommitted, record_after(uncommitted->current(), neutral_payload(NeutralHead{})));
    const auto base = uncommitted->current();
    uncommitted_base_hash = base.hash;
    const NeutralHead before;
    const auto after = with_note(before, "pending");
    uncommitted_next = record_after(base, neutral_payload(after));
    append_journal_frame(bed, directory, journal_prepare_delta, uncommitted_next, delta_between(before, after));
  }
  {
    std::ofstream tail(directory / "journal.log", std::ios::binary | std::ios::app);
    tail.write("bad", 3);
  }
  {
    auto recovered = bed.open("uncommitted");
    EXPECT_EQ(recovered->current().hash, uncommitted_next.hash);
    EXPECT_EQ(recovered->committed().hash, uncommitted_base_hash);
  }

  // A crash can extend the file to the whole frame's length with its last bytes
  // unauthenticated. The tail is kept for diagnosis, the prefix replayed.
  const auto authenticated_journal_size = std::filesystem::file_size(directory / "journal.log");
  {
    dannan::net::Writer envelope;
    const std::array<uint8_t, 12> nonce{};
    const std::array<uint8_t, 16> tag{};
    envelope.fixed(nonce);
    envelope.fixed(tag);
    envelope.bytes(Bytes{0x42});
    const auto payload = envelope.take();
    dannan::net::Writer frame;
    frame.u32(static_cast<uint32_t>(payload.size()));
    frame.raw(payload);
    const auto bytes = frame.take();
    std::ofstream tail(directory / "journal.log", std::ios::binary | std::ios::app);
    tail.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  {
    auto recovered = bed.open("uncommitted");
    EXPECT_EQ(recovered->current().hash, uncommitted_next.hash);
    EXPECT_EQ(recovered->committed().hash, uncommitted_base_hash);
    EXPECT_EQ(std::filesystem::file_size(directory / "journal.log"), authenticated_journal_size);
    EXPECT_TRUE(any_file_starts_with(directory, "journal.log.corrupt."));
  }
}

// The same case's compaction half: at the threshold it checkpoints and truncates
// rather than allowing an unbounded replay log.
TEST(HeadsReplica, CompactionBoundsTheJournal) {
  ReplicaBed bed;
  const auto directory = bed.path("compact-node");
  {
    auto opened = bed.open("compact-node");
  }
  // Below the threshold it leaves the journal as it is.
  for (size_t i = 0; i < 127; ++i) append_journal_frame(bed, directory, journal_commit, bed.genesis());
  const auto below = std::filesystem::file_size(directory / "journal.log");
  {
    auto untouched = bed.open("compact-node");
    untouched->compact();
  }
  EXPECT_EQ(std::filesystem::file_size(directory / "journal.log"), below);
  append_journal_frame(bed, directory, journal_commit, bed.genesis());
  {
    auto compacted = bed.open("compact-node");
    EXPECT_GT(std::filesystem::file_size(directory / "journal.log"), 4096U);
    compacted->compact();
  }
  EXPECT_LT(std::filesystem::file_size(directory / "journal.log"), 4096U);
  auto compacted_again = bed.open("compact-node");
  EXPECT_EQ(compacted_again->current().generation, bed.genesis().generation);
  EXPECT_EQ(compacted_again->current().hash, compacted_again->committed().hash);
}

// The journal reader's other branches, which no case of Macha's reaches and no frame
// of Macha's proves: 0.93.4 writes journal.log only empty. Kept by the plan's Q3, so
// held here to its code at 9a08846 (md.cpp:2248-2429).
TEST(HeadsReplica, TheJournalReadersBranchesHold) {
  const auto write_kind = [](const ReplicaBed& bed, const std::filesystem::path& directory, uint8_t kind, const Record& record, std::span<const uint8_t> body) {
    append_journal_frame(bed, directory, kind, record, body);
  };
  constexpr uint8_t journal_seed_full = 3;

  // A prepared delta replays through the codec and a commit makes it committed.
  {
    ReplicaBed bed;
    const auto directory = bed.path();
    Record base;
    const NeutralHead before;
    {
      auto opened = bed.open();
      commit_record(*opened, record_after(opened->current(), neutral_payload(before)));
      base = opened->committed();
    }
    const auto after = with_note(before, "journaled");
    const auto next = record_after(base, neutral_payload(after));
    append_journal_frame(bed, directory, journal_prepare_delta, next, delta_between(before, after));
    append_journal_frame(bed, directory, journal_commit, next);
    auto replayed = bed.open();
    // Its certificate never existed: a head with none is not authority, so recovery
    // is required and the preferred head goes back to what was certified. The record
    // stays in history, as a root.
    EXPECT_TRUE(replayed->recovery_required());
    EXPECT_EQ(hashes_of(replayed->accepted_heads()), just(base));
    EXPECT_EQ(replayed->committed().hash, base.hash);
    EXPECT_EQ(replayed->current().hash, base.hash);
    EXPECT_TRUE(replayed->history_contains(next.hash));
    EXPECT_FALSE(replayed->history_links(next.hash)->previous_known);
  }

  // A prepare that does not extend what is current ends the prefix.
  {
    ReplicaBed bed;
    {
      auto opened = bed.open();
    }
    const auto stray = record_of(5, bed.genesis().hash, head_with({"stray"}));
    append_journal_frame(bed, bed.path(), journal_prepare_full, stray, stray.payload);
    auto opened = bed.open();
    EXPECT_EQ(opened->current().hash, bed.genesis().hash);
    EXPECT_EQ(std::filesystem::file_size(bed.path() / "journal.log"), 0U);
    EXPECT_TRUE(any_file_starts_with(bed.path(), "journal.log.corrupt."));
  }

  // A seed advances a fresh replica, rolls a proposal back, and is refused past the
  // committed head when it does not descend from it.
  {
    ReplicaBed bed;
    {
      auto opened = bed.open();
    }
    const auto seeded = record_of(4, Hash256{{3}}, head_with({"seeded"}));
    write_kind(bed, bed.path(), journal_seed_full, seeded, seeded.payload);
    auto opened = bed.open();
    EXPECT_EQ(opened->current().hash, seeded.hash);
    EXPECT_EQ(opened->committed().hash, bed.genesis().hash);
  }
  {
    ReplicaBed bed;
    Record base;
    {
      auto opened = bed.open();
      commit_record(*opened, record_after(opened->current(), neutral_payload(head_with({"base"}))));
      base = opened->committed();
    }
    const auto proposal = record_after(base, neutral_payload(head_with({"base", "proposed"})));
    append_journal_frame(bed, bed.path(), journal_prepare_full, proposal, proposal.payload);
    write_kind(bed, bed.path(), journal_seed_full, base, base.payload);
    {
      auto opened = bed.open();
      EXPECT_EQ(opened->current().hash, base.hash);
      EXPECT_EQ(opened->committed().hash, base.hash);
    }
    const auto unrelated = record_of(9, Hash256{{5}}, head_with({"unrelated"}));
    write_kind(bed, bed.path(), journal_seed_full, unrelated, unrelated.payload);
    auto opened = bed.open();
    EXPECT_EQ(opened->current().hash, base.hash);
    EXPECT_TRUE(any_file_starts_with(bed.path(), "journal.log.corrupt."));
  }

  // A kind it does not know ends the prefix too.
  {
    ReplicaBed bed;
    {
      auto opened = bed.open();
    }
    const auto next = record_after(bed.genesis(), neutral_payload(head_with({"x"})));
    write_kind(bed, bed.path(), 9, next, next.payload);
    auto opened = bed.open();
    EXPECT_EQ(opened->current().hash, bed.genesis().hash);
    EXPECT_EQ(std::filesystem::file_size(bed.path() / "journal.log"), 0U);
  }
}

// H16: each node truncates alone, past 256 records or 64 MiB, the sole accepted head
// becoming the root.
TEST(HeadsReplica, TruncationFiresAtItsThresholds) {
  ReplicaBed bed;
  auto replica = bed.open();
  NeutralHead state;
  Record head = bed.genesis();
  // Genesis is the first record.
  for (uint64_t generation = 2; generation <= 255; ++generation) {
    state.notes.set("n", std::to_string(generation));
    auto next = record_after(head, neutral_payload(state));
    ASSERT_TRUE(replica->store_commit(next));
    head = std::move(next);
  }
  ASSERT_TRUE(replica->accept_commit(accepted_by_two(head)));
  EXPECT_EQ(replica->diagnostics().history_records, 255U);
  EXPECT_FALSE(replica->compact_history_if_safe());
  // By bytes, the same call at a threshold under what is there.
  const auto bytes = replica->diagnostics().history_file_bytes;
  EXPECT_FALSE(replica->compact_history_if_safe(256, bytes + 1));

  state.notes.set("n", "256");
  auto last = record_after(head, neutral_payload(state));
  ASSERT_TRUE(replica->store_commit(last));
  ASSERT_TRUE(replica->accept_commit(accepted_by_two(last)));
  EXPECT_EQ(replica->diagnostics().history_records, 256U);
  EXPECT_TRUE(replica->compact_history_if_safe());
  EXPECT_EQ(replica->diagnostics().history_records, 1U);
  const auto entry = replica->history_entry(last.hash);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->body, HistoryEntry::Body::full);
  EXPECT_FALSE(entry->previous_known);
  EXPECT_EQ(entry->previous, head.hash);
  // A truncated history is below the threshold again.
  EXPECT_FALSE(replica->compact_history_if_safe());

  auto reopened = [&] {
    replica.reset();
    return bed.open();
  }();
  EXPECT_EQ(reopened->committed().hash, last.hash);
  EXPECT_FALSE(reopened->history_contains(head.hash));
}

// C4: across generations and between siblings, live and after reopen; a set-aside
// head is never preferred.
TEST(HeadsReplica, ThePreferredHeadIsTheHighestGenerationThenTheHigherHash) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto low = record_with_clock(3, genesis.hash, {{author(1), 1}}, {"low"});
  auto one = record_with_clock(5, genesis.hash, {{author(2), 1}}, {"one"});
  auto two = record_with_clock(5, genesis.hash, {{author(3), 1}}, {"two"});
  if (two.hash < one.hash) std::swap(one, two);
  ASSERT_LT(one.hash, two.hash);

  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(low));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(low)));
    EXPECT_EQ(replica->committed().hash, low.hash);

    // Across generations.
    ASSERT_TRUE(replica->store_commit(one));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(one)));
    EXPECT_EQ(replica->committed().hash, one.hash);

    // Between siblings: the higher hash, whichever arrived first.
    ASSERT_TRUE(replica->store_commit(two));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(two)));
    EXPECT_EQ(replica->committed().hash, two.hash);
    EXPECT_EQ(replica->current().hash, two.hash);
  }
  {
    auto reopened = bed.open();
    EXPECT_EQ(reopened->committed().hash, two.hash);
    EXPECT_EQ(reopened->accepted_heads().size(), 3U);

    // A set-aside head is never preferred.
    ASSERT_TRUE(reopened->set_aside(two.hash, 1000));
    EXPECT_EQ(reopened->committed().hash, one.hash);
    ASSERT_TRUE(reopened->set_aside(one.hash, 1000));
    EXPECT_EQ(reopened->committed().hash, low.hash);
    // Cleared, the rule stands again.
    reopened->clear_set_aside();
    EXPECT_EQ(reopened->committed().hash, two.hash);
  }

  // The other arrival order reaches the same head.
  auto other = bed.open("other");
  ASSERT_TRUE(other->store_commit(two));
  ASSERT_TRUE(other->accept_commit(accepted_by_two(two)));
  ASSERT_TRUE(other->store_commit(one));
  ASSERT_TRUE(other->accept_commit(accepted_by_two(one)));
  EXPECT_EQ(other->committed().hash, two.hash);
  ASSERT_TRUE(other->store_commit(low));
  ASSERT_TRUE(other->accept_commit(accepted_by_two(low)));
  EXPECT_EQ(other->committed().hash, two.hash);
}

// C20, the first half: an ancestor of the held set changes nothing, by history or by
// clock; no checkpoint is written for it. Live and after reopen.
TEST(HeadsReplica, AnAncestorChangesNothing) {
  ReplicaBed bed;
  const auto a = author(1);
  const auto genesis = bed.genesis();
  const auto old = record_with_clock(2, genesis.hash, {{a, 1}}, {"old"});
  const auto now = record_with_clock(3, old.hash, {{a, 2}}, {"old", "now"});
  Hash256 elsewhere{};
  elsewhere.bytes[0] = 0x31;
  const auto covered = record_with_clock(2, elsewhere, {{a, 1}}, {"covered"});

  std::atomic_size_t checkpoint_writes{0};
  auto options = bed.options();
  const auto checkpoint_path = bed.path() / "checkpoint.meta";
  options.replace_file = [&](const std::filesystem::path& path, std::string_view contents) {
    if (path == checkpoint_path) checkpoint_writes.fetch_add(1);
    dannan::identity::durable_replace_file(path, contents);
  };
  {
    Replica replica(options);
    ASSERT_TRUE(replica.store_commit(old));
    ASSERT_TRUE(replica.accept_commit(accepted_by_two(old)));
    ASSERT_TRUE(replica.store_commit(now));
    ASSERT_TRUE(replica.accept_commit(accepted_by_two(now)));
    ASSERT_TRUE(replica.import_history(rootless_entry(covered)));

    const auto writes = checkpoint_writes.load();
    const auto revision_heads = replica.accepted_head_certificates();
    for (const auto* ancestor : {&old, &covered}) {
      bool changed = true;
      EXPECT_TRUE(replica.accept_commit(accepted_by_two(*ancestor), &changed));
      EXPECT_FALSE(changed);
    }
    EXPECT_EQ(replica.accepted_head_certificates(), revision_heads);
    EXPECT_EQ(hashes_of(replica.accepted_heads()), just(now));
    EXPECT_EQ(replica.committed().hash, now.hash);
    EXPECT_EQ(checkpoint_writes.load(), writes);
  }
  auto reopened = bed.open();
  EXPECT_EQ(hashes_of(reopened->accepted_heads()), just(now));
  EXPECT_EQ(reopened->committed().hash, now.hash);
}

// C20, the second half: an unrelated commit is a divergence, whichever generation is
// higher, preferred by C4's rule; both after reopen.
TEST(HeadsReplica, AnUnrelatedCommitIsADivergenceNeverAReplacement) {
  ReplicaBed bed;
  Hash256 unseen{};
  unseen.bytes[0] = 0x77;
  const auto genesis = bed.genesis();
  const auto mine = record_with_clock(6, genesis.hash, {{author(1), 3}}, {"mine"});
  const auto higher = record_with_clock(9, unseen, {{author(2), 1}}, {"higher"});
  const auto lower = record_with_clock(4, unseen, {{author(3), 1}}, {"lower"});

  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(mine));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(mine)));

    ASSERT_TRUE(replica->import_history(rootless_entry(lower)));
    bool changed = false;
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(lower), &changed));
    EXPECT_TRUE(changed);
    EXPECT_EQ(hashes_of(replica->accepted_heads()), sorted_hashes({&mine, &lower}));
    EXPECT_EQ(replica->committed().hash, mine.hash);

    ASSERT_TRUE(replica->import_history(rootless_entry(higher)));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(higher)));
    EXPECT_EQ(hashes_of(replica->accepted_heads()), sorted_hashes({&mine, &lower, &higher}));
    EXPECT_EQ(replica->committed().hash, higher.hash);
  }
  auto reopened = bed.open();
  EXPECT_EQ(hashes_of(reopened->accepted_heads()), sorted_hashes({&mine, &lower, &higher}));
  EXPECT_EQ(reopened->committed().hash, higher.hash);
}

// A long walk's stop is nobody's failure: it leaves every walk, and it never turns
// into a recovery from the seed. Macha's load swallows nothing of this because it
// has no stop inside a load; Dagda's replay has one.
TEST(HeadsReplica, AStopPassesThroughEveryWalkAndNeverQuarantines) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  auto state = head_with({});
  auto head = record_of(2, genesis.hash, state);
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(head));
    for (size_t i = 0; i < 4; ++i) {
      auto next = state;
      next.clock[author(1)] = i + 1;
      const auto child = record_after(head, neutral_payload(next));
      ASSERT_TRUE(replica->store_commit(child, delta_between(state, next)));
      state = std::move(next);
      head = child;
    }
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(head)));
  }
  const auto stopping = []() { throw contract::MaintenanceStopping(); };

  // At open, with a seed offered: nothing recovers, nothing is quarantined.
  {
    auto options = bed.options();
    options.pause = stopping;
    options.recovery_seed = genesis;
    EXPECT_THROW((Replica(std::move(options))), contract::MaintenanceStopping);
    EXPECT_FALSE(std::filesystem::exists(bed.path() / "recovery.required"));
    EXPECT_FALSE(any_file_starts_with(bed.path(), "checkpoint.meta.corrupt."));
    EXPECT_FALSE(any_file_starts_with(bed.path(), "history.log.corrupt."));
  }
  // The live walks, on a replica opened with nothing in the cache but its head.
  std::atomic_bool stop{false};
  auto options = bed.options();
  options.pause = [&stop]() {
    if (stop.load()) throw contract::MaintenanceStopping();
  };
  options.materialization_cache_limit_bytes = 1;
  Replica replica(std::move(options));
  EXPECT_EQ(replica.committed().hash, head.hash);
  EXPECT_FALSE(replica.recovery_required());
  const auto middle = replica.history_links(head.hash)->previous;
  stop = true;
  EXPECT_THROW((void)replica.materialized(middle), contract::MaintenanceStopping);
  EXPECT_THROW((void)replica.history_is_ancestor(genesis.hash, head.hash), contract::MaintenanceStopping);
  EXPECT_THROW((void)replica.history_common_ancestor(genesis.hash, head.hash), contract::MaintenanceStopping);
  EXPECT_THROW((void)replica.full_history_record(middle), contract::MaintenanceStopping);
  // A stop is not a broken head.
  EXPECT_TRUE(replica.unreconstructable_heads().empty());
  stop = false;
  EXPECT_NE(replica.materialized(middle), nullptr);
}

// The file set of section 4.1: content under the domain's key, this node's standing
// under its own, every file under v19's associated data, set-aside sealed.
TEST(HeadsReplica, TheFileSetIsV19s) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto mine = record_with_clock(2, genesis.hash, {{author(1), 1}}, {"mine"});
  const auto theirs = record_with_clock(4, genesis.hash, {{author(2), 1}}, {"theirs"});
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(mine));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(mine)));
    ASSERT_TRUE(replica->import_history(rootless_entry(theirs)));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(theirs)));
    ASSERT_TRUE(replica->set_aside(theirs.hash, 1000));
    (void)replica->reserve_mutation_dot(author(9), {});
  }
  const auto rule = replica_aad(test::neutral_domain(), test::neutral_space());
  const auto file_bytes = [&](const char* name) {
    std::ifstream in(bed.path() / name, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
  };
  const auto& magics = test::neutral_magics();
  const auto check_opens = [&](const char* name, const Magic& magic, const domain::StorageKey& right, const domain::StorageKey& wrong) {
    const auto file = file_bytes(name);
    EXPECT_NO_THROW((void)open_file(right, magic, rule(magic), file)) << name;
    EXPECT_THROW((void)open_file(wrong, magic, rule(magic), file), std::runtime_error) << name;
    // Neither another domain's nor another space's data opens it, nor the magic alone.
    const auto other_domain = domain::DomainId{{9}};
    EXPECT_THROW((void)open_file(right, magic, replica_aad(other_domain, test::neutral_space())(magic), file), std::runtime_error) << name;
    EXPECT_THROW((void)open_file(right, magic, replica_aad(test::neutral_domain(), space_id(test::neutral_domain(), "other"))(magic), file), std::runtime_error)
        << name;
    EXPECT_THROW((void)open_file(right, magic, magic, file), std::runtime_error) << name;
  };
  check_opens("checkpoint.meta", magics.checkpoint, *bed.content_key, *bed.local_key);
  check_opens("heads.meta", magics.heads, *bed.local_key, *bed.content_key);
  check_opens("author.meta", magics.author, *bed.local_key, *bed.content_key);
  check_opens("set-aside.meta", magics.set_aside, *bed.local_key, *bed.content_key);

  // A history frame is the domain's, with the frame's AAD.
  const auto history = file_bytes("history.log");
  dannan::net::Reader reader(history);
  const auto length = reader.u32();
  const auto envelope = parse_frame_envelope(reader.view(length));
  EXPECT_NO_THROW((void)open_frame_envelope(*bed.content_key, rule(magics.history), envelope));
  EXPECT_THROW((void)open_frame_envelope(*bed.local_key, rule(magics.history), envelope), std::runtime_error);

  // The associated data and the space's id, byte for byte the plan's section 4.1.
  const auto expected_aad = [](std::string_view label, const domain::DomainId& domain, const SpaceId& space, const Magic& magic) {
    Bytes out(label.begin(), label.end());
    out.insert(out.end(), domain.bytes.begin(), domain.bytes.end());
    out.insert(out.end(), space.bytes.begin(), space.bytes.end());
    out.insert(out.end(), magic.begin(), magic.end());
    return out;
  };
  EXPECT_EQ(rule(magics.history), expected_aad("dagda/heads/19", test::neutral_domain(), test::neutral_space(), magics.history));
  EXPECT_EQ(seed_aad(test::neutral_domain(), test::neutral_space(), magics.heads),
            expected_aad("dagda/seed/19", test::neutral_domain(), test::neutral_space(), magics.heads));
  Bytes space_input{'d', 'a', 'g', 'd', 'a', '/', 's', 'p', 'a', 'c', 'e', '/', '1'};
  space_input.insert(space_input.end(), test::neutral_domain().bytes.begin(), test::neutral_domain().bytes.end());
  space_input.insert(space_input.end(), {'a'});
  EXPECT_EQ(space_id(test::neutral_domain(), "a"), dannan::identity::sha256(space_input));

  // The space's path and id are the plan's.
  const auto directory = space_directory("/state", test::neutral_space());
  EXPECT_EQ(directory.parent_path(), std::filesystem::path("/state/heads"));
  EXPECT_EQ(directory.filename().string().size(), 64U);
  EXPECT_NE(space_id(test::neutral_domain(), "a"), space_id(test::neutral_domain(), "b"));
  EXPECT_NE(space_id(domain::DomainId{{9}}, "a"), space_id(test::neutral_domain(), "a"));

  // An unreadable set-aside record only restarts the wait; a corrupt author record
  // stops writes, Macha's R10 and D3, reproduced.
  flip_byte_at(bed.path() / "set-aside.meta", 40);
  flip_byte_at(bed.path() / "author.meta", 40);
  auto reopened = bed.open();
  EXPECT_EQ(reopened->accepted_heads().size(), 2U);
  EXPECT_TRUE(reopened->set_aside(theirs.hash, 5000));
  EXPECT_EQ(reopened->expire_set_aside(5999, std::chrono::milliseconds(1000)), 0U);
  EXPECT_EQ(reopened->expire_set_aside(6000, std::chrono::milliseconds(1000)), 1U);
  EXPECT_THROW((void)reopened->reserve_mutation_dot(author(9), {}), std::runtime_error);
}

// The plan's section 4.2, item 2: opening is lazy and forgiving. A whole body is
// hashed and not decoded; only what can become authority is.
TEST(HeadsReplica, OpeningDecodesOnlyWhatCanBecomeAuthority) {
  ReplicaBed bed;
  CostCounters costs;
  auto state = head_with({});
  auto head = record_of(2, bed.genesis().hash, state);
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(head));
    for (size_t i = 0; i < 40; ++i) {
      state.notes.set("n", std::to_string(i));
      auto next = record_after(head, neutral_payload(state));
      ASSERT_TRUE(replica->store_commit(next));
      head = std::move(next);
    }
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(head)));
  }
  auto options = bed.options();
  options.costs = &costs;
  Replica replica(std::move(options));
  EXPECT_EQ(replica.diagnostics().history_records, 42U);
  EXPECT_EQ(replica.committed().hash, head.hash);
  EXPECT_LE(costs.full_decodes.load(), 2U);
  EXPECT_GE(costs.record_bytes_hashed.load(), 41U * head.payload.size() / 2);
}

// The same item: a frame that will not replay, mid-file, is skipped and what needs it
// flagged, not fatal; bytes after the last frame are quarantined and cut.
TEST(HeadsReplica, OpeningSkipsAFrameThatCannotReplayAndKeepsTheTail) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto parent_head = head_with({"parent"});
  const auto parent = record_of(2, genesis.hash, parent_head);
  const auto child_head = with_note(parent_head, "child");
  const auto child = record_after(parent, neutral_payload(child_head));
  const auto after_head = with_note(child_head, "after");
  const auto after = record_after(child, neutral_payload(after_head));
  const auto history = bed.path() / "history.log";
  uint64_t parent_frame_end = 0;
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(parent));
    parent_frame_end = std::filesystem::file_size(history);
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(parent)));
    ASSERT_TRUE(replica->store_commit(child, delta_between(parent_head, child_head)));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(child)));
    ASSERT_TRUE(replica->store_commit(after, delta_between(child_head, after_head)));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(after)));
  }
  const auto size = std::filesystem::file_size(history);

  // The anchor's frame stops opening: both deltas lose their predecessor, each
  // skipped, and the head, whose record the checkpoint still holds, is re-rooted.
  flip_history_frame(history, parent_frame_end);
  {
    auto replica = bed.open();
    EXPECT_FALSE(replica->history_contains(parent.hash));
    EXPECT_FALSE(replica->history_contains(child.hash));
    EXPECT_TRUE(replica->history_contains(after.hash));
    EXPECT_EQ(replica->committed().hash, after.hash);
    EXPECT_EQ(hashes_of(replica->accepted_heads()), just(after));
    EXPECT_FALSE(replica->recovery_required());
    // Nothing was cut for frames in the middle, and the root was appended after.
    EXPECT_GT(std::filesystem::file_size(history), size);
    EXPECT_FALSE(any_file_starts_with(bed.path(), "history.log.corrupt."));
  }

  // A torn tail: a length with nothing behind it.
  const auto whole = std::filesystem::file_size(history);
  {
    std::ofstream tail(history, std::ios::binary | std::ios::app);
    const std::array<char, 6> torn{0, 0, 1, 0, 9, 9};
    tail.write(torn.data(), torn.size());
  }
  {
    auto replica = bed.open();
    EXPECT_EQ(std::filesystem::file_size(history), whole);
    EXPECT_TRUE(any_file_starts_with(bed.path(), "history.log.corrupt."));
    EXPECT_EQ(replica->committed().hash, after.hash);
  }

  // A final frame that does not authenticate is a tail too, not a skipped frame.
  std::filesystem::remove_all(bed.path("other"));
  const auto other_history = bed.path("other") / "history.log";
  {
    auto replica = bed.open("other");
    ASSERT_TRUE(replica->store_commit(parent));
  }
  const auto kept = std::filesystem::file_size(other_history);
  {
    dannan::net::Writer envelope;
    const std::array<uint8_t, 12> nonce{};
    const std::array<uint8_t, 16> tag{};
    envelope.fixed(nonce);
    envelope.fixed(tag);
    envelope.bytes(Bytes{0x42});
    const auto payload = envelope.take();
    dannan::net::Writer frame;
    frame.u32(static_cast<uint32_t>(payload.size()));
    frame.raw(payload);
    const auto bytes = frame.take();
    std::ofstream tail(other_history, std::ios::binary | std::ios::app);
    tail.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  auto replica = bed.open("other");
  EXPECT_EQ(std::filesystem::file_size(other_history), kept);
  EXPECT_TRUE(any_file_starts_with(bed.path("other"), "history.log.corrupt."));
}

// test_a_damaged_history_frame_loses_only_that_frame (SM at Macha 98b816d, 0.93.5). A
// frame's length word is outside the AEAD, so damage to it looks like a torn tail.
// Whatever it says, every intact frame after it stays, a node opens, and a commit made
// afterwards survives the next open. Only bytes nothing can read are ever cut.
TEST(HeadsReplica, ADamagedHistoryFrameLosesOnlyThatFrame) {
  ReplicaBed bed;

  struct Damage {
    const char* name;
    std::function<void(std::fstream&, uint64_t offset, uint32_t length)> apply;
  };
  const auto put_length = [](std::fstream& file, uint64_t offset, uint32_t value) {
    const std::array<char, 4> bytes{static_cast<char>(value >> 24), static_cast<char>(value >> 16), static_cast<char>(value >> 8), static_cast<char>(value)};
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(bytes.data(), bytes.size());
  };
  const std::vector<Damage> damages{
      {"length past the end of the file", [&](auto& file, uint64_t offset, uint32_t) { put_length(file, offset, 0x10000000); }},
      {"length over any record", [&](auto& file, uint64_t offset, uint32_t) { put_length(file, offset, 0xFFFFFFFF); }},
      {"length short, inside the file", [&](auto& file, uint64_t offset, uint32_t length) { put_length(file, offset, length / 2); }},
      {"length long, inside the file", [&](auto& file, uint64_t offset, uint32_t length) { put_length(file, offset, length + 9); }},
      {"length zero", [&](auto& file, uint64_t offset, uint32_t) { put_length(file, offset, 0); }},
      {"a body byte",
       [](auto& file, uint64_t offset, uint32_t length) {
         const auto at = static_cast<std::streamoff>(offset + 4 + length / 2);
         char byte{};
         file.seekg(at);
         file.get(byte);
         file.seekp(at);
         file.put(static_cast<char>(byte ^ 0xFF));
       }},
  };

  size_t index = 0;
  for (const auto& damage : damages) {
    SCOPED_TRACE(damage.name);
    const auto name = "damaged-" + std::to_string(index++);
    const auto history = bed.path(name) / "history.log";
    auto state = head_with({});

    std::vector<Record> chain;
    std::vector<uint64_t> offsets;
    Record head = bed.genesis();
    const auto next_record = [&](uint64_t generation) {
      state.clock[author(1)] = generation;
      return record_of(generation, head.hash, state);
    };
    {
      auto writer = bed.open(name);
      for (uint64_t generation = 2; generation <= 9; ++generation) {
        offsets.push_back(std::filesystem::file_size(history));
        auto next = next_record(generation);
        ASSERT_TRUE(writer->store_commit(next));
        chain.push_back(next);
        head = next;
      }
    }
    const auto size_before = std::filesystem::file_size(history);

    // The fourth of eight, with intact frames on both sides of it.
    constexpr size_t damaged = 3;
    const auto length = static_cast<uint32_t>(offsets[damaged + 1] - offsets[damaged] - 4);
    {
      std::fstream file(history, std::ios::binary | std::ios::in | std::ios::out);
      damage.apply(file, offsets[damaged], length);
    }

    const auto check_survivors = [&](Replica& replica) {
      for (size_t i = 0; i < chain.size(); ++i) {
        if (i == damaged) continue;
        const auto entry = replica.history_entry(chain[i].hash);
        ASSERT_TRUE(entry.has_value()) << "lost frame " << i;
        EXPECT_EQ(entry->payload, Bytes(chain[i].payload.begin(), chain[i].payload.end()));
      }
    };
    auto after = next_record(10);
    {
      auto reopened = bed.open(name);
      check_survivors(*reopened);
      EXPECT_FALSE(reopened->history_entry(chain[damaged].hash).has_value());
      EXPECT_EQ(std::filesystem::file_size(history), size_before);
      ASSERT_TRUE(reopened->store_commit(after));
    }
    // What was appended past the damage is found again, and the damage has not spread.
    auto again = bed.open(name);
    check_survivors(*again);
    EXPECT_TRUE(again->history_entry(after.hash).has_value());
  }
}

// test_a_torn_history_tail_is_still_cut (SM at Macha 9153011). A frame cut short by a
// crash has nothing readable after it: it is cut, kept aside, and everything before it
// stays. A byte that is not a frame at all goes the same way.
TEST(HeadsReplica, ATornHistoryTailIsStillCut) {
  ReplicaBed bed;
  const auto history = bed.path() / "history.log";

  auto state = head_with({});
  std::vector<Record> chain;
  uint64_t last_frame = 0;
  Record head = bed.genesis();
  {
    auto writer = bed.open();
    for (uint64_t generation = 2; generation <= 5; ++generation) {
      state.clock[author(1)] = generation;
      auto next = record_of(generation, head.hash, state);
      last_frame = std::filesystem::file_size(history);
      ASSERT_TRUE(writer->store_commit(next));
      chain.push_back(next);
      head = next;
    }
  }
  std::filesystem::resize_file(history, std::filesystem::file_size(history) - 7);
  {
    auto reopened = bed.open();
    for (size_t i = 0; i + 1 < chain.size(); ++i) EXPECT_TRUE(reopened->history_entry(chain[i].hash).has_value());
    EXPECT_FALSE(reopened->history_entry(chain.back().hash).has_value());
    EXPECT_EQ(std::filesystem::file_size(history), last_frame);
  }
  size_t kept_aside = 0;
  for (const auto& item : std::filesystem::directory_iterator(bed.path()))
    kept_aside += item.path().filename().string().find("history.log") == 0 && item.path().filename() != "history.log";
  EXPECT_EQ(kept_aside, 1U);
}

namespace {

// A disk that stops answering at a byte: what lies past it cannot be read.
struct FailingBuffer final : std::streambuf {
  Bytes bytes;
  std::streamoff fails_from;
  FailingBuffer(Bytes value, std::streamoff at) : bytes(std::move(value)), fails_from(at) { place(0); }
  char* chars() { return reinterpret_cast<char*>(bytes.data()); }
  void place(off_type off) {
    setg(chars(), chars() + off, chars() + std::min<off_type>(std::max<off_type>(fails_from, off), static_cast<off_type>(bytes.size())));
  }
  pos_type seekoff(off_type off, std::ios_base::seekdir, std::ios_base::openmode) override {
    place(off);
    return off;
  }
  pos_type seekpos(pos_type at, std::ios_base::openmode mode) override { return seekoff(at, std::ios_base::beg, mode); }
};

struct FailingStream final : std::istream {
  FailingBuffer buffer;
  FailingStream(Bytes value, std::streamoff at) : std::istream(nullptr), buffer(std::move(value), at) { rdbuf(&buffer); }
};

Bytes read_all(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// test_a_failed_read_in_the_history_scan_is_not_nothing_later (SM at Macha 98b816d). A
// disk that stops answering says nothing about what is on it, so the scan reports that
// apart from finding nothing.
TEST(HeadsReplica, AFailedReadInTheHistoryScanIsNotNothingLater) {
  constexpr uint32_t inner = 64;
  dannan::net::Writer writer;
  writer.raw(Bytes(100, 0xEE));
  writer.u32(inner + 32);
  writer.raw(Bytes(28, 1));
  writer.u32(inner);
  writer.raw(Bytes(inner, 2));
  const auto file = writer.take();
  const auto open = [](std::span<const uint8_t>) -> std::optional<Bytes> { return Bytes{}; };
  constexpr uint64_t max_frame = 1024 * 1024;

  {
    FailingStream stream(file, static_cast<std::streamoff>(file.size()));
    bool read_failed = false;
    const auto found = heads::next_frame_from(stream, file.size(), 1, max_frame, open, read_failed);
    EXPECT_FALSE(read_failed);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, 100U);
  }
  {
    // Nothing but bytes that are not a frame: nothing later, and no failure.
    const Bytes noise(4096, 0xEE);
    FailingStream stream(noise, static_cast<std::streamoff>(noise.size()));
    bool read_failed = false;
    EXPECT_FALSE(heads::next_frame_from(stream, noise.size(), 1, max_frame, open, read_failed).has_value());
    EXPECT_FALSE(read_failed);
  }
  {
    // The disk fails before the frame: a failure, not nothing.
    FailingStream stream(file, 50);
    bool read_failed = false;
    EXPECT_FALSE(heads::next_frame_from(stream, file.size(), 1, max_frame, open, read_failed).has_value());
    EXPECT_TRUE(read_failed);
  }
  {
    // The disk fails inside the candidate frame.
    FailingStream stream(file, 120);
    bool read_failed = false;
    EXPECT_FALSE(heads::next_frame_from(stream, 100 + 4 + inner + 32, 100, max_frame, open, read_failed).has_value());
    EXPECT_TRUE(read_failed);
  }
}

// Macha's case tests the scan alone, and its main loop only by reading: this is the whole
// load, at each place a read can fail. Nothing is cut or kept aside, the frames read
// stand, a later commit lands past every byte of the file, and the next open on a disk
// that answers finds all of it.
TEST(HeadsReplica, AFailedReadOfHistoryCutsNothing) {
  struct Failure {
    const char* name;
    bool damage_fourth_length;       // Sends the load through the scan.
    uint64_t main_fails_past_frame;  // The frame whose bytes the main stream stops at.
    uint64_t into_frame;
    bool scan_fails;
  };
  const std::vector<Failure> failures{
      {"in a frame's length word", false, 3, 2, false},
      {"in a frame's body", false, 3, 20, false},
      {"in the scan past a damaged frame", true, 7, 0, true},
  };
  size_t index = 0;
  for (const auto& failure : failures) {
    SCOPED_TRACE(failure.name);
    ReplicaBed bed;
    const auto name = "failed-" + std::to_string(index++);
    const auto history = bed.path(name) / "history.log";
    auto state = head_with({});
    std::vector<Record> chain;
    std::vector<uint64_t> offsets;
    Record head = bed.genesis();
    {
      auto writer = bed.open(name);
      for (uint64_t generation = 2; generation <= 9; ++generation) {
        offsets.push_back(std::filesystem::file_size(history));
        state.clock[author(1)] = generation;
        auto next = record_of(generation, head.hash, state);
        ASSERT_TRUE(writer->store_commit(next));
        ASSERT_TRUE(writer->accept_commit(accepted_by_two(next)));
        chain.push_back(next);
        head = next;
      }
    }
    if (failure.damage_fourth_length) {
      std::fstream file(history, std::ios::binary | std::ios::in | std::ios::out);
      file.seekp(static_cast<std::streamoff>(offsets[3]));
      const std::array<char, 4> zero{};
      file.write(zero.data(), zero.size());
    }
    const auto before = read_all(history);
    const auto fails_at = static_cast<std::streamoff>(offsets[failure.main_fails_past_frame] + failure.into_frame);

    const auto sink = std::make_shared<test::RecordingSink>();
    auto options = bed.options(name);
    options.logger = test::logger_into(sink);
    size_t opened = 0;
    options.history_reader = [&](const std::filesystem::path& path) -> std::unique_ptr<std::istream> {
      const bool is_scan = opened++ == 1;
      const auto limit = is_scan && failure.scan_fails ? static_cast<std::streamoff>(offsets[3] + 8) : fails_at;
      return std::make_unique<FailingStream>(read_all(path), limit);
    };
    const size_t read_before = failure.damage_fourth_length ? 3 : failure.main_fails_past_frame;
    state.clock[author(1)] = 10;
    const auto after = record_of(10, head.hash, state);
    {
      Replica replica(std::move(options));
      // Every byte stays. The head, unread, is rooted again past them, as at any open.
      const auto now = read_all(history);
      ASSERT_GE(now.size(), before.size());
      EXPECT_TRUE(std::equal(before.begin(), before.end(), now.begin()));
      EXPECT_FALSE(any_file_starts_with(bed.path(name), "history.log.corrupt."));
      EXPECT_TRUE(sink->saw("metadata history read failed"));
      EXPECT_TRUE(sink->saw("nothing cut"));
      EXPECT_FALSE(sink->saw("metadata history recovered"));
      EXPECT_FALSE(replica.recovery_required());
      for (size_t i = 0; i < read_before; ++i) EXPECT_TRUE(replica.history_entry(chain[i].hash).has_value()) << i;
      for (size_t i = read_before; i < chain.size() - 1; ++i) EXPECT_FALSE(replica.history_entry(chain[i].hash).has_value()) << i;
      ASSERT_TRUE(replica.store_commit(after));
      // Appended past every byte, at the offset the index holds.
      EXPECT_GT(std::filesystem::file_size(history), before.size());
      const auto appended = replica.history_entry(after.hash);
      ASSERT_TRUE(appended.has_value());
      EXPECT_EQ(appended->payload, Bytes(after.payload.begin(), after.payload.end()));
    }
    auto again = bed.open(name);
    for (size_t i = 0; i < chain.size(); ++i) {
      if (failure.damage_fourth_length && i == 3) continue;
      EXPECT_TRUE(again->history_entry(chain[i].hash).has_value()) << "lost frame " << i;
    }
    EXPECT_TRUE(again->history_entry(after.hash).has_value());
  }
}

// Beyond Macha's two: what 9153011 says that they do not assert. The journal's words on
// a tail, and that a length over any record no longer reaches a seed.
TEST(HeadsReplica, ATornHistoryTailSaysWhyAndNeverReachesTheSeed) {
  const auto length_of = [](uint32_t value) {
    dannan::net::Writer out;
    out.u32(value);
    return out.take();
  };
  const auto sealed_junk = [] {
    dannan::net::Writer envelope;
    envelope.fixed(std::array<uint8_t, 12>{});
    envelope.fixed(std::array<uint8_t, 16>{});
    envelope.bytes(Bytes{0x42});
    const auto payload = envelope.take();
    dannan::net::Writer frame;
    frame.u32(static_cast<uint32_t>(payload.size()));
    frame.raw(payload);
    return frame.take();
  };
  struct Tail {
    Bytes bytes;
    const char* reason;
  };
  const std::vector<Tail> tails{
      {sealed_junk(), "reason=last frame failed AES-GCM authentication"},  {length_of(0), "reason=last frame failed AES-GCM authentication"},
      {length_of(0x10000000), "reason=incomplete trailing frame"},         {length_of(0xFFFFFFFF), "reason=incomplete trailing frame"},
      {Bytes{1, 2, 3}, "reason=trailing bytes after last complete frame"},
  };

  size_t index = 0;
  for (const auto& tail : tails) {
    SCOPED_TRACE(tail.reason);
    ReplicaBed bed;
    const auto name = "tail-" + std::to_string(index++);
    const auto history = bed.path(name) / "history.log";
    const auto record = record_of(2, bed.genesis().hash, head_with({"kept"}));
    {
      auto replica = bed.open(name);
      ASSERT_TRUE(replica->store_commit(record));
      ASSERT_TRUE(replica->accept_commit(accepted_by_two(record)));
    }
    const auto kept = std::filesystem::file_size(history);
    {
      std::ofstream out(history, std::ios::binary | std::ios::app);
      out.write(reinterpret_cast<const char*>(tail.bytes.data()), static_cast<std::streamsize>(tail.bytes.size()));
    }
    const auto sink = std::make_shared<test::RecordingSink>();
    auto options = bed.options(name);
    options.logger = test::logger_into(sink);
    options.recovery_seed = bed.genesis();
    Replica replica(std::move(options));
    EXPECT_TRUE(sink->saw(tail.reason));
    EXPECT_FALSE(sink->saw("record too large"));
    EXPECT_EQ(std::filesystem::file_size(history), kept);
    EXPECT_TRUE(any_file_starts_with(bed.path(name), "history.log.corrupt."));
    EXPECT_FALSE(replica.recovery_required());
    EXPECT_EQ(replica.committed().hash, record.hash);
  }

  // A length over any record in the middle of the file reaches no seed either.
  ReplicaBed bed;
  const auto history = bed.path() / "history.log";
  const auto first = record_of(2, bed.genesis().hash, head_with({"first"}));
  const auto second = record_after(first, neutral_payload(head_with({"second"})));
  uint64_t second_frame = 0;
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(first));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(first)));
    second_frame = std::filesystem::file_size(history);
    ASSERT_TRUE(replica->store_commit(second));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(second)));
  }
  const auto third_frame = std::filesystem::file_size(history);
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(record_after(second, neutral_payload(head_with({"third"})))));
  }
  {
    std::fstream file(history, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(second_frame));
    const std::array<char, 4> over{'\xff', '\xff', '\xff', '\xff'};
    file.write(over.data(), over.size());
  }
  const auto sink = std::make_shared<test::RecordingSink>();
  auto options = bed.options();
  options.logger = test::logger_into(sink);
  options.recovery_seed = bed.genesis();
  Replica replica(std::move(options));
  EXPECT_FALSE(replica.recovery_required());
  EXPECT_FALSE(std::filesystem::exists(bed.path() / "recovery.required"));
  EXPECT_EQ(replica.committed().hash, second.hash);
  EXPECT_TRUE(sink->saw("offset=" + std::to_string(second_frame) + ": frame does not authenticate"));
  EXPECT_GT(std::filesystem::file_size(history), third_frame);
}

// 9153011: a frame that authenticates and will not decode is skipped by its length, even
// at the end of the file, where a frame that does not authenticate would be a tail.
TEST(HeadsReplica, AnAuthenticatedFrameThatWillNotDecodeIsNeverATail) {
  ReplicaBed bed;
  const auto history = bed.path() / "history.log";
  const auto record = record_of(2, bed.genesis().hash, head_with({"kept"}));
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(record));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(record)));
  }
  const auto aad = heads::replica_aad(test::neutral_domain(), test::neutral_space())(test::neutral_magics().history);
  const auto frame = heads::seal_frame(*bed.content_key, aad, Bytes{0xEE});
  {
    std::ofstream out(history, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
  }
  const auto size = std::filesystem::file_size(history);
  auto replica = bed.open();
  EXPECT_EQ(std::filesystem::file_size(history), size);
  EXPECT_FALSE(any_file_starts_with(bed.path(), "history.log.corrupt."));
  EXPECT_EQ(replica->committed().hash, record.hash);
}

// What lies beyond one window: Macha's overlap of 35 bytes, read at its edge, with a frame
// starting on each side of it. Dagda's own over Macha's: a stop passes through the scan,
// window by window, leaving the file as it was, and the scan is reachable with no replica.
TEST(HeadsReplica, TheForwardScanWalksWindowsAndAStopLeavesTheFile) {
  constexpr uint64_t window = 1024 * 1024;
  constexpr uint64_t header = 36;
  const auto aad = heads::replica_aad(test::neutral_domain(), test::neutral_space())(test::neutral_magics().history);
  std::mt19937 random(9153011);
  for (const uint64_t from_start : {window - header - 1, window - header, window - header + 1, window - 1, window, window + 7, 3 * window}) {
    SCOPED_TRACE(from_start);
    ReplicaBed bed;
    const auto history = bed.path() / "history.log";
    const auto first = record_of(2, bed.genesis().hash, head_with({"first"}));
    const auto last = record_after(first, neutral_payload(head_with({"last"})));
    uint64_t boundary = 0;
    {
      auto replica = bed.open();
      ASSERT_TRUE(replica->store_commit(first));
      ASSERT_TRUE(replica->accept_commit(accepted_by_two(first)));
      boundary = std::filesystem::file_size(history);
      ASSERT_TRUE(replica->store_commit(last));
      ASSERT_TRUE(replica->accept_commit(accepted_by_two(last)));
    }
    std::ifstream in(history, std::ios::binary);
    Bytes whole(std::istreambuf_iterator<char>(in), {});
    in.close();
    // The scan starts one past the first bad byte, so the next frame is `from_start` from there.
    Bytes region(from_start + 1);
    for (auto& byte : region) byte = static_cast<uint8_t>(random());
    Bytes damaged(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(boundary));
    damaged.insert(damaged.end(), region.begin(), region.end());
    damaged.insert(damaged.end(), whole.begin() + static_cast<std::ptrdiff_t>(boundary), whole.end());
    {
      std::ofstream out(history, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(damaged.data()), static_cast<std::streamsize>(damaged.size()));
    }
    const auto next = heads::next_frame_from(history, damaged.size(), boundary + 1, *bed.content_key, aad);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, boundary + region.size());
    size_t windows = 0;
    EXPECT_TRUE(heads::next_frame_from(history, damaged.size(), boundary + 1, *bed.content_key, aad, [&] { ++windows; }).has_value());
    EXPECT_EQ(windows, 1 + from_start / (window - header + 1));
    EXPECT_FALSE(heads::next_frame_from(history, damaged.size(), *next + 1, *bed.content_key, aad).has_value());

    if (from_start == 3 * window) {
      // At open, with a seed on offer: the first window's pause stops it, and nothing moved.
      auto stopping = bed.options();
      size_t calls = 0;
      stopping.pause = [&calls] {
        if (++calls > 3) throw contract::MaintenanceStopping();
      };
      stopping.recovery_seed = bed.genesis();
      EXPECT_THROW((Replica(std::move(stopping))), contract::MaintenanceStopping);
      EXPECT_FALSE(std::filesystem::exists(bed.path() / "recovery.required"));
      EXPECT_FALSE(any_file_starts_with(bed.path(), "history.log.corrupt."));
      EXPECT_EQ(std::filesystem::file_size(history), damaged.size());
    }

    auto replica = bed.open();
    EXPECT_EQ(replica->committed().hash, last.hash);
    EXPECT_TRUE(replica->history_contains(first.hash));
    EXPECT_TRUE(replica->history_contains(last.hash));
    EXPECT_EQ(replica->diagnostics().history_records, 3U);
    EXPECT_EQ(std::filesystem::file_size(history), damaged.size());
  }
}

// A torn tail over the quarantine limit is cut and not kept, as it always was.
TEST(HeadsReplica, ATornHistoryTailOverTheLimitIsCutAndNotKept) {
  ReplicaBed bed;
  const auto history = bed.path() / "history.log";
  const auto record = record_of(2, bed.genesis().hash, head_with({"kept"}));
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(record));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(record)));
  }
  const auto kept = std::filesystem::file_size(history);
  std::filesystem::resize_file(history, kept + 16ULL * 1024 * 1024 + 1);
  const auto sink = std::make_shared<test::RecordingSink>();
  auto options = bed.options();
  options.logger = test::logger_into(sink);
  Replica replica(std::move(options));
  EXPECT_EQ(std::filesystem::file_size(history), kept);
  EXPECT_FALSE(any_file_starts_with(bed.path(), "history.log.corrupt."));
  EXPECT_TRUE(sink->saw("<discarded: over quarantine limit>"));
  EXPECT_EQ(replica.committed().hash, record.hash);
}

// S3's seam for S4: a corrupt delta frame the codec refuses at open is a skipped
// frame. A delta whose frame opens but will not apply leaves its head flagged, not
// fatal (the plan's section 4.2, item 3).
TEST(HeadsReplica, AHeadWhoseDeltaWillNotApplyIsFlaggedNotFatal) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  SharedTreeStore store;
  const auto tree = test::neutral_tree();
  NeutralHead parent_head;
  {
    const auto writer = store.factory()();
    NeutralEntries entries;
    entries["k"] = test::make_item(1, 1);
    parent_head.root = tree.build(entries, *writer);
  }
  const auto parent = record_of(2, genesis.hash, parent_head);
  test::NeutralDelta delta;
  delta.upsert_keys["l"] = test::make_item(2, 1);
  auto child_head = parent_head;
  {
    const auto writer = store.factory()();
    NeutralTree::Delta changes;
    changes.upsert = delta.upsert_keys;
    child_head.root = tree.apply(*parent_head.root, *writer, changes);
  }
  const auto child = record_after(parent, neutral_payload(child_head));
  const auto mine = record_of(9, genesis.hash, head_with({"mine"}));
  {
    auto options = bed.options();
    options.tree_store = store.factory();
    Replica replica(options);
    ASSERT_TRUE(replica.store_commit(parent));
    ASSERT_TRUE(replica.store_commit(child, test::encode_neutral_delta(delta)));
    ASSERT_TRUE(replica.accept_commit(accepted_by_two(child)));
    ASSERT_TRUE(replica.store_commit(mine));
    ASSERT_TRUE(replica.accept_commit(accepted_by_two(mine)));
    ASSERT_EQ(replica.committed().hash, mine.hash);
  }
  // Reopened with no node store: the frames open, and the delta will not apply.
  auto replica = bed.open("space", {}, true, 1);
  EXPECT_EQ(replica->committed().hash, mine.hash);
  EXPECT_EQ(replica->accepted_head_certificates().size(), 2U);
  EXPECT_EQ(replica->unreconstructable_heads(), just(child));
  EXPECT_EQ(hashes_of(replica->accepted_heads()), just(mine));
}

// H17: the cache's bounds. At most 64 entries and its bytes, least recently used out,
// and the committed head pinned, whatever comes through.
TEST(HeadsReplica, TheCacheHoldsAtMostSixtyFourAndPinsTheHead) {
  ReplicaBed bed;
  auto replica = bed.open();
  std::vector<Record> records;
  NeutralHead state;
  Record head = bed.genesis();
  for (uint64_t generation = 2; generation <= 90; ++generation) {
    state.notes.set("n", std::to_string(generation));
    head = record_after(head, neutral_payload(state));
    ASSERT_TRUE(replica->store_commit(head));
    records.push_back(head);
    EXPECT_LE(replica->diagnostics().materialization_cache_entries, 64U);
  }
  const auto first = records.front();
  ASSERT_TRUE(replica->accept_commit(accepted_by_two(first)));
  for (const auto& record : records) {
    ASSERT_NE(replica->materialized(record.hash), nullptr);
    EXPECT_LE(replica->diagnostics().materialization_cache_entries, 64U);
  }
  const auto after = replica->diagnostics();
  EXPECT_EQ(after.materialization_cache_entries, 64U);
  EXPECT_GT(after.materialization_cache_evictions, 0U);
  // The committed head, first in and least recently used, is not what went.
  const auto hits = after.materialization_cache_hits;
  EXPECT_NE(replica->materialized(first.hash), nullptr);
  EXPECT_EQ(replica->diagnostics().materialization_cache_hits, hits + 1);
}

// Macha's oddities that a case can hold, written down as requirements (the plan's
// section 7): R5 and R8 here; R9's memory-only acceptance, by what a reopen forgets.
TEST(HeadsReplica, MachasOdditiesStand) {
  ReplicaBed bed;
  const auto genesis = bed.genesis();
  const auto mine = record_with_clock(2, genesis.hash, {{author(1), 1}}, {"mine"});
  const auto theirs = record_with_clock(4, genesis.hash, {{author(2), 1}}, {"theirs"});
  const auto a = author(1);
  const auto b = author(2);
  const auto node = author(7);
  {
    auto replica = bed.open();
    ASSERT_TRUE(replica->store_commit(mine));
    // R5: a repeat takes the min of `required` and the union: it can lower it.
    ASSERT_TRUE(replica->accept_commit({mine.generation, mine.hash, 2, {a, b}}));
    bool changed = false;
    ASSERT_TRUE(replica->accept_commit({mine.generation, mine.hash, 1, {author(3)}}, &changed));
    EXPECT_TRUE(changed);
    const auto certificate = replica->acceptance(mine.hash);
    ASSERT_TRUE(certificate.has_value());
    EXPECT_EQ(certificate->required, 1U);
    EXPECT_EQ(certificate->replicas, (std::vector<NodeId>{a, b, author(3)}));

    ASSERT_TRUE(replica->import_history(rootless_entry(theirs)));
    ASSERT_TRUE(replica->accept_commit(accepted_by_two(theirs)));
    ASSERT_TRUE(replica->set_aside(theirs.hash, 1000));
    EXPECT_EQ(replica->committed().hash, mine.hash);

    // R9: accepted lives in memory until the next reservation.
    const auto dot = replica->reserve_mutation_dot(node, {});
    replica->note_author_accepted(dot);
  }
  auto reopened = bed.open();
  // R8: a set-aside is this run's alone; a restart prefers the head again.
  EXPECT_EQ(reopened->committed().hash, theirs.hash);
  EXPECT_EQ(reopened->set_aside_generation(), 0U);
  // R9: the crash lost `accepted`, so a head without the dot does not rotate.
  const auto dot = reopened->reserve_mutation_dot(node, {});
  EXPECT_EQ(dot.author, node);
  EXPECT_EQ(dot.sequence, 2U);
}

// R2's clause in the replica: a clock equal to another's is not ancestry (md.cpp:2802),
// so two heads with one clock stay until a merge collapses them.
TEST(HeadsReplica, AHeadWithTheSameClockIsNotAnAncestor) {
  ReplicaBed bed;
  Hash256 unseen{};
  unseen.bytes[0] = 0x44;
  const HeadClock clock{{author(1), 3}};
  const auto one = record_with_clock(5, unseen, clock, {"one"});
  const auto two = record_with_clock(6, unseen, clock, {"two"});
  auto replica = bed.open();
  ASSERT_TRUE(replica->import_history(rootless_entry(one)));
  ASSERT_TRUE(replica->accept_commit(accepted_by_two(one)));
  ASSERT_TRUE(replica->import_history(rootless_entry(two)));
  ASSERT_TRUE(replica->accept_commit(accepted_by_two(two)));
  EXPECT_EQ(hashes_of(replica->accepted_heads()), sorted_hashes({&one, &two}));
}

}  // namespace
}  // namespace dagda::heads
