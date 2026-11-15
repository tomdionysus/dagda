//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirements S1 to S9 and S16 to S18 at one store: loose files and packs,
// pack recovery, compaction, admission, accounting, and device I/O held on
// one object never blocking another.
//
// Moved from Macha (develop 5a06d3c), every assertion kept:
// tests/test_storage_v18.cpp: test_packs_store_compact_and_survive_restart,
// test_pack_recovery_settles_each_kind_of_damage,
// test_held_device_io_blocks_only_its_own_object,
// test_presence_is_known_without_reading_objects, and the LocalStore block
// of test_admission_respects_reserve_and_capacity (its StoragePool and
// placement blocks are in store_storage_pool_test);
// tests/test_storage_metadata.cpp: test_local_store. Macha's cluster key
// file becomes domain v1's known-answer key. The bounded waits on futures are
// a safety net against a hang; "not ready within 20 ms" is Macha's own proof
// that an operation is held behind another.

#include "dagda/test/fixtures/store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <vector>

#include "dagda/store/local_store.h"
#include "dagda/store/object_id.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using test::InterposedLocalStoreFiles;
using test::pattern;
using test::ready;
using test::StoreBed;
using test::wait_until;

size_t regular_files_below(const std::filesystem::path& root) {
  std::error_code error;
  size_t count = 0;
  if (!std::filesystem::exists(root, error)) return 0;
  for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    std::error_code type_error;
    if (it->is_regular_file(type_error) && !type_error) ++count;
  }
  return count;
}

LocalStoreOptions packed_options(uint64_t limit, uint64_t target_size) {
  LocalStoreOptions options;
  options.limit = limit;
  options.pack_threshold = 256 * 1024;
  options.pack_target_size = target_size;
  return options;
}

std::filesystem::path only_pack(const std::filesystem::path& store) {
  std::filesystem::path pack;
  for (const auto& entry : std::filesystem::directory_iterator(store / "packs")) {
    if (entry.is_regular_file()) {
      EXPECT_TRUE(pack.empty());
      pack = entry.path();
    }
  }
  EXPECT_FALSE(pack.empty());
  return pack;
}

// One device operation held at its start until released, so a test can act
// while it is provably in progress.
class HeldOperation {
 public:
  HeldOperation() = default;
  HeldOperation(const HeldOperation&) = delete;
  HeldOperation& operator=(const HeldOperation&) = delete;
  // Releases a holder a failed check left waiting, and detaches from the
  // device.
  ~HeldOperation() {
    release();
    if (_files) _files->before({});
  }

  // Holds the first call matching `op` (and `path`, if given); counts every
  // matching call.
  void hold(InterposedLocalStoreFiles& files, InterposedLocalStoreFiles::Op op, std::filesystem::path path = {}) {
    _files = &files;
    files.before([this, op, path](InterposedLocalStoreFiles::Op seen, const std::filesystem::path& at) {
      if (seen != op || (!path.empty() && at != path)) return;
      if (_calls.fetch_add(1) == 0) {
        _entered.set_value();
        _released.wait();
      }
    });
  }
  bool entered() { return _entered_future.wait_for(10s) == std::future_status::ready; }
  void release() {
    if (!_released_once.exchange(true)) _release.set_value();
  }
  uint64_t calls() const { return _calls.load(); }

 private:
  std::promise<void> _entered;
  std::future<void> _entered_future = _entered.get_future();
  std::promise<void> _release;
  std::shared_future<void> _released = _release.get_future().share();
  std::atomic_uint64_t _calls{0};
  std::atomic_bool _released_once{false};
  InterposedLocalStoreFiles* _files = nullptr;
};

// The milliseconds a packed object's age is kept in: each reading converts them to a
// file time afresh, which may differ below a millisecond.
int64_t age_ms(std::filesystem::file_time_type time) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::file_clock::to_sys(time).time_since_epoch()).count();
}

// Macha's put checks room before it seals (local_store.cpp:1099-1113 at f0597a4): a
// put refused for room encrypts nothing (the store map, pool gap 3).
TEST(StoreLocalStore, APutRefusedForRoomSealsNothing) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  for (const bool packed : {false, true}) {
    auto options = packed ? packed_options(64 * 1024, 40 * 1024) : LocalStoreOptions::loose_only(64 * 1024);
    if (packed) options.pack_threshold = 40 * 1024;
    auto store = bed.open(root / (packed ? "packed" : "loose"), options);
    const auto first = pattern(36 * 1024, packed ? 31 : 21);
    ASSERT_TRUE(store->put(object_id(first), first));
    const auto seals = store->diagnostics().seals;
    EXPECT_EQ(seals, 1u);
    const auto second = pattern(36 * 1024, packed ? 32 : 22);
    EXPECT_FALSE(store->put(object_id(second), second));
    EXPECT_EQ(store->diagnostics().seals, seals) << (packed ? "packed" : "loose");
  }
}

// Pack compaction keeps what replay needs (Tom, 2026-11-14: Dagda's to fix; a defect
// in Macha's store too, local_store.cpp:1738-1843 at f0597a4). A remove recorded in the
// pack compacted away, whose put lies in an older pack, is carried forward: the object
// stays removed across compaction and a restart.
TEST(StoreLocalStore, ARemovedObjectStaysRemovedAcrossCompactionAndRestart) {
  StoreBed bed;
  auto options = packed_options(64ULL * 1024 * 1024, 40 * 1024);
  options.pack_threshold = 40 * 1024;
  const auto root = bed.dir.path() / "store";
  const auto a = pattern(8 * 1024, 1);
  const auto filler = pattern(34 * 1024, 2);
  const auto b = pattern(30 * 1024, 3);
  {
    auto store = bed.open(root, options);
    // Pack one: a's put, and a filler that ends the pack.
    ASSERT_TRUE(store->put(object_id(a), a));
    ASSERT_TRUE(store->put(object_id(filler), filler));
    // Pack two: a's remove, and b put and removed, so pack two is the most dead.
    ASSERT_TRUE(store->remove(object_id(a)));
    ASSERT_TRUE(store->put(object_id(b), b));
    ASSERT_TRUE(store->remove(object_id(b)));
    ASSERT_NE(store->object_path(object_id(filler)), store->object_path(object_id(b)));
    ASSERT_TRUE(store->compact_packs());
    EXPECT_FALSE(store->has(object_id(a)));
  }
  auto reopened = bed.open(root, options);
  EXPECT_FALSE(reopened->has(object_id(a)));
  EXPECT_FALSE(reopened->get(object_id(a)).has_value());
  EXPECT_FALSE(reopened->has(object_id(b)));
  EXPECT_TRUE(reopened->has(object_id(filler)));
}

