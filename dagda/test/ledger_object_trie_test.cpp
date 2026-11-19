//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirements L1 and L2, and layout v18's trie files.
//
// Moved from Macha's tests/test_object_ledger.cpp (develop 5a06d3c):
// test_the_trie_holds_exactly_what_was_written,
// test_the_trie_hash_depends_only_on_its_records,
// test_a_subtree_reads_the_same_in_every_trie_holding_it,
// test_a_reopened_trie_is_the_one_that_was_closed,
// test_a_crash_at_any_step_of_a_checkpoint_loses_nothing,
// test_a_trie_rewrites_its_node_file_and_stays_bounded,
// test_a_trie_larger_than_its_cache_reads_what_it_needs,
// test_a_trie_with_a_torn_journal_keeps_what_was_whole,
// test_a_format_1_trie_opens_and_is_rewritten,
// test_a_snapshot_is_frozen_through_writes_and_rewrites, and the heavy
// test_a_large_trie_opens_within_the_bound (disabled, as Macha's heavy tests
// are outside its gate).
//
// The known answers were computed outside both codebases from Macha's
// object_trie.cpp:33-57, :208-238, :430-445 and sealed_journal.cpp: SHA-256
// by a reference library, AES-256 blocks by `openssl enc -aes-256-ecb`,
// GHASH by hand, checked first against the GCM specification's test case
// 16. Their ids are the SHA-256 of "dagda trie known answer <i>", each held
// (1 as eight big-endian bytes).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/sealed_journal.h"
#include "dagda/store/object_id.h"
#include "dagda/test/fixtures/ledger.h"
#include "dannan/identity/crypto.h"
#include "dannan/net/codec.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

using dannan::identity::hex;
using dannan::identity::sha256;
using dannan::net::Writer;
using test::check_matches;
using test::clustered_id;
using test::contents;
using test::id_of;
using test::key_of;
using test::prefix_of;
using test::quiet_logger;
using test::small_options;
using test::trie_of;
using test::value_of;

Bytes unhex(const std::string& text) { return dannan::identity::unhex(text).value(); }

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes{std::istreambuf_iterator<char>(in), {}};
}

