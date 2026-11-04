//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "dagda/store/object_id.h"
#include "dannan/identity/crypto.h"

// A local store's files as layout v18 has them (requirements section 4.1,
// S2, S3, S8), byte for byte Macha's: every integer big-endian, every encoder
// and decoder pure. Waits on nothing; thread-safe.
namespace dagda::store::local_format {

// A loose object, objects/xx/yy/<hex>.obj: the magic, the plaintext's size,
// the nonce, the tag, then the ciphertext (as long as the plaintext), sealed
// with the id as the associated data.
inline constexpr std::array<uint8_t, 8> loose_magic{'D', 'H', 'T', 'O', 'B', 'J', '0', '1'};
inline constexpr size_t loose_header_size = loose_magic.size() + sizeof(uint64_t) + 12 + 16;

// The bytes before a loose object's ciphertext.
Bytes encode_loose_header(uint64_t plain_size, const dannan::identity::SealedData& sealed);

struct LooseObject {
  uint64_t plain_size = 0;
  std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> tag{};
  std::span<const uint8_t> ciphertext;
};
// Borrows `file`. Throws std::runtime_error ("bad object header") for
// another magic and dannan::net::DecodeError for a file shorter than its
// header.
LooseObject decode_loose(std::span<const uint8_t> file);

// Where a loose object lives below the store's root.
std::filesystem::path loose_path(const std::filesystem::path& objects, const ObjectId& id);
// "<64 hex>.obj" to its id, either case; nothing for any other name.
std::optional<ObjectId> object_id_from_file_name(std::string_view name);

// A pack, packs/pack-<20 digits>.pack: records appended one after another,
// each a header then its payload. A put carries the ciphertext; a remove (a
// tombstone) and a touch carry none.
inline constexpr std::array<uint8_t, 8> pack_magic{'M', 'A', 'C', 'H', 'P', 'K', '0', '1'};
inline constexpr uint8_t pack_put = 1;
inline constexpr uint8_t pack_remove = 2;
inline constexpr uint8_t pack_touch = 3;
// Magic, type, touched_ms, id, plain_size, payload_size, nonce, tag.
inline constexpr size_t pack_header_prefix_size = 93;
// The prefix and its SHA-256.
inline constexpr size_t pack_header_size = pack_header_prefix_size + 32;
inline constexpr uint64_t pack_payload_safety_limit = 128ULL * 1024 * 1024;

struct PackHeader {
  uint8_t type = 0;
  uint64_t touched_ms = 0;
  ObjectId id{};
  uint64_t plain_size = 0;
  uint64_t payload_size = 0;
  std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> tag{};
};

Bytes encode_pack_header(const PackHeader& header);
// Nothing unless the bytes are a whole header, its checksum holds, its type
// is known and its sizes fit its type.
std::optional<PackHeader> decode_pack_header(std::span<const uint8_t> bytes);

std::string pack_name(uint64_t sequence);
// The sequence a pack's file name carries (from 1); nothing for any other
// name.
std::optional<uint64_t> pack_sequence(std::string_view name);

// The accounting file, .macha.accounting: two slots of this size, A and B,
// the later sequence current.
inline constexpr std::array<uint8_t, 8> accounting_magic{'M', 'A', 'C', 'H', 'A', 'A', 'C', '1'};
inline constexpr size_t accounting_slot_size = 128;
inline constexpr size_t accounting_body_size = accounting_slot_size - 32;
inline constexpr uint8_t accounting_clean = 0;
inline constexpr uint8_t accounting_dirty = 3;

struct AccountingRecord {
  uint64_t sequence = 0;
  uint64_t used = 0;
  uint8_t operation = 0;
  ObjectId id{};
  uint64_t size = 0;
};

// One slot: the body padded with zeros, then its SHA-256.
Bytes encode_accounting(const AccountingRecord& record);
// Nothing unless the slot is whole, its checksum holds and its operation is
// clean or dirty.
std::optional<AccountingRecord> decode_accounting(std::span<const uint8_t> bytes);

}  // namespace dagda::store::local_format