// And a re-affirmed object keeps its age: its put is copied with the time its touch
// gave it, so a restart does not hand the collector its first write's age.
TEST(StoreLocalStore, AReaffirmedObjectKeepsItsAgeAcrossCompactionAndRestart) {
  StoreBed bed;
  auto options = packed_options(64ULL * 1024 * 1024, 40 * 1024);
  options.pack_threshold = 40 * 1024;
  const auto root = bed.dir.path() / "store";
  const auto a = pattern(8 * 1024, 4);
  const auto dead = pattern(30 * 1024, 5);
  const auto filler = pattern(4 * 1024, 6);
  std::filesystem::file_time_type reaffirmed{};
  {
    auto store = bed.open(root, options);
    // Pack one: a, a large object to be removed, and a filler that ends the pack.
    ASSERT_TRUE(store->put(object_id(a), a));
    ASSERT_TRUE(store->put(object_id(dead), dead));
    ASSERT_TRUE(store->put(object_id(filler), filler));
    // An hour on, a is re-affirmed (a touch record in pack two) and the large object
    // removed, so pack one is the most dead.
    bed.wall.advance(std::chrono::hours(1));
    ASSERT_TRUE(store->put(object_id(a), a));
    reaffirmed = store->last_write(object_id(a));
    ASSERT_TRUE(store->remove(object_id(dead)));
    ASSERT_TRUE(store->compact_packs());
    EXPECT_EQ(age_ms(store->last_write(object_id(a))), age_ms(reaffirmed));
  }
  auto reopened = bed.open(root, options);
  EXPECT_EQ(age_ms(reopened->last_write(object_id(a))), age_ms(reaffirmed));
  EXPECT_FALSE(reopened->older_than(object_id(a), std::chrono::minutes(30)));
}

// A remove followed by a put again is never carried (the Macha session's review of the
// fix): the object put again is live in a later pack, so compacting the remove's pack
// carries nothing that would replay after it. Present after a restart, with its re-put's
// age.
TEST(StoreLocalStore, AnObjectPutAgainAfterItsRemoveSurvivesCompactionOfTheRemove) {
  StoreBed bed;
  auto options = packed_options(64ULL * 1024 * 1024, 40 * 1024);
  options.pack_threshold = 40 * 1024;
  const auto root = bed.dir.path() / "store";
  const auto x = pattern(8 * 1024, 10);
  const auto filler = pattern(31 * 1024, 11);
  const auto big = pattern(32 * 1024, 12);
  std::filesystem::file_time_type put_again{};
  {
    auto store = bed.open(root, options);
    // Pack one: x and a filler. Pack two: a large object and x's remove. Pack three:
    // x put again, and the large object's remove, so pack two is the most dead.
    ASSERT_TRUE(store->put(object_id(x), x));
    ASSERT_TRUE(store->put(object_id(filler), filler));
    ASSERT_TRUE(store->put(object_id(big), big));
    ASSERT_TRUE(store->remove(object_id(x)));
    bed.wall.advance(std::chrono::hours(1));
    ASSERT_TRUE(store->put(object_id(x), x));
    put_again = store->last_write(object_id(x));
    const auto removed_in = store->object_path(object_id(big));
    ASSERT_NE(store->object_path(object_id(x)), removed_in);
    ASSERT_TRUE(store->remove(object_id(big)));
    ASSERT_TRUE(store->compact_packs());
    EXPECT_FALSE(std::filesystem::exists(removed_in)) << "the remove's pack was not the one compacted";
    EXPECT_TRUE(store->has(object_id(x)));
  }
  auto reopened = bed.open(root, options);
  EXPECT_TRUE(reopened->has(object_id(x)));
  EXPECT_EQ(reopened->get(object_id(x)), x);
  EXPECT_EQ(age_ms(reopened->last_write(object_id(x))), age_ms(put_again));
  EXPECT_FALSE(reopened->has(object_id(big)));
}

// The touch's own pack compacted away, its put in another: the touch is carried.
TEST(StoreLocalStore, AReaffirmationOutlivesTheCompactionOfItsOwnPack) {
  StoreBed bed;
  auto options = packed_options(64ULL * 1024 * 1024, 40 * 1024);
  options.pack_threshold = 40 * 1024;
  const auto root = bed.dir.path() / "store";
  const auto a = pattern(8 * 1024, 7);
  const auto filler = pattern(31 * 1024, 8);
  const auto big = pattern(30 * 1024, 9);
  std::filesystem::file_time_type reaffirmed{};
  {
    auto store = bed.open(root, options);
    // Pack one: a and a filler. Pack two: a large object, a's touch, the large
    // object's remove, so pack two is the most dead and pack one holds none.
    ASSERT_TRUE(store->put(object_id(a), a));
    ASSERT_TRUE(store->put(object_id(filler), filler));
    ASSERT_TRUE(store->put(object_id(big), big));
    ASSERT_EQ(store->object_path(object_id(a)), store->object_path(object_id(filler)));
    ASSERT_NE(store->object_path(object_id(a)), store->object_path(object_id(big)));
    bed.wall.advance(std::chrono::hours(1));
    ASSERT_TRUE(store->put(object_id(a), a));
    reaffirmed = store->last_write(object_id(a));
    ASSERT_TRUE(store->remove(object_id(big)));
    ASSERT_TRUE(store->compact_packs());
    EXPECT_EQ(age_ms(store->last_write(object_id(a))), age_ms(reaffirmed));
  }
  auto reopened = bed.open(root, options);
  EXPECT_TRUE(reopened->has(object_id(a)));
  EXPECT_EQ(age_ms(reopened->last_write(object_id(a))), age_ms(reaffirmed));
  EXPECT_FALSE(reopened->older_than(object_id(a), std::chrono::minutes(30)));
}