void write_file(const std::filesystem::path& path, const Bytes& bytes) {
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::map<ObjectId, Bytes> below(const std::map<ObjectId, Bytes>& records, const ObjectTrie::Prefix& prefix) {
  std::map<ObjectId, Bytes> out;
  for (const auto& [id, value] : records)
    if (prefix.covers(id)) out.emplace(id, value);
  return out;
}

// Macha: test_the_trie_holds_exactly_what_was_written.
TEST(LedgerObjectTrie, HoldsExactlyWhatWasWritten) {
  dannan::test::TempDir dir;
  std::mt19937_64 random(11);
  std::map<ObjectId, Bytes> model;
  std::vector<ObjectId> known;
  ObjectTrie trie(dir.path(), key_of(6), small_options(), quiet_logger());
  EXPECT_EQ(trie.size(), 0u);
  EXPECT_EQ(trie.root_hash(), Hash256{});
  for (int round = 0; round < 60; ++round) {
    std::vector<ObjectTrie::Change> batch;
    const int count = 1 + static_cast<int>(random() % 400);
    for (int i = 0; i < count; ++i) {
      const auto roll = random() % 10;
      if (roll < 6 || known.empty()) {
        const auto id = id_of(random);
        known.push_back(id);
        batch.push_back({id, value_of(random)});
      } else if (roll < 8) {
        batch.push_back({known[random() % known.size()], value_of(random)});
      } else {
        batch.push_back({known[random() % known.size()], std::nullopt});
      }
    }
    trie.apply(batch);
    for (const auto& change : batch) {
      if (change.value)
        model[change.id] = *change.value;
      else
        model.erase(change.id);
    }
    EXPECT_EQ(trie.size(), model.size());
  }
  check_matches(trie, model, random);
  EXPECT_GT(trie.stats().checkpoints, 0u);
}

// Macha: test_the_trie_hash_depends_only_on_its_records.
TEST(LedgerObjectTrie, TheHashDependsOnlyOnItsRecords) {
  std::mt19937_64 random(12);
  std::vector<ObjectTrie::Change> records;
  for (int i = 0; i < 3000; ++i) records.push_back({id_of(random), value_of(random)});
  dannan::test::TempDir one_dir;
  dannan::test::TempDir two_dir;
  ObjectTrie one(one_dir.path(), key_of(7), small_options(), quiet_logger());
  ObjectTrie two(two_dir.path(), key_of(7), small_options(), quiet_logger());
  one.apply(records);
  // The same records in another order, by way of extra records written and
  // erased again, and in many small batches.
  auto shuffled = records;
  std::shuffle(shuffled.begin(), shuffled.end(), random);
  // Extra records sharing a first byte, so one subtree grows interior two
  // levels down and must collapse back to a leaf when they go.
  std::vector<ObjectTrie::Change> extra;
  for (int i = 0; i < 500; ++i) {
    auto id = id_of(random);
    id.bytes[0] = 7;
    id.bytes[1] = static_cast<uint8_t>(i % 3);
    extra.push_back({id, value_of(random)});
  }
  two.apply(extra);
  for (size_t from = 0; from < shuffled.size(); from += 37)
    two.apply(std::span<const ObjectTrie::Change>(shuffled).subspan(from, std::min<size_t>(37, shuffled.size() - from)));
  for (auto& change : extra) change.value.reset();
  two.apply(extra);
  EXPECT_EQ(one.size(), two.size());
  EXPECT_EQ(one.root_hash(), two.root_hash());
  EXPECT_TRUE(contents(one) == contents(two));
  // One value different: the hashes differ.
  two.apply(std::vector<ObjectTrie::Change>{{records[0].id, Bytes{9, 9, 9}}});
  EXPECT_NE(one.root_hash(), two.root_hash());
}

// Macha: test_a_subtree_reads_the_same_in_every_trie_holding_it. What is
// below a prefix reads the same from every trie holding the same records
// below it: one holding only those, one holding thousands more (where the
// prefix sits deeper in interior nodes or inside a larger leaf), and the
// same trie after a checkpoint and a reopen, read from disk.
TEST(LedgerObjectTrie, ASubtreeReadsTheSameInEveryTrieHoldingIt) {
  std::mt19937_64 random(21);
  std::map<ObjectId, Bytes> records;
  while (records.size() < 3000) records.emplace(clustered_id(random), value_of(random));
  dannan::test::TempDir whole_dir;
  auto whole = trie_of(whole_dir.path(), records, random);
  std::vector<ObjectTrie::Prefix> prefixes{ObjectTrie::Prefix{}};
  for (uint8_t length = 1; length <= 4; ++length)
    for (int i = 0; i < 3; ++i) {
      auto at = records.begin();
      std::advance(at, static_cast<long>(random() % records.size()));
      prefixes.push_back(prefix_of(at->first, length));
    }
  // Nothing below it.
  prefixes.push_back(prefix_of(id_of(random), 2));
  const auto expected_children = whole->snapshot().children(prefixes);
  const auto expected_records = whole->snapshot().records(prefixes);
  // Asked for at most a leaf, the root's thousands are refused unread.
  EXPECT_THROW((void)whole->snapshot().records(std::span(&prefixes.front(), 1), ObjectTrie::leaf_max), std::length_error);
  whole->checkpoint();
  whole.reset();
  ObjectTrie reopened(whole_dir.path(), key_of(8), small_options(), quiet_logger());
  EXPECT_TRUE(reopened.snapshot().children(prefixes) == expected_children);
  EXPECT_TRUE(reopened.snapshot().records(prefixes) == expected_records);

  for (size_t i = 0; i < prefixes.size(); ++i) {
    const auto& prefix = prefixes[i];
    const auto mine = below(records, prefix);
    EXPECT_TRUE(expected_records[i] == std::vector<ObjectTrie::Record>(mine.begin(), mine.end()));
    uint64_t counted = 0;
    for (const auto& summary : expected_children[i]) counted += summary.count;
    EXPECT_EQ(counted, mine.size());
    // Alone, and among others that are not below it.
    auto crowded = mine;
    // (Everything is below the empty prefix.)
    while (prefix.length && crowded.size() < mine.size() + 600) {
      const auto id = clustered_id(random);
      if (!prefix.covers(id)) crowded.emplace(id, value_of(random));
    }
    const std::map<ObjectId, Bytes>* const sets[] = {&mine, &crowded};
    for (const auto* set : sets) {
      dannan::test::TempDir dir;
      const auto other = trie_of(dir.path(), *set, random);
      const auto snapshot = other->snapshot();
      EXPECT_TRUE(snapshot.children(std::span(&prefix, 1)).front() == expected_children[i]);
      EXPECT_TRUE(snapshot.records(std::span(&prefix, 1)).front() == expected_records[i]);
    }
  }
}

// Macha: test_a_reopened_trie_is_the_one_that_was_closed.
TEST(LedgerObjectTrie, AReopenedTrieIsTheOneThatWasClosed) {
  dannan::test::TempDir dir;
  std::mt19937_64 random(13);
  std::map<ObjectId, Bytes> model;
  Hash256 hash{};
  {
    ObjectTrie trie(dir.path(), key_of(8), small_options(), quiet_logger());
    for (int round = 0; round < 20; ++round) {
      std::vector<ObjectTrie::Change> batch;
      for (int i = 0; i < 300; ++i) {
        const auto id = id_of(random);
        const auto value = value_of(random);
        batch.push_back({id, value});
        model[id] = value;
      }
      trie.apply(batch);
    }
    const auto last = id_of(random);
    model[last] = Bytes{1};
    trie.apply(std::vector<ObjectTrie::Change>{{last, Bytes{1}}});
    hash = trie.root_hash();
    // Some changes since the last checkpoint stay only in the journal.
    EXPECT_GT(trie.stats().journal_bytes, 0u);
  }
  ObjectTrie reopened(dir.path(), key_of(8), small_options(), quiet_logger());
  EXPECT_EQ(reopened.root_hash(), hash);
  check_matches(reopened, model, random);
}

// Macha: test_a_crash_at_any_step_of_a_checkpoint_loses_nothing. A
// checkpoint is three writes: the changed nodes, then the root, then the
// emptied journal. A crash after any of them reopens to the same records.
TEST(LedgerObjectTrie, ACrashAtAnyStepOfACheckpointLosesNothing) {
  std::mt19937_64 random(14);
  for (int step = 0; step < 3; ++step) {
    dannan::test::TempDir dir;
    std::map<ObjectId, Bytes> model;
    ObjectTrie::Options options = small_options();
    // Only when asked.
    options.checkpoint_bytes = 1ULL << 40;
    const auto keep = [&](const std::string& name) {
      const auto from = dir.path() / name;
      const auto to = dir.path() / (name + ".saved");
      std::error_code ec;
      std::filesystem::remove(to, ec);
      if (std::filesystem::exists(from)) std::filesystem::copy_file(from, to);
    };
    const auto restore = [&](const std::string& name) {
      const auto saved = dir.path() / (name + ".saved");
      std::error_code ec;
      std::filesystem::remove(dir.path() / name, ec);
      if (std::filesystem::exists(saved)) std::filesystem::rename(saved, dir.path() / name);
    };
    {
      ObjectTrie trie(dir.path(), key_of(9), options, quiet_logger());
      std::vector<ObjectTrie::Change> first;
      for (int i = 0; i < 400; ++i) {
        first.push_back({id_of(random), value_of(random)});
        model[first.back().id] = *first.back().value;
      }
      trie.apply(first);
      trie.checkpoint();
      std::vector<ObjectTrie::Change> second;
      for (int i = 0; i < 400; ++i) {
        second.push_back({id_of(random), value_of(random)});
        model[second.back().id] = *second.back().value;
      }
      trie.apply(second);
      keep("root");
      keep("journal.log");
      trie.checkpoint();
    }
    // Undo what came after the crash point: step 0, the root and the
    // journal as before (nodes appended, nothing published); step 1, the
    // journal as before (root published, journal not emptied); step 2,
    // nothing (the whole checkpoint).
    if (step == 0) {
      restore("root");
      restore("journal.log");
    } else if (step == 1) {
      restore("journal.log");
    }
    ObjectTrie reopened(dir.path(), key_of(9), options, quiet_logger());
    check_matches(reopened, model, random);
  }
}

// Macha: test_a_trie_rewrites_its_node_file_and_stays_bounded.
TEST(LedgerObjectTrie, RewritesItsNodeFileAndStaysBounded) {
  dannan::test::TempDir dir;
  std::mt19937_64 random(15);
  std::map<ObjectId, Bytes> model;
  std::vector<ObjectId> ids;
  for (int i = 0; i < 2000; ++i) ids.push_back(id_of(random));
  ObjectTrie::Options options = small_options();
  ObjectTrie trie(dir.path(), key_of(10), options, quiet_logger());
  // The same ids rewritten many times: every checkpoint supersedes nodes.
  for (int round = 0; round < 40; ++round) {
    std::vector<ObjectTrie::Change> batch;
    for (const auto& id : ids) {
      const auto value = value_of(random);
      batch.push_back({id, value});
      model[id] = value;
    }
    trie.apply(batch);
  }
  const auto stats = trie.stats();
  EXPECT_GT(stats.rewrites, 0u);
  EXPECT_LE(stats.node_file_bytes, 2 * stats.live_bytes + options.rewrite_floor_bytes + options.checkpoint_bytes * 4);
  // Exactly one node file is left.
  size_t node_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path())) node_files += entry.path().filename().string().starts_with("nodes-") ? 1 : 0;
  EXPECT_EQ(node_files, 1u);
  check_matches(trie, model, random);
  ObjectTrie reopened(dir.path(), key_of(10), options, quiet_logger());
  check_matches(reopened, model, random);
}

