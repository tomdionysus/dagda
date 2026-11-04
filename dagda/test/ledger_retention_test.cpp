//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirements L12 and L13: claims as an observed-remove set of dots per
// object and class, released only as far as the releasing clock dominates,
// journaled to layout v18's claims.log and checkpointed into its two tries.
//
// Moved from Macha's tests/test_storage_metadata.cpp (develop 5a06d3c),
// every assertion kept: test_retention_claims_are_causal_durable_and_observed_remove,
// test_retention_prune_cursor_cannot_starve_later_tombstones,
// test_retention_prune_keeps_a_row_claimed_while_it_checks_presence,
// test_retention_claims_checkpoint_into_the_ledger_and_restart,
// test_retention_checkpoints_after_enough_changes,
// test_retention_start_checkpoints_a_long_journal,
// test_retention_prune_survives_a_restart. Its
// test_retention_shard_checkpoint_moves_into_the_ledger tested a reader Tom
// dropped (2026-11-04): here the same files are refused rather than read,
// and the assertions about their removal at the first checkpoint are kept.
// Macha's cluster key file becomes domain v1's known-answer key.
//
// The known answers were computed outside both codebases from Macha's
// retention.cpp:18-26, :78-91, :476-640, sealed_journal.cpp:158-175 and
// object_trie.cpp:208-238, :430-445: SHA-256 by a reference library,
// AES-256 blocks by `openssl enc -aes-256-ecb`, GHASH by hand, checked first
// against ledger_sealed_journal_test's known frame. Origins are 0a.. and
// 0b.. (sixteen bytes each); object n is n then 31 zero bytes.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/sealed_journal.h"
#include "dagda/store/object_id.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/identity/crypto.h"
#include "dannan/net/codec.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

using contract::RetentionClass;
using dannan::identity::hex;
using test::known_answer_key;
using test::quiet_logger;

constexpr auto data = RetentionClass::data;
constexpr auto control = RetentionClass::control;

RetentionStore open_store(const std::filesystem::path& state) { return RetentionStore(state, known_answer_key(), quiet_logger()); }

Bytes unhex(const std::string& text) { return dannan::identity::unhex(text).value(); }

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, const Bytes& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

ObjectId id(uint8_t n) {
  ObjectId out{};
  out.bytes[0] = n;
  return out;
}

// Macha: test_retention_claims_are_causal_durable_and_observed_remove.
TEST(LedgerRetention, ClaimsAreCausalDurableAndObservedRemove) {
  dannan::test::TempDir t;
  const auto state = t.path() / "retention-state";
  const auto origin_a = NodeId::random();
  const auto origin_b = NodeId::random();
  const auto object = store::object_id(test::pattern(4097));
  const auto second = store::object_id(test::pattern(8193));

  {
    auto retention = open_store(state);
    retention.retain(data, object, {origin_a, 1});
    EXPECT_TRUE(retention.retained(data, object));

    // A removal clears only claims causally visible in its clock, so B's
    // concurrent claim survives an A-only release.
    retention.retain(data, object, {origin_b, 1});
    std::vector<ObjectId> no_live;
    EXPECT_EQ(retention.release_unreferenced(data, no_live, RetentionClock{{origin_a, 1}}, 16), 1u);
    EXPECT_TRUE(retention.retained(data, object));

    // Once a reconciled view has seen both branch dots, the unreferenced
    // object may lose both claims.
    EXPECT_EQ(retention.release_unreferenced(data, no_live, RetentionClock{{origin_a, 1}, {origin_b, 1}}, 16), 1u);
    EXPECT_FALSE(retention.retained(data, object));

    // Removed clocks suppress delayed or replayed old adds, not a genuinely
    // later claim from the same origin.
    retention.retain(data, object, {origin_a, 1});
    EXPECT_FALSE(retention.retained(data, object));
    retention.retain(data, object, {origin_a, 2});
    EXPECT_TRUE(retention.retained(data, object));

    retention.retain(control, second, {origin_b, 7});
    EXPECT_TRUE(retention.retained(control, second));
    // Compaction keeps the causal state while bounding the journal.
    EXPECT_TRUE(retention.compact_if_needed(1));
    EXPECT_FALSE(retention.compact_if_needed(1));
  }

  // Claims and remove contexts survive a restart: the later DATA and CONTROL
  // claims, and the tombstone that suppresses a delayed A:1 replay.
  auto reopened = open_store(state);
  EXPECT_TRUE(reopened.retained(data, object));
  EXPECT_TRUE(reopened.retained(control, second));
  reopened.retain(data, object, {origin_a, 1});
  EXPECT_TRUE(reopened.retained(data, object));
  EXPECT_EQ(reopened.release_unreferenced(data, {}, RetentionClock{{origin_a, 2}}, 16), 1u);
  EXPECT_FALSE(reopened.retained(data, object));
}

