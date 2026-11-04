//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirements L17 to L22: garbage collection on one node, through the
// owner's ReachabilityProvider, its three gates and its two sweeps.
//
// Moved from Macha (develop 5a06d3c), every assertion kept:
// tests/test_storage_metadata.cpp test_retention_claim_is_physical_gc_barrier;
// tests/test_hydration_catalogue.cpp, the CONTROL GC block of
// test_catalogue_on_one_node_predicts_caches_and_protects_its_control_objects
// ("CONTROL publication is data-before-metadata"), with the catalogue's root
// epoch as the provider's CONTROL epoch and its 5 ms sleeps as stepped time.
// The rest of that case, and the catalogue's own cases in
// test_catalogue_maintenance.cpp, test the catalogue: what they gave the
// ledger (an inventory complete only at the head, a release of what nothing
// refers to) is the provider's answer here.
//
// New: an inventory is never used destructively in the pass that built it;
// an incomplete release is never published; a claim from an unseen branch
// survives the sweeps; the grace runs on this node's own clock; tombstone
// collection's decisions; a removal never races a new CONTROL epoch. Time,
// steady and wall, is stepped.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "dagda/ledger/collector.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/retention_ledger.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/store/control_store.h"
#include "dagda/test/fixtures/reachability.h"
#include "dagda/test/fixtures/store.h"

namespace dagda::ledger {
namespace {

using namespace std::chrono_literals;
using contract::Garbage;
using contract::GarbageBatch;
using contract::Hash256;
using contract::Reachability;
using contract::RetentionClass;
using test::pattern;

constexpr auto data = RetentionClass::data;
constexpr auto control = RetentionClass::control;
constexpr uint64_t mib = 1024 * 1024;

Hash256 head_of(uint8_t n) {
  Hash256 out{};
  out.bytes[0] = n;
  return out;
}

Reachability complete_release(contract::RetentionClock clock, std::vector<ObjectId> referenced = {}) {
  Reachability out;
  out.data = std::move(referenced);
  out.clock = std::move(clock);
  out.complete = true;
  return out;
}

// One node's stores, claims, ledger and collector over a test provider: a
// grace of an hour, a retry window of a second.
struct GcBed {
  test::StoreBed bed;
  std::filesystem::path state = bed.dir.path() / "state";
  std::filesystem::path disk = bed.dir.path() / "disk";
  std::unique_ptr<store::StoragePool> pool;
  std::unique_ptr<store::LocalStore> control_store;
  std::unique_ptr<RetentionStore> retention;
  std::unique_ptr<RetentionLedger> ledger;
  std::unique_ptr<UnreferencedSince> data_seen;
  std::unique_ptr<UnreferencedSince> control_seen;
  test::TestProvider provider;
  std::vector<std::string> traces;
  std::unique_ptr<Collector> collector;

  GcBed() {
    std::filesystem::create_directories(disk);
    pool = bed.pool(state, {{disk, 64 * mib}});
    EXPECT_TRUE(test::wait_until([&] { return pool->online_backends() == 1; }));
    control_store = store::open_control_store(state, {}, bed.key, bed.services());
    retention = std::make_unique<RetentionStore>(state, bed.key, test::quiet_logger());
    ledger = std::make_unique<RetentionLedger>(*retention, *pool, *control_store, bed.time);
    data_seen = std::make_unique<UnreferencedSince>(state / "retention" / "unreferenced-data.bin", test::quiet_logger());
    control_seen = std::make_unique<UnreferencedSince>(state / "retention" / "unreferenced-control.bin", test::quiet_logger());
    CollectorOptions options;
    options.garbage_grace = 1h;
    options.retry_window = 1s;
    collector = std::make_unique<Collector>(
        *ledger, provider, *pool, *control_store, *data_seen, *control_seen, options,
        CollectorServices{dannan::status::ScopedLogger(bed.sink, "gc"), bed.observations, bed.time, bed.wall}, nullptr,
        [this](std::string_view kind, std::string_view detail) { traces.push_back(std::string(kind) + " " + std::string(detail)); });
  }