// Macha: test_a_trie_larger_than_its_cache_reads_what_it_needs.
TEST(LedgerObjectTrie, ATrieLargerThanItsCacheReadsWhatItNeeds) {
  dannan::test::TempDir dir;
  std::mt19937_64 random(16);
  std::map<ObjectId, Bytes> model;
  ObjectTrie::Options options = small_options();
  options.cache_bytes = 16 * 1024;
  {
    ObjectTrie trie(dir.path(), key_of(11), options, quiet_logger());
    std::vector<ObjectTrie::Change> batch;
    for (int i = 0; i < 20000; ++i) {
      batch.push_back({id_of(random), value_of(random)});
      model[batch.back().id] = *batch.back().value;
    }
    trie.apply(batch);
    trie.checkpoint();
  }
  ObjectTrie reopened(dir.path(), key_of(11), options, quiet_logger());
  check_matches(reopened, model, random);
  const auto stats = reopened.stats();
  EXPECT_GT(stats.loads, 0u);
  // The cache keeps to its bound, give or take the node being read.
  EXPECT_LE(stats.cache_bytes, options.cache_bytes + 64 * 1024);
}

// Macha: test_a_trie_with_a_torn_journal_keeps_what_was_whole.
TEST(LedgerObjectTrie, ATornJournalKeepsWhatWasWhole) {
  dannan::test::TempDir dir;
  std::mt19937_64 random(17);
  std::map<ObjectId, Bytes> whole;
  ObjectTrie::Options options = small_options();
  options.checkpoint_bytes = 1ULL << 40;
  {
    ObjectTrie trie(dir.path(), key_of(12), options, quiet_logger());
    std::vector<ObjectTrie::Change> first;
    for (int i = 0; i < 200; ++i) {
      first.push_back({id_of(random), value_of(random)});
      whole[first.back().id] = *first.back().value;
    }
    trie.apply(first);
    std::vector<ObjectTrie::Change> second;
    for (int i = 0; i < 200; ++i) second.push_back({id_of(random), value_of(random)});
    trie.apply(second);
  }
  // The second batch's frame is cut short: only the first survives.
  const auto journal = dir.path() / "journal.log";
  std::filesystem::resize_file(journal, std::filesystem::file_size(journal) - 3);
  ObjectTrie reopened(dir.path(), key_of(12), options, quiet_logger());
  check_matches(reopened, whole, random);
}

