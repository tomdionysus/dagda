//
// Dagda - The Distributed Object Store
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
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#include "dagda/contract/id_lookup.h"
#include "dagda/contract/object_store.h"
#include "dagda/domain/storage_key.h"
#include "dagda/ledger/trie_union.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/store/durability_domain.h"
#include "dagda/store/durability_ticket.h"
#include "dagda/store/io_pressure.h"
#include "dagda/store/local_store.h"
#include "dagda/store/object_id.h"
#include "dagda/store/object_lock.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/identity/node_id.h"

namespace dagda::store {

// One DATA backend: Macha's storage.data entry (config.hpp:23-30).
struct StorageBackendOptions {
  std::filesystem::path path{};
  // Most authoritative DATA bytes; control objects never count.
  uint64_t limit = 0;
  // Free space kept for the OS, control state and crash recovery; DATA
  // admission stops before crossing it.
  uint64_t reserve_free = 0;
};

// What a pool is configured with, Macha's defaults: packs at or below 1 MiB
// in 64 MiB packs (storage.packing), a 500 ms durability window, and the DATA
// backends' share of a 64 MiB ledger cache, divided among them.
struct StoragePoolOptions {
  std::vector<StorageBackendOptions> backends{};
  std::chrono::milliseconds durability_batch_window{500};
  size_t pack_threshold = 1024 * 1024;
  size_t pack_target_size = 64 * 1024 * 1024;
  uint64_t ledger_cache_bytes = ledger_cache_share(default_ledger_cache_bytes, LedgerShare::data_held);
  // The pool's domain, as its backends are sealed and marked; v1 (Macha's)
  // by default. A backend made for one domain is refused by a pool of
  // another.
  SealingScheme sealing{};
  // This node's role in the domain: a blind pool takes no storage key.
  domain::Role role = domain::Role::member;
  // The key every backend's held ledger is sealed under: the node-local key
  // (StorageKey::derive_node_local), whatever the role, never the domain's.
  // Required: a node without custody cannot run a pool.
  std::shared_ptr<const domain::StorageKey> ledger_key{};
  // What the service monitor counts as a slow device, set before any operation it
  // could observe.
  DiskServiceThresholds service_thresholds{};
  // Called each time a backend's path is found to be a directory, before anything
  // on it is read or written: the owner locks the root there (a node holds each
  // root's StorageLock), so a disk mounted after start is held from the moment it
  // comes online. Throws if another holds it, which leaves the backend offline with
  // the reason. Never called for a path that is not a directory: a missing one is a
  // disk not mounted, and nothing is made in its place. None: nothing is locked.
  std::function<void(const std::filesystem::path&)> lock_root{};
  // A test's device: called before each barrier's sync, and what it throws
  // fails the barrier as the device would. None in service.
  std::function<void()> before_sync{};
};

// <state>/durability-failures: one line a latched barrier failure, "<epoch>
// <domain> <durable generation>". A ticket of that process and domain past that
// generation is never re-derived: after a failed sync the failed pages stay
// cached and clean until a reboot, so no read in the same boot proves the disk
// holds them, and a later incarnation's domain has no latch of its own.
inline constexpr std::string_view durability_failures_name = "durability-failures";

// A backend's marker, `.macha.backend` in its root, mirrored under
// <state>/backend-identities/<SHA-256 of the path>.id: "dagda-backend-v19
// <node> <token> <domain> <check>", the check being backend_key_check under
// the domain key, or "blind" when a blind node adopted the disk with no key
// to check with. The migration (migrate/) writes these for Macha's disks.
inline constexpr std::string_view backend_marker_name = ".macha.backend";
inline constexpr std::string_view backend_marker_v19 = "dagda-backend-v19";
inline constexpr std::string_view backend_blind_check = "blind";
// The first sixteen bytes of HMAC-SHA256 under the domain key over
// "dagda/backend-key-check/19" || domain || token, as hex: a node that lacks
// the key is refused at open and learns nothing of the key from it.
std::string backend_key_check(const domain::StorageKey& key, const domain::DomainId& domain, const dannan::identity::NodeId& token);
std::string backend_marker_text(const SealingScheme& sealing, const dannan::identity::NodeId& node, const dannan::identity::NodeId& token,
                                const std::string& check);
std::filesystem::path backend_identity_path(const std::filesystem::path& state, const std::filesystem::path& backend);
// The refusal of a backend whose marker is layout v18 (Macha's), naming
// dagda-migrate and the state path; nothing for any other backend, marked or
// not. A node checks every backend before it opens the pool, so a v18 disk
// stops the node rather than leaving one backend offline. Waits on the
// device; thread-safe.
std::optional<std::string> backend_refusal_v18(const std::filesystem::path& state, const std::filesystem::path& backend);

// A durability ticket naming another process, or a domain or backend
// incarnation that is gone: it satisfies nothing here (protocol 23 answers
// it 409 epoch_changed).
class StaleDurabilityTicket : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// DATA over several backends, each a LocalStore on its own disk with its own
// limit and reserve (requirements S10, S12 to S15, L4). An object ranks the
// backends by capacity placement under the store's scheme (placement.h,
// Macha's salts), each backend named by its disk's identity token and
// weighted by its limit; a put goes to the first that takes it, so a refused
// put falls through to the next, and a backend that throws is taken offline.
// Rebalance moves each object onto its preferred backend.
//
// A backend is a directory holding a marker (backend_marker_text, above),
// mirrored under <state>/backend-identities/. A disk of another node, the
// wrong disk in a known place, a disk of another domain, a layout v18 disk
// (Macha's: refused with a reason naming dagda-migrate, nothing on it
// changed), or a non-empty unmarked directory is refused and stays offline;
// an empty one is adopted. A member proves its key against the marker's
// check; on a disk a blind node adopted there is no check, so the member
// probes instead: it opens held records (up to eight) and the backend comes
// online as a member's only if one opens, else it stays offline with the
// reason and nothing is discarded. Until a backend's key has been checked
// one way or the other, a copy that does not open is reported, never
// removed. Backends on one filesystem share a durability domain. A
// backend's incarnation is a counter, new each time it comes online, so a
// ticket outlives neither its process nor its disk.
//
// Lock order: the pool's lock, then a backend's, then the domain lock. All
// three guard memory only: no device operation runs under any of them.
// Thread-safe, apart from the maintenance steps (each has one owner).
class StoragePool final : public contract::ObjectStore {
  struct Backend;