// Macha: test_packs_store_compact_and_survive_restart. Small objects are
// packed, deletions are tombstones, compaction reclaims dead space a bounded
// slice at a time (and not at all once shutdown is requested), and a restart
// rebuilds the pack index from the pack records alone.
TEST(StoreLocalStore, PacksStoreCompactAndSurviveRestart) {
  StoreBed bed;
  const auto options = packed_options(128ULL * 1024 * 1024, 512 * 1024);
  const auto root = bed.dir.path() / "store";

  std::vector<std::pair<ObjectId, Bytes>> live;
  std::vector<ObjectId> removed;
  {
    auto store = bed.open(root, options);
    std::stop_source shutdown;
    shutdown.request_stop();
    EXPECT_FALSE(store->compact_packs(shutdown.get_token()));

    for (size_t i = 0; i < 40; ++i) {
      auto data = pattern(96 * 1024 + i, static_cast<uint8_t>(17 + i));
      const auto id = object_id(data);
      ASSERT_TRUE(store->put(id, data));
      EXPECT_TRUE(store->is_packed(id));
      if (i % 2)
        live.emplace_back(id, std::move(data));
      else
        removed.push_back(id);
    }
    EXPECT_EQ(regular_files_below(root / "objects"), 0u);
    EXPECT_LT(regular_files_below(root / "packs"), live.size() + removed.size());

    for (const auto& id : removed) ASSERT_TRUE(store->remove(id));
    // Dead packs remain after one bounded victim rewrite, so a second slice
    // makes further progress; no call needs space for the whole corpus.
    const auto before = store->used();
    ASSERT_TRUE(store->compact_packs());
    const auto after_one = store->used();
    EXPECT_LT(after_one, before);
    ASSERT_TRUE(store->compact_packs());
    EXPECT_LT(store->used(), after_one);
    for (const auto& id : removed) EXPECT_FALSE(store->has(id));
    for (const auto& [id, data] : live) EXPECT_EQ(store->get(id), std::optional<Bytes>{data});
  }

  auto reopened = bed.open(root, options);
  for (const auto& id : removed) EXPECT_FALSE(reopened->has(id));
  for (const auto& [id, data] : live) {
    EXPECT_EQ(reopened->get(id), std::optional<Bytes>{data});
    EXPECT_TRUE(reopened->is_packed(id));
  }
}

// Macha: test_admission_respects_reserve_and_capacity, its LocalStore block.
// The free-space reserve refuses a put before the filesystem is exhausted.
TEST(StoreLocalStore, AdmissionRespectsTheFreeSpaceReserve) {
  StoreBed bed;
  auto options = LocalStoreOptions::loose_only(64ULL * 1024 * 1024);
  options.reserve_free = std::numeric_limits<uint64_t>::max();
  auto store = bed.open(bed.dir.path() / "reserved", options);
  const auto data = pattern(128 * 1024, 31);
  const auto id = object_id(data);
  EXPECT_FALSE(store->put(id, data));
  EXPECT_FALSE(store->has(id));
  EXPECT_EQ(store->used(), 0u);
}

// Requirement S9's other half: used + reserved + need stays within the
// limit, for a loose object and a packed one.
TEST(StoreLocalStore, AdmissionRespectsTheLimit) {
  StoreBed bed;
  auto store = bed.open(bed.dir.path() / "store", packed_options(256 * 1024, 1024 * 1024));
  const auto loose = pattern(300 * 1024, 1);
  EXPECT_FALSE(store->put(object_id(loose), loose));
  const auto first = pattern(200 * 1024, 2);
  ASSERT_TRUE(store->put(object_id(first), first));
  const auto second = pattern(100 * 1024, 3);
  EXPECT_FALSE(store->put(object_id(second), second));
  EXPECT_FALSE(store->has(object_id(second)));
  EXPECT_LE(store->used(), 256u * 1024);
}

// Macha: test_pack_recovery_settles_each_kind_of_damage. Damage a power loss
// or a bad sector leaves in a pack. A torn or undecodable record at the tail
// is truncated and the pack stays the active one; an unreadable span with
// live records after it is skipped, reported, and reclaimed by compaction,
// and the lost object can be written again. (Crash matrix: "Pack tail
// torn".)
TEST(StoreLocalStore, PackRecoverySettlesEachKindOfDamage) {
  StoreBed bed;
  const auto options = packed_options(32ULL * 1024 * 1024, 1024 * 1024);
  // A record is this header plus ciphertext as long as the plaintext.
  constexpr uint64_t pack_header_size = 93 + 32;

  struct Tail {
    const char* what;
    Bytes garbage;
    // Shorter than a header: discarded before any header is decoded, so not
    // counted among undecodable tails.
    bool short_of_a_header;
  };
  std::vector<Tail> tails;
  tails.push_back({"incomplete record", Bytes{'M', 'A', 'C', 'H', 'P', 'K', '0', '1', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, true});
  tails.push_back({"zero-filled header", Bytes(pack_header_size, 0), false});
  tails.push_back({"garbage header", pattern(pack_header_size, 0x5a), false});
  tails.push_back({"garbage header and partial payload", pattern(pack_header_size + 40000, 0x5a), false});

  size_t index = 0;
  for (auto& tail : tails) {
    SCOPED_TRACE(tail.what);
    const auto root = bed.dir.path() / ("store-" + std::to_string(index++));
    const auto first = pattern(96 * 1024, 11);
    const auto second = pattern(96 * 1024, 12);
    {
      auto store = bed.open(root, options);
      ASSERT_TRUE(store->put(object_id(first), first));
      ASSERT_TRUE(store->put(object_id(second), second));
    }
    const auto pack = only_pack(root);
    const auto intact_size = std::filesystem::file_size(pack);
    {
      std::ofstream out(pack, std::ios::binary | std::ios::app);
      out.write(reinterpret_cast<const char*>(tail.garbage.data()), static_cast<std::streamsize>(tail.garbage.size()));
      ASSERT_TRUE(out.good());
    }
    ASSERT_EQ(std::filesystem::file_size(pack), intact_size + tail.garbage.size());

    auto reopened = bed.open(root, options);
    EXPECT_EQ(reopened->get(object_id(first)), std::optional<Bytes>{first});
    EXPECT_EQ(reopened->get(object_id(second)), std::optional<Bytes>{second});
    EXPECT_EQ(std::filesystem::file_size(pack), intact_size);
    const auto diagnostics = reopened->diagnostics();
    EXPECT_EQ(diagnostics.pack_recovery_truncated_tails, tail.short_of_a_header ? 0u : 1u);
    EXPECT_EQ(diagnostics.pack_recovery_skipped_regions, 0u);
    EXPECT_EQ(diagnostics.pack_recovery_skipped_bytes, 0u);
    // Appends continue behind the restored boundary.
    const auto third = pattern(64 * 1024, 13);
    ASSERT_TRUE(reopened->put(object_id(third), third));
    EXPECT_TRUE(reopened->is_packed(object_id(third)));
  }

  // A header failing its checksum with intact records after it.
  const auto root = bed.dir.path() / "store-damaged-middle";
  const std::vector<Bytes> objects{pattern(96 * 1024, 21), pattern(96 * 1024, 22), pattern(96 * 1024, 23)};
  {
    auto store = bed.open(root, options);
    for (const auto& object : objects) ASSERT_TRUE(store->put(object_id(object), object));
  }
  const auto pack = only_pack(root);
  const auto intact_size = std::filesystem::file_size(pack);
  const uint64_t second_offset = pack_header_size + objects[0].size();
  {
    // Flip one byte inside the second record's header checksum.
    std::fstream io(pack, std::ios::binary | std::ios::in | std::ios::out);
    io.seekg(static_cast<std::streamoff>(second_offset + 100));
    char byte = 0;
    io.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x01);
    io.seekp(static_cast<std::streamoff>(second_offset + 100));
    io.write(&byte, 1);
    ASSERT_TRUE(io.good());
  }
  auto reopened = bed.open(root, options);
  EXPECT_EQ(std::filesystem::file_size(pack), intact_size);
  EXPECT_EQ(reopened->get(object_id(objects[0])), std::optional<Bytes>{objects[0]});
  EXPECT_FALSE(reopened->has(object_id(objects[1])));
  EXPECT_EQ(reopened->get(object_id(objects[2])), std::optional<Bytes>{objects[2]});
  const auto diagnostics = reopened->diagnostics();
  EXPECT_EQ(diagnostics.pack_recovery_truncated_tails, 0u);
  EXPECT_EQ(diagnostics.pack_recovery_skipped_regions, 1u);
  EXPECT_EQ(diagnostics.pack_recovery_skipped_bytes, pack_header_size + objects[1].size());
  // What replica repair does: write the lost object again. Compaction then
  // reclaims the unreadable span without touching live data.
  ASSERT_TRUE(reopened->put(object_id(objects[1]), objects[1]));
  const auto before_compaction = reopened->used();
  ASSERT_TRUE(reopened->compact_packs());
  EXPECT_LT(reopened->used(), before_compaction);
  for (const auto& object : objects) EXPECT_EQ(reopened->get(object_id(object)), std::optional<Bytes>{object});
}

