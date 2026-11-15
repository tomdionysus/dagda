//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>

#include "dannan/identity/node_id.h"

namespace dagda::domain {

// A domain's identity: sixteen bytes, bound into every sealed object of a
// layout v19 backend and written into its marker. Ordered, for maps.
struct DomainId {
  std::array<uint8_t, 16> bytes{};
  auto operator<=>(const DomainId&) const = default;

  // Thirty-two hex digits, either case; nothing on anything else.
  static std::optional<DomainId> parse(const std::string& text);
};

// Thirty-two lowercase hex digits.
std::string to_string(const DomainId&);

// Domain v1 is Macha's one domain, every node a member, its key from the
// cluster key (storage_key.h). Its id is fixed and well known: the first
// sixteen bytes of SHA-256("dagda/domain/v1"). Nothing on a v18 disk carries
// it; it names the domain in memory and on the wire only.
inline constexpr DomainId domain_v1{{0x82, 0x7b, 0x07, 0x10, 0x1d, 0x79, 0xd9, 0xd8, 0x4a, 0x43, 0x3d, 0x8e, 0x9c, 0xb2, 0x62, 0xca}};

// A node's node-local domain (requirement D4): the first sixteen bytes of
// SHA-256("dagda/node-local/1" || node id), so each node's is its own and a
// node-local disk names whose it is. Waits on nothing; thread-safe.
DomainId node_local_domain(const dannan::identity::NodeId& node);

}  // namespace dagda::domain
