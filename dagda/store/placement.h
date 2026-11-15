//
// Dagda - The Distributed Object Store
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
// Within a pool it is a backend's limit; across nodes it is the node's pool
// limit, which its storage server publishes.
inline constexpr const char* capacity_advert = "dagda/capacity";
// Present on a node that hosts DATA extents (Macha's node_flag_hosts_extents,
// types.hpp:49): only such nodes are placement candidates (requirement S25).
inline constexpr const char* extents_advert = "dagda/extents";
// The DATA bytes a node holds, as a u64: with its capacity, whether it has
// room for one more extent (Macha's NodeInfo::used).
inline constexpr const char* used_advert = "dagda/used";
// How many of a node's DATA disks are online, as a u64, refreshed with its storage
// (Macha's storage_backends_online, storage_server.cpp:56-57 at f0597a4).
inline constexpr const char* backends_online_advert = "dagda/backends-online";
// The node's cache, each a u64, refreshed with its storage (Macha's advertise_cache,
// storage_server.cpp:62-65 at f0597a4): capacity and use in bytes, and its hits,
// misses and evictions since start.
inline constexpr const char* cache_capacity_advert = "dagda/cache-capacity";
inline constexpr const char* cache_used_advert = "dagda/cache-used";
inline constexpr const char* cache_hits_advert = "dagda/cache-hits";
inline constexpr const char* cache_misses_advert = "dagda/cache-misses";
inline constexpr const char* cache_evictions_advert = "dagda/cache-evictions";

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
