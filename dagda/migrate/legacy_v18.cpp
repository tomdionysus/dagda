//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/migrate/legacy_v18.h"

#include <vector>

#include "dagda/store/sealing.h"
#include "dannan/identity/crypto.h"

namespace dagda::migrate {

std::optional<BackendMarkerV18> parse_backend_marker_v18(std::string_view text) {
  std::vector<std::string> fields;
  for (size_t from = 0; from <= text.size();) {
    const auto end = std::min(text.find(' ', from), text.size());
    fields.emplace_back(text.substr(from, end - from));
    from = end + 1;
  }
  if (fields.size() != 3 || fields[0] != backend_marker_v18) return std::nullopt;
  const auto node = dannan::identity::NodeId::parse(fields[1]);
  const auto token = dannan::identity::NodeId::parse(fields[2]);
  if (!node || !token) return std::nullopt;
  return BackendMarkerV18{*node, *token};
}

Bytes associated_data_v18(const ObjectId& id) { return Bytes(id.bytes.begin(), id.bytes.end()); }

Bytes open_object_v18(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag,
                      std::span<const uint8_t> ciphertext, uint64_t plain_size) {
  auto plain = dannan::identity::aes_gcm_open(key.bytes(), nonce, tag, ciphertext, associated_data_v18(id));
  if (plain.size() != plain_size || store::object_id(plain) != id) throw store::ObjectIntegrityError("object integrity failure");
  return plain;
}

}  // namespace dagda::migrate
