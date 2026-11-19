//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A tag moved from one associated data to another with no second pass must be
// exactly what sealing under the other would give, at every size, because a
// disk's whole migration rests on it.

#include <gtest/gtest.h>

#include <vector>

#include "dagda/migrate/retag.h"
#include "dagda/store/sealing.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/identity/crypto.h"

namespace dagda::migrate {
namespace {

using dannan::identity::hex;
using store::Bytes;

// The GCM specification's test case 13: an all-zero 256-bit key.
TEST(MigrateRetag, TheHashKeyIsTheSpecificationsKnownAnswer) {
  std::array<uint8_t, 32> zero{};
  const domain::StorageKey key{dannan::identity::Secret(std::span<const uint8_t>(zero))};
  EXPECT_EQ(hex(TagDeriver(key).hash_key()), "dc95c078a2408989ad48a21492842087");
}

// Every ciphertext length around a block, a large one, and associated data of
// the two real shapes, shorter and longer than a block, in both directions.
TEST(MigrateRetag, ADerivedTagIsTheTagSealingGives) {
  const auto key = test::key_of(0x42);
  const TagDeriver deriver(*key);
  const auto id = store::object_id(test::pattern(32));
  const Bytes v18(id.bytes.begin(), id.bytes.end());
  const auto v19 = store::SealingScheme::v19(domain::domain_v1).associated_data(id);
  const std::vector<std::pair<Bytes, Bytes>> associated{
      {v18, v19}, {Bytes{}, v19}, {Bytes{1, 2, 3}, Bytes(16, 9)}, {Bytes(17, 1), Bytes(48, 2)}, {Bytes(40, 3), Bytes(40, 4)}};
  std::array<uint8_t, 12> nonce{};
  nonce[3] = 7;
  for (const size_t size : std::vector<size_t>{0, 1, 15, 16, 17, 31, 32, 33, 4096, 1024 * 1024 + 5}) {
    const auto plain = test::pattern(size, static_cast<uint8_t>(size));
    for (const auto& [a, b] : associated) {
      const auto under_a = dannan::identity::aes_gcm_seal(key->bytes(), nonce, plain, a);
      const auto under_b = dannan::identity::aes_gcm_seal(key->bytes(), nonce, plain, b);
      EXPECT_EQ(hex(deriver.derive(under_a.tag, a, b, under_a.ciphertext.size())), hex(under_b.tag)) << size;
      EXPECT_EQ(hex(deriver.derive(under_b.tag, b, a, under_b.ciphertext.size())), hex(under_a.tag)) << size;
    }
  }
}

// A record whose tag did not authenticate still does not: rot is not hidden.
TEST(MigrateRetag, ADamagedRecordStaysDamaged) {
  const auto key = test::key_of(0x42);
  const TagDeriver deriver(*key);
  const auto plain = test::pattern(4096);
  const Bytes a(32, 1);
  const Bytes b(49, 2);
  std::array<uint8_t, 12> nonce{};
  auto sealed = dannan::identity::aes_gcm_seal(key->bytes(), nonce, plain, a);
  sealed.ciphertext[100] ^= 1;
  const auto tag = deriver.derive(sealed.tag, a, b, sealed.ciphertext.size());
  EXPECT_THROW((void)dannan::identity::aes_gcm_open(key->bytes(), nonce, tag, sealed.ciphertext, b), std::runtime_error);
}

}  // namespace
}  // namespace dagda::migrate
