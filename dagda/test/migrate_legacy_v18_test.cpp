//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Layout v18's sealing, Macha's, read by the migration alone (the maintainer,
// 2026-11-04 night). The known answers are step 1a's, computed outside both
// codebases (store_sealing_test had them while the store opened v18): the
// cases moved here are OpensWhatMachaSealed, SealsMachasBytesUnderTheSameNonce
// and TheEmptyObjectSealsToATagAlone (its v18 tag), every assertion kept, and
// the v18 forms of AnotherIdDoesNotOpen and OpenChecksTheSizeAndTheId, whose
// v19 forms stayed with the store.

#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dagda/domain/storage_key.h"
#include "dagda/migrate/legacy_v18.h"
#include "dagda/store/object_id.h"
#include "dagda/store/sealing.h"
#include "dagda/test/fixtures/legacy_v18.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::migrate {
namespace {

using dannan::identity::hex;

Bytes unhex(const std::string& text) { return dannan::identity::unhex(text).value(); }

Bytes bytes_of(std::string_view text) { return Bytes(text.begin(), text.end()); }

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

constexpr std::string_view plaintext = "An object Macha sealed under its own id.";
constexpr char object_hex[] = "3e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8";
constexpr char nonce_hex[] = "000102030405060708090a0b";
constexpr char ciphertext_hex[] = "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
constexpr char tag_hex[] = "6f6aa83135d5e163f1b04d0c9ec0758c";

// Macha: OpensWhatMachaSealed. Bytes Macha sealed open here, with the size
// and the id checked.
TEST(MigrateLegacyV18, OpensWhatMachaSealed) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = store::object_id(bytes_of(plaintext));
  ASSERT_EQ(store::to_string(id), object_hex);
  const auto nonce = unhex(nonce_hex);
  const auto tag = unhex(tag_hex);
  const auto ciphertext = unhex(ciphertext_hex);
  const auto opened =
      open_object_v18(key, id, std::span<const uint8_t, 12>(nonce.data(), 12), std::span<const uint8_t, 16>(tag.data(), 16), ciphertext, plaintext.size());
  EXPECT_EQ(opened, bytes_of(plaintext));
  EXPECT_EQ(hex(associated_data_v18(id)), object_hex);
}

// Macha: SealsMachasBytesUnderTheSameNonce. Under the same nonce the v18
// sealing produces Macha's bytes exactly: AES-256-GCM with the id as the
// associated data.
TEST(MigrateLegacyV18, SealsMachasBytesUnderTheSameNonce) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = store::object_id(bytes_of(plaintext));
  const auto nonce = unhex(nonce_hex);
  std::array<uint8_t, 12> fixed{};
  std::copy(nonce.begin(), nonce.end(), fixed.begin());
  const auto sealed = test::legacy::seal_v18(key, id, bytes_of(plaintext), fixed);
  EXPECT_EQ(hex(sealed.ciphertext), ciphertext_hex);
  EXPECT_EQ(hex(sealed.tag), tag_hex);
}

// A v18 ciphertext cannot be passed off as another object's either: the id
// is authenticated.
TEST(MigrateLegacyV18, AnotherIdDoesNotOpen) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(1000, 4);
  const auto id = store::object_id(plain);
  const auto sealed = test::legacy::seal_v18(key, id, plain);
  auto other = id;
  other.bytes[31] ^= 1;
  EXPECT_THROW((void)open_object_v18(key, other, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), std::runtime_error);
  EXPECT_EQ(open_object_v18(key, id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()), plain);
  // Nor does it open as a v19 object of any domain.
  EXPECT_THROW((void)store::open_object(key, store::SealingScheme::v19(domain::domain_v1), id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size()),
               std::runtime_error);
}

// Authentic v18 bytes that are not the object recorded are refused.
TEST(MigrateLegacyV18, OpenChecksTheSizeAndTheId) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto plain = pattern(1000, 5);
  const auto id = store::object_id(plain);
  const auto sealed = test::legacy::seal_v18(key, id, plain);
  EXPECT_THROW((void)open_object_v18(key, id, sealed.nonce, sealed.tag, sealed.ciphertext, plain.size() + 1), store::ObjectIntegrityError);
  const auto stranger = store::object_id(pattern(1000, 6));
  const auto misnamed = test::legacy::seal_v18(key, stranger, plain);
  EXPECT_THROW((void)open_object_v18(key, stranger, misnamed.nonce, misnamed.tag, misnamed.ciphertext, plain.size()), store::ObjectIntegrityError);
}

// Macha: TheEmptyObjectSealsToATagAlone, its v18 tag.
TEST(MigrateLegacyV18, TheEmptyObjectSealsToATagAlone) {
  dannan::test::TempDir dir;
  const auto key = domain::StorageKey::derive_v1(cluster_key(dir));
  const auto id = store::object_id({});
  const auto nonce = unhex(nonce_hex);
  std::array<uint8_t, 12> fixed{};
  std::copy(nonce.begin(), nonce.end(), fixed.begin());
  const auto sealed = test::legacy::seal_v18(key, id, Bytes{}, fixed);
  EXPECT_TRUE(sealed.ciphertext.empty());
  EXPECT_EQ(hex(sealed.tag), "ead2fafb66dc09be7023213356c8d91f");
  EXPECT_TRUE(open_object_v18(key, id, sealed.nonce, sealed.tag, sealed.ciphertext, 0).empty());
}

// Macha's backend marker, byte for byte, is parsed and nothing else is.
TEST(MigrateLegacyV18, ParsesMachasBackendMarker) {
  const auto marker = parse_backend_marker_v18("macha-backend-v18 abababababababababababababababab cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd");
  ASSERT_TRUE(marker.has_value());
  EXPECT_EQ(dannan::identity::to_string(marker->node), "abababababababababababababababab");
  EXPECT_EQ(dannan::identity::to_string(marker->token), "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd");
  EXPECT_FALSE(parse_backend_marker_v18("macha-backend-v17 abababababababababababababababab cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd").has_value());
  EXPECT_FALSE(parse_backend_marker_v18("macha-backend-v18 abababababababababababababababab").has_value());
  EXPECT_FALSE(parse_backend_marker_v18("dagda-backend-v19 abababababababababababababababab cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd "
                                        "827b07101d79d9d84a433d8e9cb262ca 96fe387af04a9cdb77b3cd0086f4b356")
                   .has_value());
  EXPECT_EQ(state_layout_v18, "macha-state-layout-v18");
}

}  // namespace
}  // namespace dagda::migrate
