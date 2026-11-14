//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "dagda/contract/object_store.h"
#include "dagda/domain/domain.h"
#include "dagda/domain/storage_key.h"
#include "dagda/ledger/held_ledger.h"
#include "dagda/store/durability_domain.h"
#include "dagda/store/local_store_files.h"
#include "dagda/store/object_id.h"
#include "dagda/store/object_lock.h"
#include "dagda/store/presence_index.h"
#include "dagda/store/sealing.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/identity/node_identity.h"
#include "dannan/status/log.h"
#include "dannan/status/observation.h"
#include "dannan/supervise/startup.h"
#include "dannan/supervise/supervised.h"

namespace dagda::store {

// A record that was read in full and does not open (Macha 0.93.4): the bytes on
// the disk are not this object. Any other exception from a read says the read did
// not finish, and nothing about the object.
class ObjectCorrupt final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// An exclusive lock on a store's root (`.macha.lock`): Dannan's state lock
// under the store's name, so two processes, or two nodes in one, never open
// one store. Throws std::runtime_error if it is held.
inline constexpr std::string_view storage_lock_name = ".macha.lock";
class StorageLock final : public dannan::identity::StateLock {
 public:
  explicit StorageLock(const std::filesystem::path& root) : StateLock(root, storage_lock_name) {}
};

enum class LocalStoreMode : uint8_t {
  // Durable copies: barriers through a durability domain, byte accounting.
  authoritative,
  // A cache: no domain, no barriers, no accounting checkpoint.
  ephemeral,
};

// What a local store is configured with: plain values a host's own config
// reader fills (Macha's YAML keeps reading them), with Macha's defaults.
struct LocalStoreOptions {
  // Bytes the store may hold; non-zero.
  uint64_t limit = 0;
  // Free space kept on the filesystem; a put that would cross it is refused.
  uint64_t reserve_free = 0;
  // Objects at or below the threshold are records in packs, the small-object
  // path; larger ones are loose files. Both zero (no packs) or both set, the
  // target at least the threshold (requirement S3).
  size_t pack_threshold = 1024 * 1024;
  // Approximate: one record may push a pack slightly over.
  size_t pack_target_size = 64 * 1024 * 1024;
  // Where the store keeps its held ledger, on the state device; none keeps
  // none, and the store walks its objects at every start.
  std::filesystem::path ledger_dir{};
  size_t ledger_cache_bytes = 8ULL * 1024 * 1024;
  // The domain objects are sealed for: the domain in the associated data
  // (sealing.h); v1, Macha's, by default.
  SealingScheme sealing{};
  // This node's role in the store's domain (requirement D3). A blind store
  // takes no storage key and holds ciphertext only: put_sealed, get_sealed
  // and held_digest; put and get refuse.
  domain::Role role = domain::Role::member;
  // The key the held ledger (on the state device) is sealed under: the
  // node's node-local key (StorageKey::derive_node_local), whatever the
  // role, never the domain's. Required with a ledger_dir: a node without
  // custody has no such key and cannot keep a ledger.
  std::shared_ptr<const domain::StorageKey> ledger_key{};
  // A test's seam, empty otherwise: run on the appending thread once a pack
  // record is written and the pack's writer lock let go, before the index
  // lock is taken again.
  std::function<void(const ObjectId&)> after_pack_append{};

