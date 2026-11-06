//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/heads/codec.h"
#include "dagda/heads/record.h"
#include "dagda/heads/sealed_file.h"
#include "dagda/heads/tree.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/work.h"
#include "dannan/status/log.h"

namespace dagda::heads {

// A record with its decoded state, shared: a reader keeps it past any eviction.
struct Materialization {
  Record record;
  State state;
  HeadFields fields;
  // Conservative deep weight of the record and state, computed off the lock.
  uint64_t resident_bytes = 0;
};

struct ReplicaDiagnostics {
  uint64_t historical_requests = 0;
  uint64_t historical_reconstructions = 0;
  uint64_t historical_deltas_applied = 0;
  uint64_t materialization_cache_hits = 0;
  uint64_t materialization_cache_misses = 0;
  uint64_t materialization_cache_evictions = 0;
  size_t materialization_cache_entries = 0;
  uint64_t materialization_cache_bytes = 0;
  uint64_t materialization_cache_limit_bytes = 0;
  uint64_t history_records = 0;
  uint64_t history_file_bytes = 0;
  uint64_t history_resident_payload_bytes = 0;
  uint64_t accepted_head_persistence_writes = 0;
  uint64_t accepted_head_persistence_bytes = 0;
  uint64_t accepted_head_persistence_failures = 0;
};

// One node's replica of one head space on disk: Macha's MetadataReplica at
// 9a08846, its owner's bytes behind a Codec and its files under v19's split (the
// plan's section 4.1). The behaviour is Macha's, including what looks odd (section
// 7): this is what "unchanged" is measured against.
//
// Files, in `directory`: history.log, checkpoint.meta and journal.log are the
// domain's content, under its key; heads.meta, author.meta and set-aside.meta are
// this node's standing, under its node-local key; recovery.required and
// author-chain-broken are text markers, whose authority is their existence. A
// quarantine is `<name>.corrupt.<stamp>`.
//
// History holds ancestry and frame offsets in memory and payloads on disk. Heads
// are a set, kept to what nothing else in the set descends from; the preferred
// head is the highest generation, then the higher hash; nothing but a certificate
// gives a record authority. Opening reads frames lazily: a whole body is hashed and
// not decoded, a delta is checked and not applied, and a head is only materialised
// when it can become authority.
//
// The journal has a reader and no writer: Macha's last version wrote none, and a
// state may still hold frames from an older one (the plan's Q3).
//
// A long walk calls `pause` between its steps (`ReplicaOptions::pause`). Only the
// walks that change nothing if abandoned do: opening, materialise() and the two
// public ancestry questions. A pause under a head-set change would leave memory
// behind the disk. contract::MaintenanceStopping goes through every catch here; in
// particular it never turns into a recovery from the seed.
//
// Thread-safe. Lock order: the durable mutation mutex or the materialisation
// mutex, then the replica's mutex. See the mutexes for what is held across what.
struct ReplicaOptions {
  std::filesystem::path directory;
  // The domain's key, for content, and the node's own, for its standing.
  std::shared_ptr<const domain::StorageKey> content_key;
  std::shared_ptr<const domain::StorageKey> local_key;
  ReplicaMagics magics;
  AadRule aad;
  std::shared_ptr<const Codec> codec;
  // The last committed record from the cache: read only if the primary state
  // fails or a recovery marker stands, and never authority.
  std::optional<Record> recovery_seed;
  // A founder holds genesis as an accepted head; a joiner (bootstrap peers
  // configured) never does.
  bool accept_pristine_genesis_authority = true;
  uint64_t materialization_cache_limit_bytes = 128ULL * 1024 * 1024;
  // Makes the node store a delta's replay writes to and reads from, one per apply.
  // None: a delta that needs a tree is refused by the codec.
  std::function<std::unique_ptr<TreeNodeStore>()> tree_store;
  const dannan::contract::TimeSource* time = &dannan::contract::steady_time_source();
  std::optional<dannan::status::ScopedLogger> logger;
  // A long walk's stop: may throw contract::MaintenanceStopping.
  TreePause pause;
  CostCounters* costs = nullptr;
  // Replaces a whole file durably; dannan::identity::durable_replace_file unless a
  // test holds one. Called with no reader-visible lock held for a checkpoint.
  std::function<void(const std::filesystem::path&, std::string_view)> replace_file;
  // Opens history.log to read at open, once per stream wanted; a plain file unless a
  // test holds a disk that stops answering.
  std::function<std::unique_ptr<std::istream>(const std::filesystem::path&)> history_reader;
};

class Replica {
 public:
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::thread_safe;
  // Replaying a tree-backed delta writes nodes and fetches what it reads.
  static constexpr dannan::sched::Waits open_waits = dannan::sched::local_waits | dannan::sched::Waits::network;
  static constexpr dannan::sched::Waits read_waits = dannan::sched::Waits::locks;
  static constexpr dannan::sched::Waits history_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::Waits materialize_waits = dannan::sched::local_waits | dannan::sched::Waits::network;
  static constexpr dannan::sched::Waits commit_waits = dannan::sched::local_waits | dannan::sched::Waits::network;
  static constexpr dannan::sched::Waits author_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::Waits truncate_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::Waits atomic_waits = dannan::sched::Waits::none;

