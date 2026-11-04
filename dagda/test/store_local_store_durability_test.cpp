//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirements S8, S13, S16 and S17 at one store: durability comes from the
// domain's barriers, accounting is marked dirty once per session, a dirty
// checkpoint is reconciled to a durable baseline, a re-put waits only for
// what is not yet durable, and a delete never waits. Crash matrix rows "DATA
// accounting dirty" and "GC unlink lost".
//
// Moved from Macha's tests/test_invariants.cpp (develop 5a06d3c), every
// assertion kept:
// test_authoritative_deferred_generation_batches_stable_storage_barriers,
// test_accounting_dirty_marker_is_process_session_scoped,
// test_clean_accounting_checkpoint_is_trusted_with_packs,
// test_unclean_accounting_recovery_establishes_durable_baseline,
// test_restart_durable_reaffirmation_requires_no_new_barrier,
// test_authoritative_delete_is_lazy_durability,
// test_local_store_barrier_remembers_complete_physical_cut,
// test_durability_domain_group_commits_independent_publications,
// test_immediate_reaffirmation_flushes_provisional_generation.
//
// Macha counted fsync(2) and syncfs(2) by interposing them, which works only
// on Linux, so every case but one was skipped elsewhere. Here they run on
// every platform: a syncfs is one of the domain's barriers
// (physical_barriers(), one syncfs on Linux or one per-file pass elsewhere),
// and the fsyncs Macha counted outside the domain are the store's own
// (own_syncs: accounting checkpoints and compaction). Batch windows run on
// stepped time; the unclean exits are a child process that skips the
// store's teardown, as Macha's are.

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <optional>
#include <thread>

#include "dagda/store/durability_domain.h"
#include "dagda/store/local_store.h"
#include "dagda/test/fixtures/store.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using test::barrier_through_window;
using test::exit_code;
using test::pattern;
using test::StoreBed;
using test::wait_until;

const auto loose_only = LocalStoreOptions::loose_only(64ULL * 1024 * 1024);

// What the store and its domain have done since a mark: Macha's fsync and
// syncfs counters.
struct Counts {
  const LocalStore& store;
  const DurabilityDomain& domain;
  uint64_t syncs = store.diagnostics().own_syncs;
  uint64_t barriers = domain.physical_barriers();
  uint64_t fsync_calls() const { return store.diagnostics().own_syncs - syncs; }
  uint64_t syncfs_calls() const { return domain.physical_barriers() - barriers; }
};

// Macha: test_authoritative_deferred_generation_batches_stable_storage_barriers.
TEST(StoreLocalStoreDurability, DeferredGenerationsBatchStableStorageBarriers) {
  StoreBed bed;
  const auto root = bed.dir.path() / "objects";
  auto domain = bed.domain(root, 20ms);
  auto store = bed.open(root, loose_only, LocalStoreMode::authoritative, domain);
  ASSERT_TRUE(wait_until([&] { return store->scan_complete(); }));

  const auto strict_a = pattern(256 * 1024, 31);
  const auto strict_b = pattern(256 * 1024, 32);
  {
    Counts counts{*store, *domain};
    ASSERT_TRUE(store->put(object_id(strict_a), strict_a));
    ASSERT_TRUE(store->put(object_id(strict_b), strict_b));
    // Strict puts share publication's write, rename and generation path: one
    // fsync marks accounting dirty, then durability comes from the domain's
    // barriers, with no per-object or directory fsync of the store's own.
    EXPECT_EQ(counts.fsync_calls(), 1u);
    EXPECT_EQ(counts.syncfs_calls(), 2u);
  }

  const auto deferred_a = pattern(256 * 1024, 33);
  const auto deferred_b = pattern(256 * 1024, 34);
  Counts counts{*store, *domain};
  const auto ga = store->put_deferred(object_id(deferred_a), deferred_a);
  const auto gb = store->put_deferred(object_id(deferred_b), deferred_b);
  ASSERT_TRUE(ga.has_value());
  ASSERT_TRUE(gb.has_value());
  ASSERT_GT(*gb, *ga);
  EXPECT_EQ(counts.fsync_calls(), 0u);
  EXPECT_EQ(counts.syncfs_calls(), 0u);

  barrier_through_window(*store, *gb, *domain, bed.time, 20ms);
  EXPECT_EQ(counts.syncfs_calls(), 1u);
  EXPECT_EQ(counts.fsync_calls(), 0u);
  EXPECT_EQ(store->get(object_id(deferred_a)), std::optional<Bytes>{deferred_a});
  EXPECT_EQ(store->get(object_id(deferred_b)), std::optional<Bytes>{deferred_b});
}

