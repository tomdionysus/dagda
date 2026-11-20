//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Leases as a lease space carries them (lease/lease.h, lease/lease_table.h): a record,
// a table's snapshot and a table's delta, from any node. Each may throw only
// net::DecodeError.

#include <optional>
#include <string>
#include <vector>

#include "dagda/fuzz/samples.h"
#include "dagda/lease/lease.h"
#include "dagda/lease/lease_table.h"
#include "dannan/fuzz/fuzz.h"

namespace dagda::fuzz {
namespace {

using dannan::fuzz::Registration;
using dannan::net::Bytes;
using dannan::net::DecodeError;

lease::Lease sample_lease(uint8_t holder, uint64_t epoch) {
  lease::Lease out;
  out.holder = sample_node(holder);
  out.epoch = epoch;
  out.since_unix_ms = 1'800'000'000'000;
  return out;
}

lease::LeaseTable sample_table() {
  lease::LeaseTable table;
  table.join({"repair", sample_lease(1, 2), 0});
  table.join({"collector", std::nullopt, 1'800'000'000'000});
  return table;
}

// An optional record, then a bare one.
const Registration record(
    "lease_record",
    [](std::span<const uint8_t> data) {
      try {
        dannan::net::Reader reader(data);
        (void)lease::decode_optional_lease(reader);
        (void)lease::decode_lease(reader);
        reader.finish();
      } catch (const DecodeError&) {
      }
    },
    [] {
      dannan::net::Writer present;
      lease::encode(present, std::optional<lease::Lease>(sample_lease(1, 2)));
      lease::encode(present, sample_lease(2, 3));
      dannan::net::Writer absent;
      lease::encode(absent, std::optional<lease::Lease>());
      lease::encode(absent, sample_lease(3, 1));
      return std::vector<Bytes>{present.take(), absent.take()};
    });

// A snapshot, decoded and written again.
const Registration table(
    "lease_table",
    [](std::span<const uint8_t> data) {
      try {
        dannan::net::Reader reader(data);
        const auto decoded = lease::LeaseTable::decode(reader);
        reader.finish();
        dannan::net::Writer writer;
        decoded.encode(writer);
        (void)decoded.expired(1'900'000'000'000);
      } catch (const DecodeError&) {
      }
    },
    [] {
      dannan::net::Writer writer;
      sample_table().encode(writer);
      return std::vector<Bytes>{writer.take()};
    });

// A delta, decoded and applied to a table.
const Registration table_delta(
    "lease_table_delta",
    [](std::span<const uint8_t> data) {
      try {
        dannan::net::Reader reader(data);
        const auto delta = lease::LeaseTable::decode_delta(reader);
        reader.finish();
        auto applied = sample_table();
        applied.apply(delta);
      } catch (const DecodeError&) {
      }
    },
    [] {
      const auto before = sample_table();
      auto after = before;
      after.join({"repair", sample_lease(4, 3), 0});
      after.join({"sweeper", sample_lease(5, 1), 0});
      (void)after.erase(std::string("collector"));
      dannan::net::Writer writer;
      lease::LeaseTable::encode(writer, lease::LeaseTable::diff(before, after));
      return std::vector<Bytes>{writer.take()};
    });

}  // namespace
}  // namespace dagda::fuzz
