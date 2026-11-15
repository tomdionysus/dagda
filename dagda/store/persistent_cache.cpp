//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/persistent_cache.h"

#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace dagda::store {
namespace {

using dannan::contract::Lock;

}  // namespace

PersistentBlockCache::PersistentBlockCache(CacheOptions options, std::shared_ptr<const domain::StorageKey> key, LocalStore::Services services)
    : _key(std::move(key)), _services(std::move(services)) {
  Lock lock(_state_mutex);
  _options = std::move(options);
  open_locked();
}

void PersistentBlockCache::open_locked() {
  _store.reset();
  _lru.clear();
  _lru_index.clear();
  _block_count.store(0, std::memory_order_relaxed);
  ++_opening;
  if (_options.path.empty() || !_options.max_blocks) return;
  try {
    auto local = LocalStoreOptions::loose_only(std::numeric_limits<uint64_t>::max());
    local.sealing = _options.sealing;
    _store = std::make_shared<LocalStore>(_options.path, local, _key, _services, LocalStoreMode::ephemeral);
    rebuild_lru_locked();
  } catch (const std::exception& error) {
    _store.reset();
    _lru.clear();
    _lru_index.clear();
    _block_count.store(0, std::memory_order_relaxed);
    _services.logger.warn("persistent cache disabled: " + std::string(error.what()));
  }
}

void PersistentBlockCache::reconfigure(CacheOptions options) {
  Lock writer(_writer_mutex);
  Lock seed(_seed_mutex);

  std::shared_ptr<LocalStore> store;
  size_t limit = 0;
  {
    Lock lock(_state_mutex);
    const bool reopen = options.path != _options.path;
    _options = std::move(options);
    ++_opening;
    if (_options.path.empty() || !_options.max_blocks) {
      _store.reset();
      _lru.clear();
      _lru_index.clear();
      _block_count.store(0, std::memory_order_relaxed);
      return;
    }
    if (reopen || !_store) open_locked();
    store = _store;
    limit = _options.max_blocks;
  }
  trim_to_limit(store, limit);
}

bool PersistentBlockCache::enabled() const {
  Lock lock(_state_mutex);
  return _store && _options.max_blocks;
}

size_t PersistentBlockCache::max_blocks() const {
  Lock lock(_state_mutex);
  return _store ? _options.max_blocks : 0;
}

void PersistentBlockCache::rebuild_lru_locked() {
  _lru.clear();
  _lru_index.clear();
  if (!_store) return;
  // The only full walk of the cache; eviction is incremental from here.
  for (const auto& id : _store->list()) {
    _lru.push_back(id);
    _lru_index[id] = std::prev(_lru.end());
  }
  _block_count.store(_lru.size(), std::memory_order_relaxed);
}

void PersistentBlockCache::mark_used(const std::shared_ptr<LocalStore>& store, const ObjectId& id) {
  Lock lock(_state_mutex);
  if (!_store || _store != store) return;
  const auto found = _lru_index.find(id);
  // It may have been evicted after this reader opened the object.
  if (found == _lru_index.end()) return;
  _lru.splice(_lru.end(), _lru, found->second);
  found->second = std::prev(_lru.end());
}

void PersistentBlockCache::trim_to_limit(const std::shared_ptr<LocalStore>& store, size_t limit) {
  if (!store) return;
  while (true) {
    std::optional<ObjectId> victim;
    {
      Lock lock(_state_mutex);
      if (!_store || _store != store || _lru.size() <= limit) return;
      victim = _lru.front();
      _lru_index.erase(*victim);
      _lru.pop_front();
      _block_count.store(_lru.size(), std::memory_order_relaxed);
      _evictions.fetch_add(1, std::memory_order_relaxed);
    }
    // Outside the state lock: the store serialises its own mutations, the
    // cache issues no barriers, and readers hold only a shared_ptr.
    (void)store->remove(*victim);
  }
}

