//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/sealing.h"

#include "dannan/net/codec.h"

namespace dagda::store {

Bytes SealingScheme::associated_data(const ObjectId& id) const {
  dannan::net::Writer out;
  out.raw({reinterpret_cast<const uint8_t*>(seal_label_v19.data()), seal_label_v19.size()});
  out.fixed(domain.bytes);
  out.fixed(id.bytes);
  return out.take();
}

dannan::identity::SealedData seal_object(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<const uint8_t> plaintext) {
  return dannan::identity::aes_gcm_seal(key.bytes(), plaintext, scheme.associated_data(id));
}

Bytes open_object(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                  std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size) {
  auto plain = dannan::identity::aes_gcm_open(key.bytes(), nonce, tag, ciphertext, scheme.associated_data(id));
  if (plain.size() != plain_size || object_id(plain) != id) throw ObjectIntegrityError("object integrity failure");
  return plain;
}

dannan::identity::GcmSeal seal_object_in_place(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<uint8_t> bytes) {
  return dannan::identity::aes_gcm_seal_into(key.bytes(), bytes, bytes, scheme.associated_data(id));
}

void open_object_in_place(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                          std::span<const uint8_t, 16> tag, std::span<uint8_t> bytes, uint64_t plain_size) {
  if (bytes.size() != plain_size) throw ObjectIntegrityError("object integrity failure");
  dannan::identity::aes_gcm_open_into(key.bytes(), nonce, tag, bytes, bytes, scheme.associated_data(id));
  if (object_id(bytes) != id) throw ObjectIntegrityError("object integrity failure");
}

dannan::identity::Hash256 blind_digest(std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext) {
  dannan::identity::Sha256Hasher hasher;
  hasher.update(nonce);
  hasher.update(tag);
  hasher.update(ciphertext);
  return hasher.finish();
}

}  // namespace dagda::store
