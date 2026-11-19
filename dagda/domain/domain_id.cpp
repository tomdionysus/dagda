//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/domain/domain_id.h"

#include <algorithm>
#include <string_view>

#include "dannan/identity/crypto.h"

namespace dagda::domain {

std::optional<DomainId> DomainId::parse(const std::string& text) {
  const auto bytes = dannan::identity::unhex(text);
  if (!bytes || bytes->size() != 16) return std::nullopt;
  DomainId out;
  std::copy(bytes->begin(), bytes->end(), out.bytes.begin());
  return out;
}

std::string to_string(const DomainId& id) { return dannan::identity::hex(id.bytes); }

DomainId node_local_domain(const dannan::identity::NodeId& node) {
  constexpr std::string_view label = "dagda/node-local/1";
  dannan::identity::Sha256Hasher hasher;
  hasher.update({reinterpret_cast<const uint8_t*>(label.data()), label.size()});
  hasher.update(node.bytes);
  const auto hash = hasher.finish();
  DomainId out;
  std::copy_n(hash.bytes.begin(), 16, out.bytes.begin());
  return out;
}

}  // namespace dagda::domain