 public:
  // Persistent traversal of the objects the backends hold. Maintenance owns
  // one cursor per class and advances it a bounded number of objects a slice.
  struct Cursor {
    size_t backend_index = 0;
    size_t completed_backends = 0;
    size_t backend_count = 0;
    std::shared_ptr<LocalStore> store;
    LocalStore::Cursor local;
  };

  struct MaintenanceResult {
    uint64_t bytes = 0;
    size_t objects = 0;
    bool complete = false;
    bool yielded = false;
    bool deferred = false;
  };

  // Whether an object is claimed here: the seam step 2's claims attach to. A
  // copy with any local claim is untouchable (requirement L21).
  using ClaimCheck = std::function<bool(const ObjectId&)>;

  // `node` is this node's identity, written into every backend's marker;
  // `epoch` its process's durability epoch, which every ticket carries. Each
  // backend keeps its held ledger under <state>/ledger/data-<token>. Opens
  // every backend it can; one that cannot be opened is offline, never an
  // error. Throws std::invalid_argument for two backends at one path, for a
  // member pool with no key, or a pool with no ledger key. Waits on the
  // devices.
  StoragePool(std::filesystem::path state, dannan::identity::NodeId node, dannan::identity::NodeId epoch, StoragePoolOptions options,
              std::shared_ptr<const domain::StorageKey> key, LocalStore::Services services);
  ~StoragePool();
  StoragePool(const StoragePool&) = delete;
  StoragePool& operator=(const StoragePool&) = delete;