// New (requirements L12, extraction 430-433): a claim from a branch this
// node has not seen survives a partition. A head whose clock has seen the
// other node only up to an earlier write releases the object and leaves the
// later claim, however often it is asked and across a restart; only a clock
// that has seen the claim's dot takes it.
TEST(LedgerRetention, AClaimFromAnUnseenBranchSurvivesAPartition) {
  dannan::test::TempDir t;
  const auto state = t.path() / "state";
  const auto here = NodeId::random();
  const auto away = NodeId::random();
  const auto object = id(7);
  {
    auto retention = open_store(state);
    retention.retain(data, object, {here, 1});
    retention.retain(data, object, {away, 3});
    // Across the partition the other node claimed again, at 4; this node's
    // head saw it only up to 3.
    retention.retain(data, object, {away, 4});
    for (int pass = 0; pass < 3; ++pass) retention.release_unreferenced(data, {}, RetentionClock{{here, 1}, {away, 3}}, 16);
    EXPECT_TRUE(retention.retained(data, object));
    const auto claims = retention.claims(data, object);
    EXPECT_EQ(claims.adds, (std::map<NodeId, uint64_t>{{away, 4}}));
    EXPECT_EQ(claims.removed.at(here), 1u);
    EXPECT_EQ(claims.removed.at(away), 3u);
    // A clock that never saw the other node at all leaves it too.
    retention.release_unreferenced(data, {}, RetentionClock{{here, 9}}, 16);
    EXPECT_TRUE(retention.retained(data, object));
  }
  auto reopened = open_store(state);
  EXPECT_TRUE(reopened.retained(data, object));
  // A delayed replay of what was released stays released.
  reopened.retain(data, object, {away, 3});
  EXPECT_EQ(reopened.claims(data, object).adds, (std::map<NodeId, uint64_t>{{away, 4}}));
  // Healed: a head that has seen 4 releases it.
  EXPECT_EQ(reopened.release_unreferenced(data, {}, RetentionClock{{here, 1}, {away, 4}}, 16), 1u);
  EXPECT_FALSE(reopened.retained(data, object));
}

// Macha: test_retention_prune_cursor_cannot_starve_later_tombstones.
TEST(LedgerRetention, ThePruneCursorCannotStarveLaterTombstones) {
  dannan::test::TempDir t;
  auto retention = open_store(t.path() / "state");
  const auto origin = NodeId::random();

  std::array<ObjectId, 5> objects{};
  for (size_t i = 0; i < objects.size(); ++i) objects[i].bytes.back() = static_cast<uint8_t>(i + 1);

  for (const auto& object : objects) retention.retain(data, object, {origin, 1});
  EXPECT_EQ(retention.release_unreferenced(data, {}, RetentionClock{{origin, 1}}, 32), objects.size());

  // The first two tombstones are still backed by objects; a fixed budget
  // must still progress to the later dead rows rather than restart at the
  // beginning.
  auto exists = [&](const ObjectId& object) { return object == objects[0] || object == objects[1]; };
  EXPECT_EQ(retention.prune_unclaimed(data, exists, 2), 0u);
  EXPECT_EQ(retention.prune_unclaimed(data, exists, 2), 2u);

  // A pruned tombstone no longer suppresses an ancient replay: the row was
  // erased.
  retention.retain(data, objects[2], {origin, 1});
  EXPECT_TRUE(retention.retained(data, objects[2]));

  // The protected leading row was examined but kept, so its remove clock
  // still suppresses the same old dot.
  retention.retain(data, objects[0], {origin, 1});
  EXPECT_FALSE(retention.retained(data, objects[0]));
}