// Crash matrix: "Pack compaction after replacement install but before old
// deletion": the replacement is live and the old pack is duplicate,
// reclaimable bytes. The victim comes back as an unlink lost in a crash
// would leave it.
TEST(StoreLocalStore, ACompactionWhoseOldPackSurvivedIsReclaimedLater) {
  StoreBed bed;
  const auto options = packed_options(32ULL * 1024 * 1024, 1024 * 1024);
  const auto root = bed.dir.path() / "store";
  const auto live = pattern(96 * 1024, 41);
  const auto dead = pattern(96 * 1024, 42);
  std::filesystem::path victim;
  Bytes victim_bytes;
  {
    auto store = bed.open(root, options);
    ASSERT_TRUE(store->put(object_id(live), live));
    ASSERT_TRUE(store->put(object_id(dead), dead));
    ASSERT_TRUE(store->remove(object_id(dead)));
    victim = only_pack(root);
    std::ifstream in(victim, std::ios::binary);
    victim_bytes.assign(std::istreambuf_iterator<char>(in), {});
    ASSERT_TRUE(store->compact_packs());
    EXPECT_FALSE(std::filesystem::exists(victim));
  }
  {
    std::ofstream out(victim, std::ios::binary);
    out.write(reinterpret_cast<const char*>(victim_bytes.data()), static_cast<std::streamsize>(victim_bytes.size()));
    ASSERT_TRUE(out.good());
  }
  // The crash left no clean checkpoint behind (the session's first mutation
  // marked it dirty): the next open reconciles the bytes on disk.
  std::filesystem::remove(root / ".macha.accounting");
  auto reopened = bed.open(root, options);
  ASSERT_TRUE(wait_until([&] { return reopened->scan_complete(); }));
  EXPECT_EQ(reopened->get(object_id(live)), std::optional<Bytes>{live});
  EXPECT_FALSE(reopened->has(object_id(dead)));
  // The newer pack holds the live copy.
  EXPECT_NE(reopened->object_path(object_id(live)), victim);
  const auto before = reopened->used();
  ASSERT_TRUE(reopened->compact_packs());
  EXPECT_LT(reopened->used(), before);
  EXPECT_FALSE(std::filesystem::exists(victim));
  EXPECT_EQ(reopened->get(object_id(live)), std::optional<Bytes>{live});
}

// A compaction started on another thread in the gap between a watched object's
// pack append and its index change, while the appending thread waits there: the
// pack lock is free and so is the index lock, so only the store itself can keep
// the compaction from running to its end inside the gap.
class CompactionInTheGap {
 public:
  void watch(const ObjectId& id) {
    std::lock_guard lock(_mutex);
    _watched = id;
  }
  // The store's seam.
  std::function<void(const ObjectId&)> seam(std::unique_ptr<LocalStore>& store) {
    return [this, &store](const ObjectId& id) {
      {
        std::lock_guard lock(_mutex);
        if (!_watched || *_watched != id) return;
        _watched.reset();
      }
      _compaction = std::async(std::launch::async, [&store] { return store->compact_packs(); });
      _finished_in_gap = ready(_compaction, std::chrono::milliseconds(200));
    };
  }
  bool finished_in_gap() const { return _finished_in_gap; }
  // Once, after the append's call has returned.
  bool compacted() { return _compaction.valid() && ready(_compaction) && _compaction.get(); }

 private:
  std::mutex _mutex;
  std::optional<ObjectId> _watched;
  std::future<bool> _compaction;
  bool _finished_in_gap = false;
};