// Macha: test_a_format_1_trie_opens_and_is_rewritten. A trie written in
// format 1 (a u64 value a record) opens with every record, each value its
// eight big-endian bytes, and is rewritten in the current one.
TEST(LedgerObjectTrie, AFormat1TrieOpensAndIsRewritten) {
  dannan::test::TempDir dir;
  std::mt19937_64 random(19);
  const auto key = key_of(20);
  const auto be64 = [](uint64_t value) {
    Writer writer;
    writer.u64(value);
    return writer.take();
  };
  std::map<ObjectId, uint64_t> saved;
  for (int i = 0; i < 600; ++i) saved[id_of(random)] = random();
  {
    // Format 1's node file: a leaf per first byte, then the interior root.
    constexpr std::array<uint8_t, 8> nodes_aad{'M', 'A', 'C', 'H', 'L', 'N', '0', '1'};
    SealedJournal nodes(dir.path() / "nodes-0.bin", key, nodes_aad, 4U << 20, quiet_logger());
    struct Slot {
      uint64_t offset = 0;
      uint32_t size = 0;
      uint64_t count = 0;
      Hash256 hash{};
    };
    std::map<uint8_t, Slot> slots;
    auto at = saved.begin();
    while (at != saved.end()) {
      const auto byte = at->first.bytes[0];
      Writer leaf;
      Writer hashed;
      hashed.u8('L');
      std::vector<std::pair<ObjectId, uint64_t>> records;
      for (; at != saved.end() && at->first.bytes[0] == byte; ++at) records.emplace_back(at->first, at->second);
      ASSERT_LE(records.size(), ObjectTrie::leaf_max);
      leaf.u8(1);
      leaf.u8(1);
      leaf.u32(static_cast<uint32_t>(records.size()));
      for (const auto& [id, value] : records) {
        leaf.fixed(id.bytes);
        leaf.u64(value);
        hashed.fixed(id.bytes);
        hashed.u64(value);
      }
      const auto before = nodes.bytes();
      const auto offset = nodes.append_unsynced(leaf.data());
      slots[byte] = {offset, static_cast<uint32_t>(nodes.bytes() - before), records.size(), sha256(hashed.data())};
    }
    Writer interior;
    Writer hashed;
    hashed.u8('I');
    interior.u8(2);
    interior.u8(0);
    std::array<uint8_t, 32> present{};
    for (const auto& [byte, slot] : slots) present[byte / 8] |= static_cast<uint8_t>(1U << (byte % 8));
    interior.fixed(present);
    for (const auto& [byte, slot] : slots) {
      interior.u64(slot.offset);
      interior.u32(slot.size);
      interior.u64(slot.count);
      interior.fixed(slot.hash.bytes);
      hashed.u8(byte);
      hashed.fixed(slot.hash.bytes);
      hashed.u64(slot.count);
    }
    const auto before = nodes.bytes();
    const auto offset = nodes.append_unsynced(interior.data());
    const auto size = static_cast<uint32_t>(nodes.bytes() - before);
    nodes.sync();

    Writer root;
    root.fixed(std::array<uint8_t, 8>{'M', 'L', 'T', 'R', '0', '0', '0', '1'});
    root.u64(0);
    root.u64(offset);
    root.u32(size);
    root.u64(saved.size());
    root.fixed(sha256(hashed.data()).bytes);
    root.u64(nodes.bytes());
    const auto check = sha256(root.data());
    root.fixed(check.bytes);
    write_file(dir.path() / "root", root.data());

    // Format 1's journal: one record changed, one erased, one added.
    auto changed = saved.begin();
    auto erased = std::next(changed);
    const auto added = id_of(random);
    std::map<ObjectId, std::optional<uint64_t>> frame{{changed->first, 77}, {erased->first, std::nullopt}, {added, 88}};
    changed->second = 77;
    saved.erase(erased);
    saved[added] = 88;
    constexpr std::array<uint8_t, 8> journal_aad{'M', 'A', 'C', 'H', 'L', 'J', '0', '1'};
    SealedJournal journal(dir.path() / "journal.log", key, journal_aad, 4U << 20, quiet_logger());
    Writer writer;
    writer.u8(1);
    writer.u32(static_cast<uint32_t>(frame.size()));
    for (const auto& [id, value] : frame) {
      writer.fixed(id.bytes);
      writer.u8(value ? 1 : 0);
      if (value) writer.u64(*value);
    }
    journal.append(writer.data());
  }
  std::map<ObjectId, Bytes> model;
  for (const auto& [id, value] : saved) model[id] = be64(value);
  {
    ObjectTrie trie(dir.path(), key, small_options(), quiet_logger());
    check_matches(trie, model, random);
    EXPECT_EQ(trie.stats().journal_bytes, 0u);
  }
  const auto root = read_file(dir.path() / "root");
  ASSERT_GE(root.size(), 8u);
  EXPECT_EQ(std::string(root.begin(), root.begin() + 8), "MLTR0002");
  ObjectTrie reopened(dir.path(), key, small_options(), quiet_logger());
  check_matches(reopened, model, random);
}

