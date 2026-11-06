//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/sealed_file.h"

#include <algorithm>
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

}  // namespace dagda::heads
