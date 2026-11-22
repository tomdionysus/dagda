//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

#include "dagda/domain/domain_id.h"
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

// The store has one layout, v19 (the maintainer, 2026-11-04 night: "we don't need
// backward compatibility"), for every domain, Macha's v1 included. A sealed
// object's associated data binds the domain as well as the id: seal_label_v19
// || domain id || id, so an object moved to another domain or another id
// does not open. Macha's v18 disks (the id alone as associated data) are
// migrated in place by dagda-migrate (migrate/migrate.h), never read here.
inline constexpr uint32_t layout_version = 19;
inline constexpr std::string_view seal_label_v19 = "dagda/seal/19";

struct SealingScheme {
  domain::DomainId domain = domain::domain_v1;

  static SealingScheme v19(const domain::DomainId& domain) noexcept { return {domain}; }
  bool operator==(const SealingScheme&) const = default;

  // The associated data of `id` under this scheme.
  Bytes associated_data(const ObjectId& id) const;
};

// Requirement D2, as Macha sealed every object, loose or packed: AES-256-GCM
// under the domain's storage key, a fresh random 12-byte nonce, a 16-byte
// tag, and the scheme's associated data, so a ciphertext cannot be passed
// off as another object's or another domain's. The id is the caller's;
// nothing here checks that it names the plaintext. Throws std::runtime_error
// if the cipher fails. Waits on nothing; thread-safe.
dannan::identity::SealedData seal_object(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<const uint8_t> plaintext);

// Opens what seal_object sealed and checks it is the object: the tag under
// the associated data, then the size recorded beside it, then that the
// plaintext hashes to the id. Throws std::runtime_error when authentication
// fails and ObjectIntegrityError when the size or the hash does not match.
// Waits on nothing; thread-safe.
Bytes open_object(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                  std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size);

// As seal_object and open_object, where the bytes lie, so an extent costs no
// second buffer of its size: seal_object_in_place leaves the ciphertext in
// `bytes` and returns its nonce and tag; open_object_in_place leaves the
// plaintext. A ciphertext that does not authenticate leaves `bytes` zeroed;
// one of another size than recorded is refused before anything is written.
// Throws as the two above. Waits on nothing; thread-safe.
dannan::identity::GcmSeal seal_object_in_place(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<uint8_t> bytes);
void open_object_in_place(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                          std::span<const uint8_t, 16> tag, std::span<uint8_t> bytes, uint64_t plain_size);

// What a blind node verifies (requirement D3; plan 116): the ciphertext
// digest, SHA-256 over nonce || tag || ciphertext, which a member computes
// when it seals and binds to the id where it directs a blind node to hold
// the object. A blind node holding the record recomputes it and compares.
// It proves the record is the bytes the member sealed; it cannot prove, with
// no key, that those bytes open to the plaintext the id names, nor check the
// tag or the domain binding, nor that the member was honest. Waits on
// nothing; thread-safe.
dannan::identity::Hash256 blind_digest(std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext);

}  // namespace dagda::store
