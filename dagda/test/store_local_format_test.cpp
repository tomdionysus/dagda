//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The local store's files, byte for byte Macha's layout v18 (requirements
// section 4.1, S2, S3, S7, S8), since only the tag's binding changed with
// the one layout. The known answers were worked out from Macha's code
// (src/storage/local_store.cpp, develop 5a06d3c: the loose header at
// :1092-1096, the pack header at :139-156, the accounting slot at :96-112)
// and computed outside both codebases, the SHA-256s by Python's hashlib, over
// 1a's pinned sealing of "An object Macha sealed under its own id." under
// domain v1's known-answer key (store_sealing_test). A pack record's time is
// the stepped wall clock's. The codec cases keep Macha's v18 tag in the
// bytes, since the codecs do not look at it; the cases that opened Macha's
// files through the store (OpensALooseObjectMachaWrote, OpensAPackMachaWrote)
// are the migration's now (migrate_migrate_test), where those files are
// migrated and then opened.

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <string_view>

#include "dagda/store/local_format.h"
#include "dagda/store/local_store.h"
#include "dagda/store/sealing.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/identity/crypto.h"

namespace dagda::store {
namespace {

using dannan::identity::hex;
using namespace local_format;
using test::StoreBed;

Bytes unhex(std::string_view text) { return dannan::identity::unhex(std::string(text)).value(); }

constexpr std::string_view plaintext = "An object Macha sealed under its own id.";
constexpr char object_hex[] = "3e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8";
constexpr char nonce_hex[] = "000102030405060708090a0b";
constexpr char ciphertext_hex[] = "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
constexpr char tag_hex[] = "6f6aa83135d5e163f1b04d0c9ec0758c";
constexpr uint64_t put_ms = 1793700000000ull;

// The whole loose file Macha writes for the object under that nonce.
constexpr char loose_file_hex[] =
    "4448544f424a30310000000000000028000102030405060708090a0b6f6aa83135d5e163f1b04d0c9ec0758c"
    "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
// A pack's put record header at put_ms, then its payload (the ciphertext).
constexpr char put_header_hex[] =
    "4d414348504b303101000001a1a0d9f1003e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "00000000000000280000000000000028000102030405060708090a0b6f6aa83135d5e163f1b04d0c9ec0758c"
    "adcbbd1ad98f932c95b0af893dce6de8efa1d701f299bb7a0e25358c929fc5a2";
// A touch at put_ms + 500 and a tombstone at put_ms + 1000: no payload.
constexpr char touch_hex[] =
    "4d414348504b303103000001a1a0d9f2f43e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "de2a9d5429d594851c0975a73134b1cb11c2f5a0d13ed3015333df6bb545b580";
constexpr char tombstone_hex[] =
    "4d414348504b303102000001a1a0d9f4e83e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "dde293c4c0f89a1ec8e5b3ec8656f0cbf163c2ebbc563e5e0c159e9eea53718a";
// An accounting slot: sequence 7, used 123456, dirty, no id, no size.
constexpr char accounting_hex[] =
    "4d414348414143310000000000000007000000000001e240030000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "185a858568a81c0fba14835279750257f781ea62f7c8caec518bf8bd9a3d02bf";

ObjectId the_id() {
  ObjectId id;
  const auto bytes = unhex(object_hex);
  std::copy(bytes.begin(), bytes.end(), id.bytes.begin());
  return id;
}

dannan::identity::SealedData the_sealed() {
  dannan::identity::SealedData sealed;
  const auto nonce = unhex(nonce_hex);
  const auto tag = unhex(tag_hex);
  std::copy(nonce.begin(), nonce.end(), sealed.nonce.begin());
  std::copy(tag.begin(), tag.end(), sealed.tag.begin());
  sealed.ciphertext = unhex(ciphertext_hex);
  return sealed;
}

PackHeader header_of(uint8_t type, uint64_t touched_ms) {
  PackHeader header;
  header.type = type;
  header.touched_ms = touched_ms;
  header.id = the_id();
  if (type == pack_put) {
    const auto sealed = the_sealed();
    header.plain_size = plaintext.size();
    header.payload_size = sealed.ciphertext.size();
    header.nonce = sealed.nonce;
    header.tag = sealed.tag;
  }
  return header;
}

Bytes file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), {});
}

