//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/ledger/unreferenced_since.h"

#include <fstream>
#include <iterator>
#include <string_view>

#include "dannan/identity/durable_file.h"
#include "dannan/net/codec.h"

namespace dagda::ledger {
namespace {

using dannan::contract::Lock;

constexpr uint32_t persisted_schema = 1;

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

UnreferencedSince::UnreferencedSince(std::filesystem::path path, dannan::status::ScopedLogger logger) : _path(std::move(path)), _logger(std::move(logger)) {
  std::ifstream in(_path, std::ios::binary);
  if (!in) return;
  const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  try {
    Lock lock(_mutex);
    for (const auto& [id, first_unix_ms] : decode_unreferenced_since(bytes)) _seen.emplace(id, Sighting{first_unix_ms, true});
  } catch (const dannan::net::DecodeError& error) {
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
  const auto bytes = encode_unreferenced_since(sightings);
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