// Found from the delete stress case: a node's held ledger named an object no pack
// held, and every read logged "cannot read local object". A put's record is
// appended to the active pack under the pack lock, which is let go before the
// index lock is taken again. A compaction in that gap counted the record dead,
// could pick that very pack, rewrote the rest and unlinked it, and the put then
// indexed its record in the unlinked pack. Macha's local_store.cpp at 98b816d has
// the same gap (:844-866, the snapshot :1694, the unlink :1842); Tom chose to
// close it here (2026-11-08). Compaction now waits for every written record to be
// indexed before it reads the index.
TEST(StoreLocalStore, ACompactionWaitsForAPutInItsAppendGap) {
  StoreBed bed;
  CompactionInTheGap between;
  const auto root = bed.dir.path() / "store";
  std::unique_ptr<LocalStore> store;
  auto options = packed_options(32ULL * 1024 * 1024, 1024 * 1024);
  options.ledger_dir = bed.dir.path() / "ledger";
  options.after_pack_append = between.seam(store);
  const auto kept = pattern(8 * 1024, 51);
  const auto fresh = pattern(8 * 1024, 52);
  std::filesystem::path pack;
  {
    store = bed.open(root, options);
    ASSERT_TRUE(wait_until([&] { return store->held_view().identity.complete; }));
    ASSERT_TRUE(store->put(object_id(kept), kept));
    for (uint8_t i = 0; i < 2; ++i) {
      const auto gone = pattern(8 * 1024, static_cast<uint8_t>(60 + i));
      ASSERT_TRUE(store->put(object_id(gone), gone));
      ASSERT_TRUE(store->remove(object_id(gone)));
    }
    pack = only_pack(root);
    between.watch(object_id(fresh));
    ASSERT_TRUE(store->put(object_id(fresh), fresh));
    EXPECT_FALSE(between.finished_in_gap());
    ASSERT_TRUE(between.compacted());
    // The compaction ran after the put was indexed and carried it forward.
    EXPECT_FALSE(std::filesystem::exists(pack));
    EXPECT_NE(store->object_path(object_id(fresh)), pack);
    EXPECT_EQ(store->get(object_id(fresh)), std::optional<Bytes>{fresh});
    EXPECT_TRUE(store->held_view().held(object_id(fresh)));
    EXPECT_EQ(store->get(object_id(kept)), std::optional<Bytes>{kept});
    store.reset();
  }
  options.after_pack_append = {};
  auto reopened = bed.open(root, options);
  EXPECT_TRUE(reopened->is_packed(object_id(fresh)));
  EXPECT_EQ(reopened->get(object_id(fresh)), std::optional<Bytes>{fresh});
  EXPECT_TRUE(reopened->held_view().held(object_id(fresh)));
  const auto listed = reopened->list();
  EXPECT_EQ(std::count(listed.begin(), listed.end(), object_id(fresh)), 1);
  EXPECT_EQ(reopened->get(object_id(kept)), std::optional<Bytes>{kept});
}

// The same gap on a remove: the tombstone is in the active pack but the index
// still names the object, so a compaction there copied the record forward into a
// newer pack and unlinked the tombstone with its victim, and the object a remove
// reported gone was held again at the next start. Closed as above.
TEST(StoreLocalStore, ACompactionWaitsForARemoveInItsAppendGap) {
  StoreBed bed;
  CompactionInTheGap between;
  const auto root = bed.dir.path() / "store";
  std::unique_ptr<LocalStore> store;
  auto options = packed_options(32ULL * 1024 * 1024, 1024 * 1024);
  options.ledger_dir = bed.dir.path() / "ledger";
  options.after_pack_append = between.seam(store);
  const auto back = pattern(8 * 1024, 71);
  {
    store = bed.open(root, options);
    ASSERT_TRUE(wait_until([&] { return store->held_view().identity.complete; }));
    ASSERT_TRUE(store->put(object_id(back), back));
    const auto gone = pattern(8 * 1024, 72);
    ASSERT_TRUE(store->put(object_id(gone), gone));
    ASSERT_TRUE(store->remove(object_id(gone)));
    between.watch(object_id(back));
    ASSERT_TRUE(store->remove(object_id(back)));
    EXPECT_FALSE(between.finished_in_gap());
    ASSERT_TRUE(between.compacted());
    EXPECT_FALSE(store->has(object_id(back)));
    EXPECT_FALSE(store->held_view().held(object_id(back)));
    EXPECT_EQ(store->get(object_id(back)), std::nullopt);
    store.reset();
  }
  options.after_pack_append = {};
  auto reopened = bed.open(root, options);
  EXPECT_FALSE(reopened->is_packed(object_id(back)));
  EXPECT_FALSE(reopened->has(object_id(back)));
  EXPECT_EQ(reopened->get(object_id(back)), std::nullopt);
  const auto listed = reopened->list();
  EXPECT_EQ(std::count(listed.begin(), listed.end(), object_id(back)), 0);
}

