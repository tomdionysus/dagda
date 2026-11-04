//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirements L14 and L23: the retention ledger checked against the
// RetentionStore it pages, and the claim walk step by step against a
// reference walk over fakes, for every held pattern and every credit limit
// that reaches a different decision.
//
// Moved from Macha's tests/test_claim_walk.cpp (develop 5a06d3c), every
// assertion kept: test_retention_ledger_pages_every_live_claim_once,
// test_retention_ledger_stops_for_cancellation_first,
// test_retention_ledger_holds_by_class,
// test_retention_ledger_publishes_horizons_and_refuses_an_incomplete_release,
// test_retention_ledger_forwards_claims_releases_prunes_and_compacts,
// test_claim_walk_matches_the_walk_it_replaced_exhaustively,
// test_claim_walk_matches_the_walk_it_replaced_at_the_bound,
// test_claim_walk_follows_a_changing_ledger; and from
// tests/test_baselines.cpp: test_baseline_claim_walk_per_object,
// test_baseline_claim_walk_on_the_ledger_per_object (they print their
// figures and assert only that the walk did its work).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dagda/ledger/claim_walk.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/retention_ledger.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

using contract::InventoryHorizon;
using contract::ReleaseBuild;
using contract::ReleaseHorizon;
using contract::RetentionClass;
using dannan::sched::Budget;
using dannan::sched::Cursor;
using dannan::sched::Page;
using dannan::sched::Stop;

ObjectId id_of(size_t i) {
  ObjectId id{};
  id.bytes[30] = static_cast<uint8_t>(i >> 8);
  id.bytes[31] = static_cast<uint8_t>(i + 1);
  return id;
}

RetentionStore open_store(const std::filesystem::path& state) { return RetentionStore(state, test::known_answer_key(), test::quiet_logger()); }

struct FakeStore final : contract::ObjectStore {
  contract::HeldView held_view() const override { return {}; }
  std::set<ObjectId> objects;
  mutable size_t asked = 0;
  bool has(const ObjectId& id) const override {
    ++asked;
    return objects.contains(id);
  }
};

// Claim counts 0..9 (every third released, so the ledger must skip entries)
// and page bounds 1..10: each live claim comes once, in order, pages stay
// within the bound, only the last page is complete, and its cursor is the
// start again.
TEST(LedgerClaimWalk, TheRetentionLedgerPagesEveryLiveClaimOnce) {
  dannan::test::TempDir t;
  const auto origin = NodeId::random();
  FakeStore data;
  FakeStore control;
  for (size_t claims = 0; claims <= 9; ++claims) {
    auto retention = open_store(t.path() / ("state-" + std::to_string(claims)));
    std::vector<ObjectId> live;
    std::vector<ObjectId> released;
    for (size_t i = 0; i < claims; ++i) {
      retention.retain(RetentionClass::data, id_of(i), {origin, 1});
      (i % 3 == 2 ? released : live).push_back(id_of(i));
    }
    // A control claim the data pages must never show.
    retention.retain(RetentionClass::control, id_of(500), {origin, 1});
    if (!released.empty()) {
      std::vector<ObjectId> keep = live;
      EXPECT_EQ(retention.release_unreferenced(RetentionClass::data, keep, RetentionClock{{origin, 1}}, 64), released.size());
    }
    const RetentionLedger ledger(retention, data, control);
    for (size_t bound = 1; bound <= 10; ++bound) {
      std::vector<ObjectId> walked;
      Cursor<ObjectId> cursor;
      for (size_t pages = 0;; ++pages) {
        EXPECT_LE(pages, claims + 1);
        Budget budget;
        budget.operations(bound);
        const auto page = ledger.claimed(RetentionClass::data, cursor, budget);
        EXPECT_LE(page.items.size(), bound);
        walked.insert(walked.end(), page.items.begin(), page.items.end());
        if (page.complete()) {
          EXPECT_FALSE(page.next.after);
          break;
        }
        EXPECT_EQ(page.stopped, Stop::budget);
        EXPECT_EQ(page.items.size(), bound);
        EXPECT_EQ(page.next.after, page.items.back());
        cursor = page.next;
      }
      EXPECT_EQ(walked, live);
    }
    // No budget: nothing is read and the cursor does not move.
    Budget none;
    none.operations(0);
    const Cursor<ObjectId> from{id_of(0)};
    const auto page = ledger.claimed(RetentionClass::data, from, none);
    EXPECT_TRUE(page.items.empty());
    EXPECT_EQ(page.stopped, Stop::budget);
    EXPECT_EQ(page.next, from);
  }
}

