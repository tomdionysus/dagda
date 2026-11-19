//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Session S4 on one space: the mutation path, its claims, its retries by dot and its
// reads, over the neutral owner. Macha's cases are named where one is converted; what
// a Macha case says of its filesystem stays with it, and each such half is named.
// fixtures/heads_space.h is the owner. A node's own spaces are heads_node_test.cpp.

#include "dagda/test/fixtures/heads_space.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/heads/retirement.h"
#include "dagda/heads/space.h"
#include "dagda/test/fixtures/heads.h"
#include "dannan/test/fixtures/gate.h"

namespace dagda::heads {
namespace {

using dannan::sched::FrameType;
using dannan::sched::Waits;
using dannan::sched::WorkContext;
using test::author;
using test::commit_record;
using test::make_group;
using test::make_item;
using test::neutral_batches;
using test::neutral_mutation;
using test::neutral_payload;
using test::neutral_tree;
using test::NeutralCodec;
using test::NeutralEditing;
using test::NeutralEntries;
using test::NeutralHead;
using test::NeutralItem;
using test::NeutralRef;
using test::NeutralTree;
using test::record_after;
using test::record_of;
using test::SpaceBed;
using test::StepLog;

const NeutralHead& head_of(const SpaceView& view) { return *NeutralCodec::as_head(view.state); }

Dot dot_of(const CommitOutcome& outcome) { return *outcome.dot; }

// An item naming objects, a gap beside them: a gap is not an object.
NeutralItem item_of(std::initializer_list<ObjectId> ids) {
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
  NeutralRef gap;
  gap.start = start;
  gap.span = 4096;
  gap.gap = true;
  item.refs.push_back(gap);
  return item;
}

// The order Macha's mutate_delta keeps (mgr:1658-1869): the dot durable, then the
// owner's edit, its stamps against the parent's clock, the clock, the tree, the claims,
// and only then the commit. A mutation publishes to this node alone (H4, mgr:171-179).
TEST(HeadsCommit, AMutationKeepsMachasOrderAndIsAcceptedHereAlone) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  StepLog steps;
  bool durable_before_edit = false;
  bool unaccepted_at_claim = false;
  uint64_t committed_at_claim = 0;
  bed.barrier.on_claim = [&] {
    committed_at_claim = space->replica().committed_generation();
    unaccepted_at_claim = space->replica().accepted_heads().size() == 1;
  };
  bed.barrier.here.insert(test::fake_object(1));
  const auto outcome = space->mutate_delta(neutral_mutation(
      [&](NeutralEditing& editing) {
        durable_before_edit = std::filesystem::exists(bed.bed.path() / "author.meta");
        editing.delta.upsert_keys["b"] = item_of({test::fake_object(1)});
      },
      &steps));
  EXPECT_EQ(steps, (StepLog{"edit", "stamp", "advance", "apply"}));
  EXPECT_TRUE(durable_before_edit);
  EXPECT_TRUE(unaccepted_at_claim);
  EXPECT_EQ(committed_at_claim, 2U);
  EXPECT_EQ(outcome.kind, CommitOutcome::Kind::committed);
  EXPECT_EQ(outcome.record.generation, 3U);
  EXPECT_EQ(outcome.attempts, 1U);
  EXPECT_EQ(space->replica().committed().hash, outcome.record.hash);

  // Certificate: this node alone, required its own count (R5). No peer was asked.
  const auto certificate = space->replica().acceptance(outcome.record.hash);
  ASSERT_TRUE(certificate);
  EXPECT_EQ(certificate->required, 1U);
  EXPECT_EQ(certificate->replicas, std::vector<NodeId>{bed.self});

  // Stamped with the dot, in the clock, and the tree holds what the delta said.
  const auto view = space->current();
  ASSERT_TRUE(view);
  EXPECT_EQ(view->hash, outcome.record.hash);
  EXPECT_TRUE(clock_covers(head_of(*view).clock, dot_of(outcome)));
  const auto reader = bed.store.factory()();
  const auto stored = neutral_tree().lookup(*head_of(*view).root, "b", *reader);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->stamp, static_cast<int64_t>(dot_of(outcome).sequence));
}

// C2, a commit's half (mgr:1834); the merge's is HeadsReconcile.AMergeBodyIsADeltaOnlyWhenStrictlySmaller. A delta strictly smaller than
// the record is the frame's body; equal or larger, the record is.
HistoryEntry::Body body_with_padding(size_t padding, size_t* delta_bytes = nullptr, size_t* record_bytes = nullptr) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto outcome = space->mutate_delta(neutral_mutation([&](NeutralEditing& editing) {
    // Erasing a note the head does not hold changes nothing but the delta's size.
    editing.delta.erase_notes.push_back(std::string(padding, 'p'));
  }));
  const auto entry = space->replica().history_entry(outcome.record.hash);
  EXPECT_TRUE(entry);
  EXPECT_EQ(entry->body, outcome.body);
  if (delta_bytes) *delta_bytes = entry->payload.size();
  if (record_bytes) *record_bytes = outcome.record.payload.size();
  return outcome.body;
}

TEST(HeadsCommit, ABodyIsADeltaOnlyWhenStrictlySmallerThanTheRecord) {
  size_t delta = 0;
  size_t record = 0;
  ASSERT_EQ(body_with_padding(1, &delta, &record), HistoryEntry::Body::delta);
  ASSERT_LT(delta, record);
  // One byte of padding is one byte of delta, so the three sizes are exact.
  const auto equal = 1 + (record - delta);
  EXPECT_EQ(body_with_padding(equal - 1), HistoryEntry::Body::delta);
  EXPECT_EQ(body_with_padding(equal), HistoryEntry::Body::full);
  EXPECT_EQ(body_with_padding(equal + 1), HistoryEntry::Body::full);
}

// mutate() has no change declared: the codec's encode_delta says it, and when it says
// a delta that does not reach the record, the frame is whole.
TEST(HeadsCommit, MutateAsksTheCodecForTheDelta) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto noted = space->mutate(neutral_mutation([](NeutralEditing& editing) { editing.head.notes.set("k", "v"); }));
  EXPECT_EQ(noted.body, HistoryEntry::Body::delta);
  EXPECT_EQ(head_of(*space->current()).notes.get().at("k"), "v");
  // The neutral codec's delta leaves the tree aside: it cannot say this one.
  const auto keyed = space->mutate(neutral_mutation([](NeutralEditing& editing) {
    editing.delta.upsert_keys["z"] = make_group(9);
    editing.head.notes.set("k", "w");
  }));
  EXPECT_EQ(keyed.body, HistoryEntry::Body::full);
  EXPECT_EQ(keyed.record.generation, noted.record.generation + 1);
}

