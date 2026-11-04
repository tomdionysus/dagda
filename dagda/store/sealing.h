//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>

#include "dagda/domain/storage_key.h"
#include "dagda/store/object_id.h"
#include "dannan/identity/crypto.h"

namespace dagda::store {

// Opened bytes that are not the object they were stored as: a plaintext of
// another size than recorded, or one whose SHA-256 is not the id.
class ObjectIntegrityError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Requirement D2, as Macha seals every object, loose or packed: AES-256-GCM
// under the domain's storage key, a fresh random 12-byte nonce, a 16-byte
// tag, and the 32-byte id as the associated data, so a ciphertext cannot be
// passed off as another object's. The id is the caller's; nothing here
// checks that it names the plaintext. Throws std::runtime_error if the
// cipher fails. Waits on nothing; thread-safe.
dannan::identity::SealedData seal_object(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t> plaintext);

// Opens what seal_object sealed and checks it is the object: the tag under
// the id, then the size recorded beside it, then that the plaintext hashes
// to the id. Throws std::runtime_error when authentication fails and
// ObjectIntegrityError when the size or the hash does not match. Waits on
// nothing; thread-safe.
Bytes open_object(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag,
                  std::span<const uint8_t> ciphertext, uint64_t plain_size);

}  // namespace dagda::store
