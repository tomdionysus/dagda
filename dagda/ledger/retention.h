//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "dagda/contract/claim_store.h"
#include "dagda/domain/storage_key.h"
#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/sealed_journal.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/status/log.h"

namespace dagda::ledger {

using contract::NodeId;
using contract::RetentionClass;
using contract::RetentionClock;
using contract::RetentionDot;

// The claims as bytes, Macha's layout v18's (requirements 4.1, "Retention
// WAL" and the claims tries), sealed under the node-local key. Pure; pinned
// by known answers.
namespace retention_format {

// claims.log's associated data. The WAL has no file magic: it is a sealed
// journal (sealed_journal.h).
inline constexpr std::array<uint8_t, 8> journal_aad{'M', 'A', 'C', 'H', 'R', 'T', 'J', '1'};
// Frames are written at most this large, and read up to max_read_frame (an
// older writer's limit).
inline constexpr uint32_t max_journal_frame = 4U * 1024U * 1024U;
inline constexpr uint32_t max_read_frame = 64U * 1024U * 1024U;
inline constexpr uint8_t op_add = 1;
inline constexpr uint8_t op_release = 2;
inline constexpr uint8_t op_prune = 3;

// One object's observed-remove state: per origin, the highest undominated
// add and the highest remove context seen.
struct ObjectState {
  std::map<NodeId, uint64_t> adds;
  std::map<NodeId, uint64_t> removed;
  bool empty() const noexcept { return adds.empty() && removed.empty(); }
  bool operator==(const ObjectState&) const = default;
};

// A claims trie record: u16 n | (origin, u64 sequence) x n, for the adds
// and then the removes, each in origin order. Throws std::runtime_error for
// more than 65,535 origins on one side.
Bytes encode_state(const ObjectState&);
// Throws dannan::net::DecodeError for a zero origin or sequence, a repeated
// origin, or truncated or trailing bytes.
ObjectState decode_state(std::span<const uint8_t>);
// Whether a record holds any claim: its adds' count leads it.
bool claimed(std::span<const uint8_t> record) noexcept;

// The WAL's frames, plaintext: op | class | ...
// add: origin 16 | u64 sequence | u32 n | id x n.
Bytes encode_add(RetentionClass, const RetentionDot&, std::span<const ObjectId>);
// release: u32 n | (node 16, u64 sequence) x n | u32 m | id x m.
Bytes encode_release(RetentionClass, const RetentionClock&, std::span<const ObjectId>);
// prune: u32 n | id x n.
Bytes encode_prune(RetentionClass, std::span<const ObjectId>);

}  // namespace retention_format

// Durable local evidence of which objects this node has promised to keep
// (requirements L12, L13): the claims as an observed-remove set per object
// and class. For each object and origin only the highest undominated add
// sequence and the highest remove context observed are kept, so a release
// erases only the claims visible to the clock that released them, and a
// concurrent branch's claim survives it.
//
// Each class's claims live in an object trie (`retention/ledger-data` and
// `retention/ledger-control` under the state directory), one record an
// object, so memory is the tries' caches whatever the number of claims.
// `retention/claims.log` is the write-ahead log: every add, release and
// prune is journaled and fsynced, then applied to the trie in memory; a
// checkpoint saves both tries and empties the log, at 8,192 changed objects
// or a 4 MiB log. Replaying the log over a checkpoint is idempotent. The
// file `retention/ledger` ("1\n") says the tries are the claims; tries that
// hold claims under no such file are refused, never rebuilt empty.
//
// Macha read two older checkpoint forms once, to move them into the tries
// (`claims.meta`, MRTS0001; `claims.current` naming `checkpoints/gen-*`,
// MRTS0002). Their readers are not here (Tom, 2026-11-04): a state holding
// one that was never moved is refused, never read as empty. Their names stay
// reserved, and the first checkpoint removes them as Macha's does.
//
// Thread-safe; the release and prune walks each have one owner.
class RetentionStore final : public contract::ClaimStore {
 public:
  // Opens or creates the claims under `state`/retention, replaying the log.
  // `cache_bytes` is shared by the two tries, three quarters to the DATA one.
  // Throws std::runtime_error for a state with an unmoved older checkpoint,
  // or if the tries cannot be opened. Waits on the state device.
  RetentionStore(const std::filesystem::path& state, std::shared_ptr<const domain::StorageKey> key, dannan::status::ScopedLogger logger,
                 size_t cache_bytes = 8ULL * 1024 * 1024);