// test_local_metadata_store_falls_back_from_invalid_delta (SM:4340). The supplied exact
// delta omits the change: the local replica rejects it and takes the full record, as a
// remote replica does. Macha's edit sets a catalogue root, an owner's field: a note here.
TEST(HeadsCommit, ADeltaThatOmitsAChangeFallsBackToTheWholeRecord) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto committed = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.head.notes.set("omitted", "from the delta"); }));
  // metadata.snapshot().catalogue_root == root
  EXPECT_EQ(head_of(space->converged()).notes.get().at("omitted"), "from the delta");
  const auto history = space->replica().history_entry(committed.record.hash);
  ASSERT_TRUE(history);
  EXPECT_EQ(history->body, HistoryEntry::Body::full);
  EXPECT_EQ(committed.body, HistoryEntry::Body::full);
  EXPECT_TRUE(bed.sink->saw("local metadata delta rejected; retrying full record generation=3"));
}

// C5. The first publish throws once the commit is accepted; the second attempt keeps
// the dot, finds it covered and returns the head, generation unchanged, and the edit
// runs once. Macha's retry has no case of its own.
TEST(HeadsCommit, ARetryKeepsItsDotAndAnAcceptedAttemptIsNotMadeTwice) {
  SpaceBed bed;
  int publishes = 0;
  bed.accepted = [&](const Acceptance&, bool, const Record&) {
    if (publishes++ == 0) throw HeadsNotReady("interrupted after acceptance");
  };
  auto space = bed.open();
  bed.found(*space);
  publishes = 0;
  int edits = 0;
  std::optional<Dot> seen;
  const auto outcome = space->mutate_delta(neutral_mutation([&](NeutralEditing& editing) {
    ++edits;
    seen = editing.context.dot;
    editing.set_note("once", "only");
  }));
  EXPECT_EQ(edits, 1);
  EXPECT_EQ(publishes, 1);
  EXPECT_EQ(outcome.kind, CommitOutcome::Kind::covered);
  EXPECT_EQ(outcome.attempts, 2U);
  EXPECT_EQ(outcome.dot, seen);
  EXPECT_EQ(outcome.record.generation, 3U);
  EXPECT_EQ(space->replica().committed().generation, 3U);
  EXPECT_EQ(space->owed_commits(), 1U);
  // The next mutation has the next dot: the retried one was not spent again.
  const auto next = space->mutate_delta(neutral_mutation([](NeutralEditing&) {}));
  EXPECT_EQ(next.dot->sequence, seen->sequence + 1);
  EXPECT_EQ(next.record.generation, 4U);
}

// The dot is kept whatever the next attempt finds: here its own commit has been set aside,
// so the head it extends does not cover the dot, and it commits again under the same one
// rather than reserve another (mgr:1693-1695).
TEST(HeadsCommit, ARetryThatFindsItsDotUncoveredCommitsUnderTheSameDot) {
  SpaceBed bed;
  Space* holder = nullptr;
  int publishes = 0;
  bed.accepted = [&](const Acceptance& acceptance, bool, const Record&) {
    if (!holder || publishes++ != 0) return;
    EXPECT_TRUE(holder->set_aside(*holder->replica().historical(acceptance.hash), "as if it could not be merged"));
    throw HeadsNotReady("interrupted after acceptance");
  };
  auto space = bed.open();
  bed.found(*space);
  const auto base = space->replica().committed();
  auto other = *NeutralCodec::as_head(space->converged().state);
  auto sibling = other;
  other.clock[author(2)] = 1;
  commit_record(space->replica(), record_after(base, neutral_payload(other)));
  sibling.clock[author(3)] = 1;
  commit_record(space->replica(), record_after(base, neutral_payload(sibling)));
  holder = space.get();
  std::vector<Dot> dots;
  const auto outcome = space->mutate_delta(neutral_mutation([&](NeutralEditing& editing) { dots.push_back(editing.context.dot); }));
  ASSERT_EQ(dots.size(), 2U);
  EXPECT_EQ(dots[0], dots[1]);
  EXPECT_EQ(outcome.kind, CommitOutcome::Kind::committed);
  EXPECT_EQ(outcome.attempts, 2U);
  EXPECT_EQ(outcome.dot, dots[1]);
}

// R10, D3: a corrupt author.meta throws out of the reservation, and every mutation with
// it: Macha has no recovery, while reads go on.
TEST(HeadsCommit, ACorruptAuthorRecordStopsMutationsAndNotReads) {
  SpaceBed bed;
  Record head;
  {
    auto space = bed.open();
    bed.found(*space);
    head = space->mutate_delta(neutral_mutation([](NeutralEditing&) {})).record;
  }
  test::flip_byte_at(bed.bed.path() / "author.meta", 20);
  auto space = bed.open();
  EXPECT_ANY_THROW(space->mutate_delta(neutral_mutation([](NeutralEditing&) {})));
  EXPECT_EQ(space->replica().committed().hash, head.hash);
  EXPECT_EQ(space->local().hash, head.hash);
}

TEST(HeadsCommit, RetriesSpentRaiseWhatInterruptedThem) {
  SpaceBed bed;
  bool interrupt = true;
  bed.accepted = [&](const Acceptance&, bool, const Record&) {
    if (interrupt) throw HeadsNotReady("interrupted");
  };
  auto space = bed.open();
  interrupt = false;
  bed.found(*space);
  interrupt = true;
  EXPECT_THROW(space->mutate_delta(neutral_mutation([](NeutralEditing&) {}), 1), HeadsNotReady);
  EXPECT_THROW(space->mutate_delta(neutral_mutation([](NeutralEditing&) {}), 0), HeadsNotReady);
}

