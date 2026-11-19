//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/sealed_file.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include "dannan/identity/crypto.h"

namespace dagda::heads {
namespace {

using dannan::net::Reader;
using dannan::net::Writer;

void append(Bytes& out, std::string_view text) { out.insert(out.end(), text.begin(), text.end()); }

Bytes space_aad(std::string_view label, const domain::DomainId& domain, const SpaceId& space, const Magic& magic) {
  Bytes out;
  out.reserve(label.size() + domain.bytes.size() + space.bytes.size() + magic.size());
  append(out, label);
  out.insert(out.end(), domain.bytes.begin(), domain.bytes.end());
  out.insert(out.end(), space.bytes.begin(), space.bytes.end());
  out.insert(out.end(), magic.begin(), magic.end());
  return out;
}

}  // namespace

SpaceId space_id(const domain::DomainId& domain, std::string_view name) {
  Bytes input;
  append(input, "dagda/space/1");
  input.insert(input.end(), domain.bytes.begin(), domain.bytes.end());
  append(input, name);
  return dannan::identity::sha256(input);
}

std::filesystem::path space_directory(const std::filesystem::path& state, const SpaceId& space) { return state / "heads" / dannan::identity::hex(space); }

std::filesystem::path machas_metadata_directory(const std::filesystem::path& state) { return state / "metadata"; }

std::filesystem::path machas_metadata_seed(const std::filesystem::path& cache) { return cache / "metadata" / "current.meta"; }

AadRule replica_aad(const domain::DomainId& domain, const SpaceId& space) {
  return [domain, space](const Magic& magic) { return space_aad("dagda/heads/19", domain, space, magic); };
}

Bytes seed_aad(const domain::DomainId& domain, const SpaceId& space, const Magic& magic) { return space_aad("dagda/seed/19", domain, space, magic); }

Bytes seal_file(const domain::StorageKey& key, const Magic& magic, std::span<const uint8_t> aad, std::span<const uint8_t> plaintext) {
  const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), plaintext, aad);
  Writer writer;
  writer.raw(magic);
  writer.fixed(sealed.nonce);
  writer.fixed(sealed.tag);
  writer.bytes(sealed.ciphertext);
  return writer.take();
}

Bytes open_file(const domain::StorageKey& key, const Magic& magic, std::span<const uint8_t> aad, std::span<const uint8_t> file) {
  Reader reader(file);
  const auto found = reader.view(magic.size());
  if (!std::equal(found.begin(), found.end(), magic.begin())) throw std::runtime_error("bad file header");
  const auto nonce = reader.fixed<12>();
  const auto tag = reader.fixed<16>();
  const auto ciphertext = reader.view_bytes();
  reader.finish();
  return dannan::identity::aes_gcm_open(key.bytes(), nonce, tag, ciphertext, aad);
}

Bytes seal_frame(const domain::StorageKey& key, std::span<const uint8_t> aad, std::span<const uint8_t> plaintext) {
  const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), plaintext, aad);
  Writer envelope;
  envelope.fixed(sealed.nonce);
  envelope.fixed(sealed.tag);
  envelope.bytes(sealed.ciphertext);
  const auto payload = envelope.take();
  if (payload.size() > std::numeric_limits<uint32_t>::max()) throw std::runtime_error("metadata history record too large");
  Writer frame;
  frame.u32(static_cast<uint32_t>(payload.size()));
  frame.raw(payload);
  return frame.take();
}

FrameEnvelope parse_frame_envelope(std::span<const uint8_t> envelope) {
  Reader reader(envelope);
  FrameEnvelope out;
  out.nonce = reader.fixed<12>();
  out.tag = reader.fixed<16>();
  out.ciphertext = reader.bytes();
  reader.finish();
  return out;
}

Bytes open_frame_envelope(const domain::StorageKey& key, std::span<const uint8_t> aad, const FrameEnvelope& envelope) {
  return dannan::identity::aes_gcm_open(key.bytes(), envelope.nonce, envelope.tag, envelope.ciphertext, aad);
}

std::optional<Bytes> open_frame(const domain::StorageKey& key, std::span<const uint8_t> aad, std::span<const uint8_t> envelope) {
  try {
    return open_frame_envelope(key, aad, parse_frame_envelope(envelope));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<uint64_t> next_frame_from(std::istream& file, uint64_t file_size, uint64_t from, uint64_t max_frame,
                                        const std::function<std::optional<Bytes>(std::span<const uint8_t>)>& open, bool& read_failed,
                                        const std::function<void()>& pause) {
  constexpr uint64_t envelope_bytes = 12 + 16 + 4;
  constexpr uint64_t header_bytes = 4 + envelope_bytes;
  constexpr uint64_t window = 1024 * 1024;
  Bytes buffer(window);
  uint64_t at = from;
  while (at + header_bytes <= file_size) {
    if (pause) pause();
    const auto wanted = std::min(window, file_size - at);
    file.clear();
    file.seekg(static_cast<std::streamoff>(at), std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(wanted))) {
      read_failed = true;
      return std::nullopt;
    }
    const std::span<const uint8_t> bytes(buffer.data(), wanted);
    const auto u32_at = [&](uint64_t position) {
      Reader reader(bytes.subspan(position, 4));
      return static_cast<uint64_t>(reader.u32());
    };
    const auto positions = wanted - header_bytes + 1;
    for (uint64_t i = 0; i < positions; ++i) {
      const auto length = u32_at(i);
      if (length < envelope_bytes || length > max_frame || at + i + 4 + length > file_size || u32_at(i + 4 + 12 + 16) != length - envelope_bytes) continue;
      Bytes frame(static_cast<size_t>(length));
      file.clear();
      file.seekg(static_cast<std::streamoff>(at + i + 4), std::ios::beg);
      if (!file.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(length))) {
        read_failed = true;
        return std::nullopt;
      }
      if (open(frame)) return at + i;
    }
    at += positions;
  }
  return std::nullopt;
}

std::optional<uint64_t> next_frame_from(const std::filesystem::path& path, uint64_t file_size, uint64_t from, const domain::StorageKey& key,
                                        std::span<const uint8_t> aad, const std::function<void()>& pause) {
  std::ifstream scan(path, std::ios::binary);
  bool read_failed = false;
  const auto next = next_frame_from(
      scan, file_size, from, max_history_frame_bytes, [&](std::span<const uint8_t> frame) { return open_frame(key, aad, frame); }, read_failed, pause);
  if (read_failed) throw std::runtime_error("cannot read frames from " + path.string());
  return next;
}

}  // namespace dagda::heads