// Macha: test_retention_ledger_stops_for_cancellation_first.
TEST(LedgerClaimWalk, TheRetentionLedgerStopsForCancellationFirst) {
  dannan::test::TempDir t;
  auto retention = open_store(t.path() / "state");
  retention.retain(RetentionClass::data, id_of(0), {NodeId::random(), 1});
  FakeStore stores;
  const RetentionLedger ledger(retention, stores, stores);
  std::atomic_bool cancelled{true};
  Budget budget(dannan::sched::WorkContext(dannan::sched::FrameType::control, {}, &cancelled));
  const auto page = ledger.claimed(RetentionClass::data, {}, budget);
  EXPECT_TRUE(page.items.empty());
  EXPECT_EQ(page.stopped, Stop::cancelled);
}

// Macha: test_retention_ledger_holds_by_class.
TEST(LedgerClaimWalk, TheRetentionLedgerHoldsByClass) {
  dannan::test::TempDir t;
  auto retention = open_store(t.path() / "state");
  FakeStore data;
  FakeStore control;
  data.objects = {id_of(1)};
  control.objects = {id_of(2)};
  const RetentionLedger ledger(retention, data, control);
  for (size_t i : {1, 2, 3}) {
    EXPECT_EQ(ledger.held(RetentionClass::data, id_of(i)), i == 1);
    EXPECT_EQ(ledger.held(RetentionClass::control, id_of(i)), i == 2);
  }
  EXPECT_EQ(data.asked, 3u);
  EXPECT_EQ(control.asked, 3u);
}

// Macha: test_retention_ledger_publishes_horizons_and_refuses_an_incomplete_release.
TEST(LedgerClaimWalk, TheRetentionLedgerPublishesHorizonsAndRefusesAnIncompleteRelease) {
  dannan::test::TempDir t;
  auto retention = open_store(t.path() / "state");
  FakeStore data;
  FakeStore control;
  RetentionLedger ledger(retention, data, control);
  EXPECT_FALSE(ledger.inventory());
  EXPECT_FALSE(ledger.release());

  const auto first =
      std::make_shared<const InventoryHorizon>(3, true, std::vector<ObjectId>{id_of(1)}, std::vector<ObjectId>{}, std::vector<contract::Garbage>{});
  ledger.publish(first);
  const auto held = ledger.inventory();
  EXPECT_EQ(held, first);
  const auto second = std::make_shared<const InventoryHorizon>(4, false, std::vector<ObjectId>{}, std::vector<ObjectId>{}, std::vector<contract::Garbage>{});
  ledger.publish(second);
  EXPECT_EQ(ledger.inventory(), second);
  // A handle taken before the publish still reads its own snapshot.
  EXPECT_EQ(held->generation(), 3u);

  contract::Hash256 head{};
  head.bytes[0] = 1;
  const auto complete = std::make_shared<const ReleaseHorizon>(head, RetentionClock{}, std::vector<ObjectId>{id_of(1)}, std::vector<ObjectId>{});
  EXPECT_FALSE(ledger.publish(ReleaseBuild{complete, false}));
  EXPECT_FALSE(ledger.release());
  EXPECT_TRUE(ledger.publish(ReleaseBuild{complete, true}));
  EXPECT_EQ(ledger.release(), complete);
  contract::Hash256 newer{};
  newer.bytes[0] = 2;
  const auto partial = std::make_shared<const ReleaseHorizon>(newer, RetentionClock{}, std::vector<ObjectId>{}, std::vector<ObjectId>{});
  EXPECT_FALSE(ledger.publish(ReleaseBuild{partial, false}));
  // The previous, complete horizon is kept.
  EXPECT_EQ(ledger.release(), complete);
}