// C6. An identity writes its own clock entry beside the dot (mgr:1746-1749); a repeat
// returns with no commit; a zero sequence is refused (mgr:1643-1644).
TEST(HeadsCommit, AnExternalIdentityIsCarriedAndACoveredOneReturnsAtOnce) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const ExternalIdentity identity{author(77), 5};
  const auto first = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.set_note("a", "b"); }), Space::default_retries, identity);
  EXPECT_EQ(first.kind, CommitOutcome::Kind::committed);
  const auto clock = head_of(*space->current()).clock;
  EXPECT_EQ(clock.at(author(77)), 5U);
  EXPECT_EQ(clock.at(first.dot->author), first.dot->sequence);

  int edits = 0;
  const auto mutation = neutral_mutation([&](NeutralEditing&) { ++edits; });
  for (uint64_t sequence : {uint64_t{5}, uint64_t{4}}) {
    const auto repeat = space->mutate_delta(mutation, Space::default_retries, ExternalIdentity{author(77), sequence});
    EXPECT_EQ(repeat.kind, CommitOutcome::Kind::covered);
    EXPECT_FALSE(repeat.dot);
    EXPECT_EQ(repeat.record.hash, first.record.hash);
  }
  EXPECT_EQ(edits, 0);
  EXPECT_EQ(space->replica().committed().generation, first.record.generation);

  const auto later = space->mutate_delta(mutation, Space::default_retries, ExternalIdentity{author(77), 6});
  EXPECT_EQ(later.kind, CommitOutcome::Kind::committed);
  EXPECT_EQ(edits, 1);
  EXPECT_EQ(head_of(*space->current()).clock.at(author(77)), 6U);

  EXPECT_THROW(space->mutate_delta(mutation, Space::default_retries, ExternalIdentity{author(77), 0}), std::invalid_argument);
}

// C36, R1. The dot is in the clock before the comparison and the clock is in the payload:
// a mutation that changes nothing is a new generation. Macha has no case for it.
TEST(HeadsCommit, AnUnchangedMutationCommitsItsClock) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto before = space->replica().committed();
  const auto outcome = space->mutate_delta(neutral_mutation([](NeutralEditing&) {}));
  EXPECT_EQ(outcome.kind, CommitOutcome::Kind::committed);
  EXPECT_EQ(outcome.record.generation, before.generation + 1);
  EXPECT_NE(outcome.record.hash, before.hash);
  EXPECT_NE(outcome.record.payload, before.payload);
  EXPECT_TRUE(clock_covers(head_of(*space->current()).clock, dot_of(outcome)));
}

// The branch R1 makes unreachable is kept: an owner whose clock stays out of its payload
// reaches it, and nothing is committed or owed.
TEST(HeadsCommit, ThePayloadComparisonStandsForAnOwnerWithNoClockInIt) {
  class NoClock final : public Mutation {
   public:
    explicit NoClock(const State& parent) : _state(parent) {}
    void edit(const MutationContext&) override {}
    void stamp(const MutationContext&) override {}
    void advance_clock(const MutationContext&) override {}
    void apply(const MutationContext&) override {}
    State state() const override { return _state; }
    Bytes delta() const override { return {}; }
    Introduced introduced(const MutationContext&) const override { return {}; }

   private:
    State _state;
  };
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto before = space->replica().committed();
  const auto outcome = space->mutate_delta([](const State& parent) { return std::make_unique<NoClock>(parent); });
  EXPECT_EQ(outcome.kind, CommitOutcome::Kind::unchanged);
  EXPECT_EQ(outcome.record.hash, before.hash);
  EXPECT_EQ(space->replica().committed().hash, before.hash);
  EXPECT_EQ(space->owed_commits(), 0U);
}

// H14: with several heads a mutation extends this node's own, and waits for no merge. R4:
// on a tie the lower hash, where the preferred head takes the higher.
TEST(HeadsCommit, AMutationExtendsItsOwnHeadAmongSeveral) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto base = space->replica().committed();
  const auto mine = space->replica().author_ids(bed.self, {}).front();

  auto own = *NeutralCodec::as_head(space->converged().state);
  own.clock[mine] = 1;
  auto theirs = *NeutralCodec::as_head(space->converged().state);
  theirs.clock[author(2)] = 9;
  const auto own_record = record_after(base, neutral_payload(own));
  const auto their_record = record_after(base, neutral_payload(theirs));
  commit_record(space->replica(), own_record);
  commit_record(space->replica(), their_record);
  ASSERT_EQ(space->replica().accepted_heads().size(), 2U);

  const auto outcome = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.set_note("on", "mine"); }));
  EXPECT_EQ(outcome.record.previous, own_record.hash);
  EXPECT_EQ(space->replica().accepted_heads().size(), 2U);
  EXPECT_EQ(space->local().hash, outcome.record.hash);
}

TEST(HeadsCommit, OfTwoHeadsWithNothingOfItsOwnTheLowerHashIsPicked) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto base = space->replica().committed();
  auto left = *NeutralCodec::as_head(space->converged().state);
  left.clock[author(2)] = 1;
  auto right = *NeutralCodec::as_head(space->converged().state);
  right.clock[author(3)] = 1;
  const auto left_record = record_after(base, neutral_payload(left));
  const auto right_record = record_after(base, neutral_payload(right));
  commit_record(space->replica(), left_record);
  commit_record(space->replica(), right_record);
  const auto lower = std::min(left_record.hash, right_record.hash);
  const auto higher = std::max(left_record.hash, right_record.hash);
  EXPECT_EQ(space->replica().committed().hash, higher);
  const auto outcome = space->mutate_delta(neutral_mutation([](NeutralEditing&) {}));
  EXPECT_EQ(outcome.record.previous, lower);
}

// A head's merge parents stay behind: the commit clears them and its delta says so
// (mgr:1677, :1701, :1830-1831), so it stays a delta.
TEST(HeadsCommit, ACommitOnAMergeClearsItsParentsAndStaysADelta) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto base = space->replica().committed();
  auto merged = *NeutralCodec::as_head(space->converged().state);
  Hash256 other{};
  other.bytes[0] = 7;
  merged.merge_parents = {other};
  commit_record(space->replica(), record_after(base, neutral_payload(merged)));
  const auto outcome = space->mutate_delta(neutral_mutation([](NeutralEditing&) {}));
  EXPECT_EQ(outcome.body, HistoryEntry::Body::delta);
  EXPECT_TRUE(head_of(*space->current()).merge_parents.empty());
}

// H14, mgr:1713-1718: a mutation may not move what only a policy commit may. Nothing is
// written, nothing claimed, nothing committed, and the dot stays spent: it was durable
// before the owner saw it.
TEST(HeadsCommit, AMutationThatChangesPolicyIsRefused) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto before = space->replica().committed();
  bed.costs.tree_nodes_written = 0;
  Dot spent;
  const auto refused = neutral_mutation([&](NeutralEditing& editing) {
    spent = editing.context.dot;
    editing.delta.upsert_keys["z"] = make_group(1);
    editing.head.notes.set("policy/copies", "3");
  });
  try {
    space->mutate_delta(refused);
    ADD_FAILURE() << "not refused";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("change cluster policy"), std::string::npos) << error.what();
  }
  EXPECT_EQ(bed.costs.tree_nodes_written.load(), 0U);
  EXPECT_TRUE(bed.barrier.taken().empty());
  EXPECT_EQ(space->replica().committed().hash, before.hash);
  EXPECT_EQ(space->owed_commits(), 0U);
  const auto next = space->mutate_delta(neutral_mutation([](NeutralEditing&) {}));
  EXPECT_EQ(next.dot->sequence, spent.sequence + 1);
}

