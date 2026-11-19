//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string>

#include "dagda/domain/domain_id.h"
#include "dannan/identity/node_id.h"

namespace dagda::domain {

// What this node is to a domain (requirement D3; plan 106-124). A member
// holds the key: it seals, opens, reads and serves. A blind node holds
// ciphertext only, when directed: it counts toward durability and verifies
// what it holds by the ciphertext digest a member bound to the id
// (store/sealing.h), never by opening it. A node that is neither holds
// nothing of the domain and needs no role.
enum class Role : uint8_t { member, blind };
const char* role_name(Role) noexcept;

// A domain as a store is told of it: its id, the name its writes are scoped
// under by default (the prefix the write floor checks, the maintainer 2026-11-04), and
// this node's role in it. The catalogue of domains is a head space (step 4);
// until then a node is handed each domain it holds.
struct Domain {
  DomainId id{};
  std::string name;
  Role role = Role::member;
};

// Domain v1: Macha's one domain, every node a member, named "macha".
Domain domain_v1_descriptor();
// This node's node-local domain: its own, never wrapped to another node,
// named "node-local".
Domain node_local_descriptor(const dannan::identity::NodeId& node);

}  // namespace dagda::domain