  // An object in the pool written two days ago.
  ObjectId put_old(uint8_t salt) {
    const auto bytes = pattern(32 * 1024, salt);
    const auto id = store::object_id(bytes);
    EXPECT_TRUE(pool->put(id, bytes));
    std::filesystem::last_write_time(test::object_path(disk, id), std::filesystem::file_time_type::clock::now() - 48h);
    return id;
  }
  PassReport pass() {
    PassDue due;
    due.garbage_due = true;
    due.gc_due = true;
    return collector->pass(due);
  }
  bool traced(std::string_view text) const {
    return std::any_of(traces.begin(), traces.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
  }
};

// Macha: test_retention_claim_is_physical_gc_barrier.
TEST(LedgerCollector, ARetentionClaimIsAPhysicalGcBarrier) {
  test::StoreBed bed;
  const auto state = bed.dir.path() / "state";
  const auto disk = bed.dir.path() / "disk";
  std::filesystem::create_directories(disk);
  auto pool = bed.pool(state, {{disk, 64 * mib}});
  pool->refresh();
  RetentionStore retention(state, bed.key, test::quiet_logger());

  auto bytes = pattern(128 * 1024 + 17);
  const auto id = store::object_id(bytes);
  ASSERT_TRUE(pool->put(id, bytes));
  auto physical = test::object_path(disk, id);
  ASSERT_TRUE(std::filesystem::exists(physical));
  std::filesystem::last_write_time(physical, std::filesystem::file_time_type::clock::now() - 48h);

  const auto origin = NodeId::random();
  retention.retain(data, id, {origin, 1});
  std::vector<ObjectId> none;
  const store::StoragePool::ClaimCheck claimed = [&](const ObjectId& candidate) { return retention.retained(data, candidate); };
  auto protected_pass = pool->gc_step(none, none, 0ms, 128, {}, claimed);
  EXPECT_TRUE(protected_pass.complete);
  EXPECT_TRUE(pool->has(id));

  // The head has observed and removed the only reference: the claim goes
  // first, then physical GC may reclaim the object.
  EXPECT_EQ(retention.release_unreferenced(data, none, RetentionClock{{origin, 1}}, 16), 1u);
  EXPECT_FALSE(retention.retained(data, id));
  auto reclaim_pass = pool->gc_step(none, none, 0ms, 128, {}, claimed);
  EXPECT_TRUE(reclaim_pass.complete);
  EXPECT_FALSE(pool->has(id));
}

// Nothing happens unless tombstone collection or GC is due: no inventory is
// asked for, no gate is reached.
TEST(LedgerCollector, NothingHappensUnlessSomethingIsDue) {
  GcBed gc;
  const auto report = gc.collector->pass(PassDue{});
  EXPECT_EQ(gc.provider.inventories, 0);
  EXPECT_FALSE(report.tombstones);
  EXPECT_FALSE(report.data);
}

// An inventory is never used destructively in the pass that built it; the
// next pass uses it, and an unreferenced, unclaimed object goes only once
// this node has seen it so for the grace on its own clock.
TEST(LedgerCollector, AnInventoryIsNeverUsedDestructivelyInThePassThatBuiltIt) {
  GcBed gc;
  gc.provider.answer.complete = true;
  gc.provider.head = head_of(1);
  gc.provider.heads[head_of(1)] = complete_release({});
  const auto orphan = gc.put_old(1);

  auto first = gc.pass();
  EXPECT_TRUE(first.rebuilt_inventory);
  for (const auto& verdict : {*first.tombstones, *first.control, *first.data}) EXPECT_EQ(verdict.reason, "inventory rebuilt this pass");
  EXPECT_TRUE(gc.traced("control-gate-in-rebuilding-pass shut"));
  EXPECT_EQ(gc.data_seen->size(), 0u);
  EXPECT_TRUE(gc.pool->has(orphan));

  // Not rebuilt again: the generation stands.
  auto second = gc.pass();
  EXPECT_FALSE(second.rebuilt_inventory);
  EXPECT_TRUE(second.data->permitted);
  EXPECT_TRUE(second.swept_data.deferred);
  EXPECT_EQ(gc.provider.inventories, 1);
  EXPECT_TRUE(gc.pool->has(orphan));

  gc.bed.wall.advance(30min);
  (void)gc.pass();
  EXPECT_TRUE(gc.pool->has(orphan));
  gc.bed.wall.advance(31min);
  const auto swept = gc.pass();
  EXPECT_GT(swept.swept_data.bytes, 0u);
  EXPECT_FALSE(gc.pool->has(orphan));

  // A new generation is rebuilt, and that pass removes nothing.
  const auto later = gc.put_old(2);
  gc.provider.current_generation = 2;
  gc.provider.known = 2;
  gc.bed.wall.advance(2h);
  EXPECT_TRUE(gc.pass().rebuilt_inventory);
  EXPECT_TRUE(gc.pool->has(later));
}

// An inventory behind the known generation, or incomplete, opens no sweep.
TEST(LedgerCollector, AnInventoryBehindOrIncompleteSweepsNothing) {
  GcBed gc;
  gc.provider.answer.complete = true;
  gc.provider.head = head_of(1);
  gc.provider.heads[head_of(1)] = complete_release({});
  (void)gc.put_old(1);
  gc.provider.known = 2;
  (void)gc.pass();
  const auto behind = gc.pass();
  EXPECT_EQ(behind.data->reason, "inventory generation 1 behind known 2");
  EXPECT_EQ(gc.data_seen->size(), 0u);

  gc.provider.known = 1;
  gc.provider.answer.complete = false;
  gc.provider.current_generation = 3;
  (void)gc.pass();
  // An incomplete inventory is rebuilt every pass, so no pass may use it.
  const auto incomplete = gc.pass();
  EXPECT_TRUE(incomplete.rebuilt_inventory);
  EXPECT_FALSE(incomplete.data->permitted);
  EXPECT_EQ(gc.data_seen->size(), 0u);
}

// An incomplete release is never published: claims are not released
// against it, and a claimed object outlives any grace. A complete one at a
// later head releases them.
TEST(LedgerCollector, AnIncompleteReleaseIsNeverPublished) {
  GcBed gc;
  const auto writer = NodeId::random();
  const auto claimed = gc.put_old(1);
  gc.retention->retain(data, claimed, {writer, 1});
  gc.provider.answer.complete = true;
  gc.provider.head = head_of(1);
  auto partial = complete_release({{writer, 1}});
  partial.complete = false;
  gc.provider.heads[head_of(1)] = partial;
  for (int i = 0; i < 4; ++i) {
    (void)gc.pass();
    gc.bed.wall.advance(2h);
  }
  EXPECT_FALSE(gc.ledger->release());
  EXPECT_TRUE(gc.traced("release-horizon incomplete"));
  EXPECT_TRUE(gc.retention->retained(data, claimed));
  EXPECT_TRUE(gc.pool->has(claimed));

  gc.provider.head = head_of(2);
  gc.provider.heads[head_of(2)] = complete_release({{writer, 1}});
  const auto released = gc.pass();
  ASSERT_TRUE(gc.ledger->release());
  EXPECT_EQ(gc.ledger->release()->head(), head_of(2));
  EXPECT_EQ(released.released_data, 1u);
  EXPECT_FALSE(gc.retention->retained(data, claimed));
  // Seen unreferenced and unclaimed from here, it goes a grace later.
  EXPECT_TRUE(gc.pool->has(claimed));
  gc.bed.wall.advance(2h);
  (void)gc.pass();
  EXPECT_FALSE(gc.pool->has(claimed));
}

// A claim from a branch this node has not seen survives the sweeps: the
// head's clock has seen the other node only up to an earlier write, so its
// claim stays, and a copy with any local claim is untouchable however long
// it is unreferenced. Once a head has seen it, it goes a grace later.
TEST(LedgerCollector, AClaimFromAnUnseenBranchSurvivesTheSweeps) {
  GcBed gc;
  const auto here = NodeId::random();
  const auto away = NodeId::random();
  const auto object = gc.put_old(3);
  gc.retention->retain(data, object, {here, 1});
  gc.retention->retain(data, object, {away, 4});
  gc.provider.answer.complete = true;
  gc.provider.head = head_of(1);
  gc.provider.heads[head_of(1)] = complete_release({{here, 1}, {away, 3}});
  for (int i = 0; i < 6; ++i) {
    (void)gc.pass();
    gc.bed.wall.advance(24h);
  }
  EXPECT_TRUE(gc.retention->retained(data, object));
  EXPECT_TRUE(gc.pool->has(object));
  EXPECT_EQ(gc.data_seen->size(), 0u);

  gc.provider.head = head_of(2);
  gc.provider.heads[head_of(2)] = complete_release({{here, 1}, {away, 4}});
  (void)gc.pass();
  EXPECT_FALSE(gc.retention->retained(data, object));
  gc.bed.wall.advance(2h);
  (void)gc.pass();
  EXPECT_FALSE(gc.pool->has(object));
}

// Tombstone collection: stale retirements and those past their grace are
// erased, unstamped ones stamped, matured batches dropped, all handed to the
// owner; a retirement in its grace protects its object from the sweep.
TEST(LedgerCollector, TombstoneCollectionHandsItsDecisionsToTheOwner) {
  GcBed gc;
  const auto now_ns = static_cast<int64_t>(gc.bed.wall.unix_ms()) * 1'000'000;
  const int64_t hour_ns = 3600LL * 1'000'000'000;
  const auto stale = gc.put_old(1);
  const auto matured = gc.put_old(2);
  const auto fresh = gc.put_old(3);
  const auto unstamped = gc.put_old(4);
  ObjectId batch_old{};
  batch_old.bytes[0] = 0xb0;
  ObjectId batch_new{};
  batch_new.bytes[0] = 0xb1;
  gc.provider.answer.complete = true;
  gc.provider.answer.data = {stale};
  gc.provider.answer.garbage = {Garbage{stale, now_ns - 2 * hour_ns, {}}, Garbage{matured, now_ns - 2 * hour_ns, {}}, Garbage{fresh, now_ns, {}},
                                Garbage{unstamped, 0, {}}};
  gc.provider.answer.batches = {GarbageBatch{batch_old, now_ns - 2 * hour_ns, 3}, GarbageBatch{batch_new, now_ns, 1}};
  gc.provider.head = head_of(1);
  gc.provider.heads[head_of(1)] = complete_release({});

  (void)gc.pass();
  EXPECT_TRUE(gc.provider.collected.empty());
  const auto open = gc.pass();
  EXPECT_TRUE(open.tombstones->permitted);
  ASSERT_EQ(gc.provider.collected.size(), 1u);
  const auto& collection = gc.provider.collected.front();
  ASSERT_EQ(collection.erase.size(), 2u);
  EXPECT_EQ(collection.erase[0].id, stale);
  EXPECT_EQ(collection.erase[1].id, matured);
  ASSERT_EQ(collection.stamp.size(), 1u);
  EXPECT_EQ(collection.stamp[0].id, unstamped);
  EXPECT_EQ(collection.matured_batches, std::vector<ObjectId>{batch_old});
  // Batches are referenced CONTROL.
  EXPECT_TRUE(gc.ledger->inventory()->referenced(control, batch_old));
  EXPECT_TRUE(gc.traced("tombstones erase=["));

  gc.bed.wall.advance(61min);
  (void)gc.pass();
  // The referenced object stays; the matured retirement's goes; the fresh
  // and unstamped retirements protect theirs.
  EXPECT_TRUE(gc.pool->has(stale));
  EXPECT_FALSE(gc.pool->has(matured));
  EXPECT_TRUE(gc.pool->has(fresh));
  EXPECT_TRUE(gc.pool->has(unstamped));
}

// Macha (test_hydration_catalogue.cpp): CONTROL publication is data before
// metadata. An object written after the current CONTROL epoch began is kept
// by GC even with zero grace, and becomes an ordinary orphan once a later
// epoch stands.
TEST(LedgerCollector, AControlObjectStagedInTheCurrentEpochIsKept) {
  GcBed gc;
  gc.provider.epoch = contract::ControlEpoch{1, gc.bed.time.now()};
  Bytes staged = pattern(4096 + 37);
  staged[0] ^= 0x6d;
  const auto staged_id = store::object_id(staged);
  ASSERT_TRUE(gc.control_store->put(staged_id, staged));
  gc.bed.time.advance(5ms);
  gc.bed.wall.advance(5ms);
  const std::vector<ObjectId> no_live;
  for (int i = 0; i < 4; ++i) (void)gc.collector->control_gc_step(no_live, 0ms, 64, nullptr, gc.bed.wall.unix_ms());
  EXPECT_TRUE(gc.control_store->has(staged_id));
  // The owner publishes a successor: a new epoch.
  gc.provider.epoch = contract::ControlEpoch{2, gc.bed.time.now()};
  gc.bed.time.advance(5ms);
  gc.bed.wall.advance(5ms);
  const std::vector<ObjectId> live{store::object_id(pattern(64, 1))};
  for (int i = 0; i < 4 && gc.control_store->has(staged_id); ++i) (void)gc.collector->control_gc_step(live, 0ms, 64, nullptr, gc.bed.wall.unix_ms());
  EXPECT_FALSE(gc.control_store->has(staged_id));
}

// A CONTROL removal is held in the epoch it was judged in: a new epoch begun
// before the hold stops it. Without an epoch, CONTROL GC removes nothing; a
// claimed or referenced CONTROL object stays in any epoch.
TEST(LedgerCollector, AControlRemovalNeverRacesANewEpoch) {
  GcBed gc;
  const std::vector<ObjectId> no_live;
  const auto orphan_bytes = pattern(2048, 7);
  const auto claimed_bytes = pattern(2048, 8);
  const auto live_bytes = pattern(2048, 9);
  const auto orphan = store::object_id(orphan_bytes);
  const auto claimed = store::object_id(claimed_bytes);
  const auto live = store::object_id(live_bytes);
  for (const auto* bytes : {&orphan_bytes, &claimed_bytes, &live_bytes}) ASSERT_TRUE(gc.control_store->put(store::object_id(*bytes), *bytes));
  gc.retention->retain(control, claimed, {NodeId::random(), 1});
  gc.bed.wall.advance(1h);
  EXPECT_EQ(gc.collector->control_gc_step(no_live, 0ms, 64, nullptr, gc.bed.wall.unix_ms()), 0u);

  gc.provider.epoch = contract::ControlEpoch{1, gc.bed.time.now()};
  gc.bed.time.advance(1s);
  // First seen unreferenced in epoch 1.
  const std::vector<ObjectId> referenced{live};
  (void)gc.collector->control_gc_step(referenced, 0ms, 64, gc.control_seen.get(), gc.bed.wall.unix_ms());
  gc.provider.epoch = contract::ControlEpoch{2, gc.bed.time.now()};
  gc.bed.time.advance(1s);
  gc.bed.wall.advance(1s);
  gc.provider.before_hold = [&] { gc.provider.epoch = contract::ControlEpoch{3, gc.bed.time.now()}; };
  EXPECT_EQ(gc.collector->control_gc_step(referenced, 0ms, 64, gc.control_seen.get(), gc.bed.wall.unix_ms()), 0u);
  EXPECT_TRUE(gc.control_store->has(orphan));

  gc.provider.before_hold = {};
  gc.bed.time.advance(1s);
  gc.bed.wall.advance(1s);
  size_t removed = 0;
  for (int i = 0; i < 4; ++i) removed += gc.collector->control_gc_step(referenced, 0ms, 64, gc.control_seen.get(), gc.bed.wall.unix_ms());
  EXPECT_EQ(removed, 1u);
  EXPECT_FALSE(gc.control_store->has(orphan));
  EXPECT_TRUE(gc.control_store->has(claimed));
  EXPECT_TRUE(gc.control_store->has(live));
}

// The CONTROL sweep in a pass: release of CONTROL claims against the
// release head, the step on this node's own clock and its sightings, then
// the prune.
TEST(LedgerCollector, TheControlSweepReleasesRemovesAndPrunes) {
  GcBed gc;
  const auto writer = NodeId::random();
  const auto bytes = pattern(2048, 3);
  const auto object = store::object_id(bytes);
  ASSERT_TRUE(gc.control_store->put(object, bytes));
  gc.retention->retain(control, object, {writer, 2});
  gc.provider.answer.complete = true;
  gc.provider.head = head_of(1);
  gc.provider.heads[head_of(1)] = complete_release({{writer, 2}});
  gc.provider.epoch = contract::ControlEpoch{1, gc.bed.time.now()};
  (void)gc.pass();
  const auto released = gc.pass();
  EXPECT_TRUE(released.control->permitted);
  EXPECT_EQ(released.released_control, 1u);
  EXPECT_FALSE(gc.retention->retained(control, object));
  // First seen unreferenced, in epoch 1, by that pass: it stays through the
  // epoch and the grace.
  EXPECT_TRUE(gc.control_store->has(object));
  gc.bed.wall.advance(2h);
  (void)gc.pass();
  EXPECT_TRUE(gc.control_store->has(object));
  gc.provider.epoch = contract::ControlEpoch{2, gc.bed.time.now()};
  gc.bed.time.advance(1s);
  const auto removed = gc.pass();
  EXPECT_EQ(removed.removed_control, 1u);
  EXPECT_FALSE(gc.control_store->has(object));
  // Its causality tombstone goes once the object is gone.
  (void)gc.pass();
  EXPECT_TRUE(gc.retention->claims(control, object).removed.empty());
}

}  // namespace
}  // namespace dagda::ledger
