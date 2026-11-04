//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <compare>
#include <cstdint>

#include "dannan/identity/node_id.h"
#include "dannan/net/codec.h"

namespace dagda::store {

// What a deferred write hands back (requirement S15): the generation that
// makes it durable, bound to the process that issued it (Dannan's
// durability epoch, fresh on every start), the durability domain and the
// backend incarnation that took it. A ticket from an old process or a
// re-mounted disk names nothing current, so it satisfies nothing.
struct DurabilityTicket {
  dannan::identity::NodeId epoch;
  uint64_t domain = 0;
  uint64_t generation = 0;
  uint64_t backend_instance = 0;

  // Whether it names a domain and an incarnation at all.
  bool valid() const noexcept { return domain && backend_instance; }
  // Whether it was issued by this process (`epoch`) for this domain of this
  // backend incarnation.
  bool names(const dannan::identity::NodeId& current_epoch, uint64_t current_domain, uint64_t current_backend_instance) const noexcept {
    return valid() && epoch == current_epoch && domain == current_domain && backend_instance == current_backend_instance;
  }

  auto operator<=>(const DurabilityTicket&) const = default;
};

// Protocol 23's form, the put_object_deferred reply and the head of an
// object_durability_barrier request: epoch (16 bytes), then domain,
// generation and backend instance, each a big-endian u64.
void encode(dannan::net::Writer& writer, const DurabilityTicket& ticket);
// Throws dannan::net::DecodeError on truncated input.
DurabilityTicket decode_durability_ticket(dannan::net::Reader& reader);

}  // namespace dagda::store
