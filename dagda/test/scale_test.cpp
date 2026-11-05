//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Macha's scale (live-a: about 1.03 million objects, 42 packs of CONTROL), which no
// other case comes near. Heavy, so disabled: run with --gtest_also_run_disabled_tests
// --gtest_filter='Scale.*'. They measure the real clock on purpose and print BENCH lines;
// the bounds are only what would plainly be wrong, and nothing is driven by the clock.

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <random>
#include <vector>

#include "dagda/ledger/object_trie.h"
#include "dagda/store/local_store.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using store::Bytes;
using store::ObjectId;

int64_t ms_since(Clock::time_point started) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count(); }

void bench(const char* name, int64_t ms, uint64_t ops) {
  std::cout << "BENCH name=" << name << " ms=" << ms << " ops=" << ops << " us_per_op=" << (ops ? ms * 1000 / static_cast<int64_t>(ops) : 0) << std::endl;
}

ObjectId random_id(std::mt19937_64& random) {
  ObjectId id;
  for (auto& byte : id.bytes) byte = static_cast<uint8_t>(random());
  return id;
}

// A held trie of a million ids, Macha's largest loose count: built, reopened, looked up
// and walked whole, within the 30 s open bound of Macha's own measurement.
TEST(Scale, DISABLED_ATrieOfAMillionIds) {
  constexpr uint64_t records = 1000000;
  dannan::test::TempDir dir;
  std::mt19937_64 random(1);
  std::vector<ObjectId> sample;
  auto started = Clock::now();
  {
    ledger::ObjectTrie trie(dir.path(), test::key_of(13), {}, test::quiet_logger());
    std::vector<ledger::ObjectTrie::Change> batch;
    batch.reserve(10000);
    for (uint64_t i = 0; i < records; ++i) {
      batch.push_back({random_id(random), Bytes(8, 1)});
      if (i % 97 == 0 && sample.size() < 10000) sample.push_back(batch.back().id);
      if (batch.size() == 10000) {
        trie.apply(batch);
        batch.clear();
      }
    }
    trie.apply(batch);
    trie.checkpoint();
    const auto stats = trie.stats();
    std::cout << "BENCH name=trie.size node_file_bytes=" << stats.node_file_bytes << " journal_bytes=" << stats.journal_bytes << std::endl;
  }
  bench("trie.build", ms_since(started), records);

  started = Clock::now();
  ledger::ObjectTrie trie(dir.path(), test::key_of(13), {}, test::quiet_logger());
  const auto open_ms = ms_since(started);
  bench("trie.open", open_ms, 1);
  EXPECT_LT(open_ms, 30000);

  started = Clock::now();
  for (const auto& id : sample) EXPECT_TRUE(trie.get(id).has_value());
  bench("trie.get.present", ms_since(started), sample.size());

  started = Clock::now();
  uint64_t walked = 0;
  std::optional<ObjectId> after;
  while (true) {
    const auto page = trie.next(after, 4096);
    if (page.empty()) break;
    walked += page.size();
    after = page.back().first;
  }
  bench("trie.walk", ms_since(started), walked);
  EXPECT_EQ(walked, records);
}

// A hundred thousand objects across packs: written, reopened from the packs alone, found,
// read back, and a pack's dead bytes reclaimed. Reads every object it samples.
TEST(Scale, DISABLED_AHundredThousandObjectsAcrossPacks) {
  constexpr uint64_t objects = 100000;
  test::StoreBed bed;
  const auto root = bed.dir.path() / "store";
  store::LocalStoreOptions options;
  options.limit = 8ULL * 1024 * 1024 * 1024;
  options.pack_target_size = 16 * 1024 * 1024;
  options.pack_threshold = 1024 * 1024;
  std::vector<ObjectId> ids;
  ids.reserve(objects);
  auto started = Clock::now();
  {
    auto store = bed.open(root, options);
    ASSERT_TRUE(test::wait_until([&] { return store->scan_complete(); }));
    for (uint64_t n = 0; n < objects; ++n) {
      const auto data = test::numbered(n);
      const auto id = store::object_id(data);
      ASSERT_TRUE(store->put_deferred(id, data).has_value());
      ids.push_back(id);
    }
    store->durability_barrier();
  }
  bench("store.write", ms_since(started), objects);

  started = Clock::now();
  auto store = bed.open(root, options);
  const auto open_ms = ms_since(started);
  bench("store.open", open_ms, 1);
  EXPECT_LT(open_ms, 30000);

  started = Clock::now();
  uint64_t present = 0;
  for (const auto& id : ids) present += store->has(id) ? 1 : 0;
  bench("store.has", ms_since(started), objects);
  EXPECT_EQ(present, objects);

  started = Clock::now();
  uint64_t opened = 0;
  for (uint64_t n = 0; n < objects; n += 37) {
    const auto got = store->get(ids[n]);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, test::numbered(n));
    ++opened;
  }
  bench("store.get", ms_since(started), opened);

  // A fifth removed: a slice reclaims from one victim pack, and everything left still reads.
  for (uint64_t n = 0; n < objects; n += 5) ASSERT_TRUE(store->remove(ids[n]));
  const auto before = store->used();
  started = Clock::now();
  EXPECT_TRUE(store->compact_packs());
  bench("store.compact_one", ms_since(started), 1);
  EXPECT_LT(store->used(), before);
  for (uint64_t n = 1; n < objects; n += 997) EXPECT_TRUE(store->has(ids[n]) == (n % 5 != 0)) << n;
}

}  // namespace
}  // namespace dagda