  // Loose objects only, as the cache and some tests keep them.
  static LocalStoreOptions loose_only(uint64_t limit) {
    LocalStoreOptions options;
    options.limit = limit;
    options.pack_threshold = 0;
    options.pack_target_size = 0;
    return options;
  }
};

// The node's one ledger cache, divided among the ledgers as Macha divides
// storage.ledger_cache (cluster/local_state.cpp:29-50): three eighths to the
// claims (both classes), a quarter to the namespace's reference counts, a
// sixteenth to the control store's held ledger, the rest to the DATA
// backends', which share it equally.
enum class LedgerShare : uint8_t { data_held, control_held, claims, references };
inline constexpr uint64_t default_ledger_cache_bytes = 64ULL * 1024 * 1024;
constexpr uint64_t ledger_cache_share(uint64_t total, LedgerShare share) noexcept {
  const auto claims = total / 8 * 3;
  const auto references = total / 4;
  const auto control_held = total / 16;
  switch (share) {
    case LedgerShare::claims:
      return claims;
    case LedgerShare::references:
      return references;
    case LedgerShare::control_held:
      return control_held;
    case LedgerShare::data_held:
      break;
  }
  return total - claims - references - control_held;
}

struct LocalStoreDiagnostics {
  uint64_t loose_reaffirmation_fast_paths = 0;
  uint64_t loose_reaffirmation_full_validations = 0;
  // Loose objects the start-up presence walk found (0 until it finishes).
  uint64_t presence_index_entries = 0;
  // Pack recovery at the last open: torn tails discarded, unreadable spans
  // skipped. Non-zero skipped figures mean lost objects, repaired from
  // replicas.
  uint64_t pack_recovery_truncated_tails = 0;
  uint64_t pack_recovery_skipped_regions = 0;
  uint64_t pack_recovery_skipped_bytes = 0;
  // Every fsync the store issued itself (accounting checkpoints, a compacted
  // pack and its directory); an object's durability is the domain's barrier,
  // never one of these.
  uint64_t own_syncs = 0;
  // Objects sealed for a put: none for a put refused for room.
  uint64_t seals = 0;
};

// Objects in one directory tree on one device (requirements S1 to S9, S16 to
// S18): loose files above the pack threshold, records in packs at or below
// it, each sealed under the domain's storage key with the domain and the id
// as the associated data (sealing.h), in the one layout's formats
// (local_format.h, Macha's bytes but for the tag).
//
// The packs are replayed at every open, a torn tail truncated and a damaged
// span skipped. Compaction is copy-on-write, one victim per call. A put is
// strict (durable on return); put_deferred returns the generation to await;
// a remove is lazily durable. Byte accounting keeps A/B checkpoints, so a
// clean restart walks nothing and a dirty one reconciles while it admits
// writes on the estimate. With a ledger directory, the held ledger is kept
// current with every change and, once seeded, is has()'s answer.
//
// Lock order: an object's lock, then the pack stream's, then the index
// mutex. The index mutex is memory work only: every device operation runs
// with it released. Thread-safe unless an operation says otherwise.
class LocalStore final : public contract::ObjectStore {
 public:
  // What the store runs on, each one per node and injected.
  struct Services {
    // The scan and the presence walk run here.
    dannan::supervise::SupervisedThreads& threads;
    // The default durability domain records its barriers here.
    dannan::status::Observations& observations;
    // Ticked as the accounting scan walks, so a long reconciliation is
    // progress, not a stall.
    dannan::supervise::StartupProgress& progress;
    dannan::status::ScopedLogger logger;
    // The default domain's batch window.
    const dannan::contract::TimeSource& time = dannan::contract::steady_time_source();
    // Dates recorded on disk: a pack record's touched_ms, a loose object's
    // modification time; and the ages compared against them.
    const dannan::contract::WallClock& wall = dannan::contract::system_wall_clock();
  };

  struct Cursor {
    std::optional<ObjectId> packed_after;
    bool packed_done = false;
    std::filesystem::recursive_directory_iterator iterator{};
    bool loose_initialized = false;
  };

  // A record as a member sealed it: the plaintext's size, the nonce, the tag
  // and the ciphertext. What a blind store holds and what either role moves
  // without opening.
  struct SealedObject {
    uint64_t plain_size = 0;
    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 16> tag{};
    Bytes ciphertext;
    bool operator==(const SealedObject&) const = default;
  };

  // Opens or creates the store at `root`. Without a domain an authoritative
  // store makes its own over `root` (500 ms window); an ephemeral one keeps
  // none. The key is the domain's; a blind store (options.role) takes none.
  // Throws std::invalid_argument for options that do not hold together, and
  // std::runtime_error if the packs or the accounting cannot be opened.
  // Replays the packs, then starts the scan and the presence walk as the
  // accounting and the ledger need them. Waits on the device.
  LocalStore(std::filesystem::path root, LocalStoreOptions options, std::shared_ptr<const domain::StorageKey> key, Services services,
             LocalStoreMode mode = LocalStoreMode::authoritative, std::shared_ptr<DurabilityDomain> durability_domain = {},
             LocalStoreFiles& files = posix_local_store_files());
  // Stops the walks, makes the last mutation durable and writes a clean
  // checkpoint.
  ~LocalStore();
  LocalStore(const LocalStore&) = delete;
  LocalStore& operator=(const LocalStore&) = delete;