LocalStoreOptions packing() {
  LocalStoreOptions options;
  options.limit = 64ULL * 1024 * 1024;
  return options;
}

TEST(StoreLocalFormat, EncodesTheLooseHeaderAsMachaDoes) {
  const auto sealed = the_sealed();
  const auto header = encode_loose_header(plaintext.size(), sealed);
  ASSERT_EQ(header.size(), loose_header_size);
  Bytes file = header;
  file.insert(file.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
  EXPECT_EQ(hex(file), loose_file_hex);
  const auto decoded = decode_loose(file);
  EXPECT_EQ(decoded.plain_size, plaintext.size());
  EXPECT_EQ(decoded.nonce, sealed.nonce);
  EXPECT_EQ(decoded.tag, sealed.tag);
  EXPECT_EQ(Bytes(decoded.ciphertext.begin(), decoded.ciphertext.end()), sealed.ciphertext);
  auto other = file;
  other[0] = 'X';
  EXPECT_THROW((void)decode_loose(other), std::runtime_error);
}

TEST(StoreLocalFormat, EncodesPackRecordsAsMachaDoes) {
  EXPECT_EQ(hex(encode_pack_header(header_of(pack_put, put_ms))), put_header_hex);
  EXPECT_EQ(hex(encode_pack_header(header_of(pack_touch, put_ms + 500))), touch_hex);
  EXPECT_EQ(hex(encode_pack_header(header_of(pack_remove, put_ms + 1000))), tombstone_hex);
  for (const auto* known : {put_header_hex, touch_hex, tombstone_hex}) {
    const auto bytes = unhex(known);
    ASSERT_EQ(bytes.size(), pack_header_size);
    const auto decoded = decode_pack_header(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->id, the_id());
    EXPECT_EQ(encode_pack_header(*decoded), bytes);
  }
}

// What the decoder refuses: a bad checksum, an unknown type, sizes that do
// not fit the type, a wrong length.
TEST(StoreLocalFormat, RefusesAPackHeaderThatDoesNotHold) {
  auto damaged = unhex(put_header_hex);
  damaged[100] ^= 1;
  EXPECT_FALSE(decode_pack_header(damaged).has_value());
  EXPECT_FALSE(decode_pack_header(std::span<const uint8_t>(unhex(put_header_hex)).first(124)).has_value());
  auto unknown = header_of(pack_put, put_ms);
  unknown.type = 4;
  EXPECT_FALSE(decode_pack_header(encode_pack_header(unknown)).has_value());
  auto sized_tombstone = header_of(pack_remove, put_ms);
  sized_tombstone.payload_size = 1;
  sized_tombstone.plain_size = 1;
  EXPECT_FALSE(decode_pack_header(encode_pack_header(sized_tombstone)).has_value());
  auto mismatched = header_of(pack_put, put_ms);
  mismatched.plain_size = 41;
  EXPECT_FALSE(decode_pack_header(encode_pack_header(mismatched)).has_value());
  auto oversized = header_of(pack_put, put_ms);
  oversized.plain_size = oversized.payload_size = pack_payload_safety_limit + 1;
  EXPECT_FALSE(decode_pack_header(encode_pack_header(oversized)).has_value());
}

TEST(StoreLocalFormat, EncodesAnAccountingSlotAsMachaDoes) {
  const AccountingRecord record{7, 123456, accounting_dirty, ObjectId{}, 0};
  EXPECT_EQ(hex(encode_accounting(record)), accounting_hex);
  const auto decoded = decode_accounting(unhex(accounting_hex));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->sequence, 7u);
  EXPECT_EQ(decoded->used, 123456u);
  EXPECT_EQ(decoded->operation, accounting_dirty);
  auto damaged = unhex(accounting_hex);
  damaged[20] ^= 1;
  EXPECT_FALSE(decode_accounting(damaged).has_value());
}

