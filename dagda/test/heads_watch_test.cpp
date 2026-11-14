//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Slice 1's change notice (the three-systems design, D4, "Head spaces: placement, k,
// notices"): a watch is told when this node's committed head moves, posted to the
// watcher's executor, with the head before and after; changed() names the keys whose
// entries differ between two heads, a page at a time, reading only where their trees
// differ. Lugh's "disabled on A, refused on B" is B's notice plus the changed keys.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dagda/heads/space.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/heads_space.h"
#include "dagda/test/fixtures/tree.h"

namespace dagda::heads {
namespace {

using namespace std::chrono_literals;
using test::make_group;
using test::neutral_mutation;
using test::neutral_tree;
using test::NeutralEditing;
using test::NeutralEntries;
using test::SpaceBed;

// Notices as a watcher keeps them.
struct Told {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<HeadMoved> moves;
  void add(const HeadMoved& move) {
    {
      std::lock_guard lock(mutex);
      moves.push_back(move);
    }
    changed.notify_all();
  }
  std::vector<HeadMoved> taken() {
    std::lock_guard lock(mutex);
    return moves;
  }
  bool wait_until(const std::function<bool(const std::vector<HeadMoved>&)>& done, std::chrono::milliseconds within) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, within, [&] { return done(moves); });
  }
};

HeadIdentity identity_of(const Record& record) { return {record.generation, record.hash}; }

std::vector<std::string> all_changed(Space& space, const HeadIdentity& before, const HeadIdentity& after, uint64_t per_page = 1000) {
  std::vector<std::string> out;
  dannan::sched::Cursor<std::string> from;
  for (;;) {
    dannan::sched::Budget budget;
    budget.operations(per_page);
    const auto page = space.changed(neutral_tree(), before, after, from, budget);
    out.insert(out.end(), page.items.begin(), page.items.end());
    if (page.stopped == dannan::sched::Stop::end) return out;
    from = page.next;
  }
}

// The notice goes to the watcher's executor, never inline (the executor rule): nothing
// is told until that executor runs, and then the move from the founded head to the
// commit.
TEST(HeadsWatch, AMoveOfTheHeadIsPostedToTheWatchersExecutorNotCalledInline) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto founded = space->replica().committed_identity();
  boost::asio::io_context io;
  Told told;
  space->watch(io.get_executor(), [&](HeadMoved move) { told.add(move); });

  const auto outcome = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.delta.upsert_keys["b"] = make_group(2); }));
  ASSERT_EQ(outcome.kind, CommitOutcome::Kind::committed);
  EXPECT_TRUE(told.taken().empty());

  io.run();
  const auto moves = told.taken();
  ASSERT_EQ(moves.size(), 1U);
  EXPECT_EQ(moves[0].before, founded);
  EXPECT_EQ(moves[0].after, identity_of(outcome.record));
}

// Each move is told once, in order, the one's after the next's before.
TEST(HeadsWatch, EveryMoveIsToldOnceAndTheyChain) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  boost::asio::io_context io;
  Told told;
  space->watch(io.get_executor(), [&](HeadMoved move) { told.add(move); });
  std::vector<Record> commits;
  for (int i = 0; i < 3; ++i)
    commits.push_back(
        space->mutate_delta(neutral_mutation([i](NeutralEditing& editing) { editing.delta.upsert_keys["k" + std::to_string(i)] = make_group(i); })).record);
  io.run();
  const auto moves = told.taken();
  ASSERT_EQ(moves.size(), 3U);
  for (size_t i = 0; i < moves.size(); ++i) {
    EXPECT_EQ(moves[i].after, identity_of(commits[i]));
    if (i > 0) {
      EXPECT_EQ(moves[i].before, moves[i - 1].after);
    }
  }
}

