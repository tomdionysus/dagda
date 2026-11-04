//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <compare>
#include <cstdint>
#include <optional>

#include "dannan/identity/node_id.h"
#include "dannan/net/codec.h"

// "Who is doing this", answered without consensus (plan, "Store: leases and
// claimed collections"; requirements E2, E7, E9). A lease is a record any
// node may write and every node merges the same way; it is a soft claim, not
// a lock: two holders can coexist for one absence window, and the epoch
// fence (fence.h) is what keeps the stale one from acting.
namespace dagda::lease {

using NodeId = dannan::identity::NodeId;

// acquire(name, scope) -> {holder, epoch, since}.
struct Lease {
  NodeId holder{};
  // From 1. A takeover, or a holder's own re-claim, is epoch + 1.
  uint64_t epoch = 0;
  // When the holder claimed it, on the holder's wall clock (unix ms).
  uint64_t since_unix_ms = 0;
  auto operator<=>(const Lease&) const = default;
};

// The lease order: the higher epoch; within an epoch the earlier claim; then
// the lower node id. A strict order on distinct leases.
bool beats(const Lease& x, const Lease& y) noexcept;

// The join of two replicas' records: the one that beats the other.
// Commutative, associative and idempotent; an absent record is the bottom.
Lease merge(const Lease& a, const Lease& b) noexcept;
std::optional<Lease> merge(const std::optional<Lease>& a, const std::optional<Lease>& b) noexcept;

// The record's bytes: holder (16 bytes), epoch (u64), since (u64), as Macha's
// claim is written; an optional one is a flag byte (0 or 1) before it.
void encode(dannan::net::Writer& writer, const Lease& lease);
Lease decode_lease(dannan::net::Reader& reader);
void encode(dannan::net::Writer& writer, const std::optional<Lease>& lease);
// Throws dannan::net::DecodeError for a flag other than 0 or 1.
std::optional<Lease> decode_optional_lease(dannan::net::Reader& reader);

}  // namespace dagda::lease