// Macha: test_accounting_dirty_marker_is_process_session_scoped. Accounting
// is marked dirty once before the first mutation, not toggled per
// generation; the clean checkpoint is written at teardown, outside what is
// measured.
TEST(StoreLocalStoreDurability, TheAccountingDirtyMarkIsOncePerSession) {
  StoreBed bed;
  const auto root = bed.dir.path() / "objects";
  auto domain = bed.domain(root, 10ms);
  auto store = bed.open(root, loose_only, LocalStoreMode::authoritative, domain);
  ASSERT_TRUE(wait_until([&] { return store->scan_complete(); }));

  Counts counts{*store, *domain};
  const auto a = pattern(128 * 1024, 44);
  const auto b = pattern(128 * 1024, 45);
  const auto c = pattern(128 * 1024, 46);
  const auto ga = store->put_deferred(object_id(a), a);
  const auto gb = store->put_deferred(object_id(b), b);
  ASSERT_TRUE(ga.has_value());
  ASSERT_TRUE(gb.has_value());
  barrier_through_window(*store, *gb, *domain, bed.time, 10ms);
  const auto gc = store->put_deferred(object_id(c), c);
  ASSERT_TRUE(gc.has_value());
  barrier_through_window(*store, *gc, *domain, bed.time, 10ms);

  EXPECT_EQ(counts.fsync_calls(), 1u);
  EXPECT_EQ(counts.syncfs_calls(), 2u);
}

// Macha: test_clean_accounting_checkpoint_is_trusted_with_packs. A clean
// checkpoint is trusted with packs: "dirty" is persisted before a session's
// first mutation, so a torn pack tail only sits behind a dirty checkpoint.
TEST(StoreLocalStoreDurability, ACleanAccountingCheckpointIsTrustedWithPacks) {
  StoreBed bed;
  const auto root = bed.dir.path() / "objects";
  // Objects at or below the threshold are packed.
  LocalStoreOptions packed;
  packed.limit = 64ULL * 1024 * 1024;
  packed.pack_threshold = 256 * 1024;
  packed.pack_target_size = 1024 * 1024;
  const auto a = pattern(48 * 1024, 150);
  const auto b = pattern(48 * 1024, 151);
  uint64_t clean_used = 0;
  {
    auto domain = bed.domain(root, 500ms);
    auto store = bed.open(root, packed, LocalStoreMode::authoritative, domain);
    ASSERT_TRUE(wait_until([&] { return store->scan_complete(); }));
    const auto generation = store->put_deferred(object_id(a), a);
    ASSERT_TRUE(generation.has_value());
    barrier_through_window(*store, *generation, *domain, bed.time, 500ms);
    clean_used = store->used();
    ASSERT_GT(clean_used, 0u);
  }
  bool have_packs = false;
  for (const auto& entry : std::filesystem::directory_iterator(root / "packs")) have_packs = have_packs || entry.is_regular_file();
  ASSERT_TRUE(have_packs);
  {
    // Clean shutdown above: no scan, accounting restored at once.
    auto store = bed.open(root, packed);
    EXPECT_TRUE(store->scan_complete());
    EXPECT_EQ(store->used(), clean_used);
    ASSERT_TRUE(store->get(object_id(a)).has_value());
  }

  // An unclean exit after a mutation: the checkpoint is dirty. The store
  // must still admit a put before the walk completes, on the estimate.
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      // Secure memory is not inherited across a fork: the child derives its
      // own key and never touches the parent's, not even to free it.
      auto domain = bed.domain(root, 500ms);
      auto store = std::make_unique<LocalStore>(root, packed, test::known_answer_key(), bed.services(), LocalStoreMode::authoritative, domain);
      if (!store->scan_complete()) ::_exit(20);
      const auto generation = store->put_deferred(object_id(b), b);
      if (!generation.has_value()) ::_exit(21);
      // Macha's child waited out the window; the window is not what this case
      // is about, and nothing in a child steps its time.
      store->durability_barrier(*generation, DurabilityUrgency::immediate);
      ::_exit(0);
    } catch (...) {
      ::_exit(22);
    }
  }
  ASSERT_EQ(exit_code(child), std::optional<int>(0));
  {
    auto recovered = bed.open(root, packed);
    EXPECT_GE(recovered->used(), clean_used);  // the dirty checkpoint's estimate
    const auto c = pattern(48 * 1024, 152);
    // Admitted whether or not the walk has finished yet.
    ASSERT_TRUE(recovered->put_deferred(object_id(c), c).has_value());
    ASSERT_TRUE(wait_until([&] { return recovered->scan_complete(); }));
    ASSERT_TRUE(recovered->get(object_id(b)).has_value());
    ASSERT_TRUE(recovered->get(object_id(c)).has_value());
    EXPECT_GT(recovered->used(), clean_used);
  }
}

