//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include "dagda/domain/domain_id.h"
#include "dagda/domain/storage_key.h"
#include "dagda/heads/record.h"

// How a head space's files are sealed (the plan's section 4.1). Pure; waits on
// nothing; thread-safe.
//
// A sealed file: magic, nonce 12, tag 16, u32 length, ciphertext. A journal or
// history frame: u32 length, then nonce 12, tag 16, u32 length, ciphertext. Macha's
// forms at 9a08846; the associated data is v19's.
namespace dagda::heads {

using Magic = std::array<uint8_t, 8>;

// A file's own magic, named by the owner of the space, and kept in the file.
struct ReplicaMagics {
  Magic checkpoint{};
  Magic journal{};
  Magic history{};
  Magic heads{};
  Magic author{};
  Magic set_aside{};
};

// A space's identity: SHA-256("dagda/space/1" || domain id || name).
using SpaceId = Hash256;
SpaceId space_id(const domain::DomainId& domain, std::string_view name);
// <state>/heads/<space id hex>
std::filesystem::path space_directory(const std::filesystem::path& state, const SpaceId& space);

// The associated data a magic seals under. A parameter so a reader can be given
// another layout's (S7's migration, and Macha's frames as a byte proof).
using AadRule = std::function<Bytes(const Magic&)>;
// "dagda/heads/19" || domain id || space id || magic: every sealed file of a space.
AadRule replica_aad(const domain::DomainId& domain, const SpaceId& space);
// "dagda/seed/19" || domain id || space id || magic: the cache seed.
Bytes seed_aad(const domain::DomainId& domain, const SpaceId& space, const Magic& magic);

Bytes seal_file(const domain::StorageKey& key, const Magic& magic, std::span<const uint8_t> aad, std::span<const uint8_t> plaintext);
// Throws std::runtime_error ("bad file header", or the open's own) or DecodeError
// for anything but a whole file of that magic that opens.
Bytes open_file(const domain::StorageKey& key, const Magic& magic, std::span<const uint8_t> aad, std::span<const uint8_t> file);

Bytes seal_frame(const domain::StorageKey& key, std::span<const uint8_t> aad, std::span<const uint8_t> plaintext);

// A frame's envelope as a reader finds it: the bytes after the length.
struct FrameEnvelope {
  std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> tag{};
  Bytes ciphertext;
};
// Throws DecodeError for a malformed envelope. Opening it is separate, because a
// reader treats an envelope that will not parse and a seal that will not open
// differently on a final frame.
FrameEnvelope parse_frame_envelope(std::span<const uint8_t> envelope);
Bytes open_frame_envelope(const domain::StorageKey& key, std::span<const uint8_t> aad, const FrameEnvelope& envelope);

}  // namespace dagda::heads