// Macha: test_retention_ledger_forwards_claims_releases_prunes_and_compacts.
TEST(LedgerClaimWalk, TheRetentionLedgerForwardsClaimsReleasesPrunesAndCompacts) {
  dannan::test::TempDir t;
  auto retention = open_store(t.path() / "state");
  FakeStore data;
  FakeStore control;
  RetentionLedger ledger(retention, data, control);
  const auto origin = NodeId::random();

  // Claims as the store has them, per class.
  retention.retain(RetentionClass::data, id_of(1), {origin, 1});
  retention.retain(RetentionClass::data, id_of(2), {origin, 2});
  retention.retain(RetentionClass::control, id_of(3), {origin, 3});
  EXPECT_TRUE(ledger.retained(RetentionClass::data, id_of(1)));
  EXPECT_FALSE(ledger.retained(RetentionClass::control, id_of(1)));
  EXPECT_TRUE(ledger.retained(RetentionClass::control, id_of(3)));
  EXPECT_FALSE(ledger.retained(RetentionClass::data, id_of(4)));

  // A claim the horizon does not refer to, written before its clock, is
  // released; one it refers to stays; the other class is untouched.
  const ReleaseHorizon release(contract::Hash256{}, {{origin, 10}}, {id_of(2)}, {});
  EXPECT_EQ(ledger.release_unreferenced(RetentionClass::data, release, 64), 1u);
  EXPECT_FALSE(ledger.retained(RetentionClass::data, id_of(1)));
  EXPECT_TRUE(ledger.retained(RetentionClass::data, id_of(2)));
  EXPECT_TRUE(ledger.retained(RetentionClass::control, id_of(3)));

  // Prune forgets a released object's tombstone only once the class's store
  // no longer holds it.
  data.objects = {id_of(1)};
  EXPECT_EQ(ledger.prune_unclaimed(RetentionClass::data, 64), 0u);
  data.objects.clear();
  EXPECT_EQ(ledger.prune_unclaimed(RetentionClass::data, 64), 1u);
  // Nothing of control was released, and control's store keeps no data
  // tombstone.
  EXPECT_EQ(ledger.prune_unclaimed(RetentionClass::control, 64), 0u);

  EXPECT_FALSE(ledger.compact_if_needed(1000000));
  EXPECT_TRUE(ledger.compact_if_needed(1));
}

// Claims in id order; the ledger honours the budget as the contract says.
struct FakeLedger final : contract::ObjectLedger {
  std::vector<ObjectId> claims;
  std::set<ObjectId> held_ids;
  Page<ObjectId, ObjectId> claimed(RetentionClass, Cursor<ObjectId> from, Budget& budget) const override {
    Page<ObjectId, ObjectId> page;
    page.next = from;
    auto it = from.after ? std::upper_bound(claims.begin(), claims.end(), *from.after) : claims.begin();
    for (;; ++it) {
      if (!budget.take_operation()) {
        page.stopped = Stop::budget;
        return page;
      }
      if (it == claims.end()) {
        page.next = {};
        return page;
      }
      page.items.push_back(*it);
      page.next.after = *it;
    }
  }
  bool held(RetentionClass, const ObjectId& id) const override { return held_ids.contains(id); }
  contract::HeldView held_view(RetentionClass) const override { return {}; }
  // The claim walk reads no horizon.
  InventoryHandle inventory() const override { return {}; }
  ReleaseHandle release() const override { return {}; }
  void publish(InventoryHandle) override {}
  bool publish(ReleaseBuild) override { return false; }
  bool retained(RetentionClass, const ObjectId&) const override { return false; }
  size_t release_unreferenced(RetentionClass, const ReleaseHorizon&, size_t) override { return 0; }
  size_t prune_unclaimed(RetentionClass, size_t) override { return 0; }
  bool compact_if_needed(size_t) override { return false; }
};

// Restores until its credit runs out; `refused` ids are fetched but not
// restored and cost nothing. Records every call.
struct FakeRestorer final : ClaimRestorer {
  size_t credit = 0;
  std::set<ObjectId> refused;
  std::vector<std::pair<ObjectId, Outcome>> calls;
  Outcome restore(RetentionClass, const ObjectId& id) override {
    Outcome outcome = Outcome::restored;
    if (!credit)
      outcome = Outcome::waiting_for_credit;
    else if (refused.contains(id))
      outcome = Outcome::not_restored;
    else
      --credit;
    calls.emplace_back(id, outcome);
    return outcome;
  }
};

