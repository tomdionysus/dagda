//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "dannan/context.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/key_custody.h"
#include "dannan/identity/secure_memory.h"

namespace dagda::domain {

// Domain v1 is Macha's one domain, every node a member (requirement D1). Its
// storage key is derived exactly as Macha derives it, so Macha's objects open
// under it once their tags are migrated: HKDF-SHA256 over the cluster master
// (the SHA-256 of the cluster key file, trailing CR and LF removed), no salt,
// this label.
inline constexpr std::string_view storage_key_label_v1 = "macha/storage/v1";

// A node's node-local key (requirement D4) is derived from its key custody
// under this label (Identity::derive_local, KeyCustody::derive): it exists
// only on that node and is never wrapped to another. Every ledger a node
// keeps of its own (held tries, claims, reference counts) is sealed under it
// (the maintainer, 2026-11-04 night: "node-local key, one rule"), so a blind node needs
// nothing it does not have.
inline constexpr std::string_view node_local_label = "dagda/node-local/1";

// The key a domain's objects are sealed under, held in secure memory.
// Move-only; read-only once made, so any thread may read it.
class StorageKey {
 public:
  // Domain v1's key from the cluster key. Throws std::runtime_error if the
  // secure pages cannot be mapped. Waits on nothing.
  static StorageKey derive_v1(const dannan::identity::ClusterKey& cluster);

  // This node's node-local key, from its custody; nothing on a node without
  // custody (it is reported absent, never made up). Waits on nothing.
  static std::optional<StorageKey> derive_node_local(const dannan::Identity& identity);
  // The same key from custody opened outside a node (dagda-migrate).
  static StorageKey derive_node_local(const dannan::identity::KeyCustody& custody);
  // The node-local key, or std::runtime_error saying the node has no custody
  // and so cannot run a store: the ledgers are never sealed under any other
  // key.
  static StorageKey require_node_local(const dannan::Identity& identity);

  // A key already derived. Throws std::invalid_argument unless 32 bytes.
  explicit StorageKey(dannan::identity::Secret key);

  std::span<const uint8_t, 32> bytes() const noexcept { return std::span<const uint8_t, 32>(_key.reveal().data(), 32); }

 private:
  dannan::identity::Secret _key;
};

}  // namespace dagda::domain
