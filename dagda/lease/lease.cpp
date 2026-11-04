//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/lease/lease.h"

namespace dagda::lease {

bool beats(const Lease& x, const Lease& y) noexcept {
  if (x.epoch != y.epoch) return x.epoch > y.epoch;
  if (x.since_unix_ms != y.since_unix_ms) return x.since_unix_ms < y.since_unix_ms;
  return x.holder < y.holder;
}

Lease merge(const Lease& a, const Lease& b) noexcept { return beats(a, b) ? a : b; }

std::optional<Lease> merge(const std::optional<Lease>& a, const std::optional<Lease>& b) noexcept {
  if (a && b) return merge(*a, *b);
  return a ? a : b;
}

void encode(dannan::net::Writer& writer, const Lease& lease) {
  writer.fixed(lease.holder.bytes);
  writer.u64(lease.epoch);
  writer.u64(lease.since_unix_ms);
}

Lease decode_lease(dannan::net::Reader& reader) {
  Lease lease;
  lease.holder.bytes = reader.fixed<16>();
  lease.epoch = reader.u64();
  lease.since_unix_ms = reader.u64();
  return lease;
}

void encode(dannan::net::Writer& writer, const std::optional<Lease>& lease) {
  writer.u8(lease ? 1 : 0);
  if (lease) encode(writer, *lease);
}

std::optional<Lease> decode_optional_lease(dannan::net::Reader& reader) {
  const auto present = reader.u8();
  if (present > 1) throw dannan::net::DecodeError("bad lease flag");
  if (!present) return std::nullopt;
  return decode_lease(reader);
}

}  // namespace dagda::lease
