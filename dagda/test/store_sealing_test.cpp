//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// An object is sealed as Macha sealed it (requirement D2) but for what the
// tag authenticates: the one layout, v19, binds the domain and the id (the maintainer,
// 2026-11-04 night: v18 is read by the migration alone). The known answers
// were computed outside both codebases: the keystream and block encryptions
// by `openssl enc` (AES-256-CTR and ECB), GHASH by hand, the whole checked
// first against the GCM specification's test case 16 (build/kat/step6 and
// step6b). The v18 known answers, Macha's bytes, are the migration's now
// (migrate_legacy_v18_test).

#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dagda/domain/domain_id.h"
#include "dagda/domain/storage_key.h"
#include "dagda/store/object_id.h"
#include "dagda/store/sealing.h"
#include "dannan/encoding/base.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::store {
namespace {

using dannan::encoding::hex;

Bytes unhex(const std::string& text) { return dannan::encoding::unhex(text).value(); }

Bytes bytes_of(std::string_view text) { return Bytes(text.begin(), text.end()); }

// Macha's test pattern (tests/test_support.hpp): an xorshift stream.
Bytes pattern(size_t n, uint8_t salt = 0) {
  Bytes out(n);
  uint64_t x = 0x123456789abcdef0ULL ^ salt;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    out[i] = static_cast<uint8_t>(x);
  }
  return out;
}

dannan::identity::ClusterKey cluster_key(const dannan::test::TempDir& dir, const std::string& contents = "dagda known-answer cluster key\r\n") {
  const auto path = dir.path() / "cluster.key";
  std::ofstream(path, std::ios::binary) << contents;
  return dannan::identity::load_cluster_key(path);
}

// The known-answer object, under domain v1's key and nonce 00..0b: its v18
// bytes were Macha's; its v19 tag (domain v1) and the empty object's are
// step 6b's known answers.
constexpr std::string_view plaintext = "An object Macha sealed under its own id.";
constexpr char object_hex[] = "3e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8";
constexpr char nonce_hex[] = "000102030405060708090a0b";
constexpr char ciphertext_hex[] = "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
constexpr char v18_tag_hex[] = "6f6aa83135d5e163f1b04d0c9ec0758c";
constexpr char v1_tag_hex[] = "b05d61c20d49091b6d3ee9e145ef880c";
constexpr char v1_empty_tag_hex[] = "44b3ddd655b79590e686c9a569aa995f";
const SealingScheme v1 = SealingScheme::v19(domain::domain_v1);

// The known-answer bytes under domain v1 open here, with the size and the
// id checked.
TEST(StoreSealing, OpensTheKnownAnswerUnderDomainV1) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = object_id(bytes_of(plaintext));
  ASSERT_EQ(to_string(id), object_hex);
  const auto nonce = unhex(nonce_hex);
  const auto tag = unhex(v1_tag_hex);
  const auto ciphertext = unhex(ciphertext_hex);
  const auto opened =
      open_object(key, v1, id, std::span<const uint8_t, 12>(nonce.data(), 12), std::span<const uint8_t, 16>(tag.data(), 16), ciphertext, plaintext.size());
  EXPECT_EQ(opened, bytes_of(plaintext));
}

// Under the same nonce Dagda's sealing produces the known answer exactly:
// Dannan's AES-256-GCM with the label, the domain and the id as the
// associated data. The ciphertext is Macha's, since the keystream does not
// see the associated data; only the tag is the domain's.
TEST(StoreSealing, SealsTheKnownAnswerUnderTheSameNonce) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = object_id(bytes_of(plaintext));
  const auto nonce = unhex(nonce_hex);
  const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), std::span<const uint8_t, 12>(nonce.data(), 12), bytes_of(plaintext), v1.associated_data(id));
  EXPECT_EQ(hex(sealed.ciphertext), ciphertext_hex);
  EXPECT_EQ(hex(sealed.tag), v1_tag_hex);
  EXPECT_NE(std::string(v1_tag_hex), v18_tag_hex);
}

