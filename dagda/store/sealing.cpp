//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/sealing.h"

#include "dannan/net/codec.h"

namespace dagda::store {

Bytes SealingScheme::associated_data(const ObjectId& id) const {
  if (layout == 18) return Bytes(id.bytes.begin(), id.bytes.end());
  dannan::net::Writer out;
  out.raw({reinterpret_cast<const uint8_t*>(seal_label_v19.data()), seal_label_v19.size()});
  out.fixed(domain.bytes);
  out.fixed(id.bytes);
  return out.take();
}

dannan::identity::SealedData seal_object(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<const uint8_t> plaintext) {
  return dannan::identity::aes_gcm_seal(key.bytes(), plaintext, scheme.associated_data(id));
}

dannan::identity::SealedData seal_object(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t> plaintext) {
  return seal_object(key, SealingScheme::v18(), id, plaintext);
}

Bytes open_object(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                  std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size) {
  auto plain = dannan::identity::aes_gcm_open(key.bytes(), nonce, tag, ciphertext, scheme.associated_data(id));
  if (plain.size() != plain_size || object_id(plain) != id) throw ObjectIntegrityError("object integrity failure");
  return plain;
}

Bytes open_object(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag,
                  std::span<const uint8_t> ciphertext, uint64_t plain_size) {
  return open_object(key, SealingScheme::v18(), id, nonce, tag, ciphertext, plain_size);
}

dannan::identity::Hash256 blind_digest(std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext) {
  dannan::identity::Sha256Hasher hasher;
  hasher.update(nonce);
  hasher.update(tag);
  hasher.update(ciphertext);
  return hasher.finish();
}

}  // namespace dagda::store