  // Takes the backends as configured now: a new one is added offline, a
  // changed limit or reserve reopens it, a removed one goes offline. Opens
  // nothing: refresh() does. Throws std::invalid_argument for two backends at
  // one path. Waits on the stores it lets go.
  void reconfigure(const std::vector<StorageBackendOptions>& backends);
  // Brings every configured backend it can online, and takes one whose disk
  // has gone offline. Waits on the devices.
  void refresh();

  // Service time of this pool's DATA devices, fed by every put and get and by
  // maintenance's reads, and consulted by DATA admission.
  DiskServiceMonitor& service_monitor() noexcept { return _service_monitor; }
  const DiskServiceMonitor& service_monitor() const noexcept { return _service_monitor; }
  void configure_service_monitor(DiskServiceMonitor::Thresholds thresholds) noexcept { _service_monitor.configure(thresholds); }

  domain::Role role() const noexcept { return _role; }
  const SealingScheme& sealing() const noexcept { return _sealing; }

  // Strict: true once durable on the first backend that took it; false if
  // none would. A put of an object already held reaffirms its age. Throws
  // std::logic_error on a blind pool, which cannot seal.
  bool put(const ObjectId& id, std::span<const uint8_t> data);
  // Provisional: the ticket to await, from the backend that took it; nothing
  // if none would.
  std::optional<DurabilityTicket> put_deferred(const ObjectId& id, std::span<const uint8_t> data);
  // A record a member sealed, stored as it is on the first backend that
  // takes it (LocalStore::put_sealed); either role. Throws
  // std::invalid_argument for a record whose sizes disagree.
  bool put_sealed(const ObjectId& id, const LocalStore::SealedObject& sealed);
  // The record as held, unopened, from the first online backend holding it;
  // and its ciphertext digest. Either role.
  std::optional<LocalStore::SealedObject> get_sealed(const ObjectId& id) const;
  std::optional<dannan::identity::Hash256> held_digest(const ObjectId& id) const;
  // Returns once the ticket's generation is durable. Throws
  // StaleDurabilityTicket if it names another process, or a domain or
  // incarnation no longer online; std::invalid_argument for an invalid one;
  // and rethrows a failed barrier, taking that backend offline.
  void durability_barrier(const DurabilityTicket& ticket, DurabilityUrgency urgency = DurabilityUrgency::batchable);
  bool durability_covered(const DurabilityTicket& ticket) const;
  // A ticket for an object already held, whose earlier ticket (`stale`, when
  // known) died with its process or backend incarnation, or whose barrier
  // failed. Nothing, the object to be put again, when `stale` was issued past
  // a failed barrier (durability_failures_name), or when no online backend
  // holds a copy that reads back: a member's opens to its id, so a record whose
  // header survived a power cut without its payload is not taken for one. The
  // holding backend is then flushed, so present implies durable; a latched
  // failure makes that throw, and the next backend is asked. A blind backend
  // can check only that the whole record is there.
  std::optional<DurabilityTicket> reassert_durable(const ObjectId& id, const std::optional<DurabilityTicket>& stale = std::nullopt);
  // Reads in rank order. A copy that does not open is an object failure, not
  // a backend's: it is removed (where the backend's key check has passed)
  // and the next backend asked. Throws std::logic_error on a blind pool,
  // which cannot open.
  std::optional<Bytes> get(const ObjectId& id) const;
  bool has(const ObjectId& id) const override;
  // The union of every online backend's held trie, its identity the union's
  // root; incomplete while an online backend is being seeded. The removals
  // never fall, even as disks go and return.
  contract::HeldView held_view() const override;
  bool valid(const ObjectId& id) const;
  // From every online backend that holds it.
  bool remove(const ObjectId& id);
  std::vector<ObjectId> list() const;
  uint64_t full_list_scans() const noexcept { return _full_list_scans.load(std::memory_order_relaxed); }
  std::optional<ObjectId> next_object(Cursor& cursor, bool& pass_complete) const;
  bool older_than(const ObjectId& id, std::chrono::milliseconds age) const;