  domain::Role role() const noexcept { return _role; }
  const SealingScheme& sealing() const noexcept { return _sealing; }

  // Strict: true once the object is durable here; false if admission refused
  // it (capacity or reserve). Throws std::runtime_error for bytes that are
  // not the id's ("object hash mismatch") or a device that failed, and
  // std::logic_error on a blind store, which cannot seal.
  bool put(const ObjectId&, std::span<const uint8_t>);
  // As put, durable once the returned generation is (0: already durable);
  // nothing if admission refused it.
  std::optional<uint64_t> put_deferred(const ObjectId&, std::span<const uint8_t>);
  // Stores a record a member sealed, as it is, under either role: strict,
  // true once durable, false if admission refused it; true at once for an
  // object already held. The id is taken to name the plaintext; a blind
  // store cannot check it, and a member does not open it here. Throws
  // std::invalid_argument for a record whose sizes disagree and
  // std::runtime_error for a device that failed.
  bool put_sealed(const ObjectId&, const SealedObject&);
  // The record as held, unopened; nothing if not held. Either role. Throws
  // std::runtime_error for a record that cannot be read.
  std::optional<SealedObject> get_sealed(const ObjectId&) const;
  // The ciphertext digest of the record held (sealing.h, blind_digest):
  // what a blind node compares with the digest a member bound to the id.
  // Nothing if not held.
  std::optional<dannan::identity::Hash256> held_digest(const ObjectId&) const;
  void durability_barrier(uint64_t required_generation, DurabilityUrgency = DurabilityUrgency::batchable);
  // Everything this store has done, immediately.
  void durability_barrier();
  uint64_t durable_generation() const;
  uint64_t durability_domain_id() const noexcept;

  // The plaintext, opened and checked against its id; nothing if not held.
  // Throws ObjectIntegrityError or std::runtime_error for bytes that do not
  // open, and std::logic_error on a blind store, which cannot open. Waits on
  // the device under the object's lock.
  std::optional<Bytes> get(const ObjectId&) const;
  // True once a put has installed the object; false while it is being
  // written and after a remove. Once seeded or warm it waits only on the
  // index mutex and the ledger and touches no DATA device; before then a miss
  // is checked on disk under the object's lock and an empty file is pruned.
  // Never verifies content: get() and valid() do.
  bool has(const ObjectId&) const noexcept override;
  // A member's copy reads back and opens to its id. A blind store's is a
  // whole record whose ciphertext is the recorded size: what it can tell
  // with no key (the digest check is the caller's, held_digest).
  bool valid(const ObjectId&) const noexcept;
  // From the held ledger once seeded. Without a ledger, or before its seed,
  // the identity is incomplete and `held` answers nothing.
  contract::HeldView held_view() const override;
  // The view's removal count alone. Waits on nothing.
  uint64_t held_removals() const noexcept { return _ledger ? _ledger->removals() : 0; }
  // Objects this store has stopped holding since it opened. Waits on nothing.
  uint64_t losses() const noexcept { return _losses.load(std::memory_order_acquire); }
  // Whether warm-up has finished and has() answers from the index alone.
  bool presence_authoritative() const DANNAN_EXCLUDES(_mutex);

  // True if it held the object. A packed object gets a tombstone, a loose one
  // is unlinked; neither waits for durability (requirement S17).
  bool remove(const ObjectId&);
  // Removes it only if untouched for at least `age` by the wall clock.
  bool remove_if_older_than(const ObjectId&, std::chrono::milliseconds age);
  std::vector<ObjectId> list() const;
  // The next held id after the cursor: packed ones first, in order, then the
  // loose ones as the directory walk finds them. `exhausted` once done.
  std::optional<ObjectId> next_object(Cursor&, bool& exhausted) const;

  // One slice of the check that the held ledger matches the disk: each of
  // `directories` objects/xx/yy directories is listed and compared with the
  // ledger's ids under that prefix. A file the ledger does not list is
  // recorded held; a listed id with no file is recorded gone (a loss, unless
  // it is packed). Complete once a pass over all 65,536 has wrapped; a
  // restart resumes where it stopped. A no-op until the ledger is seeded.
  // Single owner.
  struct VerifyResult {
    size_t directories = 0;
    uint64_t recorded = 0;
    uint64_t lost = 0;
    bool complete = false;
  };
  VerifyResult verify_step(size_t directories);