// What seal_object writes is that layout: the plaintext's size of
// ciphertext, a fresh nonce, and a tag over the domain and the id.
TEST(StoreSealing, SealObjectIsAesGcmWithTheDomainAndIdAsAssociatedData) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(4096, 3);
  const auto id = object_id(plain);
  const auto first = seal_object(key, v1, id, plain);
  const auto second = seal_object(key, v1, id, plain);
  EXPECT_EQ(first.ciphertext.size(), plain.size());
  EXPECT_NE(first.nonce, second.nonce);
  EXPECT_EQ(dannan::identity::aes_gcm_open(key.bytes(), first.nonce, first.tag, first.ciphertext, v1.associated_data(id)), plain);
  EXPECT_THROW((void)dannan::identity::aes_gcm_open(key.bytes(), first.nonce, first.tag, first.ciphertext), std::runtime_error);
  EXPECT_THROW((void)dannan::identity::aes_gcm_open(key.bytes(), first.nonce, first.tag, first.ciphertext, id.bytes), std::runtime_error);
}

// A ciphertext cannot be passed off as another object's: the id is
// authenticated.
TEST(StoreSealing, AnotherIdDoesNotOpen) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(1000, 4);
  const auto id = object_id(plain);
  const auto sealed = seal_object(key, v1, id, plain);
  auto other = id;
  other.bytes[31] ^= 1;
  EXPECT_THROW((void)open_object(key, v1, other, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
  EXPECT_EQ(open_object(key, v1, id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), plain);
}

// Authentic bytes that are not the object recorded are refused: another
// size than recorded, or a plaintext that is not the id's.
TEST(StoreSealing, OpenChecksTheSizeAndTheId) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(1000, 5);
  const auto id = object_id(plain);
  const auto sealed = seal_object(key, v1, id, plain);
  EXPECT_THROW((void)open_object(key, v1, id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size() + 1), ObjectIntegrityError);

  // Sealed under an id that does not name it.
  const auto stranger = object_id(pattern(1000, 6));
  const auto misnamed = seal_object(key, v1, stranger, plain);
  EXPECT_THROW((void)open_object(key, v1, stranger, misnamed.nonce, misnamed.tag, misnamed.ciphertext, plain.size()), ObjectIntegrityError);
}

// In place, an object is the same AES-GCM as seal_object's, in the bytes it was
// handed: the ciphertext where the plaintext was, then the plaintext again, the
// size and the id checked as open_object checks them.
TEST(StoreSealing, InPlaceSealsAndOpensWhereTheBytesLie) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(4096, 8);
  const auto id = object_id(plain);
  auto bytes = plain;
  const auto* where = bytes.data();
  const auto seal = seal_object_in_place(key, v1, id, bytes);
  EXPECT_EQ(bytes.data(), where);
  EXPECT_NE(bytes, plain);
  // Interchangeable with the allocating forms.
  EXPECT_EQ(open_object(key, v1, id, seal.nonce, seal.tag, bytes, plain.size()), plain);
  const auto sealed = seal_object(key, v1, id, plain);
  auto copy = sealed.ciphertext;
  open_object_in_place(key, v1, id, sealed.nonce, sealed.tag, copy, plain.size());
  EXPECT_EQ(copy, plain);

  open_object_in_place(key, v1, id, seal.nonce, seal.tag, bytes, plain.size());
  EXPECT_EQ(bytes.data(), where);
  EXPECT_EQ(bytes, plain);
}

// A ciphertext that does not open in place leaves no plaintext behind, and one
// of another size than recorded is refused untouched.
TEST(StoreSealing, InPlaceRefusesWhatOpenObjectRefuses) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(1000, 9);
  const auto id = object_id(plain);
  const auto sealed = seal_object(key, v1, id, plain);

  auto other = id;
  other.bytes[31] ^= 1;
  auto bytes = sealed.ciphertext;
  EXPECT_THROW(open_object_in_place(key, v1, other, sealed.nonce, sealed.tag, bytes, plain.size()), std::runtime_error);
  EXPECT_EQ(bytes, Bytes(plain.size(), 0));

  bytes = sealed.ciphertext;
  EXPECT_THROW(open_object_in_place(key, v1, id, sealed.nonce, sealed.tag, bytes, plain.size() + 1), ObjectIntegrityError);
  EXPECT_EQ(bytes, sealed.ciphertext);

  const auto stranger = object_id(pattern(1000, 10));
  const auto misnamed = seal_object(key, v1, stranger, plain);
  bytes = misnamed.ciphertext;
  EXPECT_THROW(open_object_in_place(key, v1, stranger, misnamed.nonce, misnamed.tag, bytes, plain.size()), ObjectIntegrityError);
}

