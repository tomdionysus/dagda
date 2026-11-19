//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Macha's scale (macnessa: about 1.03 million objects, 42 packs of CONTROL), which no
// other case comes near. Heavy, so disabled: run with --gtest_also_run_disabled_tests
// --gtest_filter='Scale.*'. They measure the real clock on purpose and print BENCH lines;
// the bounds are only what would plainly be wrong, and nothing is driven by the clock.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/retention.h"
#include "dagda/migrate/manifest.h"
#include "dagda/migrate/migrate.h"
#include "dagda/store/local_store.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/legacy_v18.h"
#include "dagda/test/fixtures/migrate.h"
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

// The manifest at macnessa's counts (the evaluation's 1,031,852 loose objects, 42
// packs of CONTROL holding 314,390 puts and 1,883,019 touches and tombstones, as the
// dry run counted them, and 17 of DATA, whose 340,000 records are a guess, since no
// count of them has been taken), with small objects, since a manifest's size follows
// the records and not their bytes. Passes before and after a real migration of all
// of it, one object lost among them and named, and how large and how slow each is.
// DAGDA_SCALE_DIVIDE thins it for a machine that cannot hold a million files.
TEST(Scale, DISABLED_ManifestsOfMachasCounts) {
  const auto divide = std::getenv("DAGDA_SCALE_DIVIDE") ? std::strtoull(std::getenv("DAGDA_SCALE_DIVIDE"), nullptr, 10) : 1;
  const uint64_t loose = 1031852 / divide;
  const uint64_t control_puts = 314390 / divide;
  const uint64_t control_others = 1883019 / divide;
  const uint64_t data_packed = 340000 / divide;
  test::StoreBed bed;
  test::v18::V18State v18(bed);
  std::mt19937_64 random(7);
  std::vector<ObjectId> held;
  held.reserve(loose + data_packed);
  const auto root = bed.dir.path() / "scale";
  auto started = Clock::now();
  const auto object = [&] {
    Bytes data(64);
    for (auto& byte : data) byte = static_cast<uint8_t>(random());
    return data;
  };
  for (uint64_t n = 0; n < loose; ++n) {
    const auto data = object();
    const auto id = store::object_id(data);
    test::legacy::write_loose_v18(v18.backend, *bed.key, id, data);
    held.push_back(id);
  }
  bench("manifest.build_loose", ms_since(started), loose);
  // Whole records appended in one write a pack: 17 of DATA, 42 of CONTROL.
  const auto write_packs = [&](const std::filesystem::path& store_root, uint64_t packs, uint64_t puts, uint64_t others) {
    std::filesystem::create_directories(store_root / "packs");
    std::vector<ObjectId> recent;
    for (uint64_t pack = 1; pack <= packs; ++pack) {
      Bytes bytes;
      for (uint64_t n = 0; n < puts / packs; ++n) {
        const auto data = object();
        const auto id = store::object_id(data);
        const auto sealed = test::legacy::seal_v18(*bed.key, id, data);
        store::local_format::PackHeader header;
        header.type = store::local_format::pack_put;
        header.touched_ms = 1793700000000ULL + n;
        header.id = id;
        header.plain_size = data.size();
        header.payload_size = sealed.ciphertext.size();
        header.nonce = sealed.nonce;
        header.tag = sealed.tag;
        const auto encoded = store::local_format::encode_pack_header(header);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
        bytes.insert(bytes.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
        recent.push_back(id);
      }
      for (uint64_t n = 0; n < others / packs; ++n) {
        store::local_format::PackHeader header;
        header.type = n % 8 ? store::local_format::pack_touch : store::local_format::pack_remove;
        header.touched_ms = 1793800000000ULL + n;
        header.id = recent[n % recent.size()];
        const auto encoded = store::local_format::encode_pack_header(header);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
      }
      test::legacy::write_file(store_root / "packs" / store::local_format::pack_name(pack), bytes);
    }
  };
  write_packs(v18.backend, 17, data_packed, 0);
  write_packs(v18.control, 42, control_puts, control_others);
  bench("manifest.build_packs", ms_since(started), control_puts + control_others + data_packed);
  // A held ledger and the claims and counts of that size.
  std::sort(held.begin(), held.end());
  {
    ledger::ObjectTrie trie(v18.state / "ledger" / ("data-" + dannan::identity::to_string(v18.token)), bed.key, {}, test::quiet_logger());
    std::vector<ledger::ObjectTrie::Record> records;
    records.reserve(held.size());
    for (const auto& id : held) records.emplace_back(id, Bytes(8, 1));
    trie.replace_all(std::move(records));
  }
  {
    ledger::retention_format::ObjectState state;
    state.adds[v18.origin] = 1;
    const auto value = ledger::retention_format::encode_state(state);
    ledger::ObjectTrie trie(v18.state / "retention" / "ledger-data", bed.key, {}, test::quiet_logger());
    std::vector<ledger::ObjectTrie::Record> records;
    records.reserve(held.size());
    for (const auto& id : held) records.emplace_back(id, value);
    trie.replace_all(std::move(records));
  }
  bench("manifest.build_ledgers", ms_since(started), held.size());

  const auto manifests = root;
  std::filesystem::create_directories(manifests);
  migrate::ManifestOptions options;
  options.backends = {v18.backend};
  options.cache = v18.cache;
  options.node = v18.node;
  options.metadata = std::make_shared<test::v18::FilledMetadata>();
  started = Clock::now();
  const auto before = migrate::write_manifest(manifests / "before", v18.state, v18.cluster(), v18.custody(), options);
  bench("manifest.before", ms_since(started), before.items);
  std::cout << "BENCH name=manifest.size bytes=" << before.manifest_bytes << " items=" << before.items
            << " bytes_per_item=" << before.manifest_bytes / before.items << std::endl;

  started = Clock::now();
  const auto report = v18.migrate();
  bench("manifest.migrate", ms_since(started), report.loose_migrated + report.pack_records_migrated);
  EXPECT_EQ(report.loose_unreadable + report.pack_records_unreadable, 0u);

  started = Clock::now();
  const auto after = migrate::write_manifest(manifests / "after", v18.state, v18.cluster(), v18.custody(), options);
  bench("manifest.after", ms_since(started), after.items);
  EXPECT_EQ(after.items, before.items);

  started = Clock::now();
  const auto compared = migrate::compare_manifests(manifests / "before", manifests / "after");
  bench("manifest.compare", ms_since(started), compared.categories.size());
  EXPECT_TRUE(compared.passed()) << compared.summary();
  std::cout << compared.summary() << std::endl;

  // One object out of a million, and it is named.
  std::filesystem::remove(store::local_format::loose_path(v18.backend / "objects", held[held.size() / 2]));
  (void)migrate::write_manifest(manifests / "damaged", v18.state, v18.cluster(), v18.custody(), options);
  const auto named = migrate::compare_manifests(manifests / "before", manifests / "damaged");
  EXPECT_FALSE(named.passed());
  EXPECT_EQ(named.difference_count, 1u) << named.summary();
  EXPECT_TRUE(named.names(store::to_string(held[held.size() / 2])));
}

}  // namespace
}  // namespace dagda