// Macha: test_a_snapshot_is_frozen_through_writes_and_rewrites. A snapshot
// keeps the records it was taken with, read from several threads, while the
// trie changes, checkpoints and rewrites its node file under it.
TEST(LedgerObjectTrie, ASnapshotIsFrozenThroughWritesAndRewrites) {
  dannan::test::TempDir dir;
  std::mt19937_64 random(22);
  ObjectTrie trie(dir.path(), key_of(18), small_options(), quiet_logger());
  std::map<ObjectId, Bytes> model;
  std::vector<ObjectId> ids;
  std::vector<ObjectTrie::Change> first;
  for (int i = 0; i < 3000; ++i) {
    ids.push_back(id_of(random));
    first.push_back({ids.back(), value_of(random)});
    model[ids.back()] = *first.back().value;
  }
  trie.apply(first);
  trie.checkpoint();
  // Some unsaved changes too, held in memory by the snapshot.
  std::vector<ObjectTrie::Change> unsaved;
  for (int i = 0; i < 50; ++i) {
    unsaved.push_back({ids[static_cast<size_t>(i)], std::nullopt});
    model.erase(ids[static_cast<size_t>(i)]);
  }
  trie.apply(unsaved);
  const auto frozen = trie.snapshot();
  const auto hash = trie.root_hash();

  const auto read_all = [&](const ObjectTrie::Snapshot& snapshot) {
    std::map<ObjectId, Bytes> out;
    std::optional<ObjectId> after;
    while (true) {
      const auto page = snapshot.next(after, 101);
      for (const auto& [id, value] : page) out.emplace(id, value);
      if (page.size() < 101) return out;
      after = page.back().first;
    }
  };
  std::atomic_bool done{false};
  std::atomic_int mismatches{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r)
    readers.emplace_back([&, r] {
      std::mt19937_64 pick(static_cast<uint64_t>(100 + r));
      while (!done.load()) {
        const auto& id = ids[pick() % ids.size()];
        const auto found = model.find(id);
        const auto got = frozen.get(id);
        if (found == model.end() ? got.has_value() : got != found->second) ++mismatches;
      }
    });
  // Rewrite the same ids many times: checkpoints and node-file rewrites.
  for (int round = 0; round < 30; ++round) {
    std::vector<ObjectTrie::Change> batch;
    for (const auto& id : ids) batch.push_back({id, value_of(random)});
    trie.apply(batch);
  }
  done = true;
  for (auto& reader : readers) reader.join();
  EXPECT_EQ(mismatches.load(), 0);
  EXPECT_GT(trie.stats().rewrites, 0u);
  EXPECT_EQ(frozen.size(), model.size());
  EXPECT_EQ(frozen.root_hash(), hash);
  EXPECT_TRUE(read_all(frozen) == model);
  EXPECT_NE(trie.root_hash(), hash);
}