// Macha: test_held_device_io_blocks_only_its_own_object. Device I/O on one
// object never blocks the index lock an unrelated read needs; operations on
// the same object stay single-flight behind it; and compaction cannot unlink
// a pack a reader has leased but not yet opened.
TEST(StoreLocalStore, HeldDeviceIoBlocksOnlyItsOwnObject) {
  StoreBed bed;
  const auto loose = LocalStoreOptions::loose_only(64ULL * 1024 * 1024);
  const auto packed = packed_options(64ULL * 1024 * 1024, 4 * 1024 * 1024);
  // Over the pack threshold: stored loose even in a packing store.
  const auto viewer = pattern(512 * 1024, 0x41);

  {
    SCOPED_TRACE("a loose loader write held at its disk write, after admission");
    InterposedLocalStoreFiles files;
    auto store = bed.open(bed.dir.path() / "loose-write", loose, LocalStoreMode::authoritative, {}, files);
    ASSERT_TRUE(store->put(object_id(viewer), viewer));
    const auto loader = pattern(4 * 1024 * 1024, 0x82);
    HeldOperation held;
    held.hold(files, InterposedLocalStoreFiles::Op::install, store->object_path(object_id(loader)));
    auto write = std::async(std::launch::async, [&] { return store->put(object_id(loader), loader); });
    ASSERT_TRUE(held.entered());
    auto read = std::async(std::launch::async, [&] { return store->get(object_id(viewer)); });
    ASSERT_TRUE(ready(read));
    EXPECT_EQ(read.get(), std::optional<Bytes>{viewer});
    auto same = std::async(std::launch::async, [&] { return store->put(object_id(loader), loader); });
    EXPECT_FALSE(ready(same, 20ms));
    held.release();
    ASSERT_TRUE(ready(write));
    EXPECT_TRUE(write.get());
    ASSERT_TRUE(ready(same));
    EXPECT_TRUE(same.get());
    // The second put found the object installed and wrote nothing.
    EXPECT_EQ(held.calls(), 1u);
    EXPECT_EQ(store->get(object_id(loader)), std::optional<Bytes>{loader});
  }
  {
    SCOPED_TRACE("a packed read held at its device read");
    InterposedLocalStoreFiles files;
    auto store = bed.open(bed.dir.path() / "packed-read", packed, LocalStoreMode::authoritative, {}, files);
    const auto blocked = pattern(128 * 1024, 0x37);
    ASSERT_TRUE(store->put(object_id(blocked), blocked));
    ASSERT_TRUE(store->is_packed(object_id(blocked)));
    ASSERT_TRUE(store->put(object_id(viewer), viewer));
    EXPECT_FALSE(store->is_packed(object_id(viewer)));
    HeldOperation held;
    held.hold(files, InterposedLocalStoreFiles::Op::read_at);
    auto first = std::async(std::launch::async, [&] { return store->get(object_id(blocked)); });
    ASSERT_TRUE(held.entered());
    auto read = std::async(std::launch::async, [&] { return store->get(object_id(viewer)); });
    ASSERT_TRUE(ready(read));
    EXPECT_EQ(read.get(), std::optional<Bytes>{viewer});
    auto same = std::async(std::launch::async, [&] { return store->get(object_id(blocked)); });
    EXPECT_FALSE(ready(same, 20ms));
    held.release();
    ASSERT_TRUE(ready(first));
    EXPECT_EQ(first.get(), std::optional<Bytes>{blocked});
    ASSERT_TRUE(ready(same));
    EXPECT_EQ(same.get(), std::optional<Bytes>{blocked});
  }
  {
    SCOPED_TRACE("a packed write held at its append: its reservation and ownership hold no index mutex");
    InterposedLocalStoreFiles files;
    auto store = bed.open(bed.dir.path() / "packed-write", packed, LocalStoreMode::authoritative, {}, files);
    ASSERT_TRUE(store->put(object_id(viewer), viewer));
    const auto loader = pattern(128 * 1024, 0x73);
    HeldOperation held;
    held.hold(files, InterposedLocalStoreFiles::Op::append);
    auto write = std::async(std::launch::async, [&] { return store->put(object_id(loader), loader); });
    ASSERT_TRUE(held.entered());
    auto read = std::async(std::launch::async, [&] { return store->get(object_id(viewer)); });
    ASSERT_TRUE(ready(read));
    EXPECT_EQ(read.get(), std::optional<Bytes>{viewer});
    auto same = std::async(std::launch::async, [&] { return store->put(object_id(loader), loader); });
    EXPECT_FALSE(ready(same, 20ms));
    EXPECT_EQ(held.calls(), 1u);
    held.release();
    ASSERT_TRUE(ready(write));
    EXPECT_TRUE(write.get());
    ASSERT_TRUE(ready(same));
    EXPECT_TRUE(same.get());
    // The second put appends its touch only after the first write completes.
    EXPECT_EQ(held.calls(), 2u);
    EXPECT_EQ(store->get(object_id(loader)), std::optional<Bytes>{loader});
  }
  {
    SCOPED_TRACE("compaction held at its pack listing: packed and loose reads proceed");
    InterposedLocalStoreFiles files;
    auto store = bed.open(bed.dir.path() / "compaction", packed, LocalStoreMode::authoritative, {}, files);
    const auto dead = pattern(128 * 1024, 0x19);
    const auto packed_viewer = pattern(128 * 1024, 0x29);
    ASSERT_TRUE(store->put(object_id(dead), dead));
    ASSERT_TRUE(store->put(object_id(packed_viewer), packed_viewer));
    ASSERT_TRUE(store->put(object_id(viewer), viewer));
    ASSERT_TRUE(store->remove(object_id(dead)));
    HeldOperation held;
    held.hold(files, InterposedLocalStoreFiles::Op::list);
    auto compaction = std::async(std::launch::async, [&] { return store->compact_packs(); });
    ASSERT_TRUE(held.entered());
    auto packed_read = std::async(std::launch::async, [&] { return store->get(object_id(packed_viewer)); });
    auto loose_read = std::async(std::launch::async, [&] { return store->get(object_id(viewer)); });
    ASSERT_TRUE(ready(packed_read));
    ASSERT_TRUE(ready(loose_read));
    EXPECT_EQ(packed_read.get(), std::optional<Bytes>{packed_viewer});
    EXPECT_EQ(loose_read.get(), std::optional<Bytes>{viewer});
    held.release();
    ASSERT_TRUE(ready(compaction));
    EXPECT_TRUE(compaction.get());
    EXPECT_EQ(store->get(object_id(packed_viewer)), std::optional<Bytes>{packed_viewer});
    EXPECT_EQ(store->get(object_id(viewer)), std::optional<Bytes>{viewer});
  }
  {
    SCOPED_TRACE("a reader leasing a pack it has not opened: compaction switches but cannot unlink the victim until it finishes");
    InterposedLocalStoreFiles files;
    auto store = bed.open(bed.dir.path() / "lease", packed, LocalStoreMode::authoritative, {}, files);
    const auto live = pattern(128 * 1024, 0x51);
    const auto dead = pattern(128 * 1024, 0x61);
    ASSERT_TRUE(store->put(object_id(live), live));
    ASSERT_TRUE(store->put(object_id(dead), dead));
    ASSERT_TRUE(store->remove(object_id(dead)));
    HeldOperation held;
    held.hold(files, InterposedLocalStoreFiles::Op::read_at);
    auto read = std::async(std::launch::async, [&] { return store->get(object_id(live)); });
    ASSERT_TRUE(held.entered());
    auto compaction = std::async(std::launch::async, [&] { return store->compact_packs(); });
    EXPECT_FALSE(ready(compaction, 20ms));
    held.release();
    ASSERT_TRUE(ready(read));
    EXPECT_EQ(read.get(), std::optional<Bytes>{live});
    ASSERT_TRUE(ready(compaction));
    EXPECT_TRUE(compaction.get());
    EXPECT_EQ(store->get(object_id(live)), std::optional<Bytes>{live});
  }
}

