//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirements H1, H2, H3, H5, H27: a head's record, hash, entry, certificate and
// the sections every owner frames alike. Each case names Macha's it moved from
// (tests/test_storage_metadata.cpp at 9a08846, "SM"), or the plan's case.

#include <gtest/gtest.h>

#include <vector>

#include "dagda/heads/record.h"
#include "dagda/heads/replica.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/encoding/base.h"
#include "dannan/identity/crypto.h"

namespace dagda::heads {
namespace {

using dannan::net::Reader;
using dannan::net::Writer;
using test::make_record;
using test::neutral_payload;
using test::NeutralHead;
using test::pattern;
using test::ReplicaBed;

// test_metadata_record_payload_copy_is_shared (SM:541). Its resident-memory check
// is Linux's and is about the same sharing: the pointers are what say so here.
TEST(HeadsRecord, APayloadCopyIsShared) {
  Record record;
  record.payload = Bytes(4 * 1024 * 1024, 0x5a);
  const auto* backing = record.payload.data();

  Record copy = record;
  Record second_copy = copy;
  EXPECT_EQ(copy.payload.data(), backing);
  EXPECT_EQ(second_copy.payload.data(), backing);

  copy.payload = Bytes(16, 0x11);
  EXPECT_NE(copy.payload.data(), backing);
  EXPECT_EQ(record.payload.data(), second_copy.payload.data());

  record.payload = Bytes(8 * 1024 * 1024, 0xa5);
  const auto* large_backing = record.payload.data();
  const std::vector<Record> copies(64, record);
  for (const auto& retained : copies) EXPECT_EQ(retained.payload.data(), large_backing);
}

// test_metadata_hash_streaming_matches_canonical_encoding (SM:571).
TEST(HeadsRecord, TheHashStreamsTheCanonicalEncoding) {
  const uint64_t generation = 0x1122334455667788ULL;
  Hash256 previous{};
  for (size_t i = 0; i < previous.bytes.size(); ++i) previous.bytes[i] = static_cast<uint8_t>(i * 7 + 3);
  const auto payload = pattern(2 * 1024 * 1024 + 137);

  Writer canonical;
  canonical.u64(generation);
  canonical.fixed(previous.bytes);
  canonical.bytes(payload);
  EXPECT_EQ(record_hash(generation, previous, payload), dannan::identity::sha256(canonical.data()));
}

// test_history_transfer_follows_only_materialization_dependencies (SM:1524).
TEST(HeadsRecord, HistoryTransferFollowsOnlyMaterializationDependencies) {
  Hash256 previous{};
  Hash256 merge_parent{};
  previous.bytes[0] = 1;
  merge_parent.bytes[0] = 2;

  HistoryEntry checkpoint;
  checkpoint.generation = 10;
  checkpoint.previous = previous;
  checkpoint.previous_known = true;
  checkpoint.merge_parents = {merge_parent};
  checkpoint.body = HistoryEntry::Body::full;
  EXPECT_TRUE(materialization_dependencies(checkpoint).empty());

  HistoryEntry delta = checkpoint;
  delta.body = HistoryEntry::Body::delta;
  const auto required = materialization_dependencies(delta);
  ASSERT_EQ(required.size(), 1U);
  EXPECT_EQ(required.front(), previous);
}

std::vector<Hash256> many_parents(size_t count) {
  std::vector<Hash256> out(count);
  for (size_t i = 0; i < count; ++i) {
    out[i].bytes[0] = 0xaa;
    out[i].bytes[1] = static_cast<uint8_t>(i + 1);
  }
  return out;
}

// C3: the bound of H3 at every codec: the owner's sections, the entry, a frame on
// disk and import. No live path reaches it, so only this holds it.
TEST(HeadsRecord, SixtyFourMergeParentsPassAndSixtyFiveAreRefusedEverywhere) {
  const auto allowed = many_parents(64);
  const auto over = many_parents(65);

  // The section an owner's record frames.
  {
    Writer writer;
    encode_merge_parents(writer, allowed);
    const auto bytes = writer.take();
    Reader reader(bytes);
    EXPECT_EQ(decode_merge_parents(reader), allowed);
    reader.finish();
  }
  {
    Writer writer;
    EXPECT_THROW(encode_merge_parents(writer, over), std::runtime_error);
    Writer forged;
    forged.u32(65);
    for (const auto& parent : over) forged.fixed(parent.bytes);
    const auto bytes = forged.take();
    Reader reader(bytes);
    EXPECT_THROW((void)decode_merge_parents(reader), DecodeError);
  }

  // The history entry.
  const auto entry_with = [](const std::vector<Hash256>& parents) {
    HistoryEntry entry;
    entry.generation = 9;
    entry.hash.bytes[0] = 1;
    entry.merge_parents = parents;
    entry.body = HistoryEntry::Body::full;
    entry.payload = {1, 2, 3};
    return entry;
  };
  const auto encoded = encode_history_entry(entry_with(allowed));
  EXPECT_EQ(decode_history_entry(encoded), entry_with(allowed));
  EXPECT_THROW((void)encode_history_entry(entry_with(over)), std::runtime_error);
  // The same bytes with a sixty-fifth parent and the count to say so.
  Writer forged;
  forged.u8(1);
  forged.u64(9);
  forged.fixed(Hash256{}.bytes);
  auto hash = Hash256{};
  hash.bytes[0] = 1;
  forged.fixed(hash.bytes);
  forged.u8(0);
  forged.u32(65);
  for (const auto& parent : over) forged.fixed(parent.bytes);
  forged.bytes(Bytes{1, 2, 3});
  const auto forged_entry = forged.take();
  EXPECT_THROW((void)decode_history_entry(forged_entry), DecodeError);

  // Import, and a frame: a replica takes 64 and, the frame holding 65 on disk, skips it.
  ReplicaBed bed;
  auto replica = bed.open();
  NeutralHead with_allowed;
  with_allowed.merge_parents = allowed;
  const auto record = make_record(50, Hash256{{7}}, neutral_payload(with_allowed));
  EXPECT_TRUE(replica->import_history(test::rootless_entry(record, allowed)));
  EXPECT_TRUE(replica->history_contains(record.hash));

  NeutralHead with_over;
  const auto record_over = make_record(51, Hash256{{7}}, neutral_payload(with_over));
  auto entry_over = test::rootless_entry(record_over, over);
  EXPECT_FALSE(replica->import_history(entry_over));
  EXPECT_FALSE(replica->history_contains(record_over.hash));
  EXPECT_FALSE(replica->reanchor_history(entry_over));

  const auto history = bed.path() / "history.log";
  const auto records = replica->diagnostics().history_records;
  replica.reset();
  {
    const auto aad = replica_aad(test::neutral_domain(), test::neutral_space())(test::neutral_magics().history);
    const auto frame = seal_frame(*bed.content_key, aad, forged_entry);
    std::ofstream out(history, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
  }
  // A good frame after it. Authenticated, the bad one is skipped wherever it lies.
  {
    const auto aad = replica_aad(test::neutral_domain(), test::neutral_space())(test::neutral_magics().history);
    const auto after = make_record(52, Hash256{{7}}, neutral_payload(with_over));
    const auto frame = seal_frame(*bed.content_key, aad, encode_history_entry(test::rootless_entry(after)));
    std::ofstream out(history, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
  }
  replica = bed.open();
  EXPECT_EQ(replica->diagnostics().history_records, records + 1);
  EXPECT_FALSE(replica->history_contains(hash));
  EXPECT_TRUE(replica->history_contains(record.hash));
}

// H5's certificate codec, as Macha's decode refuses it.
TEST(HeadsRecord, ACertificateHoldsOrIsRefused) {
  const auto a = test::author(1);
  const auto b = test::author(2);
  Hash256 hash{};
  hash.bytes[0] = 9;
  // Replicas are written sorted and unique.
  EXPECT_EQ(decode_acceptance(encode_acceptance({3, hash, 2, {b, a, b}})), (Acceptance{3, hash, 2, {a, b}}));
  EXPECT_EQ(decode_acceptance(encode_acceptance({1, hash, 0, {}})), (Acceptance{1, hash, 0, {}}));

  EXPECT_THROW((void)encode_acceptance({3, hash, 2, {a}}), std::runtime_error);
  EXPECT_THROW((void)encode_acceptance({3, hash, 0, {a}}), std::runtime_error);
  EXPECT_THROW((void)encode_acceptance({0, hash, 0, {}}), std::runtime_error);
  EXPECT_THROW((void)encode_acceptance({3, Hash256{}, 0, {}}), std::runtime_error);
  EXPECT_THROW((void)encode_acceptance({3, hash, 1, {NodeId{}}}), std::runtime_error);

  // Not canonical on the wire.
  Writer unsorted;
  unsorted.u64(3);
  unsorted.fixed(hash.bytes);
  unsorted.u32(2);
  unsorted.u32(2);
  unsorted.fixed(b.bytes);
  unsorted.fixed(a.bytes);
  const auto unsorted_bytes = unsorted.take();
  EXPECT_THROW((void)decode_acceptance(unsorted_bytes), DecodeError);

  // A set holds a hash once.
  const auto set = encode_acceptance_set({{3, hash, 1, {a}}});
  EXPECT_EQ(decode_acceptance_set(set).size(), 1U);
  Writer twice;
  twice.u32(2);
  const auto one = encode_acceptance({3, hash, 1, {a}});
  twice.bytes(one);
  twice.bytes(one);
  const auto twice_bytes = twice.take();
  EXPECT_THROW((void)decode_acceptance_set(twice_bytes), DecodeError);
}

// C15: the owner's constant. Macha's payload hashes to its known answer, through the
// same function a replica opens on; the neutral owner's is a constant per space.
TEST(HeadsGenesis, TheConstantIsMachasOnMachasSpace) {
  const Bytes macha_payload{'D', 'H', 'T', 'M', 'G', 'E', 'N', '1'};
  const auto machas = genesis_record(macha_payload);
  EXPECT_EQ(machas.generation, 1U);
  EXPECT_EQ(machas.previous, Hash256{});
  EXPECT_EQ(dannan::encoding::hex(machas.hash.bytes), "87ad4c58cd1ccdd1326698d186eeb0ce54e5bf0cb8b1829b688a54584b6d7f06");
  EXPECT_TRUE(valid_record(machas));

  ReplicaBed bed;
  auto replica = bed.open();
  const auto first = replica->committed();
  EXPECT_EQ(first.generation, 1U);
  EXPECT_EQ(first.previous, Hash256{});
  EXPECT_EQ(first.hash, bed.genesis().hash);
  EXPECT_NE(first.hash, machas.hash);

  ReplicaBed again;
  EXPECT_EQ(again.open()->committed().hash, first.hash);

  ReplicaBed other;
  other.codec = std::make_shared<test::NeutralCodec>(nullptr, Bytes{'O', 'T', 'H', 'E', 'R'});
  auto elsewhere = other.open();
  EXPECT_NE(elsewhere->committed().hash, first.hash);
  EXPECT_EQ(elsewhere->committed().payload, (Bytes{'O', 'T', 'H', 'E', 'R'}));
}

TEST(HeadsRecord, AFormVersionIsItsLastByte) {
  EXPECT_EQ(form_version(Bytes{'D', 'H', 'T', 'M', 'D', 'L', 'T', '5'}, "DHTMDLT"), 5);
  EXPECT_EQ(form_version(Bytes{'D', 'H', 'T', 'M', 'D', 'L', 'T', 'A'}, "DHTMDLT"), 10);
  EXPECT_EQ(form_version(Bytes{'D', 'H', 'T', 'M', 'D', 'L', 'T', 'B'}, "DHTMDLT"), 11);
  EXPECT_EQ(form_version(Bytes{'D', 'H', 'T', 'M', 'D', 'L', 'T', '0'}, "DHTMDLT"), 0);
  EXPECT_EQ(form_version(Bytes{'X', 'H', 'T', 'M', 'D', 'L', 'T', '5'}, "DHTMDLT"), 0);
  EXPECT_EQ(form_version(Bytes{'D', 'H'}, "DHTMDLT"), 0);
}

}  // namespace
}  // namespace dagda::heads