  // Throws, naming the file, when the primary state fails and there is no valid
  // seed. Waits as open_waits.
  explicit Replica(ReplicaOptions options);
  Replica(const Replica&) = delete;
  Replica& operator=(const Replica&) = delete;

  Record current() const DANNAN_EXCLUDES(_mutex);
  Record committed() const DANNAN_EXCLUDES(_mutex);
  HeadIdentity current_identity() const DANNAN_EXCLUDES(_mutex);
  HeadIdentity committed_identity() const DANNAN_EXCLUDES(_mutex);
  uint64_t generation() const DANNAN_EXCLUDES(_mutex);
  // atomic_waits.
  uint64_t committed_generation() const noexcept { return _committed_generation.load(std::memory_order_acquire); }
  bool recovery_required() const DANNAN_EXCLUDES(_mutex);
  // Clears the recovery marker; a caller who has recovered from peers says so.
  void mark_recovered() DANNAN_EXCLUDES(_mutex);

  // The dot for this node's next mutation on a head with `head_clock`, durable
  // before it returns. One author's commits form a chain: when that cannot be
  // shown the node takes a new author id and starts again at 1, so a clock entry
  // always means "every mutation of that author up to here". A corrupt author.meta
  // throws: Macha has no recovery for it (its D3).
  Dot reserve_mutation_dot(const NodeId& node, const HeadClock& head_clock) DANNAN_EXCLUDES(_mutex);
  // A commit carrying this dot was accepted here. Memory only until the next
  // reservation (R9).
  void note_author_accepted(const Dot& dot) DANNAN_EXCLUDES(_mutex);
  // This node's author ids, the current one first.
  std::vector<NodeId> author_ids(const NodeId& node, const HeadClock& head_clock) DANNAN_EXCLUDES(_mutex);

  std::optional<HistoryEntry> history_entry(const Hash256& hash) const DANNAN_EXCLUDES(_mutex);
  std::optional<HistoryLinks> history_links(const Hash256& hash) const DANNAN_EXCLUDES(_mutex);
  bool history_contains(const Hash256& hash) const DANNAN_EXCLUDES(_mutex);
  // `hash` as a whole body, for a peer to re-anchor a head it cannot reconstruct;
  // nothing if this replica cannot materialise it either.
  std::optional<HistoryEntry> full_history_record(const Hash256& hash) const DANNAN_EXCLUDES(_mutex);
  // Live repair: appends a whole `entry` as a new anchor, indexed over a frame that
  // will not replay (opening prefers the whole frame). False if `entry` is not a
  // valid whole record.
  bool reanchor_history(const HistoryEntry& entry) DANNAN_EXCLUDES(_mutex);
  // Accepted heads flagged as not reconstructable, for the live-repair driver.
  std::vector<Hash256> unreconstructable_heads() const DANNAN_EXCLUDES(_mutex);

  // Sets an accepted head aside; false when it is the only head not set aside, as
  // a node always keeps a head to work on. A set-aside is this run's alone (R8);
  // only when it first happened persists.
  bool set_aside(const Hash256& hash, uint64_t now_unix_ms) DANNAN_EXCLUDES(_mutex, _durable_mutex);
  // Drops every head set aside for `horizon`, and returns how many went.
  size_t expire_set_aside(uint64_t now_unix_ms, std::chrono::milliseconds horizon) DANNAN_EXCLUDES(_mutex, _durable_mutex);
  void clear_set_aside() DANNAN_EXCLUDES(_mutex, _durable_mutex);
  // The highest generation among heads set aside, zero when none. atomic_waits.
  uint64_t set_aside_generation() const noexcept { return _set_aside_generation.load(std::memory_order_acquire); }
  // accepted_heads() without those set aside.
  std::vector<Record> usable_heads() const DANNAN_EXCLUDES(_mutex);
  std::vector<HeadIdentity> usable_head_identities() const DANNAN_EXCLUDES(_mutex);
  // Moves whenever the head set or what is set aside may have changed: a reader
  // that sees it unchanged holds a current answer. atomic_waits.
  uint64_t heads_revision() const noexcept { return _heads_revision.load(std::memory_order_acquire); }

