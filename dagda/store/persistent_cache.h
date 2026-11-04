//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>

#include "dagda/domain/storage_key.h"
#include "dagda/store/local_store.h"
#include "dagda/store/object_id.h"
#include "dannan/contract/thread_safety.h"

namespace dagda::store {

// Macha's cache settings (config.hpp:48-52), with its defaults: no path or no
// blocks is no cache.
struct CacheOptions {
  std::filesystem::path path{};
  size_t max_blocks = 0;
  // Whether a head owner may keep its recovery seed beside the cache
  // (Macha's prefer_metadata).
  bool seed = true;
  // How its objects are sealed: layout v18 (domain v1) or v19 with the
  // domain.
  SealingScheme sealing{};
};

// A node-local, persistent read cache (requirement S19). Cached objects are
// deliberately outside placement and replica accounting: losing the cache
// never reduces durability. An ephemeral LocalStore of loose objects: no
// durability domain, no barriers, no accounting checkpoint; least recently
// used out past max_blocks; a corrupt entry removed on read (crash matrix
// "Cache missing/corrupt": discard and rebuild).
//
// The seed. Macha keeps the last committed metadata record beside the cache
// (`<cache>/metadata/current.meta`, persistent_cache.cpp:325-390) to recover a
// head set from. Reading and writing that record is the heads step's
// (requirement H25, step 4), so the cache keeps none of it: with_seed() is
// where the seed attaches. The seed is not charged against max_blocks.
//
// Thread-safe. Lock order: the writer lock, then the seed lock, then the
// state lock. A read never waits on the writer lock or on another read.
class PersistentBlockCache {
 public:
  // What with_seed() hands the seed's owner: the directory it keeps its seed
  // in (`<cache>/metadata`, not created), and the cache's opening, which
  // changes whenever reconfigure() runs, so a seed remembered under an
  // earlier opening is written again.
  struct SeedPlace {
    std::filesystem::path directory;
    uint64_t opening = 0;
  };

  // Opens the cache at once; a cache that cannot open is logged and disabled,
  // never an error. Waits on the device.
  PersistentBlockCache(CacheOptions options, std::shared_ptr<const domain::StorageKey> key, LocalStore::Services services);

  // Takes effect at once: a new path reopens, fewer blocks evicts, no path or
  // no blocks disables. Waits on the device.
  void reconfigure(CacheOptions options);
  bool enabled() const;

  // Never fails: false if the cache is off or could not keep it. Makes room
  // first; issues no durability barrier.
  bool put(const ObjectId& id, std::span<const uint8_t> data);
  // A hit marks it used; a corrupt entry is removed and counted a miss.
  std::optional<Bytes> get(const ObjectId& id);
  bool has(const ObjectId& id) const;
  bool remove(const ObjectId& id);
  size_t blocks() const;

  // Cumulative since start; entries is instantaneous. Sustained zero hits
  // with high evictions is a defect.
  struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t evictions = 0;
    uint64_t entries = 0;
  };
  Stats stats() const;

  // The seed's attachment point. Calls `use` with the seed's place under the
  // seed lock, which reconfigure() also takes, so the cache cannot move or
  // close under it; true if it was called. Not called while the cache is off
  // or `seed` is unset, as Macha neither remembers nor offers a seed then.
  // Waits on whatever `use` waits on.
  bool with_seed(const std::function<void(const SeedPlace&)>& use) const;

 private:
  void open_locked() DANNAN_REQUIRES(_state_mutex);
  void rebuild_lru_locked() DANNAN_REQUIRES(_state_mutex);
  void mark_used(const std::shared_ptr<LocalStore>&, const ObjectId&) DANNAN_EXCLUDES(_state_mutex);
  void trim_to_limit(const std::shared_ptr<LocalStore>&, size_t) DANNAN_EXCLUDES(_state_mutex);

  const std::shared_ptr<const domain::StorageKey> _key;
  const LocalStore::Services _services;
  // Held across LocalStore puts and removes; serialises writers so two cannot
  // both take the last slot. Guards no state of its own.
  dannan::contract::IoMutex _writer_mutex;
  // Held across the seed's use and by reconfigure().
  mutable dannan::contract::IoMutex _seed_mutex DANNAN_ACQUIRED_AFTER(_writer_mutex);
  // Reads are on the playback path: never held while the store reads an
  // object, or a hit waits behind insertion and eviction. Held by open and
  // reconfigure across opening the cache store (its directory walk) and
  // dropping the old one.
  mutable dannan::contract::IoMutex _state_mutex DANNAN_ACQUIRED_AFTER(_writer_mutex, _seed_mutex);
  CacheOptions _options DANNAN_GUARDED_BY(_state_mutex);
  uint64_t _opening DANNAN_GUARDED_BY(_state_mutex) = 0;
  std::shared_ptr<LocalStore> _store DANNAN_GUARDED_BY(_state_mutex);
  std::list<ObjectId> _lru DANNAN_GUARDED_BY(_state_mutex);
  std::map<ObjectId, std::list<ObjectId>::iterator> _lru_index DANNAN_GUARDED_BY(_state_mutex);
  std::atomic<size_t> _block_count{0};
  // Since process start; monotonic, never reset, so consumers diff two reads.
  std::atomic<uint64_t> _hits{0};
  std::atomic<uint64_t> _misses{0};
  std::atomic<uint64_t> _evictions{0};
};

}  // namespace dagda::store