// Macha: test_presence_is_known_without_reading_objects. Presence is known
// without reading or decrypting an object: a restarted store learns what it
// holds from object names, has() touches no device, and an empty object file
// (a crash or external truncation) is pruned on read.
TEST(StoreLocalStore, PresenceIsKnownWithoutReadingObjects) {
  StoreBed bed;
  const auto options = packed_options(64ULL * 1024 * 1024, 1024 * 1024);
  const auto root = bed.dir.path() / "store";
  const auto loose = pattern(512 * 1024, 0x71);
  const auto packed = pattern(64 * 1024, 0x72);
  const auto truncated = pattern(512 * 1024, 0x73);
  std::vector<ObjectId> more_loose;
  std::filesystem::path truncated_path;
  {
    InterposedLocalStoreFiles files;
    auto store = bed.open(root, options, LocalStoreMode::authoritative, nullptr, files);
    ASSERT_TRUE(store->put(object_id(loose), loose));
    ASSERT_TRUE(store->put(object_id(packed), packed));
    ASSERT_FALSE(store->is_packed(object_id(loose)));
    ASSERT_TRUE(store->is_packed(object_id(packed)));
    for (int i = 0; i < 12; ++i) {
      auto data = pattern(300 * 1024, static_cast<uint8_t>(0x30 + i));
      ASSERT_TRUE(store->put(object_id(data), data));
      more_loose.push_back(object_id(data));
    }

    std::atomic_bool loose_read{false};
    std::atomic_bool packed_read{false};
    files.before([&](InterposedLocalStoreFiles::Op op, const std::filesystem::path&) {
      if (op == InterposedLocalStoreFiles::Op::read) loose_read = true;
      if (op == InterposedLocalStoreFiles::Op::read_at) packed_read = true;
    });
    // The recorded device reads prove has() reads nothing, not just its
    // answer.
    EXPECT_TRUE(store->has(object_id(loose)));
    EXPECT_TRUE(store->has(object_id(packed)));
    EXPECT_FALSE(store->has(ObjectId{}));
    EXPECT_FALSE(loose_read.load());
    EXPECT_FALSE(packed_read.load());
    EXPECT_TRUE(store->get(object_id(loose)).has_value());
    EXPECT_TRUE(store->get(object_id(packed)).has_value());
    EXPECT_TRUE(loose_read.load());
    EXPECT_TRUE(packed_read.load());
    files.before({});

    ASSERT_TRUE(store->put(object_id(truncated), truncated));
    truncated_path = store->object_path(object_id(truncated));
  }
  {
    std::ofstream truncate(truncated_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(truncate.good());
  }

  auto reopened = bed.open(root, options);
  // Every loose object name, including the truncated file's.
  ASSERT_TRUE(wait_until([&] { return reopened->diagnostics().presence_index_entries == more_loose.size() + 2; }));
  for (const auto& id : more_loose) EXPECT_TRUE(reopened->has(id));
  EXPECT_TRUE(reopened->has(object_id(loose)));
  EXPECT_TRUE(reopened->has(object_id(packed)));
  EXPECT_FALSE(reopened->has(ObjectId{}));
  // Presence is not stat'ed, so the empty file is found on read, which prunes
  // it and reports the object absent.
  EXPECT_FALSE(reopened->get(object_id(truncated)).has_value());
  EXPECT_FALSE(std::filesystem::exists(truncated_path));
  EXPECT_FALSE(reopened->has(object_id(truncated)));
  ASSERT_TRUE(reopened->put(object_id(truncated), truncated));
  EXPECT_TRUE(reopened->has(object_id(truncated)));
}

void corrupt_last_byte(const std::filesystem::path& path) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.good());
  file.seekg(-1, std::ios::end);
  char byte{};
  file.read(&byte, 1);
  ASSERT_TRUE(file.good());
  byte ^= 0x5a;
  file.seekp(-1, std::ios::end);
  file.write(&byte, 1);
  file.flush();
  ASSERT_TRUE(file.good());
}

// Macha (tests/test_storage_metadata.cpp): test_local_store. A loose object
// is sealed on disk; a re-put of an unchanged file takes the fast path and of
// a corrupted one the full validation, which repairs it; a clean restart
// trusts the accounting checkpoint with no scan; a corrupt checkpoint falls
// back to one reconciliation and is trusted again after; a store path is
// locked against a second opener.
TEST(StoreLocalStore, ALooseObjectIsSealedAccountedAndReaffirmed) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  const auto options = LocalStoreOptions::loose_only(64 * 1024 * 1024);
  const auto plain = pattern(1024 * 1024 + 37);
  const auto id = object_id(plain);
  uint64_t accounted_used = 0;

  {
    auto store = bed.open(root, options);
    ASSERT_TRUE(store->put(id, plain));
    EXPECT_TRUE(store->has(id));
    ASSERT_TRUE(store->get(id).has_value());
    EXPECT_EQ(*store->get(id), plain);
    ASSERT_TRUE(wait_until([&] { return store->scan_complete(); }));
    accounted_used = store->used();
    EXPECT_GT(accounted_used, plain.size());

    bool found_plain = false;
    for (auto& file : std::filesystem::recursive_directory_iterator(root / "objects")) {
      if (!file.is_regular_file()) continue;
      std::ifstream in(file.path(), std::ios::binary);
      Bytes disk(std::istreambuf_iterator<char>(in), {});
      const auto needle = std::span<const uint8_t>(plain).subspan(100, 128);
      found_plain = std::search(disk.begin(), disk.end(), needle.begin(), needle.end()) != disk.end();
    }
    EXPECT_FALSE(found_plain);

    const auto reaffirm_before = store->diagnostics();
    ASSERT_TRUE(store->put(id, plain));
    const auto reaffirm_after = store->diagnostics();
    EXPECT_EQ(reaffirm_after.loose_reaffirmation_fast_paths, reaffirm_before.loose_reaffirmation_fast_paths + 1);
    EXPECT_EQ(reaffirm_after.loose_reaffirmation_full_validations, reaffirm_before.loose_reaffirmation_full_validations);

    // A successful put means the named immutable copy holds the bytes asked
    // for, not merely that its pathname exists.
    corrupt_last_byte(store->object_path(id));
    const auto repair_before = store->diagnostics();
    ASSERT_TRUE(store->put(id, plain));
    const auto repair_after = store->diagnostics();
    EXPECT_EQ(repair_after.loose_reaffirmation_full_validations, repair_before.loose_reaffirmation_full_validations + 1);
    const auto repaired = store->get(id);
    ASSERT_TRUE(repaired.has_value());
    EXPECT_EQ(*repaired, plain);
  }

  // A clean restart restores exact accounting from the small checkpoint
  // without starting a walk of every object.
  EXPECT_TRUE(std::filesystem::exists(root / ".macha.accounting"));
  EXPECT_GE(std::filesystem::file_size(root / ".macha.accounting"), 256u);
  {
    auto reopened = bed.open(root, options);
    EXPECT_TRUE(reopened->scan_complete());
    EXPECT_EQ(reopened->used(), accounted_used);
    ASSERT_TRUE(reopened->get(id).has_value());
    EXPECT_EQ(*reopened->get(id), plain);
  }

  // Corrupt or missing state falls back to one full reconciliation, then
  // writes a trusted checkpoint again.
  {
    std::ofstream out(root / ".macha.accounting", std::ios::binary | std::ios::trunc);
    const Bytes junk(256, 0x5a);
    out.write(reinterpret_cast<const char*>(junk.data()), static_cast<std::streamsize>(junk.size()));
  }
  {
    auto recovered = bed.open(root, options);
    ASSERT_TRUE(wait_until([&] { return recovered->scan_complete(); }));
    EXPECT_EQ(recovered->used(), accounted_used);
    ASSERT_TRUE(recovered->get(id).has_value());
  }
  {
    auto recovered_restart = bed.open(root, options);
    EXPECT_TRUE(recovered_restart->scan_complete());
    EXPECT_EQ(recovered_restart->used(), accounted_used);
  }

  {
    StorageLock first(bed.dir.path() / "locked");
    bool rejected = false;
    try {
      StorageLock second(bed.dir.path() / "locked");
    } catch (...) {
      rejected = true;
    }
    EXPECT_TRUE(rejected);
  }
}

