//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/domain/domain.h"

namespace dagda::domain {

const char* role_name(Role role) noexcept { return role == Role::member ? "member" : "blind"; }

Domain domain_v1_descriptor() { return {domain_v1, "macha", Role::member}; }

Domain node_local_descriptor(const dannan::identity::NodeId& node) { return {node_local_domain(node), "node-local", Role::member}; }

}  // namespace dagda::domain