// The reference walk: next_retained() one claim at a time, 16 per step.
struct ReferenceWalk {
  std::optional<ObjectId> cursor;
  ClaimWalkStep step(const FakeLedger& ledger, FakeRestorer& restorer) {
    ClaimWalkStep result;
    for (size_t examined = 0;; ++examined) {
      if (examined == 16) {
        result.unfinished = true;
        break;
      }
      const auto resume = cursor;
      auto it = cursor ? std::upper_bound(ledger.claims.begin(), ledger.claims.end(), *cursor) : ledger.claims.begin();
      if (it == ledger.claims.end()) {
        cursor.reset();
        break;
      }
      cursor = *it;
      ++result.examined;
      if (ledger.held(RetentionClass::data, *it)) continue;
      ++result.missing;
      const auto outcome = restorer.restore(RetentionClass::data, *it);
      if (outcome == ClaimRestorer::Outcome::waiting_for_credit) {
        cursor = resume;
        result.waiting_for_credit = true;
        break;
      }
      if (outcome == ClaimRestorer::Outcome::restored) ++result.restored;
    }
    return result;
  }
};

bool same(const ClaimWalkStep& a, const ClaimWalkStep& b) {
  return a.examined == b.examined && a.missing == b.missing && a.restored == b.restored && a.waiting_for_credit == b.waiting_for_credit &&
         a.unfinished == b.unfinished;
}

// Runs both walks side by side for `steps` steps, credit topped up by
// `refill` each step; every step's result, every restorer call and every
// cursor agree.
void compare(const FakeLedger& ledger, const std::set<ObjectId>& refused, size_t initial, size_t refill, size_t steps) {
  FakeRestorer ours;
  FakeRestorer theirs;
  ours.refused = theirs.refused = refused;
  ours.credit = theirs.credit = initial;
  ClaimWalk walk(RetentionClass::data);
  EXPECT_EQ(walk.type(), RetentionClass::data);
  ReferenceWalk reference;
  for (size_t s = 0; s < steps; ++s) {
    const auto a = walk.step(ledger, ours);
    const auto b = reference.step(ledger, theirs);
    EXPECT_TRUE(same(a, b));
    EXPECT_EQ(ours.calls, theirs.calls);
    EXPECT_EQ(walk.cursor().after, reference.cursor);
    EXPECT_LE(a.examined, ClaimWalk::step_bound);
    ours.credit += refill;
    theirs.credit += refill;
  }
}

// Macha: test_claim_walk_matches_the_walk_it_replaced_exhaustively. Every
// held pattern over up to eight claims, credit 0..3 with and without refill,
// and a refused claim.
TEST(LedgerClaimWalk, MatchesTheWalkItReplacedExhaustively) {
  for (size_t n = 0; n <= 8; ++n)
    for (unsigned pattern = 0; pattern < (1U << n); ++pattern) {
      FakeLedger ledger;
      for (size_t i = 0; i < n; ++i) {
        ledger.claims.push_back(id_of(i));
        if (pattern & (1U << i)) ledger.held_ids.insert(id_of(i));
      }
      for (size_t initial = 0; initial <= 3; ++initial)
        for (size_t refill : {0, 1}) {
          compare(ledger, {}, initial, refill, n + 3);
          if (n) compare(ledger, {id_of(n / 2)}, initial, refill, n + 3);
        }
    }
}

// Macha: test_claim_walk_matches_the_walk_it_replaced_at_the_bound. Around
// the step bound: 15, 16, 17, 32, 33 and 50 claims, all held, none held, and
// every fifth missing, over enough steps to wrap twice.
TEST(LedgerClaimWalk, MatchesTheWalkItReplacedAtTheBound) {
  for (size_t n : {15, 16, 17, 32, 33, 50})
    for (int shape = 0; shape < 3; ++shape) {
      FakeLedger ledger;
      for (size_t i = 0; i < n; ++i) {
        ledger.claims.push_back(id_of(i));
        if (shape == 0 || (shape == 2 && i % 5)) ledger.held_ids.insert(id_of(i));
      }
      for (size_t initial : {0, 3, 100})
        for (size_t refill : {0, 2}) compare(ledger, {}, initial, refill, 2 * (n / 16 + 2));
    }
}