// Requirement S1: a put refuses bytes that are not the id's, and stores
// nothing.
TEST(StoreLocalStore, APutRefusesBytesThatAreNotTheId) {
  StoreBed bed;
  auto store = bed.open(bed.dir.path() / "store", LocalStoreOptions{.limit = 64ULL * 1024 * 1024});
  const auto data = pattern(1000, 1);
  const auto other = object_id(pattern(1000, 2));
  try {
    (void)store->put(other, data);
    FAIL() << "a mismatched id was stored";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "object hash mismatch");
  }
  EXPECT_FALSE(store->has(other));
  EXPECT_FALSE(store->get(other).has_value());
}

// Requirement S3: the threshold and target are both zero or both set, the
// target at least the threshold; the limit is non-zero.
TEST(StoreLocalStore, RefusesOptionsThatDoNotHoldTogether) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  EXPECT_THROW(bed.open(root, LocalStoreOptions{}), std::invalid_argument);
  EXPECT_THROW(bed.open(root, LocalStoreOptions{.limit = 1024, .pack_threshold = 0, .pack_target_size = 1024}), std::invalid_argument);
  EXPECT_THROW(bed.open(root, LocalStoreOptions{.limit = 1024, .pack_threshold = 1024, .pack_target_size = 0}), std::invalid_argument);
  EXPECT_THROW(bed.open(root, LocalStoreOptions{.limit = 1024, .pack_threshold = 2048, .pack_target_size = 1024}), std::invalid_argument);
  // Macha's defaults: packs at or below 1 MiB, 64 MiB packs.
  const LocalStoreOptions defaults;
  EXPECT_EQ(defaults.pack_threshold, 1024u * 1024);
  EXPECT_EQ(defaults.pack_target_size, 64u * 1024 * 1024);
}

// Ages are the wall clock's, injected: a touch makes an object young again,
// for a packed one through a touch record that survives a restart.
TEST(StoreLocalStore, AgesFollowTheWallClockAndATouch) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  const auto options = packed_options(64ULL * 1024 * 1024, 1024 * 1024);
  const auto small = pattern(10 * 1024, 1);
  const auto large = pattern(300 * 1024, 2);
  {
    auto store = bed.open(root, options);
    ASSERT_TRUE(store->put(object_id(small), small));
    ASSERT_TRUE(store->put(object_id(large), large));
    for (const auto& data : {small, large}) {
      const auto id = object_id(data);
      EXPECT_FALSE(store->older_than(id, 1h));
      EXPECT_FALSE(store->remove_if_older_than(id, 1h));
    }
    bed.wall.advance(2h);
    for (const auto& data : {small, large}) EXPECT_TRUE(store->older_than(object_id(data), 1h));
    store->touch(object_id(small));
    store->touch(object_id(large));
    for (const auto& data : {small, large}) EXPECT_FALSE(store->older_than(object_id(data), 1h));
  }
  auto store = bed.open(root, options);
  EXPECT_FALSE(store->older_than(object_id(small), 1h));
  bed.wall.advance(2h);
  EXPECT_TRUE(store->remove_if_older_than(object_id(small), 1h));
  EXPECT_TRUE(store->remove_if_older_than(object_id(large), 1h));
  EXPECT_FALSE(store->has(object_id(small)));
  EXPECT_FALSE(store->has(object_id(large)));
}

// Every object held, packed and loose, once each.
TEST(StoreLocalStore, ListsEveryObjectOnce) {
  StoreBed bed;
  auto store = bed.open(bed.dir.path() / "store", packed_options(64ULL * 1024 * 1024, 1024 * 1024));
  std::vector<ObjectId> held;
  for (int i = 0; i < 6; ++i) {
    const auto data = pattern(i % 2 ? 300 * 1024 : 1000, static_cast<uint8_t>(i));
    ASSERT_TRUE(store->put(object_id(data), data));
    held.push_back(object_id(data));
  }
  auto listed = store->list();
  std::sort(listed.begin(), listed.end());
  std::sort(held.begin(), held.end());
  EXPECT_EQ(listed, held);
}

// Tom: Dagda must handle small values well, and packs are the small-object
// path. Ten thousand objects of 100 to 1000 bytes land in packs (no loose
// file, a handful of pack files), survive a reopen, and read back; the time
// is printed for the record, not asserted.
TEST(StoreLocalStore, ManySmallObjectsLandInPacksAndReadBack) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  LocalStoreOptions options;
  options.limit = 1ULL << 30;
  std::mt19937_64 random(1);
  std::vector<std::pair<ObjectId, Bytes>> objects;
  objects.reserve(10000);
  uint64_t bytes = 0;
  for (int i = 0; i < 10000; ++i) {
    Bytes data(100 + random() % 901);
    for (auto& byte : data) byte = static_cast<uint8_t>(random());
    bytes += data.size();
    objects.emplace_back(object_id(data), std::move(data));
  }
  const auto started = std::chrono::steady_clock::now();
  {
    auto store = bed.open(root, options);
    for (const auto& [id, data] : objects) {
      const auto generation = store->put_deferred(id, data);
      ASSERT_TRUE(generation.has_value());
    }
    store->durability_barrier();
    for (const auto& [id, data] : objects) ASSERT_TRUE(store->is_packed(id));
  }
  const auto written = std::chrono::steady_clock::now();
  EXPECT_EQ(regular_files_below(root / "objects"), 0u);
  // Each record is its object plus a 125-byte header: about 6.7 MB in one
  // 64 MiB pack.
  EXPECT_EQ(regular_files_below(root / "packs"), 1u);
  auto reopened = bed.open(root, options);
  const auto opened = std::chrono::steady_clock::now();
  for (const auto& [id, data] : objects) {
    ASSERT_TRUE(reopened->has(id));
    ASSERT_EQ(reopened->get(id), std::optional<Bytes>{data});
  }
  const auto read = std::chrono::steady_clock::now();
  EXPECT_EQ(reopened->used(), bytes + objects.size() * 125);
  const auto ms = [](auto d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
  std::cout << "small values: " << objects.size() << " objects, " << bytes << " bytes: written and made durable in " << ms(written - started)
            << " ms, reopened in " << ms(opened - written) << " ms, read back in " << ms(read - opened) << " ms\n";
}

}  // namespace
}  // namespace dagda::store