  // One bounded slice of moving objects onto their preferred backend. A copy
  // elsewhere goes only once the preferred backend holds a valid one. Reads
  // nothing with fewer than two backends online. Single owner.
  MaintenanceResult rebalance_step(uint64_t budget_bytes, size_t operation_budget, const std::function<bool()>& should_yield = {});
  // One bounded slice of reading every object; one that does not open is
  // removed, where the backend's key check has passed. Single owner.
  MaintenanceResult scrub_step(uint64_t budget_bytes, size_t operation_budget, const std::function<bool()>& should_yield = {});
  // One bounded slice of the DATA sweep. `live` and `protected_ids` are
  // sorted and unique. An unreferenced object goes only once older than
  // `orphan_grace` (so a put not yet committed cannot race its commit), only
  // if `claimed` says it is unclaimed both before the slice decides and again
  // just before the irreversible delete, and, given `sightings`, only once
  // this node has seen it unreferenced and unclaimed for `unreferenced_grace`
  // at `now_unix_ms` on its own clock. Single owner.
  MaintenanceResult gc_step(const contract::IdLookup& live, const std::vector<ObjectId>& protected_ids, std::chrono::milliseconds orphan_grace,
                            size_t operation_budget, const std::function<bool()>& should_yield = {}, const ClaimCheck& claimed = {},
                            ledger::UnreferencedSince* sightings = nullptr, uint64_t now_unix_ms = 0,
                            std::chrono::milliseconds unreferenced_grace = std::chrono::milliseconds(0));

  // A complete rebalance pass, for tests and explicit callers. Single owner.
  uint64_t rebalance_once(uint64_t budget_bytes = 0);
  // Reclaims dead pack records incrementally: at most one pack per backend a
  // call, bounding temporary disk use. A failure keeps the backend online.
  size_t compact_packs(std::stop_token stop = {});
  // LocalStore::verify_step on every online backend, summed; complete once
  // every backend's pass is.
  LocalStore::VerifyResult verify_step(size_t directories);

  uint64_t used() const;
  // The configured limits of the backends whose identity is known.
  uint64_t limit() const;
  size_t online_backends() const noexcept { return _online_backends.load(std::memory_order_relaxed); }
  // Whether every configured backend is online now: only then is an object
  // get() did not find known not to be here.
  bool every_backend_online() const;
  // Backends are configured and none is online: the plane holds nothing it
  // should, and says so rather than reporting itself ready (requirements
  // section 7).
  bool degraded() const;
  LocalStoreDiagnostics diagnostics() const;

  // Barriers made by the pool's durability domains since construction.
  uint64_t physical_barriers() const;
  // The domains re-read the time source: for one that moves other than with
  // the steady clock (a test's stepped time).
  void time_advanced();

  // The guard a claim on this pool's objects and an instant delete of them
  // hold (object_lock.h).
  ClaimLocks& claim_locks() const noexcept { return _claim_locks; }

 private:
  // A read that did not finish says nothing about the object: it stays, and is
  // counted (storage.read_failures).
  void note_read_failure(const ObjectId& id, const std::filesystem::path& path, const std::exception& error) const;
  struct CursorItem {
    std::shared_ptr<Backend> backend;
    std::shared_ptr<LocalStore> store;
    std::filesystem::path path;
    ObjectId id;
  };

