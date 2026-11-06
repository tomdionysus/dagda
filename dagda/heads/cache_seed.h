//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>

#include "dagda/domain/domain_id.h"
#include "dagda/domain/storage_key.h"
#include "dagda/heads/record.h"
#include "dagda/heads/sealed_file.h"
#include "dagda/store/persistent_cache.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/sched/work.h"
#include "dannan/status/log.h"

namespace dagda::heads {

// The last committed record of each space, kept beside the cache to recover a head
// set from (requirement H25), and never authority: Replica takes it only when the
// primary state fails, and then holds it with no certificate. Macha's
// current.meta at 9a08846 in v19's place and seal: `<cache>/metadata/<space id
// hex>.meta`, the magic, nonce, tag and ciphertext of a sealed file, under the
// domain's key.
//
// It lives under PersistentBlockCache::with_seed, so with the cache off, or its
// seed unset, it is neither read nor written. A write failure is a debug line and
// a read failure an absent seed: losing it never costs anything but the recovery.
//
// Thread-safe. Lock order: the cache's seed lock, then this class's mutex.
class CacheSeed {
 public:
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::thread_safe;
  static constexpr dannan::sched::Waits remember_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::Waits load_waits = dannan::sched::local_waits;

  // `magic` is the owner's and stays in the file. The cache must outlive this.
  CacheSeed(const store::PersistentBlockCache& cache, std::shared_ptr<const domain::StorageKey> key, domain::DomainId domain, Magic magic,
            dannan::status::ScopedLogger logger);

  // Written durably, unless this record of the space is already what the place
  // holds. A moved place (the cache reconfigured) is written again.
  void remember(const SpaceId& space, const Record& record) DANNAN_EXCLUDES(_mutex);
  // The space's seed; nothing when the cache is off, the seed is absent, or the
  // file does not open as this domain's and space's.
  std::optional<Record> load(const SpaceId& space) DANNAN_EXCLUDES(_mutex);

 private:
  struct Remembered {
    Hash256 hash{};
    uint64_t opening = 0;
  };

  const store::PersistentBlockCache& _cache;
  const std::shared_ptr<const domain::StorageKey> _key;
  const domain::DomainId _domain;
  const Magic _magic;
  const dannan::status::ScopedLogger _logger;
  dannan::contract::Mutex _mutex;
  std::map<SpaceId, Remembered> _remembered DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dagda::heads