// An acceptance that moves nothing (one already held) tells nothing.
TEST(HeadsWatch, AnAcceptanceThatMovesNothingTellsNothing) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto outcome = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.delta.upsert_keys["b"] = make_group(2); }));
  boost::asio::io_context io;
  Told told;
  space->watch(io.get_executor(), [&](HeadMoved move) { told.add(move); });
  const auto certificate = space->replica().acceptance(outcome.record.hash);
  ASSERT_TRUE(certificate);
  EXPECT_TRUE(space->accept(*certificate));
  io.run();
  EXPECT_TRUE(told.taken().empty());
}

// A watch withdrawn is told nothing after; another watcher still is.
TEST(HeadsWatch, AWithdrawnWatchIsToldNothingMore) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  boost::asio::io_context io;
  Told gone;
  Told kept;
  const auto token = space->watch(io.get_executor(), [&](HeadMoved move) { gone.add(move); });
  space->watch(io.get_executor(), [&](HeadMoved move) { kept.add(move); });
  space->unwatch(token);
  space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.delta.upsert_keys["b"] = make_group(2); }));
  io.run();
  EXPECT_TRUE(gone.taken().empty());
  EXPECT_EQ(kept.taken().size(), 1U);
}

// changed(): exactly the keys whose entries differ: one changed, one erased, one added;
// the unchanged keys are not named.
TEST(HeadsChanged, NamesTheKeysWhoseEntriesDifferAndNoOthers) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space, NeutralEntries{{"a", make_group(1)}, {"b", make_group(2)}, {"c", make_group(3)}, {"e", make_group(5)}});
  const auto before = space->replica().committed_identity();
  const auto outcome = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) {
    editing.delta.upsert_keys["b"] = make_group(20);
    editing.delta.erase_keys.push_back("c");
    editing.delta.upsert_keys["d"] = make_group(4);
  }));
  EXPECT_EQ(all_changed(*space, before, identity_of(outcome.record)), (std::vector<std::string>{"b", "c", "d"}));
  // And the other way: the same keys.
  EXPECT_EQ(all_changed(*space, identity_of(outcome.record), before), (std::vector<std::string>{"b", "c", "d"}));
}

// Between a head and itself nothing differs, and the page is complete.
TEST(HeadsChanged, AHeadAgainstItselfHasNothingChanged) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto head = space->replica().committed_identity();
  dannan::sched::Budget budget;
  budget.operations(10);
  const auto page = space->changed(neutral_tree(), head, head, {}, budget);
  EXPECT_TRUE(page.items.empty());
  EXPECT_EQ(page.stopped, dannan::sched::Stop::end);
}

// One budget operation per key: a page stops at its budget and the next resumes after
// its cursor, together naming every key once, in key order.
TEST(HeadsChanged, PagesByTheBudgetAndResumesAfterTheCursor) {
  SpaceBed bed;
  auto space = bed.open();
  bed.found(*space);
  const auto before = space->replica().committed_identity();
  const auto outcome = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) {
    for (int i = 0; i < 10; ++i) editing.delta.upsert_keys["k" + std::to_string(i)] = make_group(i);
  }));
  dannan::sched::Budget budget;
  budget.operations(4);
  const auto first = space->changed(neutral_tree(), before, identity_of(outcome.record), {}, budget);
  EXPECT_EQ(first.items, (std::vector<std::string>{"k0", "k1", "k2", "k3"}));
  EXPECT_EQ(first.stopped, dannan::sched::Stop::budget);
  EXPECT_EQ(all_changed(*space, before, identity_of(outcome.record), 4),
            (std::vector<std::string>{"k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7", "k8", "k9"}));
}