// Macha: test_retention_prune_keeps_a_row_claimed_while_it_checks_presence.
// The presence check runs without the store's lock, so a claim made while it
// runs is seen, and the row it revived is kept.
TEST(LedgerRetention, PruneKeepsARowClaimedWhileItChecksPresence) {
  dannan::test::TempDir t;
  auto retention = open_store(t.path() / "state");
  const auto origin = NodeId::random();

  std::array<ObjectId, 2> objects{};
  for (size_t i = 0; i < objects.size(); ++i) objects[i].bytes.back() = static_cast<uint8_t>(i + 1);
  for (const auto& object : objects) retention.retain(data, object, {origin, 1});
  EXPECT_EQ(retention.release_unreferenced(data, {}, RetentionClock{{origin, 1}}, 32), objects.size());

  auto exists = [&](const ObjectId& object) {
    // Reads and writes the store from inside the check.
    EXPECT_FALSE(retention.retained(data, object));
    if (object == objects[0]) retention.retain(data, object, {origin, 2});
    return false;
  };
  EXPECT_EQ(retention.prune_unclaimed(data, exists, 8), 1u);
  EXPECT_TRUE(retention.retained(data, objects[0]));
  // objects[1]'s row is gone: its old dot is no longer suppressed.
  retention.retain(data, objects[1], {origin, 1});
  EXPECT_TRUE(retention.retained(data, objects[1]));
}

// Macha: test_retention_claims_checkpoint_into_the_ledger_and_restart.
TEST(LedgerRetention, ClaimsCheckpointIntoTheLedgerAndRestart) {
  dannan::test::TempDir t;
  const auto state = t.path() / "state";
  const auto origin = NodeId::random();
  std::vector<ObjectId> objects;
  for (uint16_t shard = 0; shard < 256; ++shard) {
    for (uint16_t suffix = 0; suffix < 2; ++suffix) {
      ObjectId object{};
      object.bytes[0] = static_cast<uint8_t>(shard);
      object.bytes[30] = static_cast<uint8_t>(suffix);
      object.bytes[31] = static_cast<uint8_t>(255 - shard);
      objects.push_back(object);
    }
  }
  ObjectId later{};
  later.bytes[5] = 9;
  {
    auto retention = open_store(state);
    retention.retain_batch(data, objects, {origin, 7});
    ASSERT_TRUE(retention.compact_if_needed(1));
    EXPECT_EQ(std::filesystem::file_size(state / "retention" / "claims.log"), 0u);
    // After the checkpoint: in the journal only.
    retention.retain(control, later, {origin, 8});
  }
  EXPECT_TRUE(std::filesystem::exists(state / "retention" / "ledger-data" / "root"));
  EXPECT_GT(std::filesystem::file_size(state / "retention" / "claims.log"), 0u);
  auto reopened = open_store(state);
  for (const auto& object : objects) EXPECT_TRUE(reopened.retained(data, object));
  EXPECT_TRUE(reopened.retained(control, later));
  EXPECT_FALSE(reopened.retained(data, later));
  EXPECT_EQ(reopened.claim_objects(data), objects.size());
  EXPECT_EQ(reopened.claim_objects(control), 1u);
}

// Macha: test_retention_checkpoints_after_enough_changes. Enough changed
// objects checkpoint the tries on their own: what the tries hold unsaved
// stays bounded whatever the batch.
TEST(LedgerRetention, CheckpointsAfterEnoughChanges) {
  dannan::test::TempDir t;
  const auto state = t.path() / "state";
  const auto origin = NodeId::random();
  std::vector<ObjectId> objects(10000);
  for (size_t i = 0; i < objects.size(); ++i) {
    objects[i].bytes[0] = static_cast<uint8_t>(i);
    objects[i].bytes[1] = static_cast<uint8_t>(i >> 8);
  }
  auto retention = open_store(state);
  retention.retain_batch(data, objects, {origin, 1});
  EXPECT_EQ(std::filesystem::file_size(state / "retention" / "claims.log"), 0u);
  EXPECT_EQ(retention.claim_objects(data), objects.size());
}

