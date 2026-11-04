//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "dagda/store/object_id.h"
#include "dannan/contract/thread_safety.h"

namespace dagda::store {

// A per-object mutex a writer holds across that object's device I/O, so
// waiting for it is waiting on that I/O: a no-I/O region may not take it.
// Keeps the mutex alive and held for the guard's lifetime.
class ObjectLock {
 public:
  explicit ObjectLock(std::shared_ptr<dannan::contract::IoMutex> mutex) DANNAN_EXCLUDES(dannan::contract::no_io)
      : _mutex(std::move(mutex)), _guard(_mutex->native()) {}
  ObjectLock(const ObjectLock&) = delete;
  ObjectLock& operator=(const ObjectLock&) = delete;

 private:
  std::shared_ptr<dannan::contract::IoMutex> _mutex;
  std::lock_guard<std::mutex> _guard;
};

// One store's per-object mutexes, so physical work on unrelated objects does
// not serialise behind the store's index mutex. An entry lives while someone
// holds its mutex and is forgotten after. Thread-safe; waits on nothing.
class ObjectLocks {
 public:
  std::shared_ptr<dannan::contract::IoMutex> mutex(const ObjectId& id) const {
    dannan::contract::Lock lock(_map_mutex);
    if (auto found = _mutexes.find(id); found != _mutexes.end()) {
      if (auto existing = found->second.lock()) return existing;
      _mutexes.erase(found);
    }
    auto created = std::make_shared<dannan::contract::IoMutex>();
    _mutexes[id] = created;
    if (_mutexes.size() > sweep_above) std::erase_if(_mutexes, [](const auto& item) { return item.second.expired(); });
    return created;
  }

 private:
  static constexpr size_t sweep_above = 4096;
  mutable dannan::contract::Mutex _map_mutex;
  mutable std::map<ObjectId, std::weak_ptr<dannan::contract::IoMutex>> _mutexes DANNAN_GUARDED_BY(_map_mutex);
};

// One node's guard over each object's claims against its copy: a claim's
// presence check and its journal write, and an instant delete's release, its
// check that no claim remains and its delete, each hold the object's guard,
// so no claim lands between a delete's check and its delete. Striped by the
// id's first byte; a batch takes its stripes in ascending order, so two
// batches never deadlock. Holders wait on the state and DATA devices.
// Thread-safe.
class ClaimLocks {
 public:
  static constexpr size_t stripes = 64;

  class Guard {
   public:
    Guard(Guard&&) noexcept = default;
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

   private:
    friend class ClaimLocks;
    Guard() = default;
    std::vector<std::unique_lock<std::mutex>> _held;
  };

  // Ids a batch holds its stripes for at once. A claim's journal write is
  // under them, and a batch may be a million ids.
  static constexpr size_t batch_ids = 4096;

  // Calls `step` on `ids` a sub-batch at a time, each under its stripes alone
  // and in stripe order, so a large publication holds only part of the guard
  // at a time and others get through between. Stops at the first step that
  // returns false, and returns false; what earlier steps did stands, a claim
  // being safe to have made and safe to make again.
  template <typename Step>
  bool in_batches(std::span<const ObjectId> ids, Step&& step, size_t batch = batch_ids) DANNAN_EXCLUDES(dannan::contract::no_io) {
    std::vector<ObjectId> sorted(ids.begin(), ids.end());
    std::sort(sorted.begin(), sorted.end(), [](const ObjectId& a, const ObjectId& b) {
      const auto stripe_a = a.bytes[0] % stripes;
      const auto stripe_b = b.bytes[0] % stripes;
      return stripe_a != stripe_b ? stripe_a < stripe_b : a < b;
    });
    for (size_t begin = 0; begin < sorted.size(); begin += batch) {
      const std::span<const ObjectId> part(sorted.data() + begin, std::min(batch, sorted.size() - begin));
      const auto guard = lock(part);
      if (!step(part)) return false;
    }
    return true;
  }

  // Every stripe `ids` fall in, held until the guard goes.
  Guard lock(std::span<const ObjectId> ids) DANNAN_EXCLUDES(dannan::contract::no_io) {
    std::array<bool, stripes> wanted{};
    for (const auto& id : ids) wanted[id.bytes[0] % stripes] = true;
    Guard guard;
    for (size_t stripe = 0; stripe < stripes; ++stripe)
      if (wanted[stripe]) guard._held.emplace_back(_stripes[stripe].native());
    return guard;
  }

 private:
  std::array<dannan::contract::IoMutex, stripes> _stripes;
};

}  // namespace dagda::store
