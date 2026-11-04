//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/domain/storage_key.h"

#include <stdexcept>
#include <utility>

#include "dannan/identity/crypto.h"

namespace dagda::domain {

StorageKey StorageKey::derive_v1(const dannan::identity::ClusterKey& cluster) {
  const std::span label(reinterpret_cast<const uint8_t*>(storage_key_label_v1.data()), storage_key_label_v1.size());
  return StorageKey(dannan::identity::hkdf_sha256_secret(cluster.master, {}, label));
}

std::optional<StorageKey> StorageKey::derive_node_local(const dannan::Identity& identity) {
  if (!identity.has_custody()) return std::nullopt;
  return StorageKey(identity.derive_local(node_local_label));
}

StorageKey::StorageKey(dannan::identity::Secret key) : _key(std::move(key)) {
  if (_key.size() != 32) throw std::invalid_argument("a storage key is 32 bytes");
}

}  // namespace dagda::domain
