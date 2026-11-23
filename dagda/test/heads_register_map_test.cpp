//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The register map (D6), the cases of TODO/2026-11-20-register-map.md in its order. Each
// says what the design requires. The merges are driven by hand over one node store, the
// owner's interfaces called exactly as Space and the reconciler call them; a commit's
// refusal, its claims and its outcome go through a real Space.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/heads/register_map.h"
#include "dagda/heads/space.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/heads_space.h"

namespace dagda::heads {
namespace {

using namespace std::chrono_literals;
using dannan::sched::Budget;
using dannan::sched::Cursor;
using dannan::sched::Stop;
using test::author;
using test::fake_object;

Bytes text(std::string_view value) { return Bytes(value.begin(), value.end()); }

RegisterOptions windowed(std::chrono::milliseconds window) {
  RegisterOptions options;
  options.form.tombstones = TombstonePolicy::within(window);
  return options;
}

RegisterOptions never_reused() {
  RegisterOptions options;
  options.form.tombstones = TombstonePolicy::never();
  options.form.tombstone_wins = true;
  return options;
}

// A node's head as a mutation and a reconciler see it.
struct BenchNode {
  NodeId id{};
  Record record;
  State state;
  SpaceView view() const { return {record.generation, 0, record.hash, state}; }
};

// Nodes over one node store, each commit and fold made by hand: what Space's mutation path
// and the reconciler call of the owner, in their order, and nothing else.
struct Bench {
  dannan::test::SteppedWall wall;
  MemoryTreeNodeStore store;
  RegisterMap map;
  Record founded;
  State founded_state;

  explicit Bench(RegisterOptions options = {}) : map(std::move(options), wall) {
    const auto genesis = genesis_record(map.codec()->genesis());
    founded_state = map.spec("test/registers").founding(store);
    founded = test::make_record(2, genesis.hash, map.codec()->encode_full(founded_state));
  }

  BenchNode node(uint8_t id) const { return {author(id), founded, founded_state}; }

  RegisterWritten commit(BenchNode& node, std::vector<RegisterOp> ops) {
    auto written = std::make_shared<RegisterWritten>();
    const auto fields = map.codec()->head_fields(node.state);
    const auto mine = fields.clock.find(node.id);
    const Dot dot{node.id, mine == fields.clock.end() ? 1 : mine->second + 1};
    const std::optional<ExternalIdentity> identity;
    const TreePause pause;
    const MutationContext context{node.record, node.state, fields, dot, identity, store, store, map.now(), pause};
    auto mutation = map.mutation(std::move(ops), written)(node.state);
    mutation->edit(context);
    mutation->stamp(context);
    mutation->advance_clock(context);
    mutation->apply(context);
    auto next = mutation->state();
    const auto payload = map.codec()->encode_full(next);
    // A peer's replica takes the delta: it must replay to the same record.
    EXPECT_EQ(map.codec()->encode_full(map.codec()->apply_delta(node.state, mutation->delta(), &store)), payload);
    node.record = test::record_after(node.record, payload);
    node.state = std::move(next);
    return *written;
  }

  // What a reconciler of the pair mints, writing through `nodes`: the lower hash is the
  // primary, as MergeHeads takes them.
  Merge::Result fold_on(const BenchNode& a, const BenchNode& b, TreeNodeStore& nodes, Record& record) const {
    const bool a_primary = a.record.hash < b.record.hash;
    const auto& primary = a_primary ? a : b;
    const auto& secondary = a_primary ? b : a;
    const TreePause pause;
    const MergeHeads heads{primary.record, secondary.record, primary.state, secondary.state, nodes, nodes, pause};
    auto result = map.merge()->merge(std::nullopt, heads);
    if (result.state)
      record = test::make_record(std::max(a.record.generation, b.record.generation) + 1, primary.record.hash, map.codec()->encode_full(result.state));
    return result;
  }

  // Both nodes take the fold.
  Merge::Result fold(BenchNode& a, BenchNode& b) {
    Record record;
    auto result = fold_on(a, b, store, record);
    EXPECT_TRUE(result.state) << result.why;
    a.record = b.record = record;
    a.state = b.state = result.state;
    return result;
  }

