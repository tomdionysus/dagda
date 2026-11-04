//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dannan/identity/node_id.h"
#include "dannan/membership/node_record.h"
#include "dannan/placement/placement.h"

// How the store places DATA: Dannan's capacity placement under the store's
// own scheme (requirements S10, S25). The salts are Macha's
// (cluster/placement.cpp:266, :377), so an object ranks the backends of a
// pool, and later the nodes of a cluster, exactly as Macha ranked them and
// what Macha placed stays where it is.
namespace dagda::store {

// Part of the bytes of every placement decision: never change them.
inline constexpr const char* data_fallback_salt = "macha/data-fallback/v7";
inline constexpr const char* data_extra_domain_salt = "macha/data-extra-domain/v7";
// The advert holding a node's DATA capacity as a u64 (NodeRecord::advert_u64).
// Within a pool it is a backend's limit; across nodes the store publishes it
// at step 3.
inline constexpr const char* capacity_advert = "dagda/capacity";

// The scheme DATA is placed under. Waits on nothing; thread-safe.
const dannan::placement::Scheme& data_scheme();

// A placement candidate: a node, or a backend of a pool named by its disk's
// identity token, with `capacity` under capacity_advert.
dannan::membership::NodeRecord placement_candidate(const dannan::identity::NodeId& id, uint64_t capacity, std::string failure_domain = {});

// The candidates in DATA order for `key`: the first `count` the preferred
// owners, the rest deterministic fallbacks. With count 1, as a pool ranks its
// backends, weighted rendezvous over the key's shard.
std::vector<dannan::identity::NodeId> data_order(std::span<const uint8_t> key, const std::vector<dannan::membership::NodeRecord>& candidates, size_t count);

}  // namespace dagda::store
