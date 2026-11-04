//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <map>
#include <memory>
#include <mutex>

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

}  // namespace dagda::store
