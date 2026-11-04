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
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/ledger/object_trie.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/status/log.h"

namespace dagda::ledger {

// Which objects one local store holds (requirement L3), kept on the state
// device so a restarted store knows at once instead of walking its disk.
// Once seeded it is the store's whole answer to "do I hold this"; until then
// the store answers as it always has and only queues what changes. A held
// record's value is 1 as eight big-endian bytes. Its files are layout v18's:
// the trie's, `seeded` and `verify-next`.
//
// The store queues each change under its own index lock, so they are queued
// in the order they happened, and flushes outside that lock: a flush
// journals the queue (an fsync) without holding the lookup lock, then
// installs it, so a lookup never waits on another write's sync. Until
// seeded a flush leaves the queue alone, so the seed's build never holds a
// write up. Thread-safe.
class HeldLedger {
 public:
  // Opens the trie in `dir`. Throws as ObjectTrie's constructor does.
  HeldLedger(std::filesystem::path dir, std::shared_ptr<const domain::StorageKey> key, ObjectTrie::Options options, dannan::status::ScopedLogger logger);

  // Whether it holds a complete record: seeded once, kept since. Waits on
  // nothing.
  bool seeded() const noexcept { return _seeded.load(std::memory_order_acquire); }
  // From the record once seeded; nothing before. May wait on the state
  // device, and on an install.
  std::optional<bool> held(const ObjectId&) const;
  // The recorded ids whose first two bytes are `prefix` (big-endian), in
  // order. Waits as held() does.
  std::vector<ObjectId> held_with_prefix(uint16_t prefix) const;
  // Queues a change. The caller holds the lock that orders its changes.
  // Waits on nothing.
  void record(const ObjectId&, bool held);
  // Journals and installs every queued change, in order. Waits on the
  // state device (the journal's fsync).
  void flush();
  // Replaces the record with what `snapshot` returns (sorted, unique),
  // marks it seeded and applies what was queued after the snapshot.
  // `snapshot` must drop the queued changes the snapshot already includes
  // (drop_queued() under the same lock it records under). Writes go on
  // meanwhile: their flushes leave the queue for the seed. Waits on the
  // state device.
  void seed(const std::function<std::vector<ObjectId>()>& snapshot);
  // Drops every queued change. Called from within seed's snapshot. Waits on
  // nothing.
  void drop_queued();
  // The record as installed now, and how many ids installs have marked not
  // held since open (a seed counts one, as it may drop any), read together.
  struct View {
    ObjectTrie::Snapshot records;
    uint64_t removals = 0;
  };
  View view() const;
  // The removals alone, without the lookup lock. Waits on nothing.
  uint64_t removals() const noexcept { return _removals.load(std::memory_order_acquire); }
  ObjectTrie::Stats stats() const;
  uint64_t size() const;
  // The objects/xx/yy directory the store's check against its disk resumes
  // at, kept across restarts. Single owner (that check).
  uint16_t verify_next() const noexcept { return _verify_next; }
  void set_verify_next(uint16_t);

 private:
  // Journals and installs what is queued, in order.
  void install_queued() DANNAN_REQUIRES(_flush_mutex);
  // Journals without the lookup lock: it touches only the trie's journal,
  // never the nodes or cache a lookup reads, and every journal write and
  // install happens under _flush_mutex, in order.
  void write_journal(std::span<const ObjectTrie::Change>) DANNAN_REQUIRES(_flush_mutex) DANNAN_NO_THREAD_SAFETY_ANALYSIS;

  const std::filesystem::path _dir;
  std::atomic_bool _seeded{false};
  uint16_t _verify_next = 0;
  // Held across a flush (the journal's fsync), so journal writes and
  // installs keep the order the changes were queued in. Taken before
  // _trie_mutex.
  dannan::contract::IoMutex _flush_mutex DANNAN_ACQUIRED_BEFORE(_trie_mutex);
  // Held to read or install; a lookup may read a node from disk.
  mutable dannan::contract::IoMutex _trie_mutex;
  std::unique_ptr<ObjectTrie> _trie DANNAN_PT_GUARDED_BY(_trie_mutex);
  // Advanced under _trie_mutex with the install it counts.
  std::atomic<uint64_t> _removals{0};
  // Memory only; taken inside _flush_mutex, never with _trie_mutex.
  dannan::contract::Mutex _queue_mutex;
  std::vector<ObjectTrie::Change> _queued DANNAN_GUARDED_BY(_queue_mutex);
};

}  // namespace dagda::ledger
