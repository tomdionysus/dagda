//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/durability_ticket.h"

namespace dagda::store {

void encode(dannan::net::Writer& writer, const DurabilityTicket& ticket) {
  writer.fixed(ticket.epoch.bytes);
  writer.u64(ticket.domain);
  writer.u64(ticket.generation);
  writer.u64(ticket.backend_instance);
}

DurabilityTicket decode_durability_ticket(dannan::net::Reader& reader) {
  DurabilityTicket ticket;
  ticket.epoch.bytes = reader.fixed<16>();
  ticket.domain = reader.u64();
  ticket.generation = reader.u64();
  ticket.backend_instance = reader.u64();
  return ticket;
}

}  // namespace dagda::store
