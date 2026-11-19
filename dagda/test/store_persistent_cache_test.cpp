//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement S19: the persistent block cache, ephemeral and least recently
// used, and crash matrix row "Cache missing/corrupt" (discard and rebuild).
//
// Moved from Macha (develop 5a06d3c): tests/test_invariants.cpp:
// test_persistent_cache_is_explicitly_ephemeral, every assertion kept;
// tests/test_storage_metadata.cpp: the cache half of
// test_storage_pool_and_persistent_cache (its pool half is in
// store_storage_pool_test). Kept but for the six that read and write the
// metadata seed (remember_metadata, metadata(), and the seed's hash before
// and after a reopen and a reconfiguration), which are the heads step's
// (requirement H25); in their place the seam the seed attaches to is checked
// at the same points. Macha counted fsync and syncfs by interposing them,
// Linux only; here a cache makes no durability domain, so no barrier is
// recorded in the node's observations, and keeps no accounting checkpoint to
// sync. Macha's 2 ms sleeps between puts ordered nothing: the order is the
// LRU list's.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "dagda/store/durability_domain.h"
#include "dagda/store/object_id.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/test/fixtures/store.h"
#include "dagda/test/fixtures/sync_count.h"

namespace dagda::store {
namespace {

using test::pattern;
using test::StoreBed;

using test::SyncCount;

bool any_barrier_recorded(dannan::status::Observations& observations) {
  const auto snapshot = observations.snapshot();
  for (const auto& [name, histogram] : snapshot.histograms)
    if (name.rfind("durability.", 0) == 0 && histogram.count) return true;
  return false;
}

// Macha: test_persistent_cache_is_explicitly_ephemeral.
TEST(StorePersistentCache, TheCacheIsExplicitlyEphemeral) {
  StoreBed bed;
  PersistentBlockCache cache({bed.dir.path() / "cache", 2, true}, bed.key, bed.services());

  const auto a = pattern(64 * 1024, 41);
  const auto b = pattern(64 * 1024, 42);
  const auto c = pattern(64 * 1024, 43);
  ASSERT_TRUE(cache.put(object_id(a), a));
  ASSERT_TRUE(cache.put(object_id(b), b));
  ASSERT_TRUE(cache.put(object_id(c), c));

  EXPECT_EQ(cache.blocks(), 2u);

  // The cache reports hits, misses and evictions, not only occupancy.
  {
    const auto before = cache.stats();
    EXPECT_EQ(before.entries, 2u);
    // Three puts into a two-block cache: one eviction.
    EXPECT_EQ(before.evictions, 1u);

    EXPECT_FALSE(cache.get(object_id(pattern(1024, 99))));
    EXPECT_EQ(cache.stats().misses, before.misses + 1);
    EXPECT_EQ(cache.stats().hits, before.hits);

    // `c` was the last put, so it survived the eviction that took `a`.
    ASSERT_TRUE(cache.get(object_id(c)));
    EXPECT_EQ(cache.stats().hits, before.hits + 1);
    EXPECT_EQ(cache.stats().misses, before.misses + 1);
  }
  // Macha's fsync_calls == 0 and syncfs_calls == 0.
  EXPECT_FALSE(any_barrier_recorded(bed.observations));
  EXPECT_FALSE(std::filesystem::exists(bed.dir.path() / "cache" / ".macha.accounting"));
}

// Macha: test_persistent_cache_is_explicitly_ephemeral, its fsync_calls and
// syncfs_calls, by the interposed count (the Macha session's answers,
// 2026-11-05): opening, three puts, an eviction, a hit, a miss and closing ask
// nothing of stable storage. A real barrier in the same library is counted, so
// a count of zero says something.
TEST(StorePersistentCache, TheCacheMakesNoCallForStableStorage) {
  StoreBed bed;
  {
    const auto root = bed.dir.path() / "barrier";
    auto domain = bed.domain(root, std::chrono::milliseconds(0));
    std::ofstream(root / "mutated") << "bytes";
    SyncCount seen;
    domain->complete_mutation(root / "mutated", root);
    domain->flush();
    ASSERT_GE(seen.count(), 1u) << "the count does not see the library's calls";
  }

  const auto a = pattern(64 * 1024, 41);
  const auto b = pattern(64 * 1024, 42);
  const auto c = pattern(64 * 1024, 43);
  SyncCount seen;
  {
    PersistentBlockCache cache({bed.dir.path() / "cache", 2, true}, bed.key, bed.services());
    ASSERT_TRUE(cache.put(object_id(a), a));
    ASSERT_TRUE(cache.put(object_id(b), b));
    ASSERT_TRUE(cache.put(object_id(c), c));
    EXPECT_EQ(cache.stats().evictions, 1u);
    ASSERT_TRUE(cache.get(object_id(c)));
    EXPECT_FALSE(cache.get(object_id(pattern(1024, 99))));
  }
  EXPECT_EQ(seen.count(), 0u);
}

// Macha (tests/test_storage_metadata.cpp): test_storage_pool_and_persistent_cache,
// its cache half. Least recently used out; reopened with what it held;
// disabled and enabled live. The seed's place is offered while the cache is
// on and its seed allowed, and moves on with every reconfiguration.
TEST(StorePersistentCache, TheCacheEvictsTheLeastRecentlyUsedAndReopens) {
  StoreBed bed;
  const auto cache_root = bed.dir.path() / "cache";
  std::optional<PersistentBlockCache::SeedPlace> first_place;
  {
    PersistentBlockCache cache({cache_root, 2, true}, bed.key, bed.services());
    auto a = pattern(8192);
    auto b = pattern(8193);
    auto c = pattern(8194);
    a[0] ^= 0x11;
    b[0] ^= 0x22;
    c[0] ^= 0x33;
    const auto ia = object_id(a);
    const auto ib = object_id(b);
    const auto ic = object_id(c);
    ASSERT_TRUE(cache.put(ia, a));
    ASSERT_TRUE(cache.put(ib, b));
    ASSERT_TRUE(cache.get(ia).has_value());  // a is now the hotter block.
    ASSERT_TRUE(cache.put(ic, c));
    EXPECT_EQ(cache.blocks(), 2u);
    EXPECT_TRUE(cache.has(ia));
    EXPECT_TRUE(cache.has(ic));
    EXPECT_FALSE(cache.has(ib));

    // In place of remember_metadata() and metadata(): the seed's place.
    EXPECT_TRUE(cache.with_seed([&](const PersistentBlockCache::SeedPlace& place) { first_place = place; }));
    ASSERT_TRUE(first_place.has_value());
    EXPECT_EQ(first_place->directory, cache_root / "metadata");
  }
  {
    PersistentBlockCache reopened({cache_root, 2, true}, bed.key, bed.services());
    EXPECT_EQ(reopened.blocks(), 2u);
    EXPECT_TRUE(reopened.with_seed([&](const PersistentBlockCache::SeedPlace& place) { EXPECT_EQ(place.directory, first_place->directory); }));

    reopened.reconfigure({cache_root, 0, true});
    EXPECT_FALSE(reopened.enabled());
    EXPECT_FALSE(reopened.with_seed([](const PersistentBlockCache::SeedPlace&) { ADD_FAILURE() << "a disabled cache offers no seed"; }));
    reopened.reconfigure({cache_root, 2, true});
    EXPECT_TRUE(reopened.enabled());
    std::optional<PersistentBlockCache::SeedPlace> again;
    EXPECT_TRUE(reopened.with_seed([&](const PersistentBlockCache::SeedPlace& place) { again = place; }));
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->directory, cache_root / "metadata");
    // A seed remembered before the reconfiguration is written again.
    std::optional<PersistentBlockCache::SeedPlace> later;
    reopened.reconfigure({cache_root, 2, true});
    EXPECT_TRUE(reopened.with_seed([&](const PersistentBlockCache::SeedPlace& place) { later = place; }));
    EXPECT_NE(later->opening, again->opening);

    // No seed kept beside the cache when its owner says not to.
    reopened.reconfigure({cache_root, 2, false});
    EXPECT_TRUE(reopened.enabled());
    EXPECT_FALSE(reopened.with_seed([](const PersistentBlockCache::SeedPlace&) { ADD_FAILURE() << "seed not allowed"; }));
  }
}

// Crash matrix "Cache missing/corrupt": discard and rebuild. A corrupt entry
// is a miss and is removed; a cache whose directory is gone starts empty; one
// that cannot open is off, never an error.
TEST(StorePersistentCache, AMissingOrCorruptCacheIsDiscardedAndRebuilt) {
  StoreBed bed;
  const auto cache_root = bed.dir.path() / "cache";
  const auto a = pattern(32 * 1024, 1);
  const auto b = pattern(32 * 1024, 2);
  {
    PersistentBlockCache cache({cache_root, 4, true}, bed.key, bed.services());
    ASSERT_TRUE(cache.put(object_id(a), a));
    ASSERT_TRUE(cache.put(object_id(b), b));
  }
  test::corrupt_object(cache_root, object_id(a));
  {
    PersistentBlockCache cache({cache_root, 4, true}, bed.key, bed.services());
    EXPECT_EQ(cache.blocks(), 2u);
    const auto before = cache.stats();
    EXPECT_FALSE(cache.get(object_id(a)).has_value());
    EXPECT_EQ(cache.stats().misses, before.misses + 1);
    EXPECT_FALSE(cache.has(object_id(a)));
    EXPECT_EQ(cache.blocks(), 1u);
    EXPECT_EQ(cache.get(object_id(b)), std::optional<Bytes>{b});
    // It can be cached again.
    ASSERT_TRUE(cache.put(object_id(a), a));
    EXPECT_EQ(cache.get(object_id(a)), std::optional<Bytes>{a});
  }

  std::filesystem::remove_all(cache_root);
  {
    PersistentBlockCache cache({cache_root, 4, true}, bed.key, bed.services());
    EXPECT_TRUE(cache.enabled());
    EXPECT_EQ(cache.blocks(), 0u);
    EXPECT_FALSE(cache.has(object_id(b)));
    ASSERT_TRUE(cache.put(object_id(b), b));
    EXPECT_EQ(cache.get(object_id(b)), std::optional<Bytes>{b});
  }

  // A file where the cache directory should be: the cache is off.
  const auto blocked = bed.dir.path() / "blocked";
  std::ofstream(blocked) << "not a directory";
  PersistentBlockCache off({blocked, 4, true}, bed.key, bed.services());
  EXPECT_FALSE(off.enabled());
  EXPECT_FALSE(off.put(object_id(a), a));
  EXPECT_FALSE(off.get(object_id(a)).has_value());
  EXPECT_TRUE(bed.sink->saw("persistent cache disabled"));
}

}  // namespace
}  // namespace dagda::store