// Macha: test_a_large_trie_opens_within_the_bound, stage 1's measurement:
// a synthetic set of DAGDA_LEDGER_MEASURE_RECORDS records (200,000 by
// default; Macha's fi-1 runs ten million), then a reopen. Opening must be
// inside the 30 s bound whatever the size; the figures are logged. Heavy,
// so disabled: run it with --gtest_also_run_disabled_tests. It measures the
// real clock on purpose, and drives nothing by it.
TEST(LedgerObjectTrie, DISABLED_ALargeTrieOpensWithinTheBound) {
  using Clock = std::chrono::steady_clock;
  uint64_t records = 200000;
  if (const char* text = std::getenv("DAGDA_LEDGER_MEASURE_RECORDS")) records = std::strtoull(text, nullptr, 10);
  dannan::test::TempDir dir;
  std::mt19937_64 random(18);
  // The defaults: 64 MiB cache, 1 MiB journal.
  ObjectTrie::Options options;
  std::vector<ObjectId> sample;
  const auto started = Clock::now();
  {
    ObjectTrie trie(dir.path(), key_of(13), options, quiet_logger());
    std::vector<ObjectTrie::Change> batch;
    batch.reserve(10000);
    for (uint64_t i = 0; i < records; ++i) {
      batch.push_back({id_of(random), Bytes(8, static_cast<uint8_t>(i))});
      if (sample.size() < 2000 && i % 97 == 0) sample.push_back(batch.back().id);
      if (batch.size() == 10000) {
        trie.apply(batch);
        batch.clear();
      }
    }
    trie.apply(batch);
    trie.checkpoint();
    // Leave a journal behind, as a crash would, just under the size at which
    // it would be checkpointed: the most an open replays.
    std::vector<ObjectTrie::Change> tail;
    for (int i = 0; i < 22000; ++i) tail.push_back({id_of(random), Bytes(8, 1)});
    trie.apply(tail);
    ASSERT_GT(trie.stats().journal_bytes, 900u * 1024);
  }
  const auto built_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();

  const auto open_started = Clock::now();
  ObjectTrie trie(dir.path(), key_of(13), options, quiet_logger());
  const auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - open_started).count();

  const auto cold_started = Clock::now();
  for (const auto& id : sample) EXPECT_TRUE(trie.get(id).has_value());
  const auto cold_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - cold_started).count();
  const auto warm_started = Clock::now();
  for (const auto& id : sample) EXPECT_TRUE(trie.get(id).has_value());
  const auto warm_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - warm_started).count();
  const auto stats = trie.stats();
  const auto per = static_cast<long long>(sample.size());
  dannan::status::ScopedLogger(std::make_shared<dannan::status::ConsoleSink>(), "ledger")
      .info("ledger measure records=" + std::to_string(trie.size()) + " built_ms=" + std::to_string(built_ms) + " open_ms=" + std::to_string(open_ms) +
            " cold_lookup_us=" + std::to_string(cold_us / per) + " warm_lookup_us=" + std::to_string(warm_us / per) +
            " node_file_bytes=" + std::to_string(stats.node_file_bytes) + " live_bytes=" + std::to_string(stats.live_bytes) +
            " cache_bytes=" + std::to_string(stats.cache_bytes) + " loads=" + std::to_string(stats.loads));
  EXPECT_LT(open_ms, 30000);
  EXPECT_LE(stats.cache_bytes, options.cache_bytes + 64 * 1024);
}