// Macha: test_unclean_accounting_recovery_establishes_durable_baseline.
// Dirty accounting forces a reconciliation of the tree and one physical
// baseline before objects of generation zero can stand as durability proof.
TEST(StoreLocalStoreDurability, UncleanAccountingRecoveryEstablishesADurableBaseline) {
  StoreBed bed;
  const auto root = bed.dir.path() / "objects";
  const auto a = pattern(128 * 1024, 140);
  const auto b = pattern(128 * 1024, 141);
  uint64_t clean_used = 0;
  {
    auto store = bed.open(root, loose_only);
    ASSERT_TRUE(wait_until([&] { return store->scan_complete(); }));
    ASSERT_TRUE(store->put(object_id(a), a));
    clean_used = store->used();
  }

  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      // Secure memory is not inherited across a fork: the child derives its
      // own key and never touches the parent's, not even to free it.
      auto store = std::make_unique<LocalStore>(root, loose_only, test::known_answer_key(), bed.services());
      if (!store->scan_complete()) ::_exit(20);
      const auto generation = store->put_deferred(object_id(b), b);
      if (!generation.has_value()) ::_exit(21);
      // Skip the store's teardown: an unclean process exit, the OS still up.
      ::_exit(0);
    } catch (...) {
      ::_exit(22);
    }
  }
  ASSERT_EQ(exit_code(child), std::optional<int>(0));

  auto domain = bed.domain(root, 10ms);
  const auto barriers = domain->physical_barriers();
  {
    auto recovered = bed.open(root, loose_only, LocalStoreMode::authoritative, domain);
    ASSERT_TRUE(wait_until([&] { return recovered->scan_complete(); }));
    EXPECT_GT(recovered->used(), clean_used);
    ASSERT_TRUE(recovered->get(object_id(b)).has_value());
  }
  EXPECT_GE(domain->physical_barriers() - barriers, 1u);
}

// Macha: test_restart_durable_reaffirmation_requires_no_new_barrier.
TEST(StoreLocalStoreDurability, ARestartedReaffirmationNeedsNoNewBarrier) {
  StoreBed bed;
  const auto root = bed.dir.path() / "objects";
  const auto bytes = pattern(128 * 1024, 142);
  const auto id = object_id(bytes);
  {
    auto store = bed.open(root, loose_only);
    ASSERT_TRUE(wait_until([&] { return store->scan_complete(); }));
    ASSERT_TRUE(store->put(id, bytes));
  }

  auto domain = bed.domain(root, 100ms);
  auto reopened = bed.open(root, loose_only, LocalStoreMode::authoritative, domain);
  ASSERT_TRUE(reopened->scan_complete());

  Counts counts{*reopened, *domain};
  const auto generation = reopened->put_deferred(id, bytes);
  ASSERT_TRUE(generation.has_value());
  EXPECT_EQ(*generation, 0u);
  reopened->durability_barrier(*generation);
  EXPECT_EQ(counts.fsync_calls(), 0u);
  EXPECT_EQ(counts.syncfs_calls(), 0u);
}

// Macha: test_authoritative_delete_is_lazy_durability. The first mutation
// dirties the accounting once. The unlink itself is deliberately not a
// synchronous durability event: losing it in a crash leaks unreachable
// garbage, never published data (crash matrix: "GC unlink lost").
TEST(StoreLocalStoreDurability, AnAuthoritativeDeleteIsLazilyDurable) {
  StoreBed bed;
  const auto root = bed.dir.path() / "objects";
  const auto bytes = pattern(128 * 1024, 143);
  const auto id = object_id(bytes);
  {
    auto store = bed.open(root, loose_only);
    ASSERT_TRUE(wait_until([&] { return store->scan_complete(); }));
    ASSERT_TRUE(store->put(id, bytes));
  }

  auto domain = bed.domain(root, 100ms);
  auto reopened = bed.open(root, loose_only, LocalStoreMode::authoritative, domain);
  ASSERT_TRUE(reopened->scan_complete());
  Counts counts{*reopened, *domain};
  ASSERT_TRUE(reopened->remove(id));
  EXPECT_EQ(counts.fsync_calls(), 1u);
  EXPECT_EQ(counts.syncfs_calls(), 0u);
}