  // Takes a peer's history entry, checked against its hash; idempotent. False for
  // one that does not hold, and for more than merge_parents_limit parents.
  bool import_history(const HistoryEntry& entry) DANNAN_EXCLUDES(_mutex, _durable_mutex);
  // Stores a commit, as a delta when one is given and its parent is here. A stored
  // commit is not yet a head.
  bool store_commit(const Record& record, std::span<const uint8_t> delta = {}) DANNAN_EXCLUDES(_mutex, _durable_mutex);
  // Accepts a stored commit under a certificate, and moves the heads as it says.
  // `heads_changed` is decided under the lock, so announce from it rather than by
  // comparing copies. A held head under a certificate adding nothing costs a lookup.
  bool accept_commit(const Acceptance& acceptance, bool* heads_changed = nullptr) DANNAN_EXCLUDES(_mutex, _durable_mutex);

  std::vector<Acceptance> accepted_head_certificates() const DANNAN_EXCLUDES(_mutex);
  // A head that cannot be reconstructed is excluded for its cooldown, not thrown.
  std::vector<Record> accepted_heads() const DANNAN_EXCLUDES(_mutex);
  std::optional<Acceptance> acceptance(const Hash256& hash) const DANNAN_EXCLUDES(_mutex);
  bool history_is_ancestor(const Hash256& ancestor, const Hash256& descendant) const DANNAN_EXCLUDES(_mutex);
  std::optional<Hash256> history_common_ancestor(const Hash256& left, const Hash256& right) const DANNAN_EXCLUDES(_mutex);
  std::optional<Record> historical(const Hash256& hash) const DANNAN_EXCLUDES(_mutex);
  // The record and state at `hash`, one reconstruction at a time; a miss holds one
  // state and one delta body at a time, and caches only the result.
  std::shared_ptr<const Materialization> materialized(const Hash256& hash) const DANNAN_EXCLUDES(_mutex, _compute_mutex);
  ReplicaDiagnostics diagnostics() const DANNAN_EXCLUDES(_mutex);

  // Checkpoints and empties the journal past 128 records or 8 MiB.
  void compact() DANNAN_EXCLUDES(_mutex, _durable_mutex);
  // Re-roots history at the sole accepted head once it is the committed record and
  // a threshold is met. Never with two heads. Each node does so alone.
  bool compact_history_if_safe(size_t record_threshold = 256, uint64_t byte_threshold = 64ULL * 1024 * 1024) DANNAN_EXCLUDES(_mutex, _durable_mutex);

 private:
  struct HistoryIndexEntry {
    uint64_t generation = 0;
    Hash256 previous{};
    Hash256 hash{};
    bool previous_known = false;
    std::vector<Hash256> merge_parents;
    HistoryEntry::Body body = HistoryEntry::Body::full;
    uint64_t file_offset = 0;
    uint64_t frame_bytes = 0;
  };
  struct MaterializedEntry {
    std::shared_ptr<const Materialization> value;
    uint64_t last_used = 0;
    uint64_t bytes = 0;
  };
  struct Author {
    NodeId id{};
    // Highest sequence handed out, and highest this node has had accepted.
    uint64_t reserved = 0;
    uint64_t accepted = 0;
    // Earlier ids of this node, newest first.
    std::vector<NodeId> past;
  };
  // Declared first in a function that may change the head set, so the revision
  // moves once the change is in place and any lock released.
  struct RevisionBump {
    std::atomic_uint64_t& revision;
    explicit RevisionBump(std::atomic_uint64_t& to) : revision(to) {}
    ~RevisionBump() { revision.fetch_add(1, std::memory_order_release); }
  };

  void open(std::optional<Record> seed) DANNAN_REQUIRES(_mutex);
  std::optional<Record> valid_seed(const std::optional<Record>& seed) const;
  void set_committed_locked(Record record) DANNAN_REQUIRES(_mutex);
  void pause_walk() const;
  void replace_file(const std::filesystem::path& path, std::span<const uint8_t> bytes) const;
  bool valid_record_counted(const Record& record) const;
  State decode_counted(std::span<const uint8_t> payload) const;
  Bytes encode_after_counted(std::span<const uint8_t> delta, const State& state) const;
  State apply_delta(const State& parent, std::span<const uint8_t> delta) const;
  std::shared_ptr<const Materialization> make_materialization(Record record, State state) const;
  std::vector<Hash256> record_parents(const Record& record) const;