// Item 6 of the plan's settled list: mgr:1699-1700.
TEST(HeadsCommit, AHeadWithNoTreeIsALogicError) {
  SpaceBed bed;
  auto space = bed.open();
  ASSERT_EQ(space->replica().committed().generation, 1U);
  EXPECT_THROW(space->mutate_delta(neutral_mutation([](NeutralEditing&) {})), std::logic_error);
  EXPECT_EQ(space->replica().committed().generation, 1U);
}

// The seam session S5 fills: a joiner, holding no head and no exchange, is not ready.
TEST(HeadsCommit, ANodeWithNoHeadAndNoExchangeIsNotReady) {
  SpaceBed bed;
  auto space = bed.open("joiner", false);
  EXPECT_TRUE(space->usable_heads().empty());
  EXPECT_THROW(space->mutate_delta(neutral_mutation([](NeutralEditing&) {})), HeadsNotReady);
  EXPECT_THROW(space->converged(), HeadsNotReady);
  EXPECT_FALSE(space->current());
}

// H22 on one node, the owner's half of retain_metadata_publication. What this node holds
// is claimed here. What it does not and the commit brings in is looked for on the nodes
// present. What the parent already named is not looked for, as its holder may be away.
TEST(HeadsCommit, ADataClaimIsHereOrOnANodePresentAndCarriedObjectsAreNotSought) {
  const auto held = test::fake_object(1);
  const auto present = test::fake_object(2);
  const auto carried = test::fake_object(3);
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space, {{"a", make_group(1)}, {"carried", item_of({carried})}});
  bed.barrier.here = {held};
  bed.barrier.elsewhere = {present};
  const auto outcome = space->mutate_delta(neutral_mutation([&](NeutralEditing& editing) {
    editing.delta.upsert_keys["new"] = item_of({held, present});
    // Rewriting the item keeps its object, whose holder is away: not sought, so it does
    // not refuse, though nobody present holds it.
    editing.delta.upsert_keys["carried"] = item_of({carried});
  }));
  EXPECT_EQ(outcome.kind, CommitOutcome::Kind::committed);
  const auto claims = bed.barrier.taken();
  ASSERT_GE(claims.size(), 3U);
  EXPECT_EQ(claims[0].kind, "data-here");
  auto wanted = std::vector<ObjectId>{held, present, carried};
  std::sort(wanted.begin(), wanted.end());
  EXPECT_EQ(claims[0].ids, wanted);
  EXPECT_EQ(claims[1].kind, "data");
  EXPECT_EQ(claims[1].ids, std::vector<ObjectId>{present});
  EXPECT_EQ(claims[2].kind, "control");
  EXPECT_EQ(outcome.claims.data, wanted);
  for (const auto& claim : claims) EXPECT_EQ(claim.dot, dot_of(outcome));
}

TEST(HeadsCommit, AnObjectNobodyPresentHoldsRefusesTheCommitAndSaysWhich) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto before = space->replica().committed();
  try {
    space->mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["phantom"] = item_of({test::fake_object(5)}); }));
    ADD_FAILURE() << "not refused";
  } catch (const HeadsNotReady& error) {
    EXPECT_EQ(std::string(error.what()), Space::data_unheld_refusal);
  }
  EXPECT_EQ(space->replica().committed().hash, before.hash);
  EXPECT_EQ(space->replica().history_entry(before.hash)->hash, before.hash);
  EXPECT_EQ(space->owed_commits(), 0U);
  EXPECT_TRUE(bed.sink->saw("data_objects=1"));
  EXPECT_TRUE(bed.sink->saw("outcome=data-unheld"));
}

TEST(HeadsCommit, ACommitWhoseControlClaimDoesNotHoldIsRefused) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto before = space->replica().committed();
  bed.barrier.control_holds = false;
  try {
    space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.delta.upsert_keys["z"] = make_group(1); }));
    ADD_FAILURE() << "not refused";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string(error.what()), Space::control_unclaimed_refusal);
  }
  EXPECT_EQ(space->replica().committed().hash, before.hash);
  EXPECT_TRUE(bed.sink->saw("outcome=control-claim-failed"));
}

