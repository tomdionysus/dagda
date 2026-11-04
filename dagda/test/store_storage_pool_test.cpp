//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirements S10, S12, S14, S15, S18, L4 and L21 at the DATA pool: backends
// ranked by capacity placement exactly as Macha ranks them, a refused put
// falling through, backend identity, tickets bound to their process, domain
// and incarnation, presence and the held identity across disks going and
// coming, rebalance, scrub and the sweep with its claim seam.
//
// Moved from Macha (develop 5a06d3c), every assertion kept:
// tests/test_storage_v18.cpp: the StoragePool and placement blocks of
// test_admission_respects_reserve_and_capacity, and the backend half of
// test_unversioned_state_is_refused (its state-layout half is the node's
// identity, which is Dannan's); tests/test_storage_metadata.cpp: the pool
// half of test_storage_pool_and_persistent_cache (its cache half is in
// store_persistent_cache_test); tests/test_invariants.cpp:
// test_rebalance_reads_nothing_with_one_backend_online,
// test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy,
// test_storage_pool_tokens_name_physical_domain_and_backend_incarnation;
// tests/test_presence.cpp: test_a_backend_offline_and_readopted_leaves_presence_exact,
// test_the_held_identity_names_exactly_what_the_pool_holds,
// test_the_held_identity_is_the_same_on_any_number_of_disks;
// tests/test_baselines.cpp: test_baseline_data_store_has. Macha's cluster
// key file becomes domain v1's known-answer key, its node id file a node id
// handed in. Macha counted syncfs by interposing it, Linux only, so the
// tickets case was skipped elsewhere; here it counts the pool's domains'
// barriers and runs everywhere. The 20 ms sleep that let a modification time
// move is the wall clock stepped instead.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dagda/contract/id_lookup.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/store/local_store.h"
#include "dagda/store/object_id.h"
#include "dagda/store/placement.h"
#include "dagda/store/storage_pool.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/identity/crypto.h"
#include "dannan/placement/placement.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using dannan::identity::NodeId;
using test::corrupt_object;
using test::numbered;
using test::object_path;
using test::pattern;
using test::StoreBed;
using test::wait_until;

constexpr uint64_t mib = 1024 * 1024;

NodeId filled(uint8_t byte) {
  NodeId id;
  id.bytes.fill(byte);
  return id;
}

// Macha: test_admission_respects_reserve_and_capacity, its StoragePool block
// (its LocalStore block is in store_local_store_test). A pool falls through
// to a backend with room once the preferred one refuses.
TEST(StoreStoragePool, ARefusedPutFallsThroughToABackendWithRoom) {
  StoreBed bed;
  const auto small = bed.dir.path() / "small";
  const auto large = bed.dir.path() / "large";
  std::filesystem::create_directories(small);
  std::filesystem::create_directories(large);
  auto pool = bed.pool(bed.dir.path() / "state", {{small, 512ULL * 1024, 0}, {large, 16 * mib, 0}}, 0ms);
  std::vector<ObjectId> ids;
  for (size_t i = 0; i < 40; ++i) {
    auto data = pattern(192 * 1024 + i);
    data[0] ^= static_cast<uint8_t>(i);
    const auto id = object_id(data);
    ASSERT_TRUE(pool->put(id, data));
    ids.push_back(id);
  }
  EXPECT_GT(pool->used(), 512ULL * 1024);
  EXPECT_EQ(pool->limit(), 512ULL * 1024 + 16 * mib);
  for (const auto& id : ids) EXPECT_TRUE(pool->valid(id));
}

// Macha: test_admission_respects_reserve_and_capacity, its placement block,
// through the store's scheme: one copy's logical capacity is the sum of the
// nodes, not the smallest.
TEST(StoreStoragePool, OneCopysLogicalCapacityIsTheSumOfTheNodes) {
  auto small_id = NodeId{};
  small_id.bytes[0] = 1;
  auto large_id = NodeId{};
  large_id.bytes[0] = 2;
  const uint64_t small = 1ULL * 1024 * 1024 * 1024;
  const uint64_t large = 2ULL * 1024 * 1024 * 1024 * 1024;
  EXPECT_EQ(dannan::placement::logical_capacity({placement_candidate(small_id, small), placement_candidate(large_id, large)}, 1, capacity_advert),
            small + large);
}