  void persist_checkpoint(const Record& record);
  std::optional<Record> load_checkpoint() const;
  void load_journal() DANNAN_REQUIRES(_mutex);
  Bytes encode_history_frame(const HistoryEntry& entry) const;
  HistoryEntry read_history_entry(const HistoryIndexEntry& index) const;
  static HistoryIndexEntry index_history_entry(const HistoryEntry& entry, uint64_t file_offset, uint64_t frame_bytes);
  void write_history_frame(std::span<const uint8_t> bytes) const;
  void append_history(const HistoryEntry& entry) DANNAN_REQUIRES(_mutex);
  void load_history() DANNAN_REQUIRES(_mutex);
  void load_heads() DANNAN_REQUIRES(_mutex);
  void persist_heads_locked() DANNAN_REQUIRES(_mutex);
  void ensure_history_root(const Record& record) DANNAN_REQUIRES(_mutex);
  void adopt_committed_head_locked() DANNAN_REQUIRES(_mutex);
  bool prune_accepted_heads_locked() DANNAN_REQUIRES(_mutex);
  bool accepted_head_is_ancestor_locked(const Hash256& ancestor, const Hash256& descendant) const DANNAN_REQUIRES(_mutex);
  bool acceptance_matches_record_policy_locked(const Acceptance& acceptance, const Materialization& materialization) const DANNAN_REQUIRES(_mutex);
  void refresh_materialized_head_locked() DANNAN_REQUIRES(_mutex);
  // The head refreshed in memory; when the committed head moved, the journal is
  // emptied and the record returned for the caller to write as the checkpoint
  // after releasing the replica's mutex, so no reader waits on a checkpoint write.
  std::optional<Record> refresh_materialized_head_deferred_locked() DANNAN_REQUIRES(_mutex);
  bool refresh_materialized_head_in_memory_locked() DANNAN_REQUIRES(_mutex);
  void note_set_aside_locked() DANNAN_REQUIRES(_mutex);
  void load_set_aside_since_locked() DANNAN_REQUIRES(_mutex);
  void persist_set_aside_since_locked() DANNAN_REQUIRES(_mutex);
  HistoryEntry history_for_current(std::span<const uint8_t> delta = {}) DANNAN_REQUIRES(_mutex);
  // The cached value for `record`, decoded and cached if it has none.
  std::shared_ptr<const Materialization> cache_materialization_locked(const Record& record) const DANNAN_REQUIRES(_mutex);
  // The cached value for `value`'s record, else `value`, cached if it fits: only
  // current and committed are pinned; over budget and unpinned it is returned uncached.
  std::shared_ptr<const Materialization> cache_materialization_locked(std::shared_ptr<const Materialization> value) const DANNAN_REQUIRES(_mutex);
  std::shared_ptr<const Materialization> materialized_locked(const Hash256& target) const DANNAN_REQUIRES(_mutex);
  std::optional<Record> historical_locked(const Hash256& target) const DANNAN_REQUIRES(_mutex);
  bool history_is_ancestor_locked(const Hash256& ancestor, const Hash256& descendant, bool may_pause) const DANNAN_REQUIRES(_mutex);
  std::optional<Hash256> history_common_ancestor_locked(const Hash256& left, const Hash256& right) const DANNAN_REQUIRES(_mutex);
  void reset_checkpoint(const Record& record) DANNAN_REQUIRES(_mutex);
  void reset_checkpoint_journal_locked() DANNAN_REQUIRES(_mutex);
  void recover_from_seed(const Record& seed, const std::string& reason) DANNAN_REQUIRES(_mutex);
  void load_author_locked(const NodeId& node, const HeadClock& head_clock) DANNAN_REQUIRES(_mutex);
  void persist_author_locked() DANNAN_REQUIRES(_mutex);
  void rotate_author_locked(std::string_view why) DANNAN_REQUIRES(_mutex);
  // Arms the cooldown, logs once per episode, and returns the diagnosis.
  std::string flag_unreconstructable_locked(const Hash256& hash, dannan::contract::TimePoint now, std::string_view context) const DANNAN_REQUIRES(_mutex);
  // Walks the delta chain and names the first break.
  std::string diagnose_unreconstructable_locked(const Hash256& hash) const DANNAN_REQUIRES(_mutex);

  static constexpr std::chrono::seconds unreconstructable_retry_cooldown{30};