// The cost follows the change, not the tree: one key changed in a tree of 4,000 reads a
// few nodes on each side, far fewer than the tree holds.
TEST(HeadsChanged, ReadsOnlyWhereTheTreesDiffer) {
  SpaceBed bed;
  auto space = bed.open();
  NeutralEntries entries;
  for (int i = 0; i < 4000; ++i) entries["user/" + std::to_string(10000 + i)] = make_group(i);
  bed.found(*space, entries);
  const auto before = space->replica().committed_identity();
  const auto outcome = space->mutate_delta(neutral_mutation([](NeutralEditing& editing) { editing.delta.upsert_keys["user/12000"] = make_group(-1); }));
  const auto held = space->replica().materialized(before.hash);
  ASSERT_TRUE(held);
  const auto reader = bed.store.factory()();
  const auto stats = neutral_tree().stats(*test::NeutralCodec::as_head(held->state)->root, *reader);
  const auto read_before = bed.costs.tree_nodes_read.load();
  EXPECT_EQ(all_changed(*space, before, identity_of(outcome.record)), std::vector<std::string>{"user/12000"});
  const auto read = bed.costs.tree_nodes_read.load() - read_before;
  EXPECT_LE(read, 4 * (stats.depth + 1));
  EXPECT_LT(read, stats.leaves + stats.branches);
}

// Lugh's case, on the installed fixture's cluster through the Context: a commit on node a
// is noticed on node b with the head it moved to, and b's changed() names the one key.
TEST(HeadsWatchCluster, ACommitOnOneNodeIsNoticedOnItsPeerWithTheKeyItChanged) {
  // Outlive the cluster: b may post to them until it stops.
  Told told;
  test::ClusterIo watcher;
  test::Cluster cluster;
  test::ClusterNodeOptions options;
  options.spaces.push_back(test::founded_spec("lugh/watch"));
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto id = a.api().heads().id_of("lugh/watch");
  // b watches from the founded head: a watch taken while it still stood at genesis would
  // be told, rightly, of the founding's keys too.
  auto* held = b.dagda_node().spaces()->find(id);
  ASSERT_NE(held, nullptr);
  const auto founded_by = std::chrono::steady_clock::now() + 10s;
  while (held->replica().committed_generation() < 2 && std::chrono::steady_clock::now() < founded_by) std::this_thread::sleep_for(10ms);
  ASSERT_GE(held->replica().committed_generation(), 2U);
  const auto watching = b.api().heads().watch(id, watcher.context.get_executor(), [&](HeadMoved move) { told.add(move); });
  ASSERT_TRUE(watching.watching) << watching.why;
  const auto result =
      a.api().heads().mutate_delta(id, neutral_mutation([](NeutralEditing& editing) { editing.delta.upsert_keys["user/7"] = make_group(7); }), 10s);
  ASSERT_TRUE(result.finished) << result.why;
  ASSERT_TRUE(result.outcome);

  // b may pass through no head between; whatever it passes through, its last move
  // reaches a's commit.
  const auto target = identity_of(result.outcome->record);
  ASSERT_TRUE(told.wait_until([&](const std::vector<HeadMoved>& moves) { return !moves.empty() && moves.back().after == target; }, 10s));
  const auto moves = told.taken();

  dannan::sched::Budget budget;
  budget.operations(100);
  const auto changes = b.api().heads().changed(neutral_tree(), id, moves.front().before, moves.back().after, {}, budget);
  ASSERT_TRUE(changes.finished) << changes.why;
  EXPECT_EQ(changes.page.items, std::vector<std::string>{"user/7"});
  EXPECT_EQ(changes.page.stopped, dannan::sched::Stop::end);
}

// Through the Context, a space not open here is evidence, not a throw: no watch, and no
// changes, each saying why.
TEST(HeadsWatchCluster, ASpaceNotOpenHereIsRefusedWithWhy) {
  test::Cluster cluster;
  auto& a = cluster.add("a", test::ClusterNodeOptions{});
  cluster.start_all();
  boost::asio::io_context io;
  const auto id = a.api().heads().id_of("nowhere");
  const auto watching = a.api().heads().watch(id, io.get_executor(), [](HeadMoved) {});
  EXPECT_FALSE(watching.watching);
  EXPECT_FALSE(watching.why.empty());
  dannan::sched::Budget budget;
  const auto changes = a.api().heads().changed(neutral_tree(), id, {}, {}, {}, budget);
  EXPECT_FALSE(changes.finished);
  EXPECT_FALSE(changes.why.empty());
}

}  // namespace
}  // namespace dagda::heads
