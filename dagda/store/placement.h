//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
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
// The store's figures, refreshed with its storage, in one advert (the maintainer, 2026-11-15:
// a roster record carries every advert through every copy, so the figures that
// nothing places on travel together). Read with figures_of, never parsed by hand.
inline constexpr const char* figures_advert = "dagda/figures";
struct StoreFigures {
  // The DATA bytes the node holds: with its capacity, whether it has room for one
  // more extent (Macha's NodeInfo::used).
  uint64_t used = 0;
  // How many of its DATA disks are online (Macha's storage_backends_online,
  // storage_server.cpp:56-57 at f0597a4).
  uint64_t backends_online = 0;
  // Its cache, when it has one (Macha's advertise_cache, storage_server.cpp:62-65
  // at f0597a4): capacity and use in bytes, and hits, misses and evictions since start.
  bool has_cache = false;
  uint64_t cache_capacity = 0;
  uint64_t cache_used = 0;
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t cache_evictions = 0;
  bool operator==(const StoreFigures&) const = default;
};
// A version byte, a flags byte (bit 0: has_cache), then the seven figures as
// big-endian u64s; a reader takes the fields it knows and ignores what follows.
std::vector<uint8_t> encode_figures(const StoreFigures& figures);
// A node's figures; nothing without the advert or with one it cannot read. Waits on
// nothing.
std::optional<StoreFigures> figures_of(const dannan::membership::NodeRecord& node);
// Its used bytes, zero when unknown (as an absent advert read before).
uint64_t used_of(const dannan::membership::NodeRecord& node);

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