  const std::filesystem::path _checkpoint_path;
  const std::filesystem::path _journal_path;
  const std::filesystem::path _history_path;
  const std::filesystem::path _heads_path;
  const std::filesystem::path _author_path;
  // Present from the moment the head set is replaced until the next mutation takes
  // a new author id.
  const std::filesystem::path _author_chain_broken_path;
  const std::filesystem::path _set_aside_path;
  const std::filesystem::path _recovery_path;
  const std::shared_ptr<const domain::StorageKey> _content_key;
  const std::shared_ptr<const domain::StorageKey> _local_key;
  const ReplicaMagics _magics;
  const Bytes _checkpoint_aad;
  const Bytes _journal_aad;
  const Bytes _history_aad;
  const Bytes _heads_aad;
  const Bytes _author_aad;
  const Bytes _set_aside_aad;
  const std::shared_ptr<const Codec> _codec;
  const Record _genesis;
  const bool _accept_pristine_genesis_authority;
  const uint64_t _materialized_limit_bytes;
  const std::function<std::unique_ptr<TreeNodeStore>()> _tree_store;
  const dannan::contract::TimeSource& _time;
  const dannan::status::ScopedLogger _logger;
  const TreePause _pause;
  CostCounters* const _costs;
  const std::function<void(const std::filesystem::path&, std::string_view)> _replace_file;
  const std::function<std::unique_ptr<std::istream>(const std::filesystem::path&)> _history_reader;

  // Serialises durable mutations whose history append and fsync run without
  // _mutex; held across them.
  mutable dannan::contract::IoMutex _durable_mutex DANNAN_ACQUIRED_BEFORE(_mutex);
  // Serialises cache-miss computation without blocking _mutex; held across history
  // reads and a replay's tree work.
  mutable dannan::contract::IoMutex _compute_mutex DANNAN_ACQUIRED_BEFORE(_mutex);
  // Held across journal resets, heads writes and their syncs, and history reads.
  mutable dannan::contract::IoMutex _mutex;

  Record _current DANNAN_GUARDED_BY(_mutex);
  Record _committed DANNAN_GUARDED_BY(_mutex);
  // _committed.generation, readable without the mutex.
  std::atomic_uint64_t _committed_generation{0};
  std::map<Hash256, HistoryIndexEntry> _history DANNAN_GUARDED_BY(_mutex);
  std::map<Hash256, Acceptance> _accepted_heads DANNAN_GUARDED_BY(_mutex);
  // Accepted heads this node cannot merge with its own now, with their generations.
  // They stay accepted and are still offered to peers, but never become committed.
  std::map<Hash256, uint64_t> _set_aside DANNAN_GUARDED_BY(_mutex);
  // When each head was first set aside, on this node's wall clock; kept across
  // restarts, so a head whose content never arrives is dropped after the horizon.
  std::map<Hash256, uint64_t> _set_aside_since DANNAN_GUARDED_BY(_mutex);
  bool _set_aside_since_loaded DANNAN_GUARDED_BY(_mutex) = false;
  std::atomic_uint64_t _set_aside_generation{0};
  mutable std::atomic_uint64_t _heads_revision{1};
  // Per head, when a broken head may be tried again, so callers that catch and
  // retry cannot spin. Meanwhile the head is treated as absent.
  mutable std::map<Hash256, dannan::contract::TimePoint> _unreconstructable_retry_at DANNAN_GUARDED_BY(_mutex);
  std::optional<HistoryEntry> _pending_history DANNAN_GUARDED_BY(_mutex);
  bool _author_loaded DANNAN_GUARDED_BY(_mutex) = false;
  Author _author DANNAN_GUARDED_BY(_mutex);
  size_t _journal_records DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _journal_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  size_t _history_records DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _history_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  bool _recovery_required DANNAN_GUARDED_BY(_mutex) = false;
  // True only inside the constructor, where a pause under the mutex abandons
  // nothing: the replica never existed.
  bool _opening DANNAN_GUARDED_BY(_mutex) = true;
  mutable std::map<Hash256, MaterializedEntry> _materialized DANNAN_GUARDED_BY(_mutex);
  mutable uint64_t _materialized_clock DANNAN_GUARDED_BY(_mutex) = 0;
  mutable uint64_t _materialized_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  mutable std::atomic_uint64_t _historical_requests{0};
  mutable std::atomic_uint64_t _historical_reconstructions{0};
  mutable std::atomic_uint64_t _historical_deltas_applied{0};
  mutable std::atomic_uint64_t _cache_hits{0};
  mutable std::atomic_uint64_t _cache_misses{0};
  mutable std::atomic_uint64_t _cache_evictions{0};
  std::atomic_uint64_t _persistence_writes{0};
  std::atomic_uint64_t _persistence_bytes{0};
  std::atomic_uint64_t _persistence_failures{0};
};

}  // namespace dagda::heads