// Macha: test_retention_start_checkpoints_a_long_journal. A start that
// replays a long journal checkpoints it, so the next start does not replay
// it again.
TEST(LedgerRetention, AStartCheckpointsALongJournal) {
  dannan::test::TempDir t;
  const auto state = t.path() / "state";
  const auto root = state / "retention";
  const auto origin = NodeId::random();
  std::vector<ObjectId> objects(60000);
  for (size_t i = 0; i < objects.size(); ++i) {
    objects[i].bytes[0] = static_cast<uint8_t>(i);
    objects[i].bytes[1] = static_cast<uint8_t>(i >> 8);
    objects[i].bytes[2] = static_cast<uint8_t>(i >> 16);
  }
  std::sort(objects.begin(), objects.end());
  {
    auto created = open_store(state);
  }
  {
    // As a crash leaves it: frames journaled, no checkpoint.
    SealedJournal journal(root / "claims.log", known_answer_key(), retention_format::journal_aad, 4U << 20, quiet_logger());
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) journal.append(retention_format::encode_add(data, {origin, sequence}, objects));
  }
  ASSERT_GT(std::filesystem::file_size(root / "claims.log"), 4ULL * 1024 * 1024);
  {
    auto reopened = open_store(state);
    EXPECT_EQ(std::filesystem::file_size(root / "claims.log"), 0u);
    EXPECT_EQ(reopened.claims(data, objects[7]).adds.at(origin), 3u);
  }
  auto again = open_store(state);
  EXPECT_EQ(again.claim_objects(data), objects.size());
}

// Macha: test_retention_prune_survives_a_restart. A prune is journaled: the
// erased row stays erased across a restart with no checkpoint between, so an
// old dot is no longer suppressed.
TEST(LedgerRetention, APruneSurvivesARestart) {
  dannan::test::TempDir t;
  const auto state = t.path() / "state";
  const auto origin = NodeId::random();
  ObjectId pruned{};
  pruned.bytes[0] = 1;
  ObjectId kept{};
  kept.bytes[0] = 2;
  {
    auto retention = open_store(state);
    retention.retain(data, pruned, {origin, 1});
    retention.retain(data, kept, {origin, 1});
    EXPECT_EQ(retention.release_unreferenced(data, {}, RetentionClock{{origin, 1}}, 8), 2u);
    EXPECT_EQ(retention.prune_unclaimed(data, [&](const ObjectId& object) { return object == kept; }, 8), 1u);
  }
  auto reopened = open_store(state);
  EXPECT_TRUE(reopened.claims(data, pruned).removed.empty());
  EXPECT_EQ(reopened.claims(data, kept).removed.size(), 1u);
  reopened.retain(data, pruned, {origin, 1});
  EXPECT_TRUE(reopened.retained(data, pruned));
  reopened.retain(data, kept, {origin, 1});
  EXPECT_FALSE(reopened.retained(data, kept));
}

// Writes Macha's sharded checkpoint (MRTS0002) as its test wrote it: the
// files Macha's legacy reader read, which Dagda does not.
void write_shard_checkpoint(const std::filesystem::path& root, const std::vector<ObjectId>& data_ids, const NodeId& a) {
  const auto generation = root / "checkpoints" / "gen-1-1-1";
  std::filesystem::create_directories(generation);
  constexpr std::array<uint8_t, 8> aad{'M', 'A', 'C', 'H', 'R', 'T', 'S', '1'};
  const auto key = known_answer_key();
  for (int shard = 0; shard < 256; ++shard) {
    dannan::net::Writer plain;
    plain.u8(2);
    plain.u8(static_cast<uint8_t>(shard));
    std::vector<ObjectId> in_shard;
    for (const auto& object : data_ids)
      if (object.bytes[0] == shard) in_shard.push_back(object);
    plain.u32(static_cast<uint32_t>(in_shard.size()));
    for (const auto& object : in_shard) {
      plain.fixed(object.bytes);
      plain.u32(1);
      plain.fixed(a.bytes);
      plain.u64(3);
      plain.u32(0);
    }
    plain.u32(0);
    const auto sealed = dannan::identity::aes_gcm_seal(key->bytes(), plain.data(), aad);
    dannan::net::Writer outer;
    outer.fixed(std::array<uint8_t, 8>{'M', 'R', 'T', 'S', '0', '0', '0', '2'});
    outer.fixed(sealed.nonce);
    outer.fixed(sealed.tag);
    outer.bytes(sealed.ciphertext);
    char name[8];
    std::snprintf(name, sizeof(name), "%02x.meta", shard);
    write_file(generation / name, outer.data());
  }
  std::ofstream(root / "claims.current") << "gen-1-1-1\n";
}

