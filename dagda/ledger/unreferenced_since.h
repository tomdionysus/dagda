//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>

#include "dagda/domain/storage_key.h"
#include "dagda/store/object_id.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/status/log.h"

// When this node first saw each object it holds unreferenced, on its own
// clock (requirement L22). The physical sweeps delete an object only once it
// has stayed unreferenced here for the deletion grace, whatever any other
// node's clock or tombstone says, so a node back from a long absence removes
// nothing before its own grace has run.
namespace dagda::ledger {

using store::Bytes;
using store::ObjectId;

// The sightings table: u32 schema 1, u64 count, then (id, u64 first
// sighting in Unix ms) per object in id order, all big-endian. On disk
// (`retention/unreferenced-{data,control}.bin`) it is one AES-256-GCM frame
// under the node-local key, as every ledger a node keeps: nonce (12), tag
// (16), ciphertext, with `sightings_aad` as the associated data.
inline constexpr std::string_view sightings_aad = "dagda/sightings/1";
Bytes encode_unreferenced_since(const std::map<ObjectId, uint64_t>& sightings);
Bytes seal_unreferenced_since(const domain::StorageKey& key, const std::map<ObjectId, uint64_t>& sightings);
// Throws std::runtime_error for a frame that does not open under `key`, and
// as decode_unreferenced_since for what it holds.
std::map<ObjectId, uint64_t> open_unreferenced_since(const domain::StorageKey& key, std::span<const uint8_t> file);
// Throws dannan::net::DecodeError for an unknown schema, a duplicate object,
// truncated or trailing bytes.
std::map<ObjectId, uint64_t> decode_unreferenced_since(std::span<const uint8_t> bytes);

// Thread-safe. Waits on nothing but the constructor and save(), which wait on
// the state device.
class UnreferencedSince {
 public:
  // Loads `path` when it exists; a file that does not open under `key` (the
  // node-local key) or does not decode is logged and ignored, which only
  // delays deletion. Throws std::invalid_argument without a key.
  UnreferencedSince(std::filesystem::path path, std::shared_ptr<const domain::StorageKey> key, dannan::status::ScopedLogger logger);

  // Records `now_unix_ms` as the first sighting when there is none, and
  // answers whether the object has been unreferenced for `grace`.
  bool matured(const ObjectId& id, uint64_t now_unix_ms, std::chrono::milliseconds grace);
  // The object is referenced again, or gone.
  void forget(const ObjectId& id);
  // A sweep visited every object it holds: sightings it did not renew since
  // the previous call belong to objects no longer held.
  void pass_complete();
  // Writes the table when it changed since the last save; a failed write is
  // logged and retried at the next.
  void save();

  size_t size() const;

 private:
  struct Sighting {
    uint64_t first_unix_ms = 0;
    bool renewed = false;
  };

  const std::filesystem::path _path;
  const std::shared_ptr<const domain::StorageKey> _key;
  const dannan::status::ScopedLogger _logger;
  mutable dannan::contract::Mutex _mutex;
  std::map<ObjectId, Sighting> _seen DANNAN_GUARDED_BY(_mutex);
  bool _dirty DANNAN_GUARDED_BY(_mutex) = false;
};

}  // namespace dagda::ledger
