//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/lease/lease_table.h"

namespace dagda::lease {

namespace {

constexpr uint8_t entry_format = 1;

}  // namespace

void LeaseEntryTraits::encode(dannan::net::Writer& writer, const LeaseEntry& entry) {
  writer.u8(entry_format);
  writer.string(entry.name);
  lease::encode(writer, entry.lease);
  writer.u64(entry.removed_unix_ms);
}

LeaseEntry LeaseEntryTraits::decode(dannan::net::Reader& reader) {
  if (reader.u8() != entry_format) throw dannan::net::DecodeError("unknown lease entry format");
  LeaseEntry entry;
  entry.name = reader.string(max_lease_name);
  entry.lease = decode_optional_lease(reader);
  entry.removed_unix_ms = reader.u64();
  return entry;
}

}  // namespace dagda::lease