  // Untouched for longer than `age` by the wall clock.
  bool older_than(const ObjectId&, std::chrono::milliseconds age) const;
  // The object's file: its pack if packed, else where its loose file is or
  // would be.
  std::filesystem::path object_path(const ObjectId&) const;
  // Bytes on disk: a packed object's record, a loose object's file.
  uint64_t stored_size(const ObjectId&) const;
  std::filesystem::file_time_type last_write(const ObjectId&) const;
  // Marks the object used now: a touch record for a packed one, the
  // modification time for a loose one.
  void touch(const ObjectId&);
  bool is_packed(const ObjectId&) const;
  // Rewrites the pack with the most dead bytes, if any. False if it could
  // not finish (no room, stopped, or the old pack could not be unlinked).
  bool compact_packs(std::stop_token = {});

  uint64_t used() const noexcept { return _used.load(std::memory_order_relaxed); }
  uint64_t limit() const noexcept { return _limit; }
  bool scan_complete() const noexcept { return _scan_complete.load(std::memory_order_acquire); }
  LocalStoreDiagnostics diagnostics() const noexcept;

  // For a process that ends without this store's teardown (a forked child's
  // simulated crash): waits for the start-up scan and the presence walk to end
  // on their own, and joins them, so no finished thread is left unjoined. Asks
  // nothing to stop and writes nothing. Single owner; not with the destructor.
  void join_background_threads();

 private:
  struct PackEntry {
    std::filesystem::path file;
    uint64_t payload_offset = 0;
    uint64_t payload_size = 0;
    uint64_t plain_size = 0;
    uint64_t record_size = 0;
    uint64_t touched_unix_ms = 0;
    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 16> tag{};
  };

  struct LooseStamp {
    uint64_t device = 0;
    uint64_t inode = 0;
    uint64_t size = 0;
    int64_t modified_ns = 0;
    int64_t changed_ns = 0;
    auto operator<=>(const LooseStamp&) const = default;
  };

  struct VerifiedLoose {
    LooseStamp stamp;
    uint64_t sequence = 0;
  };

  enum class WriteDurability : uint8_t { immediate, deferred };

