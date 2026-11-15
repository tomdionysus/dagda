//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/ledger/unreferenced_since.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"
#include "dannan/net/codec.h"

namespace dagda::ledger {
namespace {

using dannan::contract::Lock;

constexpr uint32_t persisted_schema = 1;
constexpr size_t frame_header = 12 + 16;

std::span<const uint8_t> aad() { return {reinterpret_cast<const uint8_t*>(sightings_aad.data()), sightings_aad.size()}; }

}  // namespace

Bytes encode_unreferenced_since(const std::map<ObjectId, uint64_t>& sightings) {
  dannan::net::Writer out;
  out.u32(persisted_schema);
  out.u64(sightings.size());
  for (const auto& [id, first_unix_ms] : sightings) {
    out.fixed(id.bytes);
    out.u64(first_unix_ms);
  }
  return out.take();
}

std::map<ObjectId, uint64_t> decode_unreferenced_since(std::span<const uint8_t> bytes) {
  dannan::net::Reader in(bytes);
  if (in.u32() != persisted_schema) throw dannan::net::DecodeError("unknown unreferenced-since schema");
  std::map<ObjectId, uint64_t> sightings;
  // Nothing is reserved from the count: every entry is read before it is
  // kept, so a damaged count fails on the bytes, never on memory.
  const auto count = in.u64();
  for (uint64_t i = 0; i < count; ++i) {
    ObjectId id{in.fixed<32>()};
    const auto first_unix_ms = in.u64();
    if (!sightings.emplace(id, first_unix_ms).second) throw dannan::net::DecodeError("duplicate unreferenced-since object");
  }
  in.finish();
  return sightings;
}

Bytes seal_unreferenced_since(const domain::StorageKey& key, const std::map<ObjectId, uint64_t>& sightings) {
  const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), encode_unreferenced_since(sightings), aad());
  Bytes out;
  out.reserve(frame_header + sealed.ciphertext.size());
  out.insert(out.end(), sealed.nonce.begin(), sealed.nonce.end());
  out.insert(out.end(), sealed.tag.begin(), sealed.tag.end());
  out.insert(out.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
  return out;
}

std::map<ObjectId, uint64_t> open_unreferenced_since(const domain::StorageKey& key, std::span<const uint8_t> file) {
  if (file.size() < frame_header) throw std::runtime_error("sightings file shorter than its frame header");
  const auto plain = dannan::identity::aes_gcm_open(key.bytes(), file.first<12>(), file.subspan<12, 16>(), file.subspan(frame_header), aad());
  return decode_unreferenced_since(plain);
}

UnreferencedSince::UnreferencedSince(std::filesystem::path path, std::shared_ptr<const domain::StorageKey> key, dannan::status::ScopedLogger logger)
    : _path(std::move(path)), _key(std::move(key)), _logger(std::move(logger)) {
  if (!_key) throw std::invalid_argument("the sightings file needs the node-local key");
  std::ifstream in(_path, std::ios::binary);
  if (!in) return;
  const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  try {
    Lock lock(_mutex);
    for (const auto& [id, first_unix_ms] : open_unreferenced_since(*_key, bytes)) _seen.emplace(id, Sighting{first_unix_ms, true});
  } catch (const std::exception& error) {
    _logger.warn("unreferenced-since: ignoring " + _path.string() + ": " + error.what());
  }
}

bool UnreferencedSince::matured(const ObjectId& id, uint64_t now_unix_ms, std::chrono::milliseconds grace) {
  Lock lock(_mutex);
  auto [found, inserted] = _seen.emplace(id, Sighting{now_unix_ms, true});
  if (inserted) _dirty = true;
  found->second.renewed = true;
  // A clock set back restarts the wait rather than ending it early.
  if (found->second.first_unix_ms > now_unix_ms) {
    found->second.first_unix_ms = now_unix_ms;
    _dirty = true;
  }
  return now_unix_ms - found->second.first_unix_ms >= static_cast<uint64_t>(grace.count());
}

void UnreferencedSince::forget(const ObjectId& id) {
  Lock lock(_mutex);
  if (_seen.erase(id)) _dirty = true;
}

void UnreferencedSince::pass_complete() {
  Lock lock(_mutex);
  for (auto it = _seen.begin(); it != _seen.end();) {
    if (!it->second.renewed) {
      it = _seen.erase(it);
      _dirty = true;
    } else {
      it->second.renewed = false;
      ++it;
    }
  }
}

void UnreferencedSince::save() {
  std::map<ObjectId, uint64_t> sightings;
  {
    Lock lock(_mutex);
    if (!_dirty) return;
    for (const auto& [id, sighting] : _seen) sightings.emplace(id, sighting.first_unix_ms);
    _dirty = false;
  }
  const auto bytes = seal_unreferenced_since(*_key, sightings);
  try {
    std::filesystem::create_directories(_path.parent_path());
    dannan::identity::durable_replace_file(_path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  } catch (const std::exception& error) {
    _logger.warn("unreferenced-since: cannot write " + _path.string() + ": " + error.what());
    Lock lock(_mutex);
    _dirty = true;
  }
}

size_t UnreferencedSince::size() const {
  Lock lock(_mutex);
  return _seen.size();
}

}  // namespace dagda::ledger