// Another domain's key opens nothing.
TEST(StoreSealing, AnotherKeyDoesNotOpen) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto other = domain::StorageKey::derive_v1(cluster_key(dir, "another cluster"));
  const auto plain = pattern(64, 7);
  const auto id = object_id(plain);
  const auto sealed = seal_object(key, v1, id, plain);
  EXPECT_THROW((void)open_object(other, v1, id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
}

// Moved from Macha's tests/test_foundations.cpp, test_codec_and_crypto, its
// crypto assertions (develop 5a06d3c). Macha's ClusterKeys become Dannan's
// ClusterKey and domain v1's storage key; the codec and telemetry assertions
// before them are Dannan's records and stay with Dannan.
TEST(StoreSealing, MachaCodecAndCryptoSealOpenAndRefuseAFlippedBit) {
  dannan::test::TempDir dir;
  const auto keys = cluster_key(dir, "macha test cluster key\n");
  const auto storage = domain::StorageKey::derive_v1(keys);
  auto plain = pattern(65537);
  auto sealed = dannan::identity::aes_gcm_seal(storage.bytes(), plain, keys.cluster_id);
  auto opened = dannan::identity::aes_gcm_open(storage.bytes(), sealed.nonce, sealed.tag, sealed.ciphertext, keys.cluster_id);
  EXPECT_EQ(opened, plain);

  // An empty plaintext with AAD seals to an empty ciphertext, not one sized
  // by the AAD.
  Bytes empty;
  Bytes aad{1, 2, 3, 4, 5};
  auto empty_sealed = dannan::identity::aes_gcm_seal(keys.auth, empty, aad);
  EXPECT_TRUE(empty_sealed.ciphertext.empty());
  EXPECT_TRUE(dannan::identity::aes_gcm_open(keys.auth, empty_sealed.nonce, empty_sealed.tag, empty_sealed.ciphertext, aad).empty());

  auto alice = dannan::identity::x25519_generate();
  auto bob = dannan::identity::x25519_generate();
  auto alice_shared = dannan::identity::x25519_shared(alice.private_key, bob.public_key);
  auto bob_shared = dannan::identity::x25519_shared(bob.private_key, alice.public_key);
  EXPECT_EQ(alice_shared, bob_shared);

  sealed.ciphertext[0] ^= 1;
  bool rejected = false;
  try {
    (void)dannan::identity::aes_gcm_open(storage.bytes(), sealed.nonce, sealed.tag, sealed.ciphertext, keys.cluster_id);
  } catch (...) {
    rejected = true;
  }
  EXPECT_TRUE(rejected);
}

// The empty object seals to no ciphertext and a tag over the domain and its
// id alone.
TEST(StoreSealing, TheEmptyObjectSealsToATagAlone) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = object_id({});
  const auto nonce = unhex(nonce_hex);
  const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), std::span<const uint8_t, 12>(nonce.data(), 12), Bytes{}, v1.associated_data(id));
  EXPECT_TRUE(sealed.ciphertext.empty());
  EXPECT_EQ(hex(sealed.tag), v1_empty_tag_hex);
  EXPECT_TRUE(open_object(key, v1, id, sealed.nonce, sealed.tag, sealed.ciphertext, 0).empty());
}

// ---- The domain in the associated data (step 6) ------------------------------
// The known answers were computed as the wrap's were (domain_wrap_test):
// AES blocks by `openssl enc -aes-256-ecb`, GCM by hand, checked against the
// GCM specification's test case 16.

constexpr char domain_hex[] = "00112233445566778899aabbccddeeff";
constexpr char v19_tag_hex[] = "d17467ebe8e66d7404b39ffbb2e67868";

