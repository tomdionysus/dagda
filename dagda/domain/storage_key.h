//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "dannan/identity/cluster_key.h"
#include "dannan/identity/secure_memory.h"

namespace dagda::domain {

// Domain v1 is Macha's one domain, every node a member (requirement D1). Its
// storage key is derived exactly as Macha derives it, so layout v18 state
// opens unchanged: HKDF-SHA256 over the cluster master (the SHA-256 of the
// cluster key file, trailing CR and LF removed), no salt, this label.
inline constexpr std::string_view storage_key_label_v1 = "macha/storage/v1";

// The key a domain's objects are sealed under, held in secure memory.
// Move-only; read-only once made, so any thread may read it.
class StorageKey {
 public:
  // Domain v1's key from the cluster key. Throws std::runtime_error if the
  // secure pages cannot be mapped. Waits on nothing.
  static StorageKey derive_v1(const dannan::identity::ClusterKey& cluster);

  // A key already derived. Throws std::invalid_argument unless 32 bytes.
  explicit StorageKey(dannan::identity::Secret key);

  std::span<const uint8_t, 32> bytes() const noexcept { return std::span<const uint8_t, 32>(_key.reveal().data(), 32); }

 private:
  dannan::identity::Secret _key;
};

}  // namespace dagda::domain
