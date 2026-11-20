//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "dagda/domain/storage_key.h"
#include "dagda/store/object_id.h"
#include "dannan/identity/node_id.h"
#include "dannan/identity/secure_memory.h"

// What the fuzz targets' seeds are made of: fixed ids, nodes and one key, so every run
// mutates the same inputs.
namespace dagda::fuzz {

inline store::ObjectId sample_id(uint8_t fill) {
  store::ObjectId id;
  id.bytes.fill(fill);
  return id;
}

inline dannan::identity::NodeId sample_node(uint8_t fill) {
  dannan::identity::NodeId node;
  node.bytes.fill(fill);
  return node;
}

// The key every target that opens a seal opens under, made once: it lives on secure pages.
inline const domain::StorageKey& sample_key() {
  static const domain::StorageKey key = [] {
    std::array<uint8_t, 32> bytes{};
    bytes.fill(0x5a);
    return domain::StorageKey(dannan::identity::Secret(std::span<const uint8_t>(bytes)));
  }();
  return key;
}

}  // namespace dagda::fuzz
