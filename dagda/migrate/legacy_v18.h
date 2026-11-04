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
#include <string>
#include <string_view>

#include "dagda/domain/storage_key.h"
#include "dagda/store/object_id.h"
#include "dannan/identity/node_id.h"

// What is left of layout v18, Macha's, read here and nowhere else (the maintainer,
// 2026-11-04 night: the store knows v19 only). Its files are the one
// layout's (store/local_format.h); what differs is the markers, and that a
// sealed object's associated data was the id alone. The ledgers were sealed
// under the domain's key where v19 seals them under the node's own.
namespace dagda::migrate {

using store::Bytes;
using store::ObjectId;

inline constexpr std::string_view state_layout_v18 = "macha-state-layout-v18";
inline constexpr std::string_view backend_marker_v18 = "macha-backend-v18";

// A v18 backend marker: "macha-backend-v18 <node> <token>".
struct BackendMarkerV18 {
  dannan::identity::NodeId node;
  dannan::identity::NodeId token;
};
// Nothing for text that is not one. Waits on nothing.
std::optional<BackendMarkerV18> parse_backend_marker_v18(std::string_view text);

// A v18 object's associated data: the 32-byte id (requirement D2 as Macha
// had it). Waits on nothing.
Bytes associated_data_v18(const ObjectId& id);

// Opens a v18 record as Macha's store did: the tag under the id, then the
// size recorded beside it, then that the plaintext hashes to the id. Throws
// std::runtime_error when authentication fails and store::ObjectIntegrityError
// when the size or the hash does not match. Waits on nothing; thread-safe.
Bytes open_object_v18(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag,
                      std::span<const uint8_t> ciphertext, uint64_t plain_size);

}  // namespace dagda::migrate