// Macha: test_retention_shard_checkpoint_moves_into_the_ledger, with its
// reader dropped (Tom, 2026-11-04). A state whose claims are still in the
// sharded checkpoint, or the monolithic claims.meta, was never moved into the
// tries: Dagda refuses it rather than open it empty, and changes nothing.
// Once moved (the tries' marker present) the reserved files are never read,
// and the first checkpoint removes them, as Macha's did.
TEST(LedgerRetention, AnUnmovedLegacyCheckpointIsRefusedAndAMovedOneRemoved) {
  dannan::test::TempDir t;
  const auto a = NodeId::random();
  const auto b = NodeId::random();
  std::vector<ObjectId> data_ids;
  for (int i = 0; i < 600; ++i) {
    ObjectId object{};
    object.bytes[0] = static_cast<uint8_t>(i * 7);
    object.bytes[1] = static_cast<uint8_t>(i);
    object.bytes[2] = static_cast<uint8_t>(i >> 8);
    data_ids.push_back(object);
  }
  std::sort(data_ids.begin(), data_ids.end());
  const auto journal_after = [&](const std::filesystem::path& root) {
    // The journal after the checkpoint: b claims data_ids[1].
    SealedJournal journal(root / "claims.log", known_answer_key(), retention_format::journal_aad, 4U << 20, quiet_logger());
    journal.append(retention_format::encode_add(data, {b, 6}, std::vector<ObjectId>{data_ids[1]}));
  };

  for (const char* legacy : {"claims.current", "claims.meta"}) {
    const auto state = t.path() / (std::string("unmoved-") + legacy);
    const auto root = state / "retention";
    if (std::string(legacy) == "claims.current")
      write_shard_checkpoint(root, data_ids, a);
    else
      write_file(root / "claims.meta", Bytes{'M', 'R', 'T', 'S', '0', '0', '0', '1'});
    journal_after(root);
    EXPECT_THROW(open_store(state), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(root / "ledger"));
    EXPECT_FALSE(std::filesystem::exists(root / "ledger-data"));
    EXPECT_TRUE(std::filesystem::exists(root / legacy));
  }

  const auto state = t.path() / "moved";
  const auto root = state / "retention";
  write_shard_checkpoint(root, data_ids, a);
  journal_after(root);
  std::ofstream(root / "ledger") << "1\n";
  {
    auto retention = open_store(state);
    // What the tries and the journal hold, and nothing of the reserved files.
    EXPECT_EQ(retention.claims(data, data_ids[1]).adds.size(), 1u);
    EXPECT_FALSE(retention.retained(data, data_ids[2]));
    EXPECT_EQ(retention.claim_objects(data), 1u);
    EXPECT_TRUE(std::filesystem::exists(root / "claims.current"));
    ASSERT_TRUE(retention.compact_if_needed(1));
  }
  EXPECT_FALSE(std::filesystem::exists(root / "claims.current"));
  EXPECT_FALSE(std::filesystem::exists(root / "checkpoints"));
  auto reopened = open_store(state);
  EXPECT_EQ(reopened.claims(data, data_ids[1]).adds.size(), 1u);
  EXPECT_EQ(reopened.claim_objects(data), 1u);
}

// --- Known answers: layout v18's bytes --------------------------------------

const NodeId origin_a{{0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a}};
const NodeId origin_b{{0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b}};

