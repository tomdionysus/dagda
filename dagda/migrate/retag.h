//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "dagda/domain/storage_key.h"

// A GCM tag moved from one associated data to another with no second pass over
// the object. GHASH is linear in its blocks and a ciphertext block's term
// depends only on its distance from the end, so over one ciphertext two tags
// differ by the associated data's blocks and the length block alone:
//
//   tag(B) = tag(A) xor (Y(A) xor Y(B)) * H^(c + 1) xor (L(A) xor L(B)) * H
//
// where Y is GHASH's Horner over the padded associated data, c the ciphertext's
// block count and L the length block. No plaintext or ciphertext is touched,
// and a tag that did not authenticate under A will not under B: this never
// makes a damaged record look whole.
namespace dagda::migrate {

class TagDeriver {
 public:
  using Tag = std::array<uint8_t, 16>;

  // H comes from two AES-GCM tags under `key` (Dannan's primitive; Dagda holds
  // no raw AES), whose difference is H squared. Throws std::runtime_error if
  // the cipher fails. Waits on nothing.
  explicit TagDeriver(const domain::StorageKey& key);

  // The tag of the same nonce and ciphertext under `to`, given its tag under
  // `from`. Waits on nothing; thread-safe.
  Tag derive(std::span<const uint8_t, 16> tag, std::span<const uint8_t> from, std::span<const uint8_t> to, uint64_t ciphertext_size) const;

  // GCM's hash key, for the known answer.
  Tag hash_key() const noexcept;

 private:
  uint64_t _h_high = 0;
  uint64_t _h_low = 0;
};

}  // namespace dagda::migrate