// Macha: test_local_store_barrier_remembers_complete_physical_cut. A barrier
// for one generation makes everything registered before its cut durable, so
// a later wait for a covered generation costs nothing.
TEST(StoreLocalStoreDurability, ABarrierRemembersTheWholeCut) {
  StoreBed bed;
  const auto root = bed.dir.path() / "objects";
  auto domain = bed.domain(root, 10ms);
  auto store = bed.open(root, loose_only, LocalStoreMode::authoritative, domain);
  ASSERT_TRUE(wait_until([&] { return store->scan_complete(); }));

  const auto a = pattern(256 * 1024, 47);
  const auto b = pattern(256 * 1024, 48);
  const auto ga = store->put_deferred(object_id(a), a);
  const auto gb = store->put_deferred(object_id(b), b);
  ASSERT_TRUE(ga.has_value());
  ASSERT_TRUE(gb.has_value());
  ASSERT_GT(*gb, *ga);

  {
    Counts counts{*store, *domain};
    barrier_through_window(*store, *ga, *domain, bed.time, 10ms);
    EXPECT_EQ(counts.syncfs_calls(), 1u);
    EXPECT_EQ(counts.fsync_calls(), 0u);
    EXPECT_GE(store->durable_generation(), *gb);
  }

  const auto c = pattern(256 * 1024, 49);
  const auto gc = store->put_deferred(object_id(c), c);
  ASSERT_TRUE(gc.has_value());
  ASSERT_GT(*gc, *gb);

  {
    Counts counts{*store, *domain};
    barrier_through_window(*store, *gb, *domain, bed.time, 10ms);
    EXPECT_EQ(counts.syncfs_calls(), 0u);
    EXPECT_EQ(counts.fsync_calls(), 0u);
  }
  Counts counts{*store, *domain};
  barrier_through_window(*store, *gc, *domain, bed.time, 10ms);
  EXPECT_EQ(counts.syncfs_calls(), 1u);
}

// Macha: test_durability_domain_group_commits_independent_publications.
// Three independent completions enter the same physical domain inside one
// batch window: exactly one barrier satisfies all three. Macha spaced them
// with 30 ms sleeps inside a 120 ms window; here the window is stepped time
// that only the test moves, so they are inside it by construction.
TEST(StoreLocalStoreDurability, ADomainGroupCommitsIndependentPublications) {
  StoreBed bed;
  const auto root = bed.dir.path() / "objects";
  auto domain = bed.domain(root, 120ms);
  auto store = bed.open(root, loose_only, LocalStoreMode::authoritative, domain);
  ASSERT_TRUE(wait_until([&] { return store->scan_complete(); }));

  const auto a = pattern(256 * 1024, 50);
  const auto ga = store->put_deferred(object_id(a), a);
  ASSERT_TRUE(ga.has_value());

  Counts counts{*store, *domain};
  const auto seen = bed.time.reads();
  auto first = std::async(std::launch::async, [&] { store->durability_barrier(*ga); });
  // The worker has read the time: the first is waiting in the window.
  ASSERT_TRUE(wait_until([&] { return bed.time.reads() > seen; }));

  const auto b = pattern(256 * 1024, 51);
  const auto gb = store->put_deferred(object_id(b), b);
  ASSERT_TRUE(gb.has_value());
  auto second = std::async(std::launch::async, [&] { store->durability_barrier(*gb); });

  const auto c = pattern(256 * 1024, 52);
  const auto gc = store->put_deferred(object_id(c), c);
  ASSERT_TRUE(gc.has_value());
  auto third = std::async(std::launch::async, [&] { store->durability_barrier(*gc); });
  EXPECT_NE(first.wait_for(0s), std::future_status::ready);

  bed.time.advance(120ms);
  domain->time_advanced();
  for (auto* waiter : {&first, &second, &third}) {
    ASSERT_TRUE(test::ready(*waiter));
    waiter->get();
  }
  EXPECT_EQ(counts.syncfs_calls(), 1u);
  EXPECT_GE(domain->durable_generation(), *gc);
}

// Macha: test_immediate_reaffirmation_flushes_provisional_generation. A
// strict re-put of an object whose deferred generation is not yet durable
// waits for exactly that generation.
TEST(StoreLocalStoreDurability, AnImmediateReaffirmationFlushesTheProvisionalGeneration) {
  StoreBed bed;
  const auto root = bed.dir.path() / "objects";
  auto domain = bed.domain(root, 50ms);
  auto store = bed.open(root, loose_only, LocalStoreMode::authoritative, domain);
  ASSERT_TRUE(wait_until([&] { return store->scan_complete(); }));

  const auto bytes = pattern(256 * 1024, 35);
  const auto id = object_id(bytes);
  ASSERT_TRUE(store->put_deferred(id, bytes).has_value());

  Counts counts{*store, *domain};
  ASSERT_TRUE(store->put(id, bytes));
  EXPECT_EQ(counts.syncfs_calls(), 1u);
  EXPECT_EQ(counts.fsync_calls(), 0u);
}

}  // namespace
}  // namespace dagda::store