TEST(StoreLocalFormat, NamesPacksAndObjectsAsMachaDoes) {
  EXPECT_EQ(pack_name(1), "pack-00000000000000000001.pack");
  EXPECT_EQ(pack_sequence("pack-00000000000000000042.pack"), std::optional<uint64_t>(42));
  EXPECT_FALSE(pack_sequence("pack-00000000000000000000.pack").has_value());
  EXPECT_FALSE(pack_sequence("pack-0000000000000000004.pack").has_value());
  EXPECT_FALSE(pack_sequence(".compact-1-2.tmp").has_value());
  EXPECT_EQ(loose_path("/s/objects", the_id()), std::filesystem::path("/s/objects/3e/2d") / (std::string(object_hex) + ".obj"));
  EXPECT_EQ(object_id_from_file_name(std::string(object_hex) + ".obj"), std::optional<ObjectId>(the_id()));
  std::string upper = object_hex;
  for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  EXPECT_EQ(object_id_from_file_name(upper + ".obj"), std::optional<ObjectId>(the_id()));
  EXPECT_FALSE(object_id_from_file_name(std::string(object_hex) + ".tmp").has_value());
  EXPECT_FALSE(object_id_from_file_name("zz" + std::string(object_hex).substr(2) + ".obj").has_value());
}

// Dagda writes Macha's bytes. A packed put's header is Macha's in every
// field but the nonce and tag it drew, which open under the key and the
// domain; a touch and a tombstone, which carry no secret, are Macha's
// exactly.
TEST(StoreLocalFormat, WritesThePackRecordsMachaWrites) {
  StoreBed bed(put_ms);
  const auto root = bed.dir.path() / "store";
  const Bytes plain(plaintext.begin(), plaintext.end());
  auto store = bed.open(root, packing());
  ASSERT_TRUE(store->put(the_id(), plain));
  const auto pack = root / "packs" / "pack-00000000000000000001.pack";
  ASSERT_EQ(store->object_path(the_id()), pack);
  auto bytes = file_bytes(pack);
  ASSERT_EQ(bytes.size(), pack_header_size + plain.size());
  const auto known = unhex(put_header_hex);
  // Magic, type, time, id, sizes: offsets 0 to 64.
  EXPECT_EQ(hex(std::span<const uint8_t>(bytes).first(65)), hex(std::span<const uint8_t>(known).first(65)));
  const auto header = decode_pack_header(std::span<const uint8_t>(bytes).first(pack_header_size));
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(
      open_object(*bed.key, store->sealing(), the_id(), header->nonce, header->tag, std::span<const uint8_t>(bytes).subspan(pack_header_size), plain.size()),
      plain);

  bed.wall.advance(std::chrono::milliseconds(500));
  store->touch(the_id());
  bed.wall.advance(std::chrono::milliseconds(500));
  ASSERT_TRUE(store->remove(the_id()));
  bytes = file_bytes(pack);
  ASSERT_EQ(bytes.size(), 3 * pack_header_size + plain.size());
  EXPECT_EQ(hex(std::span<const uint8_t>(bytes).subspan(pack_header_size + plain.size(), pack_header_size)), touch_hex);
  EXPECT_EQ(hex(std::span<const uint8_t>(bytes).subspan(2 * pack_header_size + plain.size())), tombstone_hex);
}

// A loose object Dagda writes is Macha's layout: the header's magic and size,
// then a nonce and tag that open the ciphertext under the key, the domain
// and the id.
TEST(StoreLocalFormat, WritesTheLooseFileMachaWrites) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  const Bytes plain(plaintext.begin(), plaintext.end());
  auto store = bed.open(root, LocalStoreOptions::loose_only(64ULL * 1024 * 1024));
  ASSERT_TRUE(store->put(the_id(), plain));
  const auto path = root / "objects" / "3e" / "2d" / (std::string(object_hex) + ".obj");
  ASSERT_EQ(store->object_path(the_id()), path);
  const auto bytes = file_bytes(path);
  ASSERT_EQ(bytes.size(), loose_header_size + plain.size());
  EXPECT_EQ(hex(std::span<const uint8_t>(bytes).first(16)), std::string(loose_file_hex).substr(0, 32));
  const auto loose = decode_loose(bytes);
  EXPECT_EQ(open_object(*bed.key, store->sealing(), the_id(), loose.nonce, loose.tag, loose.ciphertext, loose.plain_size), plain);
}

}  // namespace
}  // namespace dagda::store