  std::filesystem::path path(const ObjectId&) const;
  // Journals and installs what the ledger has queued; a failure is the
  // caller's write failing.
  void flush_ledger() const;
  void seed_ledger();
  void start_scan();
  // The scan has failed: set, and its waiters woken, under _mutex.
  void mark_scan_failed() DANNAN_EXCLUDES(_mutex);
  void wait_for_accounting(dannan::contract::Lock&) const DANNAN_REQUIRES(_mutex);
  // wait_for_accounting with no object's lock held. Waits on the scan.
  void await_accounting() const DANNAN_EXCLUDES(_mutex);
  bool wait_for_accounting(dannan::contract::Lock&, std::stop_token, bool exact = false) const DANNAN_REQUIRES(_mutex);
  bool restore_accounting(bool* clean);
  void persist_accounting(uint64_t used, uint8_t operation, const ObjectId&, uint64_t size, bool durable);
  void sync_directory(const std::filesystem::path&);
  // Releases _mutex across the marker write.
  void ensure_accounting_dirty(dannan::contract::Lock&) DANNAN_REQUIRES(_mutex);
  void reap_durable_generations_locked() DANNAN_REQUIRES(_mutex);
  bool put_impl(const ObjectId&, std::span<const uint8_t>, WriteDurability, uint64_t*) DANNAN_EXCLUDES(_mutex, _pack_io_mutex);
  // Seals under the store's scheme. The caller holds the object's lock and
  // not _mutex.
  SealedObject seal(const ObjectId&, std::span<const uint8_t>) const;
  // The sealed record opened and checked against the id.
  Bytes open(const ObjectId&, const SealedObject&) const;
  // Stores a record not held yet, sealed already, as a loose file or a pack
  // record by its plaintext size; `generation` is the durability generation
  // to await (0 when none). Called with the object's lock and _mutex held,
  // the object known absent.
  bool store_sealed_locked(const ObjectId&, const SealedObject&, uint64_t& generation, dannan::contract::Lock&) DANNAN_REQUIRES(_mutex)
      DANNAN_EXCLUDES(_pack_io_mutex);
  // Store a new object; _mutex is released across the I/O.
  bool put_loose_locked(const ObjectId&, const SealedObject&, uint64_t& generation, dannan::contract::Lock&) DANNAN_REQUIRES(_mutex)
      DANNAN_EXCLUDES(_pack_io_mutex);
  bool put_packed_locked(const ObjectId&, const SealedObject&, uint64_t& generation, dannan::contract::Lock&) DANNAN_REQUIRES(_mutex)
      DANNAN_EXCLUDES(_pack_io_mutex);
  // The record held, unopened: a packed one's payload read at its entry, a
  // loose one's file decoded. Called with the object's lock and _mutex held;
  // _mutex is released across the read.
  std::optional<SealedObject> get_sealed_locked(const ObjectId&, dannan::contract::Lock&) const DANNAN_REQUIRES(_mutex);
  void scan(std::stop_token) DANNAN_EXCLUDES(_mutex);
  void warm_presence_index(std::stop_token) DANNAN_EXCLUDES(_mutex);
  // Constructor only, before any other thread exists, so it takes no lock.
  void rebuild_pack_index(bool truncate_incomplete_tail) DANNAN_NO_THREAD_SAFETY_ANALYSIS;
  // Releases _mutex across the read.
  std::optional<Bytes> get_packed_locked(const ObjectId&, dannan::contract::Lock&) const DANNAN_REQUIRES(_mutex);
  // Opens a loose object's file bytes: integrity-checked plaintext.
  Bytes open_loose(const ObjectId&, std::span<const uint8_t> file) const;
  // Releases _mutex and takes _pack_io_mutex across the append. A put
  // record carries the sealed object; a tombstone and a touch carry none.
  bool append_pack_record_locked(uint8_t type, const ObjectId&, const SealedObject*, uint64_t touched_ms, PackEntry*, uint64_t* record_size,
                                 dannan::contract::Lock&) DANNAN_REQUIRES(_mutex) DANNAN_EXCLUDES(_pack_io_mutex);
  void select_active_pack_locked(uint64_t next_record_size) DANNAN_REQUIRES(_pack_io_mutex);
  // Releases _mutex (keeping _pack_io_mutex) across the compaction I/O.
  bool compact_packs_locked(dannan::contract::Lock&) DANNAN_REQUIRES(_mutex, _pack_io_mutex);
  // Releases _mutex across the unlink.
  bool remove_locked(const ObjectId&, dannan::contract::Lock&) DANNAN_REQUIRES(_mutex) DANNAN_EXCLUDES(_pack_io_mutex);
  bool physical_space_available_locked(uint64_t need) const DANNAN_REQUIRES(_mutex);
  bool filesystem_space_available_for_reservations() const DANNAN_EXCLUDES(_mutex);
  static std::optional<LooseStamp> loose_stamp(const std::filesystem::path&);
  void remember_verified_loose_locked(const ObjectId&, const LooseStamp&) const DANNAN_REQUIRES(_mutex);
  void forget_verified_loose_locked(const ObjectId&) const DANNAN_REQUIRES(_mutex);
  // Removes a zero-byte loose file and forgets the object. Every object has
  // a fixed header, so an empty file is only left by a crash or external
  // truncation. Caller holds the object's lock, not _mutex. True if pruned.
  bool prune_empty_loose(const ObjectId&, const std::filesystem::path&) const DANNAN_EXCLUDES(_mutex);
  // has() from the pack and presence indexes, or none before warm-up. Waits
  // only on _mutex; its body is a no-I/O region, so taking a lock held across
  // I/O there does not compile under Clang.
  std::optional<bool> presence_from_index(const ObjectId&) const DANNAN_EXCLUDES(_mutex);
  // Now on the wall clock, as a file's modification time.
  std::filesystem::file_time_type wall_file_time() const;
  // A file time's age on the wall clock, in milliseconds (negative if ahead).
  int64_t age_ms(std::filesystem::file_time_type) const;