  std::vector<std::shared_ptr<Backend>> snapshot() const DANNAN_EXCLUDES(_mutex);
  std::filesystem::path identity_path(const std::filesystem::path& backend) const;
  bool activate(const std::shared_ptr<Backend>& backend);
  // The key probe: whether this member's key opens a record the store holds.
  // Nothing when the store holds no record to try. Throws nothing.
  std::optional<bool> probe_key(LocalStore& store, std::string& reason) const;
  // Whether a copy that does not open may be removed from `backend`.
  static bool key_checked(const std::shared_ptr<Backend>& backend);
  void deactivate(const std::shared_ptr<Backend>& backend, const std::shared_ptr<LocalStore>& expected, const std::string& reason,
                  uint64_t expected_generation = 0) const;
  std::vector<std::shared_ptr<Backend>> ranked(const ObjectId& id) const;
  std::optional<CursorItem> next_physical(Cursor& cursor, bool& pass_complete) const;
  std::shared_ptr<DurabilityDomain> domain_for(const std::filesystem::path& path) DANNAN_EXCLUDES(_domain_mutex);
  // A domain's latched failure, kept in memory and under <state>.
  void record_durability_failure(uint64_t domain, uint64_t durable) DANNAN_EXCLUDES(_domain_mutex);
  bool issued_past_failure(const DurabilityTicket& ticket) const DANNAN_EXCLUDES(_domain_mutex);
  void observe_get(size_t bytes, uint64_t elapsed_ms) const;

  // Fixed at construction.
  const std::filesystem::path _state;
  const dannan::identity::NodeId _node;
  const dannan::identity::NodeId _epoch;
  const std::shared_ptr<const domain::StorageKey> _key;
  const SealingScheme _sealing;
  const domain::Role _role;
  const std::shared_ptr<const domain::StorageKey> _ledger_key;
  const std::function<void(const std::filesystem::path&)> _lock_root;
  const LocalStore::Services _services;
  const std::chrono::milliseconds _durability_batch_window;
  const size_t _pack_threshold;
  const size_t _pack_target_size;
  const uint64_t _ledger_cache_bytes;
  mutable DiskServiceMonitor _service_monitor;
  // How many backends share the ledger cache: the configured count.
  std::atomic<size_t> _ledger_shares{1};
  // TODO(Dannan: TimedLock): Macha times this lock (TimedLock "storage.pool",
  // diagnostics.hpp) to report a slow holder; a plain lock until Dannan has
  // it.
  mutable dannan::contract::Mutex _mutex;
  std::vector<std::shared_ptr<Backend>> _backends DANNAN_GUARDED_BY(_mutex);
  mutable dannan::contract::Mutex _domain_mutex;
  uint64_t _next_domain_id DANNAN_GUARDED_BY(_domain_mutex) = 1;
  uint64_t _next_backend_instance DANNAN_GUARDED_BY(_domain_mutex) = 1;
  std::map<uint64_t, std::shared_ptr<DurabilityDomain>> _domains_by_device DANNAN_GUARDED_BY(_domain_mutex);
  const std::function<void()> _before_sync;
  struct DurabilityFailure {
    dannan::identity::NodeId epoch;
    uint64_t domain = 0;
    uint64_t durable = 0;
  };
  std::vector<DurabilityFailure> _durability_failures DANNAN_GUARDED_BY(_domain_mutex);
  // Each cursor is owned by the maintenance step that runs it.
  Cursor _rebalance_cursor;
  Cursor _scrub_cursor;
  Cursor _gc_cursor;
  // Whether the sweep put anything off since its pass began; the slice that
  // completes the pass reports it, so a host waits for the grace rather than an
  // event. Owned with _gc_cursor.
  bool _gc_pass_deferred = false;
  mutable std::atomic<uint64_t> _diag_gets{0};
  mutable std::atomic<uint64_t> _diag_get_bytes{0};
  mutable std::atomic<uint64_t> _diag_get_ms{0};
  mutable std::atomic<uint64_t> _diag_get_max_ms{0};
  mutable std::atomic<int64_t> _diag_get_report_ns{0};
  mutable std::atomic<uint64_t> _full_list_scans{0};
  std::atomic<size_t> _online_backends{0};
  // The last held union made, the hint for the next: held across the making,
  // so each is made from the one before.
  mutable dannan::contract::IoMutex _union_mutex;
  mutable std::shared_ptr<const ledger::TrieUnion> _union DANNAN_GUARDED_BY(_union_mutex);
  mutable ClaimLocks _claim_locks;
};

}  // namespace dagda::store