// test_a_commit_claims_the_nodes_it_introduced_and_prunes_the_rest (NT:1191), assertion
// for assertion over the neutral tree, then the same claim from a commit through a space.
TEST(HeadsCommit, ACommitClaimsTheNodesItIntroducedAndPrunesTheRest) {
  const auto tree = neutral_tree();
  const auto entries = test::shaped_entries(60, 20);
  MemoryTreeNodeStore store;
  const auto before = tree.build(entries, store);
  const auto before_shape = tree.stats(before, store);

  // Macha changes one episode's mtime. An item with a few inline references here.
  const std::string key = "a/all/group 4/part 2/item 1";
  auto changed = entries.at(key);
  ASSERT_LT(changed.refs.size(), 8U);
  changed.stamp += 999;
  store.forget_written();
  const auto after = tree.update(before, store, {{key, changed}});
  auto written = store.written();
  std::sort(written.begin(), written.end());
  ASSERT_FALSE(written.empty());

  std::vector<ObjectId> claimed;
  tree.collect_changes(before, after, store, claimed);
  std::sort(claimed.begin(), claimed.end());
  claimed.erase(std::unique(claimed.begin(), claimed.end()), claimed.end());

  // Every node the commit wrote is claimed.
  EXPECT_TRUE(std::includes(claimed.begin(), claimed.end(), written.begin(), written.end()));
  // The walk pruned: far fewer nodes than the tree holds.
  const auto total = before_shape.leaves + before_shape.branches + before_shape.child_nodes;
  EXPECT_LT(claimed.size() * 4, total);
  // A change that rewrote no child sequence claims no child sequence.
  EXPECT_EQ(claimed, written);

  // An item given new references has its new list claimed, and no other's.
  {
    const auto regrown = make_item(424242, 600);
    store.forget_written();
    const auto grown = tree.update(after, store, {{key, regrown}});
    auto wrote = store.written();
    std::sort(wrote.begin(), wrote.end());
    std::vector<ObjectId> introduced;
    tree.collect_changes(after, grown, store, introduced);
    std::sort(introduced.begin(), introduced.end());
    introduced.erase(std::unique(introduced.begin(), introduced.end()), introduced.end());
    EXPECT_EQ(introduced, wrote);
    const auto shape = tree.stats(grown, store);
    EXPECT_LT(introduced.size() * 4, shape.leaves + shape.branches + shape.child_nodes);
  }

  // With no `before`, everything is new.
  std::vector<ObjectId> everything;
  tree.collect_changes(std::nullopt, after, store, everything);
  std::sort(everything.begin(), everything.end());
  everything.erase(std::unique(everything.begin(), everything.end()), everything.end());
  std::vector<ObjectId> all;
  tree.collect_nodes(after, store, all);
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  EXPECT_EQ(everything, all);

  // An unreadable node fails the walk rather than returning a short list.
  MemoryTreeNodeStore damaged;
  for (const auto& id : store.written())
    if (auto bytes = store.get(id)) damaged.put_at(id, *bytes);
  std::vector<ObjectId> partial;
  EXPECT_ANY_THROW(tree.collect_nodes(after, damaged, partial));

  // A commit's control claim is exactly what collect_changes says of it.
  SpaceBed bed;
  auto space = bed.open();
  const auto small = test::shaped_entries(8, 8);
  const auto founded = bed.found(*space, small);
  const auto outcome = space->mutate_delta(neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys[key] = make_group(1); }));
  const auto reader = bed.store.factory()();
  std::vector<ObjectId> expected;
  tree.collect_changes(founded.root, *head_of(*space->current()).root, *reader, expected);
  std::sort(expected.begin(), expected.end());
  expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
  EXPECT_EQ(outcome.claims.control, expected);
  EXPECT_FALSE(outcome.claims.control.empty());
}

// Mutations go one at a time, and a commit in progress holds no reader. What was
// committed is a chain, nothing lost. Macha's group commit
// (test_operations_waiting_on_a_commit_share_the_next_one, NS:497) is FileSystem's above
// this, and stays Macha's.
TEST(HeadsCommit, MutationsGoOneAtATimeAndACommitHoldsNoReader) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  (void)space->mutate_delta(neutral_mutation([](NeutralEditing&) {}));
  const auto before = space->local();

  dannan::test::Gate held;
  dannan::test::GateOpener opener(held);
  std::thread holder([&] { (void)space->mutate_delta(neutral_mutation([&](NeutralEditing&) { held.enter_and_wait(); })); });
  ASSERT_TRUE(held.wait_for_entries(1));
  // Answered at once, on this thread, with the commit held in its edit.
  EXPECT_EQ(space->current()->hash, before.hash);
  EXPECT_EQ(space->local().hash, before.hash);
  EXPECT_EQ(space->heads().preferred.hash, before.hash);

  std::atomic_int done{0};
  std::vector<std::thread> writers;
  for (int i = 0; i < 4; ++i)
    writers.emplace_back([&, i] {
      (void)space->mutate_delta(neutral_mutation([i](NeutralEditing& editing) { editing.set_note("w" + std::to_string(i), "x"); }));
      ++done;
    });
  EXPECT_EQ(done.load(), 0);
  held.open();
  holder.join();
  for (auto& writer : writers) writer.join();
  EXPECT_EQ(done.load(), 4);
  EXPECT_EQ(space->replica().committed().generation, before.generation + 5);
  EXPECT_EQ(space->replica().accepted_heads().size(), 1U);
  const auto notes = head_of(space->converged()).notes.get();
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(notes.contains("w" + std::to_string(i)));
}

// A long walk's stop goes through the mutation path untouched, and nothing is committed.
TEST(HeadsCommit, AStopInAWalkPassesThroughAndCommitsNothing) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto before = space->replica().committed();
  const TreePause stop = [] { throw contract::MaintenanceStopping(); };
  EXPECT_THROW(space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.delta.upsert_keys["z"] = make_group(1); }), Space::default_retries,
                                   std::nullopt, stop),
               contract::MaintenanceStopping);
  EXPECT_EQ(space->replica().committed().hash, before.hash);
  EXPECT_TRUE(bed.barrier.taken().empty());
}

// mgr:93-109: claims owed in order, the newest head alone, the oldest dropped past 32 MiB.
// The replicator that takes them is S5's; test_claims_owed_to_slow_peers is too.
TEST(HeadsCommit, WhatACommitOwesThePeersIsItsClaimsInOrderAndTheNewestHead) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  bed.barrier.here = {test::fake_object(1), test::fake_object(2)};
  EXPECT_EQ(space->owed_commits(), 0U);
  const auto first = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.delta.upsert_keys["x"] = item_of({test::fake_object(1)}); }));
  const auto second = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.delta.upsert_keys["y"] = item_of({test::fake_object(2)}); }));
  const auto owed = space->take_owed();
  ASSERT_EQ(owed.claims.size(), 2U);
  EXPECT_EQ(owed.claims[0].dot, dot_of(first));
  EXPECT_EQ(owed.claims[1].dot, dot_of(second));
  EXPECT_EQ(owed.claims[0].claims.data, first.claims.data);
  ASSERT_TRUE(owed.head);
  EXPECT_EQ(owed.head->hash, second.record.hash);
  EXPECT_EQ(space->owed_commits(), 0U);
}

TEST(HeadsCommit, OwedClaimsPastTheirBoundDropTheOldest) {
  class Many final : public Mutation {
   public:
    Many(const State& parent, size_t count) : _inner(parent, nullptr), _count(count) {}
    void edit(const MutationContext& context) override { _inner.edit(context); }
    void stamp(const MutationContext& context) override { _inner.stamp(context); }
    void advance_clock(const MutationContext& context) override { _inner.advance_clock(context); }
    void apply(const MutationContext& context) override { _inner.apply(context); }
    State state() const override { return _inner.state(); }
    Bytes delta() const override { return _inner.delta(); }
    Introduced introduced(const MutationContext&) const override {
      Introduced out;
      out.data.resize(_count);
      for (size_t i = 0; i < _count; ++i) {
        out.data[i].bytes[0] = static_cast<uint8_t>(i);
        out.data[i].bytes[1] = static_cast<uint8_t>(i >> 8);
        out.data[i].bytes[2] = static_cast<uint8_t>(i >> 16);
      }
      return out;
    }

   private:
    test::NeutralMutation _inner;
    size_t _count;
  };
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  // Each commit names 16 MiB of ids, so a third goes past the bound of 32 MiB.
  const size_t count = 16 * 1024 * 1024 / sizeof(ObjectId);
  const MutationFactory many = [count](const State& parent) { return std::make_unique<Many>(parent, count); };
  for (int i = 0; i < 2; ++i) (void)space->mutate_delta(many);
  EXPECT_EQ(space->owed_dropped(), 0U);
  (void)space->mutate_delta(many);
  EXPECT_EQ(space->owed_dropped(), 1U);
  EXPECT_EQ(space->take_owed().claims.size(), 2U);
}