  // Fixed at construction.
  const std::filesystem::path _root;
  const std::filesystem::path _objects;
  const std::filesystem::path _packs;
  const std::filesystem::path _accounting_path;
  const uint64_t _limit;
  const uint64_t _reserve_free;
  const size_t _pack_threshold;
  const size_t _pack_target_size;
  // Null on a blind store.
  const std::shared_ptr<const domain::StorageKey> _key;
  const SealingScheme _sealing;
  const domain::Role _role;
  const LocalStoreMode _mode;
  dannan::supervise::SupervisedThreads& _threads;
  dannan::supervise::StartupProgress& _progress;
  const dannan::status::ScopedLogger _logger;
  const dannan::contract::WallClock& _wall;
  // Object and pack file I/O; called only with _mutex released.
  LocalStoreFiles& _files;
  const std::function<void(const ObjectId&)> _after_pack_append;
  ObjectLocks _object_locks;

  std::atomic<uint64_t> _used{0};
  mutable std::atomic<uint64_t> _losses{0};
  // The index mutex: memory work only. A holder that reaches the device
  // releases it first.
  mutable dannan::contract::Mutex _mutex;
  // Held across pack appends, append rollback and compaction's pack I/O. It
  // serialises the pack stream apart from _mutex, so pack crypto and I/O do
  // not exclude loose-object or index operations. Taken before _mutex, never
  // under it.
  mutable dannan::contract::IoMutex _pack_io_mutex DANNAN_ACQUIRED_BEFORE(_mutex);
  uint64_t _reserved_write_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  // What pack compaction must keep for replay. A put no longer live, superseded or
  // removed, stays in its pack until that pack is compacted away; while it does, the
  // remove record that keeps its object removed must outlive any compaction of the
  // remove's own pack. So: the packs (by sequence) holding a dead put of each id, and
  // the one remove record (its pack and size) that keeps each such id removed. Such a
  // remove is live bytes, not dead, so its pack is not compacted for it alone. Likewise
  // the newest touch of a live object whose put lies in another pack: it carries the
  // object's age until compaction rewrites the put with that age.
  struct NeededRecord {
    uint64_t pack = 0;
    uint64_t bytes = 0;
  };
  std::map<ObjectId, std::vector<uint64_t>> _dead_put_packs DANNAN_GUARDED_BY(_mutex);
  std::map<uint64_t, std::vector<ObjectId>> _dead_puts_in_pack DANNAN_GUARDED_BY(_mutex);
  std::map<ObjectId, NeededRecord> _needed_removes DANNAN_GUARDED_BY(_mutex);
  std::map<ObjectId, NeededRecord> _needed_touches DANNAN_GUARDED_BY(_mutex);
  // The live put of the id, in `pack`, is superseded or removed: its touch is dead.
  void note_dead_put_locked(const ObjectId& id, const std::filesystem::path& pack) DANNAN_REQUIRES(_mutex);
  // A touch record of `bytes` in `pack` for the live id: needed, an earlier one dead.
  void note_touch_locked(const ObjectId& id, const std::filesystem::path& pack, uint64_t bytes) DANNAN_REQUIRES(_mutex);
  // A touch record at `touched_ms` appended or replayed: the live entry takes its
  // age, and the record is needed or dead as above.
  void note_touch_record_locked(const ObjectId& id, uint64_t touched_ms, const std::filesystem::path& pack, uint64_t bytes) DANNAN_REQUIRES(_mutex);
  // The id's needed touch, if any, is dead from here.
  void drop_touch_locked(const ObjectId& id) DANNAN_REQUIRES(_mutex);
  // A remove record of `bytes` in `pack`: needed while a dead put of the id remains,
  // else dead at once.
  void note_remove_locked(const ObjectId& id, const std::filesystem::path& pack, uint64_t bytes) DANNAN_REQUIRES(_mutex);
  // A put of the id: no earlier remove is needed any more.
  void note_put_locked(const ObjectId& id) DANNAN_REQUIRES(_mutex);
  // A pack compacted away: its dead puts are gone, and a remove kept only for them
  // becomes dead.
  void forget_pack_locked(uint64_t sequence) DANNAN_REQUIRES(_mutex);
  // Live bytes the needed removes and touches keep in each pack, by pack path.
  std::map<std::filesystem::path, uint64_t> needed_record_bytes_locked() const DANNAN_REQUIRES(_mutex);
  static constexpr size_t verified_loose_limit = 4096;
  mutable std::map<ObjectId, VerifiedLoose> _verified_loose DANNAN_GUARDED_BY(_mutex);
  // Loose objects present: published when a put installs the file, filled
  // from the object directory at start, forgotten on remove. Once warm-up has
  // listed the store, has() answers from it alone, with no per-object lock
  // and no disk; a cold stat per object is too slow on a busy disk. About 40
  // bytes per object.
  mutable PresenceIndex _presence DANNAN_GUARDED_BY(_mutex);
  mutable std::deque<std::pair<uint64_t, ObjectId>> _verified_loose_order DANNAN_GUARDED_BY(_mutex);
  mutable uint64_t _verified_loose_sequence DANNAN_GUARDED_BY(_mutex) = 0;
  std::atomic<uint64_t> _loose_reaffirmation_fast_paths{0};
  std::atomic<uint64_t> _loose_reaffirmation_full_validations{0};
  mutable std::condition_variable_any _accounting_cv;
  // Which objects this store holds, kept across restarts: once seeded, the
  // answer to has() and the reason a start walks nothing.
  std::unique_ptr<ledger::HeldLedger> _ledger;
  std::atomic<uint64_t> _presence_index_entries{0};
  std::atomic<uint64_t> _pack_recovery_truncated_tails{0};
  std::atomic<uint64_t> _pack_recovery_skipped_regions{0};
  std::atomic<uint64_t> _pack_recovery_skipped_bytes{0};
  // seal() is const: its count is bookkeeping, not state.
  mutable std::atomic<uint64_t> _seals{0};
  std::atomic<uint64_t> _own_syncs{0};
  std::atomic_bool _scan_complete{false};
  std::atomic_bool _scan_failed{false};
  std::atomic_bool _accounting_trusted{false};
  // A dirty checkpoint's `used` is an estimate while the reconciling scan
  // runs; puts and removes are admitted against it rather than waiting.
  std::atomic_bool _accounting_estimate{false};
  // Opened by the constructor, closed by the destructor.
  int _accounting_fd = -1;
  // Written by persist_accounting() outside _mutex: only by the constructor,
  // the destructor, or the holder of _accounting_dirty_in_progress.
  uint64_t _accounting_sequence = 0;
  unsigned _accounting_slot = 0;
  bool _accounting_dirty DANNAN_GUARDED_BY(_mutex) = false;
  bool _accounting_dirty_in_progress DANNAN_GUARDED_BY(_mutex) = false;
  mutable std::condition_variable _accounting_state_cv;
  // Set by the constructor, fixed afterwards.
  std::shared_ptr<DurabilityDomain> _durability_domain;
  uint64_t _last_mutation_generation DANNAN_GUARDED_BY(_mutex) = 0;
  std::map<ObjectId, uint64_t> _provisional_generations DANNAN_GUARDED_BY(_mutex);
  std::deque<std::pair<uint64_t, ObjectId>> _provisional_order DANNAN_GUARDED_BY(_mutex);

