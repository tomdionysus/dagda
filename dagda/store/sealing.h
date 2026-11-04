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

// What a sealed object's associated data binds it to. Layout v18 is Macha's
// (requirement D2): the 32-byte id alone, so v18 disks keep their bytes for
// ever. Layout v19 (the maintainer, 2026-11-04 evening; stack document, security
// requirement a) binds the domain too: seal_label_v19 || domain id || id, so
// an object moved to another domain or another id does not open.
inline constexpr std::string_view seal_label_v19 = "dagda/seal/19";

struct SealingScheme {
  uint32_t layout = 18;
  domain::DomainId domain{};

  static SealingScheme v18() noexcept { return {}; }
  static SealingScheme v19(const domain::DomainId& domain) noexcept { return {19, domain}; }
  bool operator==(const SealingScheme&) const = default;

  // The associated data of `id` under this scheme.
  Bytes associated_data(const ObjectId& id) const;
};

// Requirement D2, as Macha seals every object, loose or packed: AES-256-GCM
// under the domain's storage key, a fresh random 12-byte nonce, a 16-byte
// tag, and the scheme's associated data, so a ciphertext cannot be passed
// off as another object's (or, from v19, another domain's). The id is the
// caller's; nothing here checks that it names the plaintext. Throws
// std::runtime_error if the cipher fails. Waits on nothing; thread-safe.
dannan::identity::SealedData seal_object(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<const uint8_t> plaintext);
// Layout v18's form.
dannan::identity::SealedData seal_object(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t> plaintext);

// Opens what seal_object sealed and checks it is the object: the tag under
// the associated data, then the size recorded beside it, then that the
// plaintext hashes to the id. Throws std::runtime_error when authentication
// fails and ObjectIntegrityError when the size or the hash does not match.
// Waits on nothing; thread-safe.
Bytes open_object(const domain::StorageKey& key, const SealingScheme& scheme, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                  std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size);
// Layout v18's form.
Bytes open_object(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag,
                  std::span<const uint8_t> ciphertext, uint64_t plain_size);

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