// --- Known answers: layout v18's bytes --------------------------------------

std::vector<ObjectTrie::Change> held_ids(int count) {
  const Bytes held{0, 0, 0, 0, 0, 0, 0, 1};
  std::vector<ObjectTrie::Change> out;
  for (int i = 0; i < count; ++i) {
    const auto text = "dagda trie known answer " + std::to_string(i);
    out.push_back({store::object_id(Bytes(text.begin(), text.end())), held});
  }
  return out;
}

// Three held ids: one leaf at the root, saved as the node file's one frame.
constexpr char root_hash_3[] = "58e779cabdf80e39b1817a329b36e2b6b965123543b0921d72cf2dff4ef62132";
constexpr char root_file_3[] =
    "4d4c54523030303200000000000000000000000000000000000000aa000000000000000358e779cabdf80e39b1817a329b36e2b6b965123543b0921d72cf2dff4ef6213200000000000000aa0f"
    "e1"
    "870b3b25783a87f78f4be17a6cfa886e570d3d2e57cfa2955973941c4877";
// That frame under domain v1's known-answer storage key and nonce 00..0b.
constexpr char nodes_file_3[] =
    "0000008a000102030405060708090a0b5eefb8fe7c44425c87a93f143cabf1a8033b2b73b6c173421d68c5e51715a587a0afcf3713c1fe29c7329051576e1f66c5fe8188ea2a430d7f20832d7a"
    "74785dac42f12c03ba2ea018c389901f06052a7880c8d8932e8884ee723d3a20264435348086fd6cbe320635ccff4d692dfbfb88c78f411a2d6e87b3441901c7eb148bc288e19f7ff29941494f"
    "82eb0fa5ee06a209dfc7c5b727df3ca8";
// Three hundred: an interior root over 172 leaves, saved leaves first in
// slot order, then the root (173 frames, 28,746 bytes).
constexpr char root_hash_300[] = "afddf9e19ffb94cbba89ba7119f8c964fcab9d21a068ebba975769aec9c9c17c";
constexpr char root_file_300[] =
    "4d4c54523030303200000000000000000000000000004d1800002332000000000000012cafddf9e19ffb94cbba89ba7119f8c964fcab9d21a068ebba975769aec9c9c17c000000000000704af5"
    "56b736a90c057ded8af68e584a287d034bc7809908bcc360af6c79fe730317";

// The root hash of a fixed set of ids is Macha's, and so is the published
// root after a checkpoint: magic, generation, the root's offset, size,
// count and hash, the live bytes, and a SHA-256 over them.
TEST(LedgerObjectTrie, PublishesMachasRootForAFixedSet) {
  struct Case {
    int count;
    const char* hash;
    const char* root;
    uint64_t node_file_bytes;
  };
  for (const auto& [count, hash, root, node_file_bytes] : {Case{3, root_hash_3, root_file_3, 170}, Case{300, root_hash_300, root_file_300, 28746}}) {
    dannan::test::TempDir dir;
    ObjectTrie trie(dir.path(), test::known_answer_key(), ObjectTrie::Options{}, quiet_logger());
    auto changes = held_ids(count);
    // Written in reverse, in small batches: the order is no part of it.
    std::reverse(changes.begin(), changes.end());
    for (size_t from = 0; from < changes.size(); from += 7)
      trie.apply(std::span<const ObjectTrie::Change>(changes).subspan(from, std::min<size_t>(7, changes.size() - from)));
    EXPECT_EQ(hex(trie.root_hash()), hash);
    EXPECT_EQ(trie.size(), static_cast<uint64_t>(count));
    trie.checkpoint();
    EXPECT_EQ(hex(read_file(dir.path() / "root")), root);
    EXPECT_EQ(trie.stats().node_file_bytes, node_file_bytes);
    EXPECT_EQ(trie.stats().live_bytes, node_file_bytes);
    EXPECT_EQ(std::filesystem::file_size(dir.path() / "nodes-0.bin"), node_file_bytes);
    EXPECT_EQ(std::filesystem::file_size(dir.path() / "journal.log"), 0u);
  }
}