bool PersistentBlockCache::put(const ObjectId& id, std::span<const uint8_t> data) {
  // Writers are serialised apart from reads, so two cannot both take the last
  // slot and exceed max_blocks.
  Lock writer(_writer_mutex);

  std::shared_ptr<LocalStore> store;
  size_t limit = 0;
  {
    Lock lock(_state_mutex);
    store = _store;
    limit = _options.max_blocks;
  }
  if (!store || !limit) return false;

  try {
    if (store->has(id)) {
      mark_used(store, id);
      return true;
    }

    // Make one slot before the put. No durability barrier: a crash may discard
    // the whole cache. The LRU list is authoritative after open.
    while (true) {
      std::optional<ObjectId> victim;
      {
        Lock lock(_state_mutex);
        if (!_store || _store != store || !_options.max_blocks) return false;
        limit = _options.max_blocks;
        if (_lru.size() < limit) break;
        victim = _lru.front();
        _lru_index.erase(*victim);
        _lru.pop_front();
        _block_count.store(_lru.size(), std::memory_order_relaxed);
        // Counted here too: put() evicts inline in normal operation;
        // trim_to_limit runs only on reconfigure.
        _evictions.fetch_add(1, std::memory_order_relaxed);
      }
      (void)store->remove(*victim);
    }

    if (!store->put(id, data)) return false;
    {
      Lock lock(_state_mutex);
      // Reconfigured concurrently after the physical put.
      if (_store != store) return true;
      const auto existing = _lru_index.find(id);
      if (existing != _lru_index.end()) {
        _lru.splice(_lru.end(), _lru, existing->second);
        existing->second = std::prev(_lru.end());
      } else {
        _lru.push_back(id);
        _lru_index[id] = std::prev(_lru.end());
      }
      _block_count.store(_lru.size(), std::memory_order_relaxed);
    }
    return true;
  } catch (const std::exception& error) {
    _services.logger.debug("persistent cache write: " + std::string(error.what()));
    return false;
  }
}

std::optional<Bytes> PersistentBlockCache::get(const ObjectId& id) {
  std::shared_ptr<LocalStore> store;
  {
    Lock lock(_state_mutex);
    store = _store;
  }
  if (!store) return std::nullopt;

  try {
    auto data = store->get(id);
    if (data) {
      _hits.fetch_add(1, std::memory_order_relaxed);
      mark_used(store, id);
    } else {
      _misses.fetch_add(1, std::memory_order_relaxed);
    }
    return data;
  } catch (const std::exception& error) {
    _services.logger.debug("persistent cache read: " + std::string(error.what()));
    // A corrupt entry is disposable, but only removed from the same cache
    // instance that produced the failed read: a live reconfiguration may
    // already have installed a different cache root.
    Lock writer(_writer_mutex);
    bool current = false;
    {
      Lock lock(_state_mutex);
      current = _store == store;
    }
    // A read that threw is a miss, so a failing cache looks broken, not idle.
    _misses.fetch_add(1, std::memory_order_relaxed);
    if (current) {
      (void)store->remove(id);
      Lock lock(_state_mutex);
      if (_store == store) {
        const auto found = _lru_index.find(id);
        if (found != _lru_index.end()) {
          _lru.erase(found->second);
          _lru_index.erase(found);
          _block_count.store(_lru.size(), std::memory_order_relaxed);
        }
      }
    }
    return std::nullopt;
  }
}

bool PersistentBlockCache::has(const ObjectId& id) const {
  std::shared_ptr<LocalStore> store;
  {
    Lock lock(_state_mutex);
    store = _store;
  }
  return store && store->has(id);
}

bool PersistentBlockCache::remove(const ObjectId& id) {
  Lock writer(_writer_mutex);
  std::shared_ptr<LocalStore> store;
  {
    Lock lock(_state_mutex);
    store = _store;
  }
  if (!store) return false;

  const bool removed = store->remove(id);
  {
    Lock lock(_state_mutex);
    if (_store == store) {
      const auto found = _lru_index.find(id);
      if (found != _lru_index.end()) {
        _lru.erase(found->second);
        _lru_index.erase(found);
        _block_count.store(_lru.size(), std::memory_order_relaxed);
      }
    }
  }
  return removed;
}

size_t PersistentBlockCache::blocks() const { return _block_count.load(std::memory_order_relaxed); }

PersistentBlockCache::Stats PersistentBlockCache::stats() const {
  return Stats{_hits.load(std::memory_order_relaxed), _misses.load(std::memory_order_relaxed), _evictions.load(std::memory_order_relaxed),
               _block_count.load(std::memory_order_relaxed)};
}

bool PersistentBlockCache::with_seed(const std::function<void(const SeedPlace&)>& use) const {
  Lock seed(_seed_mutex);
  SeedPlace place;
  {
    Lock lock(_state_mutex);
    if (!_store || !_options.seed) return false;
    place = {_options.path / "metadata", _opening};
  }
  use(place);
  return true;
}

}  // namespace dagda::store