// C9. `replicas: N` at open refused, naming L2. Every-node opens.
TEST(HeadsSpace, AReplicasPolicyIsRefusedWithAReasonUntilHoldersExist) {
  const auto why = policy_refusal(Policy::replicas_of(3));
  ASSERT_TRUE(why);
  EXPECT_NE(why->find("L2"), std::string::npos) << *why;
  EXPECT_NE(why->find("3"), std::string::npos) << *why;
  EXPECT_FALSE(policy_refusal(Policy::every_node()));

  SpaceBed bed;
  bed.spec.policy = Policy::replicas_of(3);
  try {
    (void)bed.open();
    ADD_FAILURE() << "not refused";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("L2"), std::string::npos) << error.what();
  }
  EXPECT_FALSE(std::filesystem::exists(bed.bed.path() / "history.log"));

  bed.spec.policy = Policy::every_node();
  auto space = bed.open();
  EXPECT_EQ(space->policy().kind, Policy::Kind::every_node);
}

// C13, H21. current() declares no wait and reads memory: with every counter watched and
// under a wait guard that throws, on work allowed nothing. local() at an unchanged head
// revision reads nothing either; a head change is the first read to do any.
TEST(HeadsSpace, TheCurrentViewReadsNothing) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  EXPECT_FALSE(space->current());
  (void)space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.set_note("k", "v"); }));
  const WorkContext nothing_allowed(FrameType::control, {}, nullptr, "test", Waits::none);
  const auto total = [&] {
    const auto diag = space->replica().diagnostics();
    return bed.costs.tree_nodes_read.load() + bed.costs.tree_nodes_written.load() + bed.costs.record_bytes_encoded.load() +
           bed.costs.record_bytes_hashed.load() + bed.costs.full_decodes.load() + bed.costs.history_frames_written.load() + bed.costs.syncs.load() +
           diag.historical_requests + diag.materialization_cache_hits + diag.materialization_cache_misses;
  };
  auto before = total();
  for (int i = 0; i < 5; ++i) {
    const auto view = space->current(nothing_allowed);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->generation, 3U);
    EXPECT_EQ(space->current_generation(), 3U);
  }
  EXPECT_EQ(total(), before);
  EXPECT_EQ(bed.guard.violations(), 0U);

  // The first local() takes the view; at the same revision it reads nothing again.
  (void)space->local();
  before = total();
  for (int i = 0; i < 5; ++i) (void)space->local();
  EXPECT_EQ(total(), before);

  // A head that arrived by another path moves the revision: current() still reads
  // nothing and stays where it was; local() is the first to do any work.
  const auto head = space->replica().committed();
  auto next = *NeutralCodec::as_head(space->converged().state);
  next.clock[author(9)] = 1;
  const auto arrived = record_after(head, neutral_payload(next));
  commit_record(space->replica(), arrived);
  before = total();
  EXPECT_EQ(space->current()->hash, head.hash);
  EXPECT_EQ(total(), before);
  EXPECT_EQ(space->local().hash, arrived.hash);
  EXPECT_GT(total(), before);
  EXPECT_EQ(space->current()->hash, arrived.hash);
}

// The declared waits are the runtime check: work that may not wait is refused. Macha's
// snapshot_view(WorkContext) is the pattern.
TEST(HeadsSpace, TheWaitGuardRefusesWorkThatMayNotWait) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const WorkContext nothing_allowed(FrameType::control, {}, nullptr, "test", Waits::none);
  EXPECT_NO_THROW(space->current(nothing_allowed));
  EXPECT_THROW((void)space->local(nothing_allowed), std::logic_error);
  EXPECT_THROW((void)space->converged(nothing_allowed), std::logic_error);
  EXPECT_THROW((void)space->mutate_delta(nothing_allowed, neutral_mutation([](NeutralEditing&) {})), std::logic_error);
  // Control work may wait locally and never on the network.
  const WorkContext control(FrameType::control);
  EXPECT_NO_THROW((void)space->local(control));
  EXPECT_THROW((void)space->converged(control), std::logic_error);
  const WorkContext foreground(FrameType::foreground);
  EXPECT_NO_THROW((void)space->converged(foreground));
  EXPECT_NO_THROW((void)space->mutate_delta(foreground, neutral_mutation([](NeutralEditing&) {})));
  EXPECT_GE(bed.guard.violations(), 4U);
}

// The witness moves when the tree does, not with every head (mgr:299-310).
TEST(HeadsSpace, TheRevisionMovesWhenTheTreeDoes) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto first = space->local();
  const auto noted = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.set_note("k", "v"); }));
  EXPECT_EQ(space->local().generation, noted.record.generation);
  EXPECT_EQ(space->local().revision, first.revision);
  (void)space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.delta.upsert_keys["z"] = make_group(1); }));
  EXPECT_EQ(space->local().revision, first.revision + 1);
  EXPECT_EQ(space->current_revision(), first.revision + 1);
}

