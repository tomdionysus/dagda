//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A manifest's item values as the comparison reads them back from disk
// (migrate/manifest_format.h). Each may throw only net::DecodeError, but a metadata
// value, which says ManifestDamaged.

#include <vector>

#include "dagda/fuzz/samples.h"
#include "dagda/migrate/manifest.h"
#include "dagda/migrate/manifest_format.h"
#include "dagda/store/local_format.h"
#include "dannan/fuzz/fuzz.h"

namespace dagda::fuzz {
namespace {

using dannan::fuzz::Registration;
using dannan::net::Bytes;
using dannan::net::DecodeError;
namespace format = migrate::manifest_format;

const Registration loose_value(
    "migrate_loose_value",
    [](std::span<const uint8_t> data) {
      try {
        (void)format::decode_loose_value(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      format::LooseValue value;
      value.layout = format::record_v19;
      value.plain_size = 10;
      value.file_size = 46;
      value.mtime_ns = -5;
      value.nonce.fill(1);
      value.tag.fill(2);
      return std::vector<Bytes>{format::encode(value)};
    });

const Registration pack_value(
    "migrate_pack_value",
    [](std::span<const uint8_t> data) {
      try {
        const auto value = format::decode_pack_value(data);
        (void)value.total();
      } catch (const DecodeError&) {
      }
    },
    [] {
      format::PackValue record;
      record.span = format::span_record;
      record.layout = format::record_v18;
      record.header.type = store::local_format::pack_put;
      record.header.touched_ms = 1;
      record.header.id = sample_id(1);
      record.header.plain_size = 10;
      record.header.payload_size = 10;
      format::PackValue damaged;
      damaged.span = format::span_damaged;
      damaged.length = 300;
      damaged.digest = sample_id(2);
      return std::vector<Bytes>{format::encode(record), format::encode(damaged)};
    });

const Registration marker_value(
    "migrate_marker_value",
    [](std::span<const uint8_t> data) {
      try {
        (void)format::decode_marker_value(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      format::MarkerValue value;
      value.layout = 19;
      value.node = "node";
      value.token = "token";
      value.text = "text";
      return std::vector<Bytes>{format::encode(value)};
    });

const Registration accounting_value(
    "migrate_accounting_value",
    [](std::span<const uint8_t> data) {
      try {
        (void)format::decode_accounting_value(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      format::AccountingValue value;
      value.decoded = true;
      value.sequence = 4;
      value.used = 100;
      value.operation = store::local_format::accounting_clean;
      value.digest = sample_id(3);
      return std::vector<Bytes>{format::encode(value)};
    });

const Registration other_value(
    "migrate_other_value",
    [](std::span<const uint8_t> data) {
      try {
        (void)format::decode_other_value(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      format::OtherValue value;
      value.size = 12;
      value.digest = sample_id(4);
      return std::vector<Bytes>{format::encode(value)};
    });

const Registration metadata_value(
    "migrate_metadata_value",
    [](std::span<const uint8_t> data) {
      try {
        (void)format::decode_metadata_value(data);
      } catch (const migrate::ManifestDamaged&) {
      }
    },
    [] {
      const Bytes fact{1, 2, 3};
      return std::vector<Bytes>{format::encode_metadata_value(false, fact), format::encode_metadata_value(true, fact)};
    });

}  // namespace
}  // namespace dagda::fuzz