// The WAL's five frames, plaintext: add data A:3 {1, 2}; add data B:5 {2};
// release data {A:3} {1, 2}; prune data {1}; add control A:7 {9}.
constexpr const char* known_plain[] = {
    "01010a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a00000000000000030000000201000000000000000000000000000000000000000000000000000000000000000200000000000000000000000000"
    "000000000000000000000000000000000000",
    "01010b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0000000000000005000000010200000000000000000000000000000000000000000000000000000000000000",
    "0201000000010a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a000000000000000300000002010000000000000000000000000000000000000000000000000000000000000002000000000000000000"
    "00000000000000000000000000000000000000000000",
    "0301000000010100000000000000000000000000000000000000000000000000000000000000",
    "01020a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0000000000000007000000010900000000000000000000000000000000000000000000000000000000000000",
};
// Those frames sealed under domain v1's known-answer storage key, frame k's
// nonce the twelve bytes from 16k: claims.log as Macha writes it.
constexpr char known_claims_log[] =
    "0000005e000102030405060708090a0b78028e65bb8f96cce064eeaca7a3aedc033a2179bcc8fd1c614a697157a1f4a6658767186d48d5090c22187448e13eb7371ef42d0576430d7f28832d"
    "7a74785dac43170853ecad9aa23c071b077b7058bf8ac930dce2f25f2fb473b763881c33509f86fd6cb6320635ccff4d692c0000003e101112131415161718191a1b06888c62e143f8c42421"
    "0da6325650ad84649b67bebb86a4d9b4bebe3adafeab9958519183ff149da45257b63164b946e1f96eb4a597f5cf53bf1cab3cbb1f67fb6a45c928a49e21f9fbfc91219c00000062202122"
    "232425262728292a2be136bfce187d0998a2589b12e419eee6a642416a388167f383c9c1175d16190907debca7e38d019943539bdfe56ea2b22444fcf5845fa34fa7bece4147d89eca2cce"
    "03087c5792f5dacc76e3e8062dd4cf6e326af55fccaab69c133c1644c3b7e133578e9cc581f2ee83c291d238ecebed9c00000026303132333435363738393a3b07cc0eee5629babe6f9d62"
    "747a1e73530290bc953a5be6dae9f650fbf6960582f3ba414592eb3adf44f3e40f5cc7dcb884b7022365210000003e404142434445464748494a4b1fa76538971d4a33a66b695bd6a50e18"
    "741a14b6b87e8979a69437815146426c75265c89bb59ae53198321ff2e7e0ea8676a4b497bcd6529706f86b698e5a266ad52369d698a2c7d41444b791352";
// What they leave, as the tries' records: object 2 claimed by B:5 with A:3
// removed; control object 9 claimed by A:7; object 1 pruned.
constexpr char known_data_value[] = "00010b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b000000000000000500010a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0000000000000003";
constexpr char known_control_value[] = "00010a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a00000000000000070000";
// The checkpoint: each trie one leaf, saved as its node file's one frame
// (nonce 00..0b here), and its published root.
constexpr char known_data_hash[] = "95372c82af27ea7ab911fb36927b05ceba44fb92c92c24442ecd87c15f06ddec";
constexpr char known_data_root[] =
    "4d4c545230303032000000000000000000000000000000000000007e000000000000000195372c82af27ea7ab911fb36927b05ceba44fb92c92c24442ecd87c15f06ddec000000000000007e71"
    "22baddaab88a782ca7f7fa880e4ca1fa5456214bcde2d483759a465f7c9fb2";
constexpr char known_data_nodes[] =
    "0000005e000102030405060708090a0b2a40a2e836ae11b7a3702e3a1c25cafb033b2b73b6c3f5166b40637b5dabfeac6f8d67186d48d5090c21187448e33fb7371ef42d0576430d7f1c832c"
    "717f7356a7481c0358e7a691a9370c10077b7258bf8ac935dce3f85525be79bd698216395a958cf766bc320635ccff4d692f";