// The associated data is the label, the domain and the id. The ciphertext
// is the keystream's, the same whatever the associated data under the same
// key and nonce; the tag is where the binding lives, and it differs by
// domain, and from Macha's over the id alone.
TEST(StoreSealing, TheAssociatedDataBindsTheDomain) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = object_id(bytes_of(plaintext));
  const auto domain = domain::DomainId::parse(domain_hex).value();
  const auto scheme = SealingScheme::v19(domain);
  EXPECT_EQ(hex(scheme.associated_data(id)), std::string("64616764612f7365616c2f3139") + domain_hex + object_hex);
  EXPECT_EQ(hex(v1.associated_data(id)), std::string("64616764612f7365616c2f3139") + "827b07101d79d9d84a433d8e9cb262ca" + object_hex);
  const auto nonce = unhex(nonce_hex);
  const auto sealed =
      dannan::identity::aes_gcm_seal(key.bytes(), std::span<const uint8_t, 12>(nonce.data(), 12), bytes_of(plaintext), scheme.associated_data(id));
  EXPECT_EQ(hex(sealed.ciphertext), ciphertext_hex);
  EXPECT_EQ(hex(sealed.tag), v19_tag_hex);
  EXPECT_NE(std::string(v19_tag_hex), v1_tag_hex);
  EXPECT_NE(std::string(v19_tag_hex), v18_tag_hex);
  EXPECT_EQ(open_object(key, scheme, id, sealed.nonce, sealed.tag, sealed.ciphertext, plaintext.size()), bytes_of(plaintext));
  // Macha's tag over the id alone does not open under any domain.
  const auto tag = unhex(v18_tag_hex);
  EXPECT_THROW((void)open_object(key, scheme, id, sealed.nonce, std::span<const uint8_t, 16>(tag.data(), 16), sealed.ciphertext, plaintext.size()),
               std::runtime_error);
  EXPECT_THROW((void)open_object(key, v1, id, sealed.nonce, std::span<const uint8_t, 16>(tag.data(), 16), sealed.ciphertext, plaintext.size()),
               std::runtime_error);
}

// The binding test (stack document, security requirement a; decision 12): an
// object moved to another domain, or to another id, does not open.
TEST(StoreSealing, AnObjectMovedToAnotherDomainOrIdDoesNotOpen) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(3000, 9);
  const auto id = object_id(plain);
  const auto domain = domain::DomainId::parse(domain_hex).value();
  const auto sealed = seal_object(key, SealingScheme::v19(domain), id, plain);
  auto other_domain = domain;
  other_domain.bytes[15] ^= 1;
  EXPECT_THROW((void)open_object(key, SealingScheme::v19(other_domain), id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
  EXPECT_THROW((void)open_object(key, v1, id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
  auto other_id = id;
  other_id.bytes[0] ^= 1;
  EXPECT_THROW((void)open_object(key, SealingScheme::v19(domain), other_id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
  // Nor under the id alone, Macha's binding: the same key and bytes.
  EXPECT_THROW((void)dannan::identity::aes_gcm_open(key.bytes(), sealed.nonce, sealed.tag, sealed.ciphertext, id.bytes), std::runtime_error);
  EXPECT_EQ(open_object(key, SealingScheme::v19(domain), id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), plain);
}

// What a blind node verifies: SHA-256 over nonce, tag and ciphertext.
TEST(StoreSealing, TheBlindDigestIsOverTheSealedBytes) {
  const auto nonce = unhex(nonce_hex);
  const auto tag = unhex(v19_tag_hex);
  const auto ciphertext = unhex(ciphertext_hex);
  const auto digest = blind_digest(std::span<const uint8_t, 12>(nonce.data(), 12), std::span<const uint8_t, 16>(tag.data(), 16), ciphertext);
  EXPECT_EQ(hex(digest.bytes), "f064d825139550fb2b3cdf6da6a11fa6b01312a855d23760188d07fb512d1c24");
  auto other = ciphertext;
  other[0] ^= 1;
  EXPECT_NE(hex(blind_digest(std::span<const uint8_t, 12>(nonce.data(), 12), std::span<const uint8_t, 16>(tag.data(), 16), other).bytes), hex(digest.bytes));
}

// The primitive under both is standard AES-256-GCM with a 96-bit nonce: the
// GCM specification's test case 16 (McGrew and Viega).
TEST(StoreSealing, TheCipherIsStandardAes256Gcm) {
  const auto key = unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
  const auto plain = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
  const auto aad = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
  const auto iv = unhex("cafebabefacedbaddecaf888");
  const auto sealed = dannan::identity::aes_gcm_seal(std::span<const uint8_t, 32>(key.data(), 32), std::span<const uint8_t, 12>(iv.data(), 12), plain, aad);
  EXPECT_EQ(hex(sealed.ciphertext), "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
  EXPECT_EQ(hex(sealed.tag), "76fc6ece0f4e1768cddf8853bb2d551b");
}

}  // namespace
}  // namespace dagda::store