// Known answers, computed outside both codebases from Macha's
// capacity_placement_nodes with one copy (cluster/placement.cpp:333-346):
// SHA-256 over the id's first four bytes, "macha/data-fallback/v7" and the
// backend's token; u = (first eight bytes + 1) / (2^64 + 1); score -ln(u) /
// capacity (a capacity of zero weighs 1); lowest first, ties to the lower
// token. Computed at 60 digits; the closest two scores differ by 0.28%, so
// no rounding of the score can reorder them.
TEST(StoreStoragePool, RanksBackendsAsMachaDoes) {
  const std::vector<dannan::membership::NodeRecord> backends{placement_candidate(filled(0x11), 1ULL << 30), placement_candidate(filled(0x22), 2ULL << 30),
                                                             placement_candidate(filled(0x33), 4ULL << 30), placement_candidate(filled(0x44), 0)};
  const std::vector<std::vector<uint8_t>> expected{
      {0x22, 0x33, 0x11, 0x44}, {0x33, 0x22, 0x11, 0x44}, {0x33, 0x11, 0x22, 0x44}, {0x33, 0x11, 0x22, 0x44},
      {0x33, 0x11, 0x22, 0x44}, {0x11, 0x33, 0x22, 0x44}, {0x33, 0x22, 0x11, 0x44}, {0x33, 0x11, 0x22, 0x44},
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    const auto text = "dagda placement known answer " + std::to_string(i);
    const auto id = object_id({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    const auto order = data_order(id.bytes, backends, 1);
    ASSERT_EQ(order.size(), expected[i].size()) << i;
    for (size_t rank = 0; rank < order.size(); ++rank) EXPECT_EQ(order[rank], filled(expected[i][rank])) << "object " << i << " rank " << rank;
  }
  EXPECT_EQ(object_id({reinterpret_cast<const uint8_t*>("dagda placement known answer 0"), 30}).bytes[0], 0x15);
  // The scheme is the store's own and has no default in Dannan.
  EXPECT_EQ(data_scheme().fallback_salt, "macha/data-fallback/v7");
  EXPECT_EQ(data_scheme().extra_domain_salt, "macha/data-extra-domain/v7");
  EXPECT_EQ(data_scheme().capacity_advert, capacity_advert);
}

// The pool puts each object on the backend it ranks first when that one
// takes it.
TEST(StoreStoragePool, AnObjectLandsOnItsFirstRankedBackend) {
  StoreBed bed;
  const auto a = bed.dir.path() / "a";
  const auto b = bed.dir.path() / "b";
  std::filesystem::create_directories(a);
  std::filesystem::create_directories(b);
  auto pool = bed.pool(bed.dir.path() / "state", {{a, 64 * mib, 0}, {b, 192 * mib, 0}});
  ASSERT_EQ(pool->online_backends(), 2u);
  // The tokens the pool adopted, from the markers it wrote.
  const auto token_of = [](const std::filesystem::path& backend) {
    std::ifstream in(backend / ".macha.backend");
    std::string version, node, token;
    in >> version >> node >> token;
    return NodeId::parse(token).value();
  };
  const std::vector<dannan::membership::NodeRecord> candidates{placement_candidate(token_of(a), 64 * mib), placement_candidate(token_of(b), 192 * mib)};
  for (uint64_t n = 0; n < 64; ++n) {
    const auto bytes = numbered(n);
    const auto id = object_id(bytes);
    ASSERT_TRUE(pool->put(id, bytes));
    const auto first = data_order(id.bytes, candidates, 1).front() == token_of(a) ? a : b;
    EXPECT_TRUE(std::filesystem::exists(object_path(first, id))) << n;
    EXPECT_FALSE(std::filesystem::exists(object_path(first == a ? b : a, id))) << n;
  }
}

// Layout v18's marker, byte for byte: what the pool writes for an empty
// directory it adopts, and what it accepts that Macha wrote.
TEST(StoreStoragePool, TheBackendMarkerIsLayoutV18s) {
  StoreBed bed;
  const auto state = bed.dir.path() / "state";
  const auto fresh = bed.dir.path() / "fresh";
  const auto machas = bed.dir.path() / "machas";
  std::filesystem::create_directories(fresh);
  std::filesystem::create_directories(machas);
  const auto node = filled(0xab);
  const auto token = filled(0xcd);
  const std::string machas_marker = "macha-backend-v18 abababababababababababababababab cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd\n";
  std::ofstream(machas / ".macha.backend") << machas_marker;

  auto pool = bed.pool(state, {{fresh, 64 * mib, 0}, {machas, 64 * mib, 0}}, 500ms, node);
  ASSERT_EQ(pool->online_backends(), 2u);

  const auto read = [](const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
  };
  const auto marker = read(fresh / ".macha.backend");
  ASSERT_EQ(marker.size(), 17u + 1 + 32 + 1 + 32 + 1);
  EXPECT_EQ(marker.substr(0, 51), "macha-backend-v18 abababababababababababababababab ");
  EXPECT_TRUE(NodeId::parse(marker.substr(51, 32)).has_value());
  EXPECT_EQ(marker.back(), '\n');

  // Mirrored under the state, named by the SHA-256 of the backend's path.
  const auto mirror_of = [&](const std::filesystem::path& backend) {
    const auto text = backend.lexically_normal().string();
    return state / "backend-identities" /
           (dannan::identity::hex(dannan::identity::sha256({reinterpret_cast<const uint8_t*>(text.data()), text.size()})) + ".id");
  };
  EXPECT_EQ(read(mirror_of(fresh)), marker);
  EXPECT_EQ(read(mirror_of(machas)), machas_marker);
  EXPECT_EQ(read(machas / ".macha.backend"), machas_marker);
  // The held ledger is named by the disk's token.
  EXPECT_TRUE(std::filesystem::exists(state / "ledger" / ("data-" + dannan::identity::to_string(token))));
}

// Requirement S12: a disk of another node, the wrong disk where a known one
// was, and a non-empty unmarked directory each stay offline; an empty one is
// adopted.
TEST(StoreStoragePool, ABackendThatIsNotThisNodesDiskIsRefused) {
  StoreBed bed;
  const auto state = bed.dir.path() / "state";
  const auto disk = bed.dir.path() / "disk";
  const auto other = bed.dir.path() / "other";
  std::filesystem::create_directories(disk);
  std::filesystem::create_directories(other);
  const auto node = filled(0x01);
  {
    auto pool = bed.pool(state, {{disk, 64 * mib, 0}}, 500ms, node);
    ASSERT_EQ(pool->online_backends(), 1u);
  }
  {
    // Another node's pool will not take it.
    auto pool = bed.pool(bed.dir.path() / "other-state", {{disk, 64 * mib, 0}}, 500ms, filled(0x02));
    EXPECT_EQ(pool->online_backends(), 0u);
    EXPECT_TRUE(pool->degraded());
  }
  {
    // A different disk mounted where this node's was.
    auto pool = bed.pool(bed.dir.path() / "other-disk-state", {{other, 64 * mib, 0}}, 500ms, node);
    ASSERT_EQ(pool->online_backends(), 1u);
  }
  std::filesystem::rename(disk, bed.dir.path() / "disk.away");
  std::filesystem::rename(other, disk);
  {
    auto pool = bed.pool(state, {{disk, 64 * mib, 0}}, 500ms, node);
    EXPECT_EQ(pool->online_backends(), 0u);
  }
  // The known disk's marker gone: refused, not re-adopted.
  std::filesystem::remove(disk / ".macha.backend");
  {
    auto pool = bed.pool(state, {{disk, 64 * mib, 0}}, 500ms, node);
    EXPECT_EQ(pool->online_backends(), 0u);
  }
}

// Macha: test_unversioned_state_is_refused, its backend half. A non-empty
// unversioned DATA backend leaves the pool up with that backend offline, and
// the plane says it is degraded rather than ready.
TEST(StoreStoragePool, ANonEmptyUnversionedBackendIsRefused) {
  StoreBed bed;
  const auto old_data = bed.dir.path() / "old-data";
  std::filesystem::create_directories(old_data / "objects");
  std::ofstream(old_data / "objects" / "legacy") << "old namespace";
  auto pool = bed.pool(bed.dir.path() / "state", {{old_data, 8 * mib, 0}});
  EXPECT_EQ(pool->online_backends(), 0u);
  EXPECT_EQ(pool->limit(), 0u);
  EXPECT_TRUE(pool->degraded());
  EXPECT_FALSE(std::filesystem::exists(old_data / ".macha.backend"));

  // An edge node with no backends is not degraded: it holds nothing by
  // design.
  auto edge = bed.pool(bed.dir.path() / "edge-state", {});
  EXPECT_FALSE(edge->degraded());
  EXPECT_EQ(edge->limit(), 0u);
}

// Macha (tests/test_storage_metadata.cpp): test_storage_pool_and_persistent_cache,
// its pool half. Puts and gets over two disks; a resumable traversal; scrub,
// rebalance and the sweep in bounded slices that yield; a re-put reaffirms
// age; a third disk added live and rebalanced onto; a disk away and back
// keeps its weight; a backend removed and re-added live; placement weighted
// by capacity.
TEST(StoreStoragePool, PoolsSpreadTraverseSweepAndRebalance) {
  StoreBed bed;
  const auto state = bed.dir.path() / "state";
  const auto disk1 = bed.dir.path() / "disk1";
  const auto disk2 = bed.dir.path() / "disk2";
  const auto disk3 = bed.dir.path() / "disk3";
  std::filesystem::create_directories(disk1);
  std::filesystem::create_directories(disk2);
  std::filesystem::create_directories(disk3);

  const auto node = NodeId::random();
  auto pool = bed.pool(state, {{disk1, 64 * mib}, {disk2, 64 * mib}}, 500ms, node);
  EXPECT_EQ(pool->online_backends(), 2u);
  EXPECT_EQ(pool->limit(), 128 * mib);

  std::vector<std::pair<ObjectId, Bytes>> objects;
  for (size_t i = 0; i < 32; ++i) {
    auto data = pattern(128 * 1024 + i);
    data[0] ^= static_cast<uint8_t>(i);
    const auto id = object_id(data);
    ASSERT_TRUE(pool->put(id, data));
    objects.emplace_back(id, std::move(data));
  }
  for (const auto& [id, data] : objects) {
    const auto got = pool->get(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, data);
  }

  // Maintenance traversal is resumable. A one-object slice must not rebuild
  // or consume the whole object namespace, and a complete pass visits every
  // physical object without blocking foreground pool operations.
  {
    StoragePool::Cursor cursor;
    std::set<ObjectId> seen;
    bool complete = false;
    size_t calls = 0;
    while (!complete && calls++ < 256) {
      const auto id = pool->next_object(cursor, complete);
      if (id) seen.insert(*id);
    }
    EXPECT_TRUE(complete);
    EXPECT_EQ(seen.size(), objects.size());
  }
  {
    size_t slices = 0;
    bool complete = false;
    while (!complete && slices++ < 256) {
      const auto step = pool->scrub_step(0, 1);
      EXPECT_LE(step.objects, 1u);
      complete = step.complete;
    }
    EXPECT_TRUE(complete);
    EXPECT_GT(slices, 1u);
  }
  {
    const auto yielded = pool->rebalance_step(0, 1, [] { return true; });
    EXPECT_TRUE(yielded.yielded);
    EXPECT_EQ(yielded.objects, 0u);
    EXPECT_EQ(yielded.bytes, 0u);
  }

  // The sweep is a separate bounded physical cursor. It keeps live and
  // recently retired objects, ignores young uncommitted ones, and removes an
  // old orphan with no committed reference or tombstone.
  const auto put_gc_object = [&](uint8_t tag) {
    auto data = pattern(96 * 1024 + tag);
    data[0] ^= tag;
    const auto id = object_id(data);
    EXPECT_TRUE(pool->put(id, data));
    return std::pair<ObjectId, Bytes>{id, std::move(data)};
  };
  const auto gc_live = put_gc_object(0x31).first;
  const auto gc_protected = put_gc_object(0x32).first;
  const auto gc_orphan = put_gc_object(0x33).first;
  const auto gc_young = put_gc_object(0x34).first;
  const auto age_object = [&](const ObjectId& id) {
    for (const auto& disk : {disk1, disk2}) {
      const auto path = object_path(disk, id);
      if (std::filesystem::exists(path)) std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - 48h);
    }
  };
  age_object(gc_live);
  age_object(gc_protected);
  age_object(gc_orphan);

  std::vector<ObjectId> gc_live_set{gc_live};
  std::vector<ObjectId> gc_protected_set{gc_protected};
  bool gc_complete = false;
  size_t gc_slices = 0;
  uint64_t gc_reclaimed = 0;
  while (!gc_complete && gc_slices++ < 256) {
    const auto step = pool->gc_step(gc_live_set, gc_protected_set, 24h, 3);
    EXPECT_LE(step.objects, 3u);
    gc_reclaimed += step.bytes;
    gc_complete = step.complete;
  }
  EXPECT_TRUE(gc_complete);
  EXPECT_GT(gc_slices, 1u);
  EXPECT_GT(gc_reclaimed, 0u);
  EXPECT_TRUE(pool->has(gc_live));
  EXPECT_TRUE(pool->has(gc_protected));
  EXPECT_FALSE(pool->has(gc_orphan));
  EXPECT_TRUE(pool->has(gc_young));
  const auto gc_yielded = pool->gc_step(gc_live_set, gc_protected_set, 24h, 1, [] { return true; });
  EXPECT_TRUE(gc_yielded.yielded);
  EXPECT_EQ(gc_yielded.objects, 0u);

  // Re-putting an identical hash reaffirms its physical age, so a new
  // uncommitted write cannot reuse an old orphan about to be collected.
  const auto gc_reaffirmed_object = put_gc_object(0x35);
  const auto gc_reaffirmed = gc_reaffirmed_object.first;
  age_object(gc_reaffirmed);
  EXPECT_TRUE(pool->older_than(gc_reaffirmed, 24h));
  ASSERT_TRUE(pool->put(gc_reaffirmed, gc_reaffirmed_object.second));
  EXPECT_FALSE(pool->older_than(gc_reaffirmed, 24h));

  // A third disk added live, and local placement migrated onto it without
  // changing the node's identity or replica accounting.
  pool->reconfigure({{disk1, 64 * mib}, {disk2, 64 * mib}, {disk3, 64 * mib}});
  pool->refresh();
  EXPECT_EQ(pool->online_backends(), 3u);
  (void)pool->rebalance_once();
  size_t on_disk3 = 0;
  for (const auto& [id, _] : objects) on_disk3 += std::filesystem::exists(object_path(disk3, id)) ? 1 : 0;
  EXPECT_GT(on_disk3, 0u);

  // A temporary disappearance does not change placement weight: the node
  // falls back to the surviving disks without remapping the pool, and the
  // returning disk resumes its share as rebalance converges.
  const auto parked = bed.dir.path() / "disk2.offline";
  std::filesystem::rename(disk2, parked);
  pool->refresh();
  EXPECT_EQ(pool->online_backends(), 2u);
  EXPECT_EQ(pool->limit(), 192 * mib);
  // Macha checked here that the node's id was unchanged; the node id is
  // handed in and fixed for the pool's life.
  std::filesystem::rename(parked, disk2);
  pool->refresh();
  EXPECT_EQ(pool->online_backends(), 3u);
  (void)pool->rebalance_once();

  // Removing a backend from the configuration and adding it back is live too.
  pool->reconfigure({{disk1, 64 * mib}, {disk2, 64 * mib}});
  pool->refresh();
  EXPECT_EQ(pool->online_backends(), 2u);
  EXPECT_EQ(pool->limit(), 128 * mib);
  pool->reconfigure({{disk1, 64 * mib}, {disk2, 64 * mib}, {disk3, 64 * mib}});
  pool->refresh();
  EXPECT_EQ(pool->online_backends(), 3u);

  // Local placement is weighted by capacity: a backend eight times larger
  // takes overwhelmingly more objects, and live free space plays no part.
  const auto small_disk = bed.dir.path() / "weighted-small";
  const auto large_disk = bed.dir.path() / "weighted-large";
  std::filesystem::create_directories(small_disk);
  std::filesystem::create_directories(large_disk);
  auto weighted = bed.pool(bed.dir.path() / "weighted-state", {{small_disk, 8 * mib}, {large_disk, 64 * mib}});
  size_t small_objects = 0;
  size_t large_objects = 0;
  for (size_t i = 0; i < 512; ++i) {
    auto data = pattern(1024 + i);
    data[0] ^= static_cast<uint8_t>(i);
    data[1] ^= static_cast<uint8_t>(i >> 8U);
    const auto id = object_id(data);
    ASSERT_TRUE(weighted->put(id, data));
    small_objects += std::filesystem::exists(object_path(small_disk, id)) ? 1 : 0;
    large_objects += std::filesystem::exists(object_path(large_disk, id)) ? 1 : 0;
  }
  EXPECT_EQ(small_objects + large_objects, 512u);
  EXPECT_GT(large_objects, small_objects * 5);
}

// Macha: test_rebalance_reads_nothing_with_one_backend_online. With one
// backend there is nowhere to move an object: rebalance says it is complete
// without reading the store.
TEST(StoreStoragePool, RebalanceReadsNothingWithOneBackendOnline) {
  StoreBed bed;
  const auto only = bed.dir.path() / "only";
  std::filesystem::create_directories(only);
  auto pool = bed.pool(bed.dir.path() / "pool-state", {{only, 64 * mib}});
  ASSERT_TRUE(wait_until([&] { return pool->online_backends() == 1; }));
  for (uint8_t i = 0; i < 8; ++i) {
    const auto bytes = pattern(4096, i);
    ASSERT_TRUE(pool->put(object_id(bytes), bytes));
  }
  const auto step = pool->rebalance_step(8 * mib, 64);
  EXPECT_TRUE(step.complete);
  EXPECT_EQ(step.objects, 0u);
  EXPECT_EQ(step.bytes, 0u);
}

// Macha: test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy.
TEST(StoreStoragePool, RebalanceNeverDeletesTheLastValidCopyForACorruptPreferredOne) {
  StoreBed bed;
  const auto a = bed.dir.path() / "a";
  const auto b = bed.dir.path() / "b";
  std::filesystem::create_directories(a);
  std::filesystem::create_directories(b);
  const auto bytes = pattern(96 * 1024, 10);
  const auto id = object_id(bytes);
  const auto pool_state = bed.dir.path() / "pool-state";
  const auto pool_node = NodeId::random();
  const std::vector<StorageBackendOptions> backends{{a, 64 * mib}, {b, 64 * mib}};

  // Initialise backend identity while the stores are empty, then seed objects
  // through LocalStore, so reopening the pool sees validly formatted media.
  {
    auto initialise = bed.pool(pool_state, backends, 500ms, pool_node);
    ASSERT_TRUE(wait_until([&] { return initialise->online_backends() == 2; }));
  }
  {
    auto sa = bed.open(a, LocalStoreOptions::loose_only(64 * mib));
    auto sb = bed.open(b, LocalStoreOptions::loose_only(64 * mib));
    ASSERT_TRUE(wait_until([&] { return sa->scan_complete() && sb->scan_complete(); }));
    ASSERT_TRUE(sa->put(id, bytes));
    ASSERT_TRUE(sb->put(id, bytes));
  }

  auto pool = bed.pool(pool_state, backends, 500ms, pool_node);
  ASSERT_TRUE(wait_until([&] { return pool->online_backends() == 2; }));

  // A pool put reaffirms (touches) only the preferred backend.
  const auto a_before = std::filesystem::last_write_time(object_path(a, id));
  bed.wall.advance(1s);
  ASSERT_TRUE(pool->put(id, bytes));
  const auto a_after = std::filesystem::last_write_time(object_path(a, id));
  const auto preferred = a_after != a_before ? a : b;
  const auto secondary = preferred == a ? b : a;
  ASSERT_TRUE(std::filesystem::exists(object_path(secondary, id)));

  corrupt_object(preferred, id);
  for (int i = 0; i < 8; ++i) {
    if (pool->rebalance_step(8 * mib, 8).complete) break;
  }

  // Rebalance must not remove the last valid copy until the repaired
  // preferred replica has passed strong validation.
  EXPECT_TRUE(std::filesystem::exists(object_path(secondary, id)) || pool->valid(id));
  bool readable = false;
  try {
    const auto data = pool->get(id);
    readable = data && *data == bytes;
  } catch (...) {
    readable = false;
  }
  EXPECT_TRUE(readable);
}

// Macha: test_storage_pool_tokens_name_physical_domain_and_backend_incarnation.
// Two backends on one filesystem share a domain but not an incarnation, and
// one barrier satisfies a ticket from each. Macha's 100 ms window is stepped
// time here.
TEST(StoreStoragePool, TicketsNameThePhysicalDomainAndTheBackendIncarnation) {
  StoreBed bed;
  const auto disk_a = bed.dir.path() / "disk-a";
  const auto disk_b = bed.dir.path() / "disk-b";
  std::filesystem::create_directories(disk_a);
  std::filesystem::create_directories(disk_b);
  auto pool = bed.pool(bed.dir.path() / "state", {{disk_a, 64 * mib}, {disk_b, 64 * mib}}, 100ms, NodeId::random(), bed.time);
  ASSERT_TRUE(wait_until([&] { return pool->online_backends() == 2; }));

  std::optional<DurabilityTicket> first;
  std::optional<DurabilityTicket> second;
  for (uint8_t seed = 60; seed < 120 && !second; ++seed) {
    const auto bytes = pattern(128 * 1024, seed);
    const auto ticket = pool->put_deferred(object_id(bytes), bytes);
    ASSERT_TRUE(ticket.has_value());
    if (!first)
      first = ticket;
    else if (ticket->backend_instance != first->backend_instance)
      second = ticket;
  }
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  // Both configured paths are on the test directory's filesystem.
  EXPECT_EQ(first->domain, second->domain);
  EXPECT_NE(first->backend_instance, second->backend_instance);

  const auto barriers = pool->physical_barriers();
  auto a = std::async(std::launch::async, [&] { pool->durability_barrier(*first); });
  auto b = std::async(std::launch::async, [&] { pool->durability_barrier(*second); });
  const auto until = std::chrono::steady_clock::now() + 10s;
  while ((a.wait_for(1ms) != std::future_status::ready || b.wait_for(0ms) != std::future_status::ready) && std::chrono::steady_clock::now() < until) {
    bed.time.advance(100ms);
    pool->time_advanced();
  }
  ASSERT_TRUE(test::ready(a));
  ASSERT_TRUE(test::ready(b));
  a.get();
  b.get();
  EXPECT_EQ(pool->physical_barriers() - barriers, 1u);
  EXPECT_TRUE(pool->durability_covered(*first));
  EXPECT_TRUE(pool->durability_covered(*second));
}

// Requirement S15: a ticket satisfies nothing outside the process, domain and
// incarnation that issued it.
TEST(StoreStoragePool, ATicketFromAnotherProcessOrIncarnationSatisfiesNothing) {
  StoreBed bed;
  const auto disk = bed.dir.path() / "disk";
  std::filesystem::create_directories(disk);
  auto pool = bed.pool(bed.dir.path() / "state", {{disk, 64 * mib}});
  const auto bytes = pattern(64 * 1024, 7);
  const auto ticket = pool->put_deferred(object_id(bytes), bytes);
  ASSERT_TRUE(ticket.has_value());
  pool->durability_barrier(*ticket, DurabilityUrgency::immediate);
  EXPECT_TRUE(pool->durability_covered(*ticket));

  auto foreign = *ticket;
  foreign.epoch = NodeId::random();
  EXPECT_FALSE(pool->durability_covered(foreign));
  EXPECT_THROW(pool->durability_barrier(foreign), StaleDurabilityTicket);
  EXPECT_THROW(pool->durability_barrier(DurabilityTicket{}), std::invalid_argument);

  // The disk goes and comes back: a new incarnation, so the old ticket names
  // nothing, and the object is reasserted under a fresh one.
  std::filesystem::rename(disk, bed.dir.path() / "disk.away");
  pool->refresh();
  std::filesystem::rename(bed.dir.path() / "disk.away", disk);
  pool->refresh();
  ASSERT_EQ(pool->online_backends(), 1u);
  EXPECT_FALSE(pool->durability_covered(*ticket));
  EXPECT_THROW(pool->durability_barrier(*ticket), StaleDurabilityTicket);
  ASSERT_TRUE(wait_until([&] { return pool->has(object_id(bytes)); }));
  const auto reasserted = pool->reassert_durable(object_id(bytes));
  ASSERT_TRUE(reasserted.has_value());
  EXPECT_NE(reasserted->backend_instance, ticket->backend_instance);
  EXPECT_TRUE(pool->durability_covered(*reasserted));
  EXPECT_FALSE(pool->reassert_durable(object_id(pattern(64, 1))).has_value());
}

// Requirement L21 and the seam step 2's claims attach to: a claimed copy is
// never swept, however old and long unreferenced; the claim is asked again
// just before the delete; and an unreferenced object goes only once this
// node has seen it so for the grace, on its own clock.
TEST(StoreStoragePool, AClaimedCopyIsUntouchable) {
  StoreBed bed;
  const auto disk = bed.dir.path() / "disk";
  std::filesystem::create_directories(disk);
  auto pool = bed.pool(bed.dir.path() / "state", {{disk, 64 * mib}});
  const auto claimed_bytes = pattern(32 * 1024, 1);
  const auto late_bytes = pattern(32 * 1024, 2);
  const auto free_bytes = pattern(32 * 1024, 3);
  const auto claimed = object_id(claimed_bytes);
  const auto late = object_id(late_bytes);
  const auto unclaimed = object_id(free_bytes);
  for (const auto* bytes : {&claimed_bytes, &late_bytes, &free_bytes}) ASSERT_TRUE(pool->put(object_id(*bytes), *bytes));
  for (const auto& id : {claimed, late, unclaimed})
    std::filesystem::last_write_time(object_path(disk, id), std::filesystem::file_time_type::clock::now() - 48h);

  // `late` is unclaimed when the first sweep looks and when the second
  // decides, and claimed by the time the second would delete it.
  std::map<ObjectId, int> asked;
  const StoragePool::ClaimCheck claim = [&](const ObjectId& id) { return id == claimed || (id == late && ++asked[id] > 2); };
  ledger::UnreferencedSince sightings(bed.dir.path() / "state" / "retention" / "unreferenced-data.bin", dannan::status::ScopedLogger(bed.sink, "sightings"));
  const std::vector<ObjectId> none;
  const auto sweep = [&](uint64_t now) {
    StoragePool::MaintenanceResult total;
    for (int slice = 0; slice < 64; ++slice) {
      const auto step = pool->gc_step(none, none, 24h, 16, {}, claim, &sightings, now, 30 * 24h);
      total.bytes += step.bytes;
      total.deferred = total.deferred || step.deferred;
      if (step.complete) break;
    }
    return total;
  };
  const uint64_t day0 = 1'000'000'000'000;
  // First sighting: nothing has been unreferenced here for the grace.
  const auto first = sweep(day0);
  EXPECT_TRUE(first.deferred);
  EXPECT_EQ(first.bytes, 0u);
  EXPECT_TRUE(pool->has(unclaimed));
  // A claimed object is never sighted at all.
  EXPECT_EQ(sightings.size(), 2u);

  const auto later = sweep(day0 + static_cast<uint64_t>(std::chrono::milliseconds(31 * 24h).count()));
  EXPECT_GT(later.bytes, 0u);
  EXPECT_FALSE(pool->has(unclaimed));
  EXPECT_TRUE(pool->has(claimed));
  EXPECT_TRUE(pool->has(late));
  EXPECT_EQ(asked[late], 3);
}

// Macha (tests/test_presence.cpp): test_a_backend_offline_and_readopted_leaves_presence_exact.
TEST(StoreStoragePool, ABackendOfflineAndReadoptedLeavesPresenceExact) {
  StoreBed bed;
  const auto backend = bed.dir.path() / "data";
  std::filesystem::create_directories(backend);
  auto pool = bed.pool(bed.dir.path() / "pool-state", {{backend, 64ULL << 20}});
  ASSERT_TRUE(wait_until([&] { return pool->online_backends() == 1; }, 5s));
  const auto bytes = numbered(1);
  const auto id = object_id(bytes);
  ASSERT_TRUE(pool->put(id, bytes));
  const auto absent = object_id(numbered(2));
  EXPECT_TRUE(pool->has(id));

  auto away = backend;
  away += ".away";
  std::filesystem::rename(backend, away);
  pool->refresh();
  ASSERT_EQ(pool->online_backends(), 0u);
  EXPECT_FALSE(pool->has(id));
  EXPECT_FALSE(pool->has(absent));

  std::filesystem::rename(away, backend);
  pool->refresh();
  ASSERT_EQ(pool->online_backends(), 1u);
  ASSERT_TRUE(wait_until([&] { return pool->has(id); }, 5s));
  EXPECT_FALSE(pool->has(absent));
}

// Macha (tests/test_presence.cpp): test_the_held_identity_names_exactly_what_the_pool_holds.
// The identity moves with every put, removal and disk, comes back to the
// same value for the same holdings, and its removals never return to where
// they were. A view stays as it was taken.
TEST(StoreStoragePool, TheHeldIdentityNamesExactlyWhatThePoolHolds) {
  StoreBed bed;
  const auto backend = bed.dir.path() / "data";
  std::filesystem::create_directories(backend);
  auto pool = bed.pool(bed.dir.path() / "pool-state", {{backend, 64ULL << 20}});
  ASSERT_TRUE(wait_until([&] { return pool->online_backends() == 1; }, 5s));
  ASSERT_TRUE(wait_until([&] { return pool->held_view().identity.complete; }, 10s));
  const auto empty = pool->held_view();

  const auto first = numbered(1);
  const auto second = numbered(2);
  const auto absent = object_id(numbered(3));
  ASSERT_TRUE(pool->put(object_id(first), first));
  const auto one = pool->held_view();
  EXPECT_TRUE(one.identity.complete);
  EXPECT_NE(one.identity.hash, empty.identity.hash);
  EXPECT_EQ(one.identity.removals, empty.identity.removals);
  EXPECT_TRUE(one.held(object_id(first)));
  EXPECT_FALSE(one.held(absent));

  ASSERT_TRUE(pool->put(object_id(second), second));
  const auto two = pool->held_view();
  EXPECT_NE(two.identity.hash, one.identity.hash);
  EXPECT_TRUE(two.held(object_id(second)));
  EXPECT_FALSE(one.held(object_id(second)));

  // The same ids, the same identity; the removal is counted.
  ASSERT_TRUE(pool->remove(object_id(second)));
  const auto removed = pool->held_view();
  EXPECT_EQ(removed.identity.hash, one.identity.hash);
  EXPECT_GT(removed.identity.removals, one.identity.removals);
  EXPECT_FALSE(removed.held(object_id(second)));
  EXPECT_TRUE(two.held(object_id(second)));

  // The disk goes: nothing held, and its going counts as a removal.
  auto away = backend;
  away += ".away";
  std::filesystem::rename(backend, away);
  pool->refresh();
  ASSERT_EQ(pool->online_backends(), 0u);
  const auto gone = pool->held_view();
  EXPECT_TRUE(gone.identity.complete);
  EXPECT_NE(gone.identity.hash, removed.identity.hash);
  EXPECT_GT(gone.identity.removals, removed.identity.removals);
  EXPECT_FALSE(gone.held(object_id(first)));

  // It comes back holding what it held: the same identity, and the removals
  // never back where they were.
  std::filesystem::rename(away, backend);
  pool->refresh();
  ASSERT_EQ(pool->online_backends(), 1u);
  ASSERT_TRUE(wait_until([&] { return pool->held_view().identity.complete; }, 10s));
  const auto back = pool->held_view();
  EXPECT_EQ(back.identity.hash, removed.identity.hash);
  EXPECT_GE(back.identity.removals, gone.identity.removals);
  EXPECT_TRUE(back.held(object_id(first)));
}

// Macha (tests/test_presence.cpp): test_the_held_identity_is_the_same_on_any_number_of_disks.
// A pool holding ids across two disks names them as a pool holding the same
// ids on one disk does.
TEST(StoreStoragePool, TheHeldIdentityIsTheSameOnAnyNumberOfDisks) {
  StoreBed bed;
  const auto a = bed.dir.path() / "a";
  const auto b = bed.dir.path() / "b";
  const auto single = bed.dir.path() / "single";
  for (const auto& path : {a, b, single}) std::filesystem::create_directories(path);
  auto two = bed.pool(bed.dir.path() / "two-state", {{a, 64ULL << 20}, {b, 64ULL << 20}});
  auto one = bed.pool(bed.dir.path() / "one-state", {{single, 64ULL << 20}});
  ASSERT_TRUE(wait_until([&] { return two->online_backends() == 2; }, 5s));
  ASSERT_TRUE(wait_until([&] { return one->online_backends() == 1; }, 5s));
  ASSERT_TRUE(wait_until([&] { return two->held_view().identity.complete; }, 10s));
  ASSERT_TRUE(wait_until([&] { return one->held_view().identity.complete; }, 10s));
  for (uint64_t n = 0; n < 300; ++n) {
    const auto bytes = numbered(n);
    ASSERT_TRUE(two->put(object_id(bytes), bytes));
    ASSERT_TRUE(one->put(object_id(bytes), bytes));
  }
  const auto many = two->held_view();
  const auto lone = one->held_view();
  EXPECT_EQ(many.trie->parts().size(), 2u);
  EXPECT_GT(many.trie->parts()[0].size(), 0u);
  EXPECT_GT(many.trie->parts()[1].size(), 0u);
  EXPECT_EQ(many.identity.hash, lone.identity.hash);
  EXPECT_EQ(many.trie->root(), lone.trie->root());
}

// Macha (tests/test_baselines.cpp): test_baseline_data_store_has. A timing,
// printed as Macha's BENCH line; it asserts only that has() did its work.
TEST(StoreStoragePool, DataStoreHasBaseline) {
  constexpr size_t object_count = 2000;
  StoreBed bed;
  const auto backend = bed.dir.path() / "data";
  std::filesystem::create_directories(backend);
  auto pool = bed.pool(bed.dir.path() / "pool-state", {{backend, 1ULL << 30}});
  ASSERT_TRUE(wait_until([&] { return pool->online_backends() == 1; }));
  std::vector<ObjectId> present;
  std::vector<ObjectId> absent;
  for (size_t i = 0; i < object_count; ++i) {
    const auto bytes = numbered(i);
    const auto id = object_id(bytes);
    ASSERT_TRUE(pool->put(id, bytes));
    present.push_back(id);
    absent.push_back(object_id(numbered(i + object_count)));
  }
  const auto report = [](std::string_view name, std::chrono::steady_clock::duration elapsed, uint64_t ops) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    std::cout << "BENCH name=" << name << " ns_per_op=" << (ops ? ns / static_cast<int64_t>(ops) : 0) << " ops=" << ops << '\n';
  };

  constexpr int rounds = 20;
  size_t found = 0;
  auto started = std::chrono::steady_clock::now();
  for (int round = 0; round < rounds; ++round)
    for (const auto& id : present) found += pool->has(id) ? 1 : 0;
  report("data_store.has.present", std::chrono::steady_clock::now() - started, rounds * object_count);
  EXPECT_EQ(found, rounds * object_count);

  size_t missing = 0;
  started = std::chrono::steady_clock::now();
  for (int round = 0; round < rounds; ++round)
    for (const auto& id : absent) missing += pool->has(id) ? 0 : 1;
  report("data_store.has.absent", std::chrono::steady_clock::now() - started, rounds * object_count);
  EXPECT_EQ(missing, rounds * object_count);
}

}  // namespace
}  // namespace dagda::store