  void retain(RetentionClass, const ObjectId&, const RetentionDot&) override;
  void retain_batch(RetentionClass, const std::vector<ObjectId>&, const RetentionDot&) override;
  bool retained(RetentionClass, const ObjectId&) const override;
  Claims claims(RetentionClass, const ObjectId&) const override;
  std::vector<ObjectId> retained_ids(RetentionClass) const override;
  std::optional<ObjectId> next_retained(RetentionClass, std::optional<ObjectId>& cursor, bool& complete) const override;
  // `live` sorted and unique, or a membership test.
  size_t release_unreferenced(RetentionClass, const contract::IdLookup& live, const RetentionClock& observed, size_t operation_budget) override;
  size_t claim_objects(RetentionClass) const override;
  // Checkpoints once the log holds `record_threshold` frames or 64 MiB.
  bool compact_if_needed(size_t record_threshold = 4096) override;
  // Traversal is cursor-based, so a small budget cannot starve later
  // records.
  size_t prune_unclaimed(RetentionClass, const std::function<bool(const ObjectId&)>& exists, size_t operation_budget) override;

 private:
  struct ClassLedger {
    std::unique_ptr<ObjectTrie> trie;
    std::optional<ObjectId> release_after;
    std::optional<ObjectId> prune_after;
  };

  ClassLedger& class_for(RetentionClass) DANNAN_REQUIRES(_mutex);
  const ClassLedger& class_for(RetentionClass) const DANNAN_REQUIRES(_mutex);
  void apply_add_locked(RetentionClass, const RetentionDot&, std::vector<ObjectId>) DANNAN_REQUIRES(_mutex);
  size_t apply_release_locked(RetentionClass, const RetentionClock&, std::vector<ObjectId>) DANNAN_REQUIRES(_mutex);
  size_t apply_prune_locked(RetentionClass, std::vector<ObjectId>) DANNAN_REQUIRES(_mutex);
  void checkpoint_locked() DANNAN_REQUIRES(_mutex);
  void checkpoint_if_due_locked() DANNAN_REQUIRES(_mutex);
  // Visits records from after `cursor`, wrapping to the first once, until
  // `visit` returns false, `limit` are visited or it is back where it began.
  void scan_locked(const ClassLedger&, std::optional<ObjectId>& cursor, size_t limit,
                   const std::function<bool(const ObjectId&, std::span<const uint8_t>)>& visit) const DANNAN_REQUIRES(_mutex);
  void create_locked() DANNAN_REQUIRES(_mutex);
  void load_journal_locked() DANNAN_REQUIRES(_mutex);
  void load(size_t cache_bytes);

  const std::filesystem::path _root;
  const std::shared_ptr<const domain::StorageKey> _key;
  const dannan::status::ScopedLogger _logger;
  // Held across the log's fsync, a lookup's node read and a checkpoint's
  // writes.
  mutable dannan::contract::IoMutex _mutex;
  ClassLedger _data DANNAN_GUARDED_BY(_mutex);
  ClassLedger _control DANNAN_GUARDED_BY(_mutex);
  // Objects changed since the last checkpoint: what the tries hold unsaved.
  uint64_t _changed DANNAN_GUARDED_BY(_mutex) = 0;
  // claims.log: the add, release and prune frames since the last checkpoint.
  SealedJournal _journal DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dagda::ledger
