//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// An object is sealed as Macha seals it (requirement D2), so what Macha wrote
// opens here. The known answers were computed outside both codebases: the
// keystream and block encryptions by `openssl enc` (AES-256-CTR and ECB),
// GHASH by hand, the whole checked first against the GCM specification's
// test case 16.

#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dagda/domain/domain_id.h"
#include "dagda/domain/storage_key.h"
#include "dagda/store/object_id.h"
#include "dagda/store/sealing.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::store {
namespace {

using dannan::identity::hex;

Bytes unhex(const std::string& text) { return dannan::identity::unhex(text).value(); }

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

// The known-answer object.
constexpr std::string_view plaintext = "An object Macha sealed under its own id.";
constexpr char object_hex[] = "3e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8";
constexpr char nonce_hex[] = "000102030405060708090a0b";
constexpr char ciphertext_hex[] = "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
constexpr char tag_hex[] = "6f6aa83135d5e163f1b04d0c9ec0758c";

// Bytes Macha sealed open here, with the size and the id checked.
TEST(StoreSealing, OpensWhatMachaSealed) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = object_id(bytes_of(plaintext));
  ASSERT_EQ(to_string(id), object_hex);
  const auto nonce = unhex(nonce_hex);
  const auto tag = unhex(tag_hex);
  const auto ciphertext = unhex(ciphertext_hex);
  const auto opened =
      open_object(key, id, std::span<const uint8_t, 12>(nonce.data(), 12), std::span<const uint8_t, 16>(tag.data(), 16), ciphertext, plaintext.size());
  EXPECT_EQ(opened, bytes_of(plaintext));
}

// Under the same nonce Dagda's sealing produces Macha's bytes exactly: the
// AES-256-GCM of Dannan with the id as the associated data.
TEST(StoreSealing, SealsMachasBytesUnderTheSameNonce) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = object_id(bytes_of(plaintext));
  const auto nonce = unhex(nonce_hex);
  const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), std::span<const uint8_t, 12>(nonce.data(), 12), bytes_of(plaintext), id.bytes);
  EXPECT_EQ(hex(sealed.ciphertext), ciphertext_hex);
  EXPECT_EQ(hex(sealed.tag), tag_hex);
}

// What seal_object writes is that layout: the plaintext's size of
// ciphertext, a fresh nonce, and a tag over the id.
TEST(StoreSealing, SealObjectIsAesGcmWithTheIdAsAssociatedData) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(4096, 3);
  const auto id = object_id(plain);
  const auto first = seal_object(key, id, plain);
  const auto second = seal_object(key, id, plain);
  EXPECT_EQ(first.ciphertext.size(), plain.size());
  EXPECT_NE(first.nonce, second.nonce);
  EXPECT_EQ(dannan::identity::aes_gcm_open(key.bytes(), first.nonce, first.tag, first.ciphertext, id.bytes), plain);
  EXPECT_THROW((void)dannan::identity::aes_gcm_open(key.bytes(), first.nonce, first.tag, first.ciphertext), std::runtime_error);
}

// A ciphertext cannot be passed off as another object's: the id is
// authenticated.
TEST(StoreSealing, AnotherIdDoesNotOpen) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(1000, 4);
  const auto id = object_id(plain);
  const auto sealed = seal_object(key, id, plain);
  auto other = id;
  other.bytes[31] ^= 1;
  EXPECT_THROW((void)open_object(key, other, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
  EXPECT_EQ(open_object(key, id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), plain);
}

// Authentic bytes that are not the object recorded are refused: another
// size than recorded, or a plaintext that is not the id's.
TEST(StoreSealing, OpenChecksTheSizeAndTheId) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(1000, 5);
  const auto id = object_id(plain);
  const auto sealed = seal_object(key, id, plain);
  EXPECT_THROW((void)open_object(key, id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size() + 1), ObjectIntegrityError);

  // Sealed under an id that does not name it.
  const auto stranger = object_id(pattern(1000, 6));
  const auto misnamed = seal_object(key, stranger, plain);
  EXPECT_THROW((void)open_object(key, stranger, misnamed.nonce, misnamed.tag, misnamed.ciphertext, plain.size()), ObjectIntegrityError);
}

// Another domain's key opens nothing.
TEST(StoreSealing, AnotherKeyDoesNotOpen) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto other = domain::StorageKey::derive_v1(cluster_key(dir, "another cluster"));
  const auto plain = pattern(64, 7);
  const auto id = object_id(plain);
  const auto sealed = seal_object(key, id, plain);
  EXPECT_THROW((void)open_object(other, id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
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

// The empty object seals to no ciphertext and a tag over its id alone.
TEST(StoreSealing, TheEmptyObjectSealsToATagAlone) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = object_id({});
  const auto nonce = unhex(nonce_hex);
  const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), std::span<const uint8_t, 12>(nonce.data(), 12), Bytes{}, id.bytes);
  EXPECT_TRUE(sealed.ciphertext.empty());
  EXPECT_EQ(hex(sealed.tag), "ead2fafb66dc09be7023213356c8d91f");
  EXPECT_TRUE(open_object(key, id, sealed.nonce, sealed.tag, sealed.ciphertext, 0).empty());
}