  std::optional<Register> get(const BenchNode& node, std::string_view key) { return map.get(node.view(), key, store); }
  std::optional<Register> get_register(const BenchNode& node, std::string_view key) { return map.get_register(node.view(), key, store); }
  // What the tree holds, expired or not.
  std::optional<Register> held(const BenchNode& node, std::string_view key) { return map.tree().lookup(*RegisterMap::head_of(node.state).root, key, store); }
  uint64_t leaves(const BenchNode& node) { return map.tree().stats(*RegisterMap::head_of(node.state).root, store).leaves; }
};

// One node's space, founded, its commits through Space.
struct MapBed {
  test::SpaceBed bed;
  RegisterMap map;
  std::unique_ptr<Space> space;

  explicit MapBed(RegisterOptions options = {}) : map(std::move(options), bed.wall) {
    bed.spec = map.spec("test/registers");
    bed.bed.codec = map.codec();
    space = bed.open();
    const auto writer = bed.store.factory()();
    const auto state = bed.spec.founding(*writer);
    test::commit_record(space->replica(), test::make_record(2, bed.bed.genesis().hash, map.codec()->encode_full(state)));
  }

  SpaceView view() { return space->local(); }
  std::optional<Register> get(std::string_view key) {
    const auto reader = bed.store.factory()();
    return map.get(view(), key, *reader);
  }
};

// ---- The merge ----------------------------------------------------------------

// 1. Two writes to one key on two nodes: after the fold, both hold the greater (stamp, node id).
TEST(HeadsRegisterMap, TwoWritesToOneKeyFoldToTheGreaterOnBothNodes) {
  Bench bench;
  auto a = bench.node(1);
  auto b = bench.node(2);
  const auto from_a = bench.commit(a, {RegisterOp::put("k", text("from a"))}).versions.at("k");
  const auto from_b = bench.commit(b, {RegisterOp::put("k", text("from b"))}).versions.at("k");
  const auto greater = std::max(from_a, from_b);
  bench.fold(a, b);
  for (const auto* node : {&a, &b}) {
    const auto held = bench.get(*node, "k");
    ASSERT_TRUE(held);
    EXPECT_EQ(held->version(), greater);
    EXPECT_EQ(held->value, text(greater == from_a ? "from a" : "from b"));
  }
}

// 2. A remove wins against a concurrent older write, and loses to a newer one.
TEST(HeadsRegisterMap, ARemoveWinsAgainstAnOlderWriteAndLosesToANewerOne) {
  {
    Bench bench;
    auto a = bench.node(1);
    auto b = bench.node(2);
    (void)bench.commit(a, {RegisterOp::put("x", text("x"))});
    const auto removed = bench.commit(a, {RegisterOp::remove("k")}).versions.at("k");
    const auto written = bench.commit(b, {RegisterOp::put("k", text("older"))}).versions.at("k");
    ASSERT_LT(written, removed);
    bench.fold(a, b);
    EXPECT_FALSE(bench.get(a, "k"));
    const auto tombstone = bench.get_register(b, "k");
    ASSERT_TRUE(tombstone);
    EXPECT_TRUE(tombstone->tombstone);
  }
  {
    Bench bench;
    auto a = bench.node(1);
    auto b = bench.node(2);
    const auto removed = bench.commit(a, {RegisterOp::remove("k")}).versions.at("k");
    (void)bench.commit(b, {RegisterOp::put("y", text("y"))});
    const auto written = bench.commit(b, {RegisterOp::put("k", text("newer"))}).versions.at("k");
    ASSERT_LT(removed, written);
    bench.fold(a, b);
    for (const auto* node : {&a, &b}) {
      const auto held = bench.get(*node, "k");
      ASSERT_TRUE(held);
      EXPECT_EQ(held->value, text("newer"));
    }
  }
}

// 3. The merge is a pure function of the two heads: both reconcilers of one pair mint the
// same commit, whichever way round they are handed it, and no base is read.
TEST(HeadsRegisterMap, BothReconcilersOfOnePairMintTheSameCommit) {
  Bench bench;
  auto a = bench.node(1);
  auto b = bench.node(2);
  (void)bench.commit(a, {RegisterOp::put("k1", text("a1")), RegisterOp::put("k2", text("a2"), {fake_object(1)})});
  (void)bench.commit(b, {RegisterOp::put("k1", text("b1"))});
  (void)bench.commit(b, {RegisterOp::put("k3", text("b3")), RegisterOp::remove("k2")});
  EXPECT_FALSE(bench.map.merge()->reads_base());

  MemoryTreeNodeStore one = bench.store;
  MemoryTreeNodeStore other = bench.store;
  Record first;
  Record second;
  const auto result = bench.fold_on(a, b, one, first);
  (void)bench.fold_on(b, a, other, second);
  ASSERT_TRUE(result.state) << result.why;
  EXPECT_EQ(first.hash, second.hash);

  // Its delta on the primary replays to the same record.
  const auto& primary = a.record.hash < b.record.hash ? a : b;
  ASSERT_FALSE(result.delta.empty());
  EXPECT_EQ(bench.map.codec()->encode_full(bench.map.codec()->apply_delta(primary.state, result.delta, &one)),
            Bytes(first.payload.begin(), first.payload.end()));
}

// 4. Disjoint keys from two branches are both kept.
TEST(HeadsRegisterMap, DisjointKeysFromTwoBranchesAreBothKept) {
  Bench bench;
  auto a = bench.node(1);
  auto b = bench.node(2);
  (void)bench.commit(a, {RegisterOp::put("a/1", text("one"))});
  (void)bench.commit(b, {RegisterOp::put("b/1", text("two"))});
  bench.fold(a, b);
  EXPECT_EQ(bench.get(a, "a/1")->value, text("one"));
  EXPECT_EQ(bench.get(a, "b/1")->value, text("two"));
}

// The order's tie: equal (stamp, node id) with different bytes cannot come from one node.
// Should it, the merge takes the lexically greater bytes, deterministically, and counts it.
TEST(HeadsRegisterMap, AnEqualOrderWithOtherBytesTakesTheGreaterBytesAndIsAConflict) {
  Bench bench;
  auto a = bench.node(1);
  auto again = bench.node(1);
  const auto one = bench.commit(a, {RegisterOp::put("k", text("aaa"))}).versions.at("k");
  const auto two = bench.commit(again, {RegisterOp::put("k", text("zzz"))}).versions.at("k");
  ASSERT_EQ(one, two);
  const auto result = bench.fold(a, again);
  EXPECT_EQ(result.conflicts_created, 1U);
  EXPECT_EQ(bench.get(a, "k")->value, text("zzz"));
}

// ---- Lamport --------------------------------------------------------------------

// 5. A node's next stamp is above every stamp in its parent's map, so a write after a fold
// beats everything it saw.
TEST(HeadsRegisterMap, AWriteAfterAFoldIsAboveEveryStampItSaw) {
  Bench bench;
  auto a = bench.node(1);
  auto b = bench.node(2);
  for (int i = 0; i < 3; ++i) (void)bench.commit(a, {RegisterOp::put("k1", text("a" + std::to_string(i)))});
  (void)bench.commit(b, {RegisterOp::put("k2", text("b"))});
  bench.fold(a, b);

  uint64_t highest = 0;
  bench.map.tree().walk(*RegisterMap::head_of(b.state).root, bench.store,
                        [&](const std::string&, const Register& value) { highest = std::max(highest, value.stamp); });
  const auto next = bench.commit(b, {RegisterOp::put("k1", text("b after"))}).versions.at("k1");
  EXPECT_GT(next.stamp, highest);
  EXPECT_EQ(bench.get(b, "k1")->value, text("b after"));
}

// ---- Tombstones -------------------------------------------------------------------

// 6. Under a window: a tombstone past the window is dropped by the next mutation, never by a
// merge, and a fold with a branch still holding the old write does not revive it inside it.
TEST(HeadsRegisterMap, AWindowedTombstoneIsDroppedByAMutationAndNeverByAMerge) {
  Bench bench(windowed(1h));
  auto a = bench.node(1);
  (void)bench.commit(a, {RegisterOp::put("k", text("old"))});
  auto b = a;
  b.id = author(2);
  (void)bench.commit(a, {RegisterOp::remove("k")});

  bench.wall.advance(30min);
  (void)bench.commit(a, {RegisterOp::put("other", text("o"))});
  ASSERT_EQ(bench.leaves(a), 1U);
  ASSERT_TRUE(bench.get_register(a, "k"));
  EXPECT_TRUE(bench.get_register(a, "k")->tombstone);

  bench.fold(a, b);
  EXPECT_FALSE(bench.get(a, "k"));
  EXPECT_FALSE(bench.get(b, "k"));

  // Past the window, a merge still keeps it.
  auto c = bench.node(3);
  (void)bench.commit(c, {RegisterOp::put("c", text("c"))});
  bench.wall.advance(2h);
  bench.fold(a, c);
  ASSERT_TRUE(bench.held(a, "k"));
  EXPECT_TRUE(bench.held(a, "k")->tombstone);

  // The next mutation touching its leaf drops it.
  (void)bench.commit(a, {RegisterOp::put("another", text("x"))});
  EXPECT_FALSE(bench.held(a, "k"));
}

// 7. Under "never": a tombstone is kept across any number of mutations, however old.
TEST(HeadsRegisterMap, UnderNeverATombstoneIsKeptAcrossMutations) {
  RegisterOptions options;
  options.form.tombstones = TombstonePolicy::never();
  Bench bench(options);
  auto a = bench.node(1);
  (void)bench.commit(a, {RegisterOp::put("k", text("v"))});
  (void)bench.commit(a, {RegisterOp::remove("k")});
  for (int i = 0; i < 6; ++i) {
    bench.wall.advance(24h * 400);
    (void)bench.commit(a, {RegisterOp::put("n" + std::to_string(i), text("x"))});
  }
  ASSERT_EQ(bench.leaves(a), 1U);
  ASSERT_TRUE(bench.get_register(a, "k"));
  EXPECT_TRUE(bench.get_register(a, "k")->tombstone);
}

// ---- Expiry ---------------------------------------------------------------------

// 8. A key past its expiry reads as absent with no commit of its own, and the next mutation
// touching its leaf removes it.
TEST(HeadsRegisterMap, AnExpiredKeyReadsAbsentAndTheNextMutationOnItsLeafRemovesIt) {
  Bench bench;
  auto a = bench.node(1);
  (void)bench.commit(a, {RegisterOp::put("k", text("brief"), {}, bench.map.now() + 1'000'000'000), RegisterOp::put("near", text("n"))});
  ASSERT_TRUE(bench.get(a, "k"));
  const auto before = a.record.hash;

  bench.wall.advance(2s);
  EXPECT_FALSE(bench.get(a, "k"));
  EXPECT_FALSE(bench.get_register(a, "k"));
  EXPECT_EQ(a.record.hash, before);
  EXPECT_TRUE(bench.held(a, "k"));

  (void)bench.commit(a, {RegisterOp::put("other", text("o"))});
  ASSERT_EQ(bench.leaves(a), 1U);
  EXPECT_FALSE(bench.held(a, "k"));
  EXPECT_TRUE(bench.get(a, "near"));
}

// ---- The conditional write ----------------------------------------------------------

// 9. With `expect` matching, the write commits, and the outcome names the new version.
TEST(HeadsRegisterMap, AMatchingExpectCommits) {
  MapBed bed;
  const auto first = bed.map.commit(*bed.space, {RegisterOp::put("k", text("one"))});
  ASSERT_EQ(first.outcome.kind, CommitOutcome::Kind::committed);
  const auto version = first.written.at("k");
  EXPECT_EQ(version.node, bed.bed.self);
  EXPECT_EQ(bed.get("k")->version(), version);

  const auto second = bed.map.commit(*bed.space, {RegisterOp::put("k", text("two"), {}, {}, RegisterExpect::at(version))});
  EXPECT_EQ(second.outcome.kind, CommitOutcome::Kind::committed);
  EXPECT_EQ(bed.get("k")->value, text("two"));
  EXPECT_EQ(bed.get("k")->version(), second.written.at("k"));
  EXPECT_GT(second.written.at("k"), version);
}

// 10. With `expect` stale, the write is refused with a conflict and nothing commits.
TEST(HeadsRegisterMap, AStaleExpectIsRefusedAndNothingCommits) {
  MapBed bed;
  const auto stale = bed.map.commit(*bed.space, {RegisterOp::put("k", text("one"))}).written.at("k");
  (void)bed.map.commit(*bed.space, {RegisterOp::put("k", text("two"))});
  const auto head = bed.space->replica().committed().hash;
  EXPECT_THROW((void)bed.map.commit(*bed.space, {RegisterOp::put("other", text("o")), RegisterOp::put("k", text("three"), {}, {}, RegisterExpect::at(stale))}),
               RegisterConflict);
  EXPECT_EQ(bed.space->replica().committed().hash, head);
  EXPECT_EQ(bed.get("k")->value, text("two"));
  EXPECT_FALSE(bed.get("other"));
}

// 11. With `expect` absent and the key present, the write is refused.
TEST(HeadsRegisterMap, ExpectingAbsentOfAPresentKeyIsRefused) {
  MapBed bed;
  (void)bed.map.commit(*bed.space, {RegisterOp::put("k", text("one"))});
  const auto head = bed.space->replica().committed().hash;
  try {
    (void)bed.map.commit(*bed.space, {RegisterOp::put("k", text("two"), {}, {}, RegisterExpect::absent())});
    ADD_FAILURE() << "a write expecting absent over a present key commits";
  } catch (const RegisterConflict& conflict) {
    EXPECT_EQ(conflict.key(), "k");
    ASSERT_TRUE(conflict.found());
    EXPECT_EQ(conflict.found()->value, text("one"));
  }
  EXPECT_EQ(bed.space->replica().committed().hash, head);
}

// 12. Two nodes each pass their check at once: after the fold the greater order wins. Stated,
// not prevented: the check is against each node's own view.
TEST(HeadsRegisterMap, TwoNodesPassingTheirChecksAtOnceFoldByOrder) {
  Bench bench;
  auto a = bench.node(1);
  auto b = bench.node(2);
  const auto read = bench.commit(a, {RegisterOp::put("k", text("base"))}).versions.at("k");
  bench.fold(a, b);
  const auto from_a = bench.commit(a, {RegisterOp::put("k", text("a"), {}, {}, RegisterExpect::at(read))}).versions.at("k");
  const auto from_b = bench.commit(b, {RegisterOp::put("k", text("b"), {}, {}, RegisterExpect::at(read))}).versions.at("k");
  bench.fold(a, b);
  const auto greater = std::max(from_a, from_b);
  EXPECT_EQ(bench.get(a, "k")->version(), greater);
  EXPECT_EQ(bench.get(b, "k")->version(), greater);
}

// ---- Reads ----------------------------------------------------------------------

// 13. get of an absent key, a tombstone and an expired key are all absent; a missing tree node
// fails rather than reading as absent.
TEST(HeadsRegisterMap, AbsentTombstonedAndExpiredReadAbsentAndAMissingNodeFails) {
  Bench bench;
  auto a = bench.node(1);
  (void)bench.commit(a, {RegisterOp::put("gone", text("g")), RegisterOp::put("brief", text("b"), {}, bench.map.now() + 1'000'000)});
  (void)bench.commit(a, {RegisterOp::remove("gone")});
  bench.wall.advance(1s);
  EXPECT_FALSE(bench.get(a, "never"));
  EXPECT_FALSE(bench.get(a, "gone"));
  EXPECT_FALSE(bench.get(a, "brief"));

  const MemoryTreeNodeStore empty;
  EXPECT_THROW((void)bench.map.get(a.view(), "never", empty), DecodeError);
  Budget budget;
  EXPECT_THROW((void)bench.map.list(a.view(), "", {}, budget, empty), DecodeError);
}

// 14. list by prefix over pages equals one walk of the same view, and nothing outside the prefix.
TEST(HeadsRegisterMap, AListByPrefixOverPagesEqualsOneWalk) {
  Bench bench;
  auto a = bench.node(1);
  std::vector<RegisterOp> ops;
  for (int i = 0; i < 300; ++i) ops.push_back(RegisterOp::put("p/" + std::to_string(i), text(std::to_string(i))));
  for (int i = 0; i < 20; ++i) ops.push_back(RegisterOp::put("q/" + std::to_string(i), text("q")));
  ops.push_back(RegisterOp::put("o", text("before the prefix")));
  ops.push_back(RegisterOp::put("p/brief", text("b"), {}, bench.map.now() + 1'000'000));
  (void)bench.commit(a, std::move(ops));
  (void)bench.commit(a, {RegisterOp::remove("p/7")});
  bench.wall.advance(1s);

  std::vector<std::pair<std::string, Register>> walked;
  bench.map.tree().for_each_with_prefix(*RegisterMap::head_of(a.state).root, bench.store, "p/", [&](const std::string& key, const Register& value) {
    if (!value.tombstone && !bench.map.expired(value)) walked.emplace_back(key, value);
  });
  ASSERT_EQ(walked.size(), 299U);

  std::vector<std::pair<std::string, Register>> paged;
  Cursor<std::string> cursor;
  size_t pages = 0;
  for (;;) {
    Budget budget;
    budget.operations(7);
    auto page = bench.map.list(a.view(), "p/", cursor, budget, bench.store);
    ++pages;
    for (auto& item : page.items) {
      EXPECT_TRUE(item.first.starts_with("p/"));
      paged.push_back(std::move(item));
    }
    if (page.complete()) break;
    EXPECT_EQ(page.stopped, Stop::budget);
    cursor = page.next;
    ASSERT_LT(pages, 1000U);
  }
  EXPECT_GT(pages, 1U);
  EXPECT_EQ(paged, walked);
}

// ---- The stamp seam ---------------------------------------------------------------

// 15. The stamp kind is recorded in the space's head, and a node opening it with another kind,
// or other options, is refused.
TEST(HeadsRegisterMap, TheOptionsAreRecordedInTheHeadAndAnotherOpeningIsRefused) {
  MapBed bed;
  EXPECT_EQ(RegisterMap::head_of(bed.view()).form, bed.map.options().form);
  EXPECT_EQ(RegisterMap::head_of(bed.view()).form.stamp, StampKind::lamport);
  EXPECT_FALSE(bed.map.refusal(bed.view()));

  // This build issues no HLC stamp: refused at open, with its reason.
  RegisterOptions hlc;
  hlc.form.stamp = StampKind::hlc;
  EXPECT_THROW((void)RegisterMap{hlc}, RegisterRefused);

  // Other options over this head.
  const RegisterMap other(never_reused(), bed.bed.wall);
  ASSERT_TRUE(other.refusal(bed.view()));
  const auto reader = bed.bed.store.factory()();
  EXPECT_THROW((void)other.get(bed.view(), "k", *reader), RegisterRefused);
  EXPECT_THROW((void)other.commit(*bed.space, {RegisterOp::put("k", text("v"))}), RegisterRefused);

  // A head recording another stamp kind.
  auto recorded = std::make_shared<RegisterHead>(RegisterMap::head_of(bed.view()));
  recorded->form.stamp = StampKind::hlc;
  test::commit_record(bed.space->replica(), test::record_after(bed.space->replica().committed(), bed.map.codec()->encode_full(recorded)));
  const auto why = bed.map.refusal(bed.view());
  ASSERT_TRUE(why);
  EXPECT_NE(why->find("stamp=hlc"), std::string::npos);
  EXPECT_THROW((void)bed.map.commit(*bed.space, {RegisterOp::put("k", text("v"))}), RegisterRefused);
}

// ---- The users' additions ------------------------------------------------------------

// 16. Tombstone wins: a remove on A and a newer write on B; after the fold, a tombstone on both.
TEST(HeadsRegisterMap, UnderTombstoneWinsARemoveBeatsANewerWrite) {
  Bench bench(never_reused());
  auto a = bench.node(1);
  auto b = bench.node(2);
  const auto removed = bench.commit(a, {RegisterOp::remove("k")}).versions.at("k");
  (void)bench.commit(b, {RegisterOp::put("x", text("x"))});
  const auto written = bench.commit(b, {RegisterOp::put("k", text("newer"))}).versions.at("k");
  ASSERT_LT(removed, written);
  bench.fold(a, b);
  for (const auto* node : {&a, &b}) {
    EXPECT_FALSE(bench.get(*node, "k"));
    ASSERT_TRUE(bench.get_register(*node, "k"));
    EXPECT_TRUE(bench.get_register(*node, "k")->tombstone);
  }
}

// 17. Under never with tombstone wins a removed key is never written again on this node's view,
// and expecting it absent fails against its tombstone; get_register tells it from never-written.
TEST(HeadsRegisterMap, UnderNeverWithTombstoneWinsARemovedKeyIsNotReused) {
  Bench bench(never_reused());
  auto a = bench.node(1);
  (void)bench.commit(a, {RegisterOp::put("k", text("v"))});
  (void)bench.commit(a, {RegisterOp::remove("k")});
  EXPECT_THROW((void)bench.commit(a, {RegisterOp::put("k", text("again"))}), RegisterConflict);
  EXPECT_THROW((void)bench.commit(a, {RegisterOp::put("k", text("again"), {}, {}, RegisterExpect::absent())}), RegisterConflict);
  EXPECT_FALSE(bench.get(a, "k"));
  ASSERT_TRUE(bench.get_register(a, "k"));
  EXPECT_TRUE(bench.get_register(a, "k")->tombstone);
  EXPECT_FALSE(bench.get_register(a, "never"));

  // Under a window a removed key may be written again, and reads as absent to `expect`.
  Bench windowed_bench;
  auto w = windowed_bench.node(1);
  (void)windowed_bench.commit(w, {RegisterOp::put("k", text("v"))});
  (void)windowed_bench.commit(w, {RegisterOp::remove("k")});
  EXPECT_NO_THROW((void)windowed_bench.commit(w, {RegisterOp::put("k", text("again"), {}, {}, RegisterExpect::absent())}));
  EXPECT_EQ(windowed_bench.get(w, "k")->value, text("again"));
}

// 18. A branch older than the window revives a removed key: a stated limit, as such a node is
// also past Dannan's forget and rejoins as new.
TEST(HeadsRegisterMap, ABranchOlderThanTheWindowRevivesARemovedKey) {
  Bench bench(windowed(1h));
  auto a = bench.node(1);
  (void)bench.commit(a, {RegisterOp::put("k", text("old"))});
  auto away = a;
  away.id = author(2);
  (void)bench.commit(a, {RegisterOp::remove("k")});
  bench.wall.advance(2h);
  (void)bench.commit(a, {RegisterOp::put("other", text("o"))});
  ASSERT_FALSE(bench.held(a, "k"));
  bench.fold(a, away);
  ASSERT_TRUE(bench.get(a, "k"));
  EXPECT_EQ(bench.get(a, "k")->value, text("old"));
}

// 19. The keys a commit or a fold changed are listed with both sides, and nothing else.
TEST(HeadsRegisterMap, ChangedListsWhatACommitOrAFoldChangedWithBothSides) {
  Bench bench;
  auto a = bench.node(1);
  auto b = bench.node(2);
  (void)bench.commit(a, {RegisterOp::put("x", text("x")), RegisterOp::put("y", text("y")), RegisterOp::put("z", text("z"))});
  const auto before = a.view();
  (void)bench.commit(a, {RegisterOp::put("y", text("y2")), RegisterOp::remove("z")});
  const auto after = a.view();

  Budget budget;
  const auto page = bench.map.changed(before, after, "", {}, budget, bench.store);
  ASSERT_TRUE(page.complete());
  ASSERT_EQ(page.items.size(), 2U);
  EXPECT_EQ(page.items[0].key, "y");
  EXPECT_EQ(page.items[0].before->value, text("y"));
  EXPECT_EQ(page.items[0].after->value, text("y2"));
  EXPECT_EQ(page.items[1].key, "z");
  EXPECT_EQ(page.items[1].before->value, text("z"));
  EXPECT_TRUE(page.items[1].after->tombstone);

  // Bounded to the prefix, and paged.
  Budget one;
  one.operations(1);
  const auto first = bench.map.changed(before, after, "", {}, one, bench.store);
  ASSERT_EQ(first.items.size(), 1U);
  EXPECT_EQ(first.stopped, Stop::budget);
  Budget rest;
  const auto second = bench.map.changed(before, after, "", first.next, rest, bench.store);
  ASSERT_EQ(second.items.size(), 1U);
  EXPECT_EQ(second.items[0].key, "z");
  Budget bounded;
  EXPECT_EQ(bench.map.changed(before, after, "z", {}, bounded, bench.store).items.size(), 1U);

  // A fold: only what the other branch brought.
  (void)bench.commit(b, {RegisterOp::put("w", text("w"))});
  const auto unfolded = a.view();
  bench.fold(a, b);
  Budget folded;
  const auto fold_page = bench.map.changed(unfolded, a.view(), "", {}, folded, bench.store);
  ASSERT_EQ(fold_page.items.size(), 1U);
  EXPECT_EQ(fold_page.items[0].key, "w");
  EXPECT_FALSE(fold_page.items[0].before);
  EXPECT_EQ(fold_page.items[0].after->value, text("w"));
}

// 20. An object named by a register, superseded on another node and folded, is no longer named
// by the head, and one still named is. (The collector's half: the head's named objects are what
// the space's reachability answers; the provider is not wired in this draft.)
TEST(HeadsRegisterMap, AnObjectNoRegisterNamesIsNoLongerReachable) {
  Bench bench;
  auto a = bench.node(1);
  const auto superseded = fake_object(1);
  const auto kept = fake_object(2);
  const auto replacement = fake_object(3);
  (void)bench.commit(a, {RegisterOp::put("k", text("v"), {superseded}), RegisterOp::put("kept", text("v"), {kept})});
  auto b = a;
  b.id = author(2);
  (void)bench.commit(b, {RegisterOp::put("k", text("v2"), {replacement})});
  bench.fold(a, b);
  EXPECT_EQ(bench.map.named_objects(a.view(), bench.store), (std::vector<ObjectId>{std::min(kept, replacement), std::max(kept, replacement)}));

  // A commit claims what its registers name under its dot.
  MapBed bed;
  bed.bed.barrier.here.insert(superseded);
  const auto commit = bed.map.commit(*bed.space, {RegisterOp::put("k", text("v"), {superseded})});
  EXPECT_EQ(commit.outcome.claims.data, std::vector<ObjectId>{superseded});
}

// 21. A register naming 4,096 refs round-trips and its leaf stays small; the value and refs
// bounds are refused at put.
TEST(HeadsRegisterMap, FourThousandRefsRoundTripAndTheBoundsAreRefusedAtPut) {
  Bench bench;
  auto a = bench.node(1);
  std::vector<ObjectId> refs;
  for (uint64_t i = 0; i < RegisterMap::refs_max; ++i) refs.push_back(fake_object(1000 + i));
  (void)bench.commit(a, {RegisterOp::put("k", text("many"), refs)});
  const auto held = bench.get(a, "k");
  ASSERT_TRUE(held);
  EXPECT_EQ(held->refs, refs);
  const auto root = *RegisterMap::head_of(a.state).root;
  ASSERT_EQ(bench.leaves(a), 1U);
  const auto leaf = bench.store.get(root);
  ASSERT_TRUE(leaf);
  EXPECT_LT(leaf->size(), 256U);

  EXPECT_NO_THROW((void)RegisterOp::put("k", Bytes(RegisterMap::value_max_bytes, 1)));
  EXPECT_THROW((void)RegisterOp::put("k", Bytes(RegisterMap::value_max_bytes + 1, 1)), std::invalid_argument);
  refs.push_back(fake_object(1));
  EXPECT_THROW((void)RegisterOp::put("k", text("v"), refs), std::invalid_argument);
}

// 22. A commit stamped beyond the bound is refused at acceptance, and the heads of the nodes
// that accepted everything else still fold to the same commit. Waits on Dannan's
// HlcClock::admit in a release (Dannan 0.10.0 has none): the HLC stamp kind is refused at open
// until then, so there is nothing to admit.
TEST(HeadsRegisterMap, DISABLED_ACommitStampedBeyondTheBoundIsRefusedAtAcceptance) { GTEST_SKIP() << "waits on Dannan's HlcClock::admit"; }

}  // namespace
}  // namespace dagda::heads