constexpr char known_control_hash[] = "25e76a83187d927d84d538e3700c29b7eaefcba99b67006a0b780974352381f2";
constexpr char known_control_root[] =
    "4d4c5452303030320000000000000000000000000000000000000066000000000000000125e76a83187d927d84d538e3700c29b7eaefcba99b67006a0b780974352381f20000000000000066"
    "9e5237385fe783a9bcb5fa9289f23d70678cdc2b8ca1e80d5e2bba59de7c7379";
constexpr char known_control_nodes[] =
    "00000046000102030405060708090a0b276d6b654529376a57d64a8fe33acc83033b2b73b6c3fe166b40637b5dabfeac6f8d67186d48d5090c21187448e33fb7371ef42d0576430d7f3483"
    "2c707e7257a6491d0259e6a790a8360d11077b7258bf8ac937dce2";

void check_known_claims(const RetentionStore& retention) {
  EXPECT_FALSE(retention.retained(data, id(1)));
  EXPECT_TRUE(retention.claims(data, id(1)).adds.empty());
  EXPECT_TRUE(retention.claims(data, id(1)).removed.empty());
  EXPECT_TRUE(retention.retained(data, id(2)));
  EXPECT_EQ(retention.claims(data, id(2)).adds, (std::map<NodeId, uint64_t>{{origin_b, 5}}));
  EXPECT_EQ(retention.claims(data, id(2)).removed, (std::map<NodeId, uint64_t>{{origin_a, 3}}));
  EXPECT_TRUE(retention.retained(control, id(9)));
  EXPECT_FALSE(retention.retained(data, id(9)));
  EXPECT_EQ(retention.claim_objects(data), 1u);
  EXPECT_EQ(retention.claim_objects(control), 1u);
}

// The WAL's operations encode as Macha's, and a record as its trie value.
TEST(LedgerRetention, TheJournalsOperationsAreMachas) {
  const std::vector<ObjectId> one_two{id(1), id(2)};
  EXPECT_EQ(hex(retention_format::encode_add(data, {origin_a, 3}, one_two)), known_plain[0]);
  EXPECT_EQ(hex(retention_format::encode_add(data, {origin_b, 5}, std::vector<ObjectId>{id(2)})), known_plain[1]);
  EXPECT_EQ(hex(retention_format::encode_release(data, {{origin_a, 3}}, one_two)), known_plain[2]);
  EXPECT_EQ(hex(retention_format::encode_prune(data, std::vector<ObjectId>{id(1)})), known_plain[3]);
  EXPECT_EQ(hex(retention_format::encode_add(control, {origin_a, 7}, std::vector<ObjectId>{id(9)})), known_plain[4]);

  retention_format::ObjectState state;
  state.adds[origin_b] = 5;
  state.removed[origin_a] = 3;
  EXPECT_EQ(hex(retention_format::encode_state(state)), known_data_value);
  EXPECT_EQ(retention_format::decode_state(unhex(known_data_value)), state);
  EXPECT_TRUE(retention_format::claimed(unhex(known_data_value)));
  EXPECT_FALSE(retention_format::claimed(unhex("0000000000")));
  EXPECT_THROW(retention_format::decode_state(unhex("0001" + std::string(32, '0') + "0000000000000001" + "0000")), dannan::net::DecodeError);
  EXPECT_THROW(retention_format::decode_state(unhex(std::string(known_data_value) + "00")), dannan::net::DecodeError);
}