// Macha: test_claim_walk_follows_a_changing_ledger. Claims appear and
// disappear between steps: the walk resumes after its cursor, whatever is
// there now.
TEST(LedgerClaimWalk, FollowsAChangingLedger) {
  FakeLedger ledger;
  for (size_t i = 0; i < 40; ++i) ledger.claims.push_back(id_of(i * 2));
  FakeRestorer ours;
  FakeRestorer theirs;
  ours.credit = theirs.credit = 1000;
  ClaimWalk walk(RetentionClass::data);
  ReferenceWalk reference;
  for (size_t s = 0; s < 12; ++s) {
    EXPECT_TRUE(same(walk.step(ledger, ours), reference.step(ledger, theirs)));
    EXPECT_EQ(ours.calls, theirs.calls);
    // Insert one between the cursor and its successor, drop the first.
    if (walk.cursor().after) {
      const auto after = *walk.cursor().after;
      ObjectId between = after;
      ++between.bytes[31];
      ledger.claims.insert(std::upper_bound(ledger.claims.begin(), ledger.claims.end(), after), between);
      std::sort(ledger.claims.begin(), ledger.claims.end());
      ledger.claims.erase(std::unique(ledger.claims.begin(), ledger.claims.end()), ledger.claims.end());
    }
    if (!ledger.claims.empty()) ledger.claims.erase(ledger.claims.begin());
  }
}

// --- Macha's baselines: the claim walk's per-object cost --------------------

constexpr size_t object_count = 2000;

void report(std::string_view name, std::chrono::steady_clock::duration elapsed, uint64_t ops) {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  std::cout << "BENCH name=" << name << " ns_per_op=" << (ops ? ns / static_cast<int64_t>(ops) : 0) << " ops=" << ops << '\n';
}

// A one-backend DATA pool holding object_count small objects.
struct Populated {
  test::StoreBed bed;
  std::unique_ptr<store::StoragePool> pool;
  std::vector<ObjectId> present;

  Populated() {
    const auto backend = bed.dir.path() / "data";
    std::filesystem::create_directories(backend);
    pool = bed.pool(bed.dir.path() / "pool-state", {{backend, 1ULL << 30}});
    EXPECT_TRUE(test::wait_until([&] { return pool->online_backends() == 1; }));
    for (size_t i = 0; i < object_count; ++i) {
      const auto bytes = test::numbered(i);
      const auto id = store::object_id(bytes);
      EXPECT_TRUE(pool->put(id, bytes));
      present.push_back(id);
    }
  }
};

// Macha: test_baseline_claim_walk_per_object. The claim walk as the pass
// once made it: next_retained, then has() on the class's store, per object.
TEST(LedgerClaimWalk, BaselineClaimWalkPerObject) {
  Populated store;
  auto retention = open_store(store.bed.dir.path() / "retention");
  retention.retain_batch(RetentionClass::data, store.present, {NodeId::random(), 1});
  constexpr int passes = 10;
  size_t walked = 0;
  size_t held = 0;
  const auto started = std::chrono::steady_clock::now();
  for (int pass = 0; pass < passes; ++pass) {
    std::optional<ObjectId> cursor;
    for (;;) {
      bool complete = false;
      auto id = retention.next_retained(RetentionClass::data, cursor, complete);
      if (!id) break;
      ++walked;
      held += store.pool->has(*id) ? 1 : 0;
    }
  }
  report("claim_walk.per_object", std::chrono::steady_clock::now() - started, walked);
  EXPECT_EQ(walked, passes * object_count);
  EXPECT_EQ(held, walked);
}

// Macha: test_baseline_claim_walk_on_the_ledger_per_object. The same walk
// through the contracts: ClaimWalk over a RetentionLedger, in its 16-claim
// steps.
TEST(LedgerClaimWalk, BaselineClaimWalkOnTheLedgerPerObject) {
  Populated store;
  auto retention = open_store(store.bed.dir.path() / "retention");
  retention.retain_batch(RetentionClass::data, store.present, {NodeId::random(), 1});
  const RetentionLedger ledger(retention, *store.pool, *store.pool);
  struct Unreached final : ClaimRestorer {
    size_t calls = 0;
    Outcome restore(RetentionClass, const ObjectId&) override {
      ++calls;
      return Outcome::not_restored;
    }
  } restorer;
  constexpr int passes = 10;
  size_t walked = 0;
  ClaimWalk walk(RetentionClass::data, &store.bed.observations);
  const auto started = std::chrono::steady_clock::now();
  for (int pass = 0; pass < passes; ++pass)
    for (;;) {
      const auto step = walk.step(ledger, restorer);
      walked += step.examined;
      if (!step.unfinished) break;
    }
  report("claim_walk.ledger_per_object", std::chrono::steady_clock::now() - started, walked);
  EXPECT_EQ(walked, passes * object_count);
  EXPECT_EQ(restorer.calls, 0u);
  EXPECT_EQ(store.bed.observations.counter("maintenance.claim_walk.examined").load(), walked);
}

}  // namespace
}  // namespace dagda::ledger