// A view says whether it holds a commit (the maintainer, 2026-11-19, after a file committed short in
// Macha's cutover rehearsal). The shared view moves forward by generation, which is not
// causal: with a peer's head ahead of this node's own, a commit made here extends this
// node's own head and the view stays on the peer's, without the commit, until a fold
// carries both. An owner that compares a view with what it wrote asks covers() first: a
// view that does not cover the commit lags it, and says nothing about the commit's fate.
TEST(HeadsSpace, AViewSaysWhetherItHoldsACommit) {
  SpaceBed bed;
  Record selected;
  bed.converge = [&] { return selected; };
  auto space = bed.open();
  bed.found(*space);
  const auto founded = space->replica().committed();
  const auto base = *NeutralCodec::as_head(space->local().state);

  const auto first = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.set_note("mine", "1"); }));
  ASSERT_TRUE(space->current());
  EXPECT_TRUE(space->covers(*space->current(), dot_of(first)));
  // No dot is no commit: nothing covers it.
  EXPECT_FALSE(space->covers(*space->current(), Dot{}));

  // A peer's branch from the founded head, three commits long, arrives and is what the
  // exchange selects: the view moves to it, and it holds nothing this node wrote.
  auto theirs = base;
  Record peer = founded;
  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    theirs.clock[author(9)] = sequence;
    peer = record_after(peer, neutral_payload(theirs));
    commit_record(space->replica(), peer);
  }
  selected = peer;
  EXPECT_EQ(space->converged().hash, peer.hash);
  EXPECT_FALSE(space->covers(*space->current(), dot_of(first)));

  // A commit here extends this node's own head, a generation behind the peer's: the view
  // stays where it was and does not hold the commit. This node's own head does.
  const auto second = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.set_note("mine", "2"); }));
  EXPECT_EQ(second.record.previous, first.record.hash);
  ASSERT_LT(second.record.generation, peer.generation);
  EXPECT_EQ(space->current()->hash, peer.hash);
  EXPECT_FALSE(space->covers(*space->current(), dot_of(second)));
  const auto own = space->local();
  EXPECT_EQ(own.hash, second.record.hash);
  EXPECT_TRUE(space->covers(own, dot_of(first)));
  EXPECT_TRUE(space->covers(own, dot_of(second)));

  // A fold of the two carries both clocks; the view that reaches it covers the commit.
  auto folded = head_of(own);
  folded.clock[author(9)] = 3;
  const auto fold = record_after(peer, neutral_payload(folded));
  commit_record(space->replica(), fold);
  selected = fold;
  const auto after = space->converged();
  EXPECT_EQ(after.hash, fold.hash);
  EXPECT_TRUE(space->covers(after, dot_of(first)));
  EXPECT_TRUE(space->covers(after, dot_of(second)));
}

// test_the_metadata_view_pages_a_live_namespace (NE:187) stays Macha's over its noun;
// this is what S4 supplies it: a live space's pages equal the walk over the same view.
TEST(HeadsSpace, AViewPagesItsTreeLikeTheWalk) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  for (int i = 0; i < 12; ++i)
    (void)space->mutate_delta(
        neutral_mutation([i](NeutralEditing& editing) { editing.delta.upsert_keys["f" + std::to_string(i)] = make_group(static_cast<uint64_t>(i)); }));
  const auto view = space->converged();
  const auto tree = neutral_tree();
  const auto reader = bed.store.factory()();
  std::vector<std::pair<std::string, NeutralItem>> expected;
  tree.walk(*head_of(view).root, *reader, [&](const std::string& key, const NeutralItem& item) { expected.emplace_back(key, item); });
  EXPECT_GE(expected.size(), 12U);
  std::vector<std::pair<std::string, NeutralItem>> paged;
  dannan::sched::Cursor<std::string> cursor;
  for (int pages = 0; pages < 100; ++pages) {
    dannan::sched::Budget budget;
    budget.operations(5);
    auto page = space->entries(tree, view, cursor, budget);
    for (auto& item : page.items) paged.push_back(std::move(item));
    if (page.complete()) break;
    cursor = page.next;
  }
  EXPECT_EQ(paged, expected);

  // A view with no tree has nothing in it.
  SpaceBed empty;
  auto genesis = empty.open();
  dannan::sched::Budget budget;
  const auto page = genesis->entries(tree, genesis->converged(), {}, budget);
  EXPECT_TRUE(page.items.empty());
  EXPECT_TRUE(page.complete());
}

// R6: a read path can write. A set-aside is cleared on a membership change, and only
// then; R8: a set-aside is forgotten at restart.
TEST(HeadsSpace, AReadClearsASetAsideOnceTheMembershipMoves) {
  SpaceBed bed;
  uint64_t membership = 1;
  bed.membership = [&] { return membership; };
  auto space = bed.open();
  bed.found(*space);
  const auto base = space->replica().committed();
  auto other = *NeutralCodec::as_head(space->converged().state);
  other.clock[author(2)] = 1;
  const auto left = record_after(base, neutral_payload(other));
  auto mine = *NeutralCodec::as_head(space->converged().state);
  mine.clock[author(4)] = 1;
  const auto right = record_after(base, neutral_payload(mine));
  commit_record(space->replica(), left);
  commit_record(space->replica(), right);
  EXPECT_EQ(space->usable_heads().size(), 2U);
  EXPECT_TRUE(space->set_aside(left, "cannot be merged now"));
  EXPECT_EQ(space->usable_heads().size(), 1U);
  EXPECT_NE(space->replica().set_aside_generation(), 0U);
  EXPECT_TRUE(bed.sink->saw("metadata head set aside until membership changes"));
  // The only head left is not set aside.
  EXPECT_FALSE(space->set_aside(right, "the last"));
  EXPECT_EQ(space->heads().usable.size(), 1U);
  EXPECT_EQ(space->heads().certificates.size(), 2U);
  membership = 2;
  EXPECT_EQ(space->usable_heads().size(), 2U);
  EXPECT_EQ(space->replica().set_aside_generation(), 0U);
}

// A stop from what a read stands on goes through it: it is not a failure to serve from
// this node's own head instead (mgr:1492-1507 catches everything else).
TEST(HeadsSpace, AStopInAReadPassesThroughItsFallback) {
  SpaceBed bed;
  bool stop = false;
  bed.converge = [&]() -> Record {
    if (stop) throw contract::MaintenanceStopping();
    throw HeadsNotReady("no peer answered");
  };
  auto space = bed.open();
  bed.found(*space);
  // Any other failure serves this node's own head.
  EXPECT_EQ(space->converged().generation, 2U);
  (void)space->replica().committed();
  auto next = *NeutralCodec::as_head(space->converged().state);
  next.clock[author(2)] = 1;
  commit_record(space->replica(), record_after(space->replica().committed(), neutral_payload(next)));
  stop = true;
  EXPECT_THROW((void)space->converged(), contract::MaintenanceStopping);
}