// A claims.log Macha wrote replays here, and its checkpoint is Macha's: each
// trie's published root byte for byte, its node file the size Macha's is.
TEST(LedgerRetention, ReplaysAClaimsLogMachaWroteAndCheckpointsMachasBytes) {
  dannan::test::TempDir t;
  const auto state = t.path() / "state";
  const auto root = state / "retention";
  write_file(root / "claims.log", unhex(known_claims_log));
  {
    auto retention = open_store(state);
    check_known_claims(retention);
    ASSERT_TRUE(retention.compact_if_needed(1));
  }
  EXPECT_EQ(read_file(root / "ledger"), (Bytes{'1', '\n'}));
  EXPECT_EQ(std::filesystem::file_size(root / "claims.log"), 0u);
  EXPECT_EQ(hex(read_file(root / "ledger-data" / "root")), known_data_root);
  EXPECT_EQ(hex(read_file(root / "ledger-control" / "root")), known_control_root);
  EXPECT_EQ(std::filesystem::file_size(root / "ledger-data" / "nodes-0.bin"), unhex(known_data_nodes).size());
  EXPECT_EQ(std::filesystem::file_size(root / "ledger-control" / "nodes-0.bin"), unhex(known_control_nodes).size());
  ObjectTrie tree(root / "ledger-data", known_answer_key(), ObjectTrie::Options{}, quiet_logger());
  EXPECT_EQ(hex(tree.root_hash()), known_data_hash);
  EXPECT_EQ(tree.get(id(2)), unhex(known_data_value));
  ObjectTrie control_tree(root / "ledger-control", known_answer_key(), ObjectTrie::Options{}, quiet_logger());
  EXPECT_EQ(hex(control_tree.root_hash()), known_control_hash);
  EXPECT_EQ(control_tree.get(id(9)), unhex(known_control_value));
}

// What Dagda journals is Macha's plaintext in Macha's framing: each frame
// opens under claims.log's associated data to the known plaintext.
TEST(LedgerRetention, JournalsMachasFrames) {
  dannan::test::TempDir t;
  const auto state = t.path() / "state";
  {
    auto retention = open_store(state);
    retention.retain_batch(data, {id(2), id(1)}, {origin_a, 3});
    retention.retain(data, id(2), {origin_b, 5});
    EXPECT_EQ(retention.release_unreferenced(data, {}, RetentionClock{{origin_a, 3}}, 64), 2u);
    EXPECT_EQ(retention.prune_unclaimed(data, [](const ObjectId&) { return false; }, 64), 1u);
    retention.retain(control, id(9), {origin_a, 7});
    check_known_claims(retention);
  }
  SealedJournal journal(state / "retention" / "claims.log", known_answer_key(), retention_format::journal_aad, 4U << 20, quiet_logger());
  std::vector<std::string> frames;
  journal.replay([&](std::span<const uint8_t> plain) { frames.push_back(hex(plain)); }, 4U << 20, "claims");
  ASSERT_EQ(frames.size(), 5u);
  for (size_t i = 0; i < frames.size(); ++i) EXPECT_EQ(frames[i], known_plain[i]);
  // Sealed under a frame's own nonce, the bytes are Macha's.
  const auto first = unhex(known_claims_log);
  const auto nonce = unhex("000102030405060708090a0b");
  const auto sealed = dannan::identity::aes_gcm_seal(known_answer_key()->bytes(), std::span<const uint8_t, 12>(nonce.data(), 12), unhex(known_plain[0]),
                                                     retention_format::journal_aad);
  EXPECT_EQ(Bytes(first.begin() + 16, first.begin() + 32), Bytes(sealed.tag.begin(), sealed.tag.end()));
  EXPECT_EQ(Bytes(first.begin() + 32, first.begin() + 32 + static_cast<std::ptrdiff_t>(sealed.ciphertext.size())), sealed.ciphertext);
}

// A checkpoint Macha saved opens here: the tries' root and node files and
// the marker, written by hand from the known answers.
TEST(LedgerRetention, OpensACheckpointMachaSaved) {
  dannan::test::TempDir t;
  const auto state = t.path() / "state";
  const auto root = state / "retention";
  write_file(root / "ledger", Bytes{'1', '\n'});
  write_file(root / "ledger-data" / "root", unhex(known_data_root));
  write_file(root / "ledger-data" / "nodes-0.bin", unhex(known_data_nodes));
  write_file(root / "ledger-control" / "root", unhex(known_control_root));
  write_file(root / "ledger-control" / "nodes-0.bin", unhex(known_control_nodes));
  {
    auto retention = open_store(state);
    check_known_claims(retention);
    // The tombstone keeps suppressing A's released dot.
    retention.retain(data, id(2), {origin_a, 3});
    EXPECT_EQ(retention.claims(data, id(2)).adds.size(), 1u);
  }
  EXPECT_EQ(hex(read_file(root / "ledger-data" / "root")), known_data_root);
}

}  // namespace
}  // namespace dagda::ledger