// ---- Layout v19 (step 6) -----------------------------------------------------
// The known answers were computed as the wrap's were (domain_wrap_test):
// AES blocks by `openssl enc -aes-256-ecb`, GCM by hand, checked against the
// GCM specification's test case 16.

constexpr char domain_hex[] = "00112233445566778899aabbccddeeff";
constexpr char v19_tag_hex[] = "d17467ebe8e66d7404b39ffbb2e67868";

// Under v19 the associated data is the label, the domain and the id. The
// ciphertext is the keystream's, the same as v18's under the same key and
// nonce; the tag is where the binding lives, and it differs.
TEST(StoreSealing, LayoutV19BindsTheDomainIntoTheAssociatedData) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = object_id(bytes_of(plaintext));
  const auto domain = domain::DomainId::parse(domain_hex).value();
  const auto scheme = SealingScheme::v19(domain);
  EXPECT_EQ(hex(scheme.associated_data(id)), std::string("64616764612f7365616c2f3139") + domain_hex + object_hex);
  EXPECT_EQ(hex(SealingScheme::v18().associated_data(id)), object_hex);
  const auto nonce = unhex(nonce_hex);
  const auto sealed =
      dannan::identity::aes_gcm_seal(key.bytes(), std::span<const uint8_t, 12>(nonce.data(), 12), bytes_of(plaintext), scheme.associated_data(id));
  EXPECT_EQ(hex(sealed.ciphertext), ciphertext_hex);
  EXPECT_EQ(hex(sealed.tag), v19_tag_hex);
  EXPECT_NE(std::string(v19_tag_hex), tag_hex);
  EXPECT_EQ(open_object(key, scheme, id, sealed.nonce, sealed.tag, sealed.ciphertext, plaintext.size()), bytes_of(plaintext));
  // v18's bytes still open as v18, and not as v19.
  const auto tag = unhex(tag_hex);
  EXPECT_EQ(open_object(key, id, sealed.nonce, std::span<const uint8_t, 16>(tag.data(), 16), sealed.ciphertext, plaintext.size()), bytes_of(plaintext));
  EXPECT_THROW((void)open_object(key, scheme, id, sealed.nonce, std::span<const uint8_t, 16>(tag.data(), 16), sealed.ciphertext, plaintext.size()),
               std::runtime_error);
}

// The binding test (stack document, security requirement a; decision 12): a
// v19 object moved to another domain, or to another id, does not open.
TEST(StoreSealing, AV19ObjectMovedToAnotherDomainOrIdDoesNotOpen) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(3000, 9);
  const auto id = object_id(plain);
  const auto domain = domain::DomainId::parse(domain_hex).value();
  const auto sealed = seal_object(key, SealingScheme::v19(domain), id, plain);
  auto other_domain = domain;
  other_domain.bytes[15] ^= 1;
  EXPECT_THROW((void)open_object(key, SealingScheme::v19(other_domain), id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
  EXPECT_THROW((void)open_object(key, SealingScheme::v19(domain::domain_v1), id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()),
               std::runtime_error);
  auto other_id = id;
  other_id.bytes[0] ^= 1;
  EXPECT_THROW((void)open_object(key, SealingScheme::v19(domain), other_id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
  // Nor under v18's associated data: the same key and bytes, another binding.
  EXPECT_THROW((void)open_object(key, id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
  EXPECT_EQ(open_object(key, SealingScheme::v19(domain), id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), plain);
}

// What a blind node verifies: SHA-256 over nonce, tag and ciphertext.
TEST(StoreSealing, TheBlindDigestIsOverTheSealedBytes) {
  const auto nonce = unhex(nonce_hex);
  const auto tag = unhex(v19_tag_hex);
  const auto ciphertext = unhex(ciphertext_hex);
  const auto digest = blind_digest(std::span<const uint8_t, 12>(nonce.data(), 12), std::span<const uint8_t, 16>(tag.data(), 16), ciphertext);
  EXPECT_EQ(hex(digest), "f064d825139550fb2b3cdf6da6a11fa6b01312a855d23760188d07fb512d1c24");
  auto other = ciphertext;
  other[0] ^= 1;
  EXPECT_NE(hex(blind_digest(std::span<const uint8_t, 12>(nonce.data(), 12), std::span<const uint8_t, 16>(tag.data(), 16), other)), hex(digest));
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