  std::map<ObjectId, PackEntry> _packed DANNAN_GUARDED_BY(_mutex);
  mutable std::map<std::filesystem::path, size_t> _active_pack_readers DANNAN_GUARDED_BY(_mutex);
  mutable std::condition_variable _pack_readers_cv;
  // Pack records written but not yet in _packed: counted under _pack_io_mutex
  // once a record is in its pack, and no longer once the appender holds _mutex
  // again, which it keeps until it has changed the index. Compaction holds
  // _pack_io_mutex, so no more can start, and waits under _mutex for none
  // before it reads the index (compact_packs_locked).
  std::atomic<size_t> _unindexed_appends{0};
  std::condition_variable _appends_indexed_cv;
  uint64_t _pack_dead_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _next_pack_sequence DANNAN_GUARDED_BY(_pack_io_mutex) = 1;
  std::filesystem::path _active_pack DANNAN_GUARDED_BY(_pack_io_mutex);
  uint64_t _active_pack_size DANNAN_GUARDED_BY(_pack_io_mutex) = 0;

  // Started by the constructor, joined first by the destructor: declared
  // last so they go before anything they use.
  std::jthread _scan_thread;
  // Fills _presence from object directory names at start (readdir only, no
  // stat).
  std::jthread _presence_thread;
};

}  // namespace dagda::store
