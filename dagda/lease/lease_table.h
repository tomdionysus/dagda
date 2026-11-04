//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

#include "dagda/lease/claimed_collection.h"
#include "dagda/lease/lease.h"
#include "dannan/net/codec.h"

// Named leases as a claimed collection: the value a lease space carries in
// step 4, one record per lease name, merged by the lease order. Dagda's own
// format (no owner has one yet): a format byte (1), the name, the optional
// lease, the tombstone.
namespace dagda::lease {

inline constexpr size_t max_lease_name = 4096;

struct LeaseEntry {
  std::string name;
  std::optional<Lease> lease;
  uint64_t removed_unix_ms = 0;
  auto operator<=>(const LeaseEntry&) const = default;
};

struct LeaseEntryTraits {
  using Record = LeaseEntry;
  using Key = std::string;
  using DuplicateKey = std::string;

  static const std::string& key_of(const LeaseEntry& entry) { return entry.name; }
  static constexpr std::tuple<> groups{};
  static const std::optional<Lease>& lease_of(const LeaseEntry& entry) { return entry.lease; }
  static void set_lease(LeaseEntry& entry, std::optional<Lease> lease) { entry.lease = lease; }
  static uint64_t removed_unix_ms(const LeaseEntry& entry) { return entry.removed_unix_ms; }
  static void set_removed_unix_ms(LeaseEntry& entry, uint64_t ms) { entry.removed_unix_ms = ms; }
  // Names are the keys: there is nothing else to be unique.
  static std::optional<std::string> duplicate_key(const LeaseEntry&) { return std::nullopt; }
  static bool keeps(const LeaseEntry&, const LeaseEntry&) { return false; }
  static void lose(LeaseEntry&, const LeaseEntry&) {}
  static void encode(dannan::net::Writer& writer, const LeaseEntry& entry);
  // Throws dannan::net::DecodeError for another format or a bad flag.
  static LeaseEntry decode(dannan::net::Reader& reader);
  static void encode_key(dannan::net::Writer& writer, const std::string& name) { writer.string(name); }
  static std::string decode_key(dannan::net::Reader& reader) { return reader.string(max_lease_name); }
  static constexpr uint32_t max_records = 65536;
  static constexpr std::chrono::milliseconds retention = std::chrono::hours(24 * 7);
};

using LeaseTable = ClaimedCollection<LeaseEntryTraits>;

}  // namespace dagda::lease
