//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/sealing.h"

namespace dagda::store {

dannan::identity::SealedData seal_object(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t> plaintext) {
  return dannan::identity::aes_gcm_seal(key.bytes(), plaintext, id.bytes);
}

Bytes open_object(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag,
                  std::span<const uint8_t> ciphertext, uint64_t plain_size) {
  auto plain = dannan::identity::aes_gcm_open(key.bytes(), nonce, tag, ciphertext, id.bytes);
  if (plain.size() != plain_size || object_id(plain) != id) throw ObjectIntegrityError("object integrity failure");
  return plain;
}

}  // namespace dagda::store