// A trie Macha saved opens here: its root file and node file, written by
// hand from the known answers.
TEST(LedgerObjectTrie, OpensATrieMachaSaved) {
  dannan::test::TempDir dir;
  write_file(dir.path() / "root", unhex(root_file_3));
  write_file(dir.path() / "nodes-0.bin", unhex(nodes_file_3));
  ObjectTrie trie(dir.path(), test::known_answer_key(), ObjectTrie::Options{}, quiet_logger());
  EXPECT_EQ(trie.size(), 3u);
  EXPECT_EQ(hex(trie.root_hash()), root_hash_3);
  for (const auto& change : held_ids(3)) EXPECT_EQ(trie.get(change.id), change.value);
  EXPECT_EQ(trie.stats().loads, 1u);
  // Opened by the same key only.
  dannan::test::TempDir other;
  write_file(other.path() / "root", unhex(root_file_3));
  write_file(other.path() / "nodes-0.bin", unhex(nodes_file_3));
  EXPECT_THROW(ObjectTrie(other.path(), key_of(1), ObjectTrie::Options{}, quiet_logger()), std::runtime_error);
}

// The journal frame of ledger_sealed_journal_test: id 2 set to the empty
// value, id 1 erased, id 0 held, in id order.
constexpr char journal_frame[] =
    "00000078000102030405060708090a0bead7a793135e705a4d752d5e369c5bdd003b2b73b546a36043e6fd31e3f0d5634d254866e463f5c21fa93d6bc5c3ee45d76b51c25977430d7f2865092a"
    "22fb6716bc99834b91dae8653606f348b708837e4c87bd9f4caa594bab735d184f325add0039f251632f9bb71f33d621a92d12fcd7abde887608f51bed1b8214278fcb47b4732c41bf8b4d";
constexpr char journal_plaintext[] =
    "020000000384547628a69e4abe5b2bcf22a82f7e892b20cb1388251f8d20d1f2e075a5ef5c0100000000e6245056833abaff8e8b187d7772c70a01e84fcc7adbc1c64e8d43ae5806641f00ea7b"
    "c72"
    "e698d9fbf0f3dd51d9d82d3cc9b48853c92b33e4d120de4694f7db49f01000000080000000000000001";

std::vector<ObjectTrie::Change> journal_changes() {
  const auto ids = held_ids(3);
  return {{ids[0].id, ids[0].value}, {ids[1].id, std::nullopt}, {ids[2].id, Bytes{}}};
}

// A journal Macha wrote replays here: version 2, a count, and per change the
// id, a flag, and the value as a blob.
TEST(LedgerObjectTrie, ReplaysAJournalMachaWrote) {
  dannan::test::TempDir dir;
  write_file(dir.path() / "journal.log", unhex(journal_frame));
  ObjectTrie trie(dir.path(), test::known_answer_key(), ObjectTrie::Options{}, quiet_logger());
  const auto changes = journal_changes();
  EXPECT_EQ(trie.size(), 2u);
  EXPECT_EQ(trie.get(changes[0].id), changes[0].value);
  EXPECT_FALSE(trie.get(changes[1].id).has_value());
  EXPECT_EQ(trie.get(changes[2].id), std::optional<Bytes>(Bytes{}));
  EXPECT_EQ(trie.stats().journal_bytes, 152u);
}

// What Dagda journals for those changes is that plaintext, in that frame.
TEST(LedgerObjectTrie, JournalsMachasBytes) {
  dannan::test::TempDir dir;
  const auto key = test::known_answer_key();
  {
    ObjectTrie trie(dir.path(), key, ObjectTrie::Options{}, quiet_logger());
    trie.apply(journal_changes());
  }
  const auto written = read_file(dir.path() / "journal.log");
  ASSERT_EQ(written.size(), 152u);
  EXPECT_EQ(hex(std::span(written).first(4)), "00000078");
  constexpr std::array<uint8_t, 8> aad{'M', 'A', 'C', 'H', 'L', 'J', '0', '1'};
  const auto plain = dannan::identity::aes_gcm_open(key->bytes(), std::span<const uint8_t, 12>(written.data() + 4, 12),
                                                    std::span<const uint8_t, 16>(written.data() + 16, 16), std::span(written).subspan(32), aad);
  EXPECT_EQ(hex(plain), journal_plaintext);
}

}  // namespace
}  // namespace dagda::ledger