// R6: a read may commit a policy (mgr:1483), and it carries no dot (R2: C37 is S5's, with
// the peers' half). A mutation runs it too (mgr:1670). What throws from the owner ends
// what asked (mgr:1319-1320).
TEST(HeadsSpace, TheOwnersReconfigureRunsOnAReadAndOnAMutationWithNoDot) {
  SpaceBed bed;
  std::string wanted;
  bool refuse = false;
  bed.spec.reconfigure = [&](const State& state) -> std::optional<State> {
    if (refuse) throw std::runtime_error("cluster extent size does not match local configuration");
    const auto& head = *NeutralCodec::as_head(state);
    if (wanted.empty() || (head.notes.get().contains("policy/copies") && head.notes.get().at("policy/copies") == wanted)) return std::nullopt;
    auto next = std::make_shared<NeutralHead>(head);
    next->merge_parents.clear();
    next->notes.set("policy/copies", wanted);
    return State(next);
  };
  auto space = bed.open();
  bed.found(*space);
  const auto parent = NeutralCodec::as_head(space->converged().state)->clock;
  const auto before = space->replica().committed().generation;

  // A read served from the view stops before the base, so a fresh view is what shows a
  // read reaching it.
  wanted = "2";
  space = nullptr;
  auto reopened = bed.open();
  const auto seen = reopened->converged();
  EXPECT_EQ(seen.generation, before + 1);
  EXPECT_EQ(head_of(seen).clock, parent);
  EXPECT_EQ(head_of(seen).notes.get().at("policy/copies"), "2");
  EXPECT_TRUE(bed.sink->saw("metadata policy transition committed generation=3"));

  wanted = "3";
  const auto outcome = reopened->mutate_delta(neutral_mutation([](NeutralEditing&) {}));
  EXPECT_EQ(outcome.record.generation, before + 3);
  EXPECT_EQ(head_of(*reopened->current()).notes.get().at("policy/copies"), "3");

  refuse = true;
  EXPECT_THROW(reopened->mutate_delta(neutral_mutation([](NeutralEditing&) {})), std::runtime_error);
  EXPECT_EQ(reopened->replica().committed().generation, before + 3);
}

// R6: a commit clears a recovery marker it has stood on (mgr:1664-1665) and so does a read
// (:1484-1485). A seed gives no certificate, so a recovering node has nothing to commit
// on until the head comes back from a peer: `converge` is that, S5's seam, here a head
// taken from what this node knew.
TEST(HeadsSpace, RecoveryEndsWhenACommitOrAReadStandsOnWhatCameBack) {
  for (const bool by_commit : {true, false}) {
    SpaceBed bed;
    auto space = bed.open();
    bed.found(*space);
    (void)space->mutate_delta(neutral_mutation([](NeutralEditing&) {}));
    const auto committed = space->replica().committed();
    space = nullptr;
    {
      std::ofstream damage(bed.bed.path() / "checkpoint.meta", std::ios::binary | std::ios::trunc);
      damage << "not a checkpoint";
    }
    auto replica = bed.bed.options();
    replica.recovery_seed = committed;
    replica.tree_store = bed.store.factory();
    SpaceServices services;
    services.self = bed.self;
    services.commit_nodes = bed.store.factory();
    services.read_nodes = bed.store.factory();
    auto named = bed.spec;
    named.codec = bed.bed.codec;
    bool found_nothing = true;
    Replica* held = nullptr;
    services.converge = [&]() -> Record {
      if (found_nothing) throw HeadsNotReady("no peer has answered");
      commit_record(*held, committed);
      return committed;
    };
    Space recovered(std::move(named), std::move(replica), std::move(services));
    held = &recovered.replica();
    ASSERT_TRUE(recovered.replica().recovery_required());
    // Nothing came back: the marker stands, and neither a commit nor a read stands on
    // the seed. It was never authority.
    EXPECT_THROW(recovered.mutate_delta(neutral_mutation([](NeutralEditing&) {})), HeadsNotReady);
    EXPECT_THROW(recovered.converged(), HeadsNotReady);
    EXPECT_TRUE(recovered.replica().recovery_required());
    found_nothing = false;
    if (by_commit) {
      const auto outcome = recovered.mutate_delta(neutral_mutation([](NeutralEditing&) {}));
      EXPECT_EQ(outcome.record.previous, committed.hash);
    } else {
      EXPECT_EQ(recovered.converged().hash, committed.hash);
    }
    EXPECT_FALSE(recovered.replica().recovery_required());
  }
}

// H19: replay of what a commit wrote: a reopened space materialises the delta through
// the node store, byte for byte, and its next commit extends it.
TEST(HeadsSpace, WhatACommitWroteReplaysAndTheNextExtendsIt) {
  SpaceBed bed;
  Record head;
  Dot last;
  {
    auto space = bed.open();
    bed.found(*space);
    for (int i = 0; i < 3; ++i) {
      const auto outcome = space->mutate_delta(neutral_mutation([i](NeutralEditing& editing) {
        editing.delta.upsert_keys["k" + std::to_string(i)] = make_group(static_cast<uint64_t>(i));
        editing.set_note("n", std::to_string(i));
      }));
      EXPECT_EQ(outcome.body, HistoryEntry::Body::delta);
      head = outcome.record;
      last = *outcome.dot;
    }
  }
  auto space = bed.open();
  EXPECT_EQ(space->replica().committed().hash, head.hash);
  const auto view = space->local();
  EXPECT_EQ(view.hash, head.hash);
  const auto reader = bed.store.factory()();
  EXPECT_TRUE(neutral_tree().lookup(*head_of(view).root, "k2", *reader));
  EXPECT_EQ(head_of(view).notes.get().at("n"), "2");
  const auto next = space->mutate_delta(neutral_mutation([](NeutralEditing&) {}));
  EXPECT_EQ(next.record.previous, head.hash);
  EXPECT_EQ(next.dot->author, last.author);
  EXPECT_EQ(next.dot->sequence, last.sequence + 1);
}

// H12. A spent dot stays spent across a restart: a commit refused after its reservation
// and a crash after it never reuse its sequence. The durable reservation is what makes
// a clock entry mean "every mutation of that author up to here".
TEST(HeadsCommit, ADotIsDurableBeforeAnythingElseAndSurvivesARefusal) {
  SpaceBed bed;
  uint64_t refused_at = 0;
  {
    auto space = bed.open();
    bed.found(*space);
    try {
      space->mutate_delta(neutral_mutation([&](NeutralEditing& editing) {
        refused_at = editing.context.dot.sequence;
        // What a crash between the reservation and the commit leaves.
        throw std::runtime_error("crash");
      }));
    } catch (const std::runtime_error&) {
    }
  }
  ASSERT_NE(refused_at, 0U);
  auto space = bed.open();
  const auto next = space->mutate_delta(neutral_mutation([](NeutralEditing&) {}));
  EXPECT_GT(next.dot->sequence, refused_at);
}

}  // namespace
}  // namespace dagda::heads
