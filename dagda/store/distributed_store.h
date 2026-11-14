//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "dagda/contract/claim_store.h"
#include "dagda/contract/horizon.h"
#include "dagda/contract/object_ledger.h"
#include "dagda/domain/domain.h"
#include "dagda/ledger/availability.h"
#include "dagda/store/control_objects.h"
#include "dagda/store/data_work.h"
#include "dagda/store/durability_ticket.h"
#include "dagda/store/local_store.h"
#include "dagda/store/messages.h"
#include "dagda/store/object_id.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/store/remove.h"
#include "dagda/store/replica_selector.h"
#include "dagda/store/storage_pool.h"
#include "dannan/context.h"
#include "dannan/contract/scope.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/net/message.h"

namespace dagda::store {

// No node present can take a write now: no backend online, none with room,
// or none answering in time. It passes, so a caller waits and tries again
// (Macha 0.91.5, requirement S23).
class StorageUnavailable final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// What a write waits for (Tom, 2026-11-04; requirements section 8, head).
// "On disk" is synced to stable storage: the durability domain's generation
// made durable. A write is never undone.
enum class Durability : uint8_t {
  // Received in memory and queued; the call returns at once. Best effort: it
  // may be lost.
  ephemeral,
  // On disk on at least one node.
  persisted,
  // On disk on at least k nodes: for objects k is the store's write_copies.
  replicated,
  // On disk on every available node of the replication set. A node Dannan
  // already sees as dead is not waited on and is named pending; a live one
  // that does not answer is waited on to the deadline, never longer.
  durable,
};
const char* durability_name(Durability) noexcept;

enum class ReplicaState : uint8_t { acknowledged, pending, refused };
const char* replica_state_name(ReplicaState) noexcept;

// One node of a write, by Dannan node id.
struct ReplicaEvidence {
  dannan::identity::NodeId node;
  ReplicaState state = ReplicaState::pending;
  // A member of the object's replication set; otherwise another node the
  // write reached (the local first copy, a fallback a refused or stalled
  // owner spilled to).
  bool in_set = false;
  // Why pending or refused: "dead", "queued", "no answer yet", "awaiting
  // barrier", "owed to repair", a refusal's code and text.
  std::string why;
  // A deferred write's place on that node, for its barrier.
  std::optional<DurabilityTicket> durability;
  // Acknowledged after the write returned, by repair, prompt replication or
  // a returning member's catch-up: what the ticket reported again.
  bool repaired = false;
};

// A write's handle, outliving the call: status() and wait() read it.
struct Ticket {
  uint64_t serial = 0;
  bool valid() const noexcept { return serial != 0; }
  auto operator<=>(const Ticket&) const = default;
};

// What a write did, on every node it concerns: the replication set's
// members first, in placement order, then the other nodes it reached.
struct WriteEvidence {
  ObjectId id{};
  Durability level = Durability::replicated;
  // A deferred write: copies are on disk once a wait() makes them so.
  bool deferred = false;
  // Acknowledgements the level asks for (for durable, the members waited
  // on).
  size_t sought = 0;
  // The level holds: queued (ephemeral), or enough acknowledged.
  bool met = false;
  // No node is pending: nothing more will be reported.
  bool settled = false;
  // Nothing acknowledged and nothing pending: the write landed nowhere.
  bool lost = false;
  // No node is pending at all, those owed to repair included: every copy
  // the set is owed has landed (or was refused). A settled ticket may still
  // be incomplete; it reports again when the last pending member's copy
  // lands, for as long as it is kept.
  bool complete = false;
  // The caller's cancel was seen while the write still waited on a copy: the write
  // failed whatever landed (Macha's put), and what landed is reported as it stands.
  bool cancelled = false;
  std::vector<ReplicaEvidence> replicas;
  Ticket ticket;

  size_t acknowledged() const noexcept;
  // Nodes that took the bytes, on disk or awaiting a barrier.
  size_t holders() const noexcept;
  const ReplicaEvidence* on(const dannan::identity::NodeId& node) const noexcept;
};

// Who a write or a claim is for, and under what prefix: what every storing
// replica, this node included, checks through Identity::verify before it
// stores (Tom, 2026-11-04 evening; stack document, "Scope"). Nothing: this
// node, as a node principal, under the domain's name.
struct WriteScope {
  std::optional<dannan::contract::Principal> principal;
  std::string prefix;
};

struct WriteOptions {
  Durability level = Durability::replicated;
  // Written now, made durable by a later wait() (Macha's put_deferred).
  bool deferred = false;
  // The call returns by then, whatever is still pending. Zero: none of its own, as
  // Macha's: each peer's put runs under call_no_progress, so it returns when what
  // can land has landed.
  std::chrono::milliseconds deadline{0};
  dannan::sched::FrameType frame = dannan::sched::FrameType::loader;
  std::atomic_bool* cancelled = nullptr;
  // A window over the whole write, not a call (Macha's publication budget,
  // fuse.publication_no_progress_deadline, 30 s): re-armed by a copy landing, a
  // fallback starting, any pending call that moved within it, or `progress`
  // moving; when nothing does for the whole budget, the pending calls are
  // cancelled and the write ends with what landed, the rest owed to prompt
  // replication. Zero: none, as every Macha write but publication.
  std::chrono::milliseconds no_progress_budget{0};
  // Progress elsewhere in the caller's pipeline, shared by its workers; read only
  // with a budget.
  const std::atomic_uint64_t* progress = nullptr;
  // The scope the write is checked under (WriteScope): a replica that
  // refuses it stores nothing and says why in the evidence.
  std::optional<dannan::contract::Principal> principal;
  std::string prefix;
};

// An object as read, and the memory its bytes hold (a reply's retained
// frame) until the last reader lets it go.
struct ObjectBuffer {
  Bytes bytes;
  dannan::net::MemoryHold memory;
};
using ObjectData = std::shared_ptr<const ObjectBuffer>;

// What asking for an object came to: found; not_found, every online peer
// that might hold it said it does not and asking again will not change
// that; or try_later, nothing that settles it now.
enum class FetchAnswer : uint8_t { ok, not_found, try_later };
enum class ReadSource : uint8_t { none, local, cache, remote };

struct ReadOptions {
  dannan::sched::FrameType frame = dannan::sched::FrameType::foreground;
  // One time across the local, cache and remote stages. Zero: none of its own, as
  // Macha's: admission waits under the arbiter's window, and each peer's answer
  // under call_no_progress.
  std::chrono::milliseconds deadline{0};
  // Rotates equal-scored replicas, so a file's stripes spread over them.
  size_t stripe = 0;
  std::atomic_bool* cancelled = nullptr;
};

struct ReadResult {
  ObjectData object;
  ReadSource source = ReadSource::none;
  // Which node's copy it is: this node's for local and cache.
  std::optional<dannan::identity::NodeId> served_by;
  FetchAnswer answer = FetchAnswer::try_later;
};

// What an asynchronous claim came to. `ran`: it ran to its end, and then
// for DATA `unheld` names, sorted, the objects no node present holds, and
// for CONTROL `held_here` says whether the local claim held. Otherwise `why`
// says what stopped it, and nothing is known to be claimed.
struct RetainOutcome {
  bool ran = false;
  std::vector<ObjectId> unheld;
  bool held_here = false;
  std::string why;
};

// What a CONTROL offer did: `held_by` counts the nodes that hold every object
// afterwards, the offering node included.
struct ControlOffer {
  size_t held_by = 0;
  size_t objects_sent = 0;
  uint64_t bytes_sent = 0;
};

// Macha's figures (config.hpp:602-640), with its defaults, and Dagda's.
struct DistributedStoreOptions {
  // The domain the store's objects belong to: its name is the prefix a
  // write is checked under when the caller names none. Domain v1, Macha's,
  // until the catalogue (step 4) hands a store its domain.
  domain::Domain domain = domain::domain_v1_descriptor();
  // Copies each object is placed on (Macha's dht.replicas).
  size_t replication = 3;
  // k of the replicated level: Tom's default of 2 (2026-11-04). Macha's own
  // write_copies of 1 is the persisted level, which is what Macha asks for.
  size_t write_copies = 2;
  // Nodes a CONTROL claim is sought on.
  size_t metadata_write_copies = 2;
  size_t extent_size = 16 * 1024 * 1024;
  // A peer whose put has moved no byte for this long is spilled past.
  std::chrono::milliseconds write_stall{2500};
  // The bound on a peer's put or fetch (Dannan's CallWatch): one that has sent no
  // byte of its request and received none of its answer for this long (Dannan 0.5.1
  // counts both, as Macha's touch does) is cancelled (the peer told, its reply slot
  // freed, a late reply dropped), and the copy is owed to prompt replication. One
  // still moving goes on, however long, as Macha's does, past the write or read that
  // started it if that had a deadline, so a slow peer's acknowledgement still reaches
  // the write's ticket. Longer than the wait for credit a peer's server may make
  // before it answers (Macha's 120 s window), so that answer comes first. Macha has
  // no such bound on a data call (data_no_progress_deadline 0): a peer that keeps its
  // session and never answers holds a call there for ever.
  std::chrono::milliseconds call_no_progress{300000};
  // A claim, a single presence probe and a CONTROL presence batch to one peer: Macha's
  // bounded control call, which gives up after max(dead_after, connect_timeout).
  std::chrono::milliseconds control_call{30000};
  // The latest each other call to a peer may end, moving or not, named by its purpose
  // as Macha's port names them (its call_deadline.hpp, Tom 2026-11-11). A put, a get
  // or a repair push, still under call_no_progress:
  std::chrono::milliseconds object_transfer_call{std::chrono::minutes(30)};
  // A presence batch (have_objects, have_valid_objects):
  std::chrono::milliseconds presence_call{std::chrono::minutes(2)};
  // A delete asked of a peer:
  std::chrono::milliseconds delete_call{std::chrono::minutes(2)};
  // A CONTROL object put to or fetched from a peer:
  std::chrono::milliseconds metadata_call{std::chrono::minutes(5)};
  // A call other than a transfer that Macha's port waits on synchronously (a CONTROL
  // fetch, a delete) ends sooner when it moves no byte for this long: Macha's
  // control_no_progress_deadline.
  std::chrono::milliseconds control_no_progress{30000};
  size_t retention_check_batch_size = 2000;
  size_t retention_check_concurrency = 8;
  // Threads that run writes and reads for the asynchronous calls and the
  // ephemeral queue.
  size_t workers = 4;
  // Bytes the ephemeral queue holds before it refuses.
  size_t ephemeral_queue_bytes = 64 * 1024 * 1024;
  // Settled tickets kept for a later status(), the oldest forgotten first past this
  // bound. A ticket still waiting is kept, and so is a deferred one no caller has yet
  // asked after (a wait or a status), settled or not, as Macha's batch, the caller's,
  // kept every extent a publication handed it until consumed, up to
  // `max_waiting_tickets` (4 TiB of publication at Macha's 4 MiB extents): past that
  // the oldest kept is forgotten, with a warning, so a deferred write nobody waits on
  // cannot hold memory for ever. A batch the caller owns, through the Context, is the
  // proper end of that (the store map).
  size_t max_tickets = 4096;
  size_t max_waiting_tickets = 1024 * 1024;
  // Prompt replication's first retry, doubling (Macha's 30 s).
  std::chrono::milliseconds prompt_retry{30000};
  // Whether a prompt copy's read of the local object is admitted as speculative
  // device work: it yields to a waiting foreground request and to device pressure,
  // counts against the background ceiling and holds a device slot. Macha's takes
  // none (distributed_store.cpp:1306): its facade turns this off.
  bool prompt_read_admission = true;
  // Opportunistic local copies queued (cache fill, promotion).
  size_t local_copy_bytes = 256 * 1024 * 1024;
  // How long work here waits for a control slot. DATA work whose caller named
  // no deadline (background work, repair, opportunistic copies, a read or a
  // write with none) waits under the arbiter's no-progress window instead.
  std::chrono::milliseconds admission_wait{30000};
  // Repair's decision trace (each copy pushed, pull and local drop, in
  // decision order), for tests: kind "repair" and the decision.
  std::function<void(std::string_view kind, std::string_view detail)> repair_trace;
  // Told as a read's local and cache stages end, for tests.
  std::function<void(std::string_view stage)> read_trace;
};

// What one repair step works from: the availability survey's lists, never a
// walk of the store or a probe of a peer (repair is diff only, Tom,
// 2026-11-04).
struct RepairLists {
  // What is referenced, at its generation. Without one repair does nothing.
  std::shared_ptr<const contract::InventoryHorizon> inventory;
  // What is referenced and this node lacks, sorted: what the pull considers.
  std::shared_ptr<const std::vector<ObjectId>> pull;
  // Per peer, what it lacks of what this node holds: what the push sends,
  // and only when every hosting peer answered for this inventory.
  std::shared_ptr<const ledger::PeerLacks> lacks;
  // Held by no reachable node: passed without asking anyone.
  std::function<bool(const ObjectId&)> unavailable;
};

// What a repair step did (Macha's RepairResult).
struct RepairResult {
  uint64_t bytes_transferred = 0;
  size_t push_examined = 0;
  size_t pull_examined = 0;
  size_t remote_operations = 0;
  // Objects this step wanted to pull and no peer would supply.
  size_t pull_unsourceable = 0;
  // A whole pass ended, and nothing it passed changed under it.
  bool complete = true;
  bool yielded = false;
  // The step stopped at an object needing a transfer the byte budget could
  // not cover; that object is the first of the next step.
  bool credit_limited = false;
};

// Repair's progress and silent failures since start (Macha's
// RepairDiagnostics, without the pass's gates, which are the driver's).
struct RepairDiagnostics {
  // Pull: an object this node should own that no peer would supply. Push:
  // a listed local object that would not read back.
  uint64_t pull_unsourceable = 0;
  uint64_t local_unreadable = 0;
  std::vector<ObjectId> unsourceable_sample;
  uint64_t push_examined = 0;
  uint64_t pull_examined = 0;
  uint64_t bytes_transferred = 0;
  uint64_t passes_completed = 0;
  bool push_phase_complete = false;
  uint64_t copies_pushed = 0;
  uint64_t copies_pulled = 0;
  uint64_t local_drops = 0;
};

// The distributed store (requirements S22 to S29; Macha's
// cluster/distributed_store.*), on Dannan.
//
// Placement: Dannan's capacity placement under the store's scheme
// (placement.h, Macha's salts) over the nodes advertising extents. An
// object's replication set is its first `replication` owners over every
// hosting node the roster knows, dead ones included; a write goes to the
// active ones, local first.
//
// Writes (S22, S23, S28): Macha's write path with the four levels in place
// of its write_copies-only wait. Persisted and replicated seek their count
// local first, spill past a peer stalled beyond write_stall or one that
// refuses, and return on what landed when nothing more can be had promptly;
// the rest is owed to prompt replication (a second copy) and repair. Durable
// writes to every live member of the set and waits for each. With a caller's
// deadline a write returns by it; with none it has no clock of its own, and every
// call it waits on is bounded by call_no_progress. Every write returns evidence
// naming each node, and a ticket: calls still in flight when the write returns run
// on and report into it, and a durable write's ticket pushes the object to a member
// that was dead once it is back.
//
// Reads (S27): local, then the persistent cache, then remote owners and the
// other ranked nodes in the replica selector's order; concurrent readers of
// one object share one fetch and one buffer. has() is the local ground
// truth.
//
// CONTROL objects (S29): offered to every node, claimed on
// metadata_write_copies nodes local first then nearest, fetched from any
// peer at speculative priority without DATA credit.
//
// Repair (requirements L9 to L11; Macha's distributed_store.cpp:2421-3087,
// diff only): one bounded step at a time from the survey's lists. The push
// sends what each peer is known to lack to the owners that lack it,
// pipelined, at speculative priority, and drops this node's own copy of an
// object it does not own once every owner holds it and nothing here claims
// it: that is how objects move between nodes as placement changes (Macha has
// no other cluster rebalance). The pull fetches what this node should own
// and lacks, from the cache first, passing over what no reachable node
// holds. Credit pays for bytes moved, never for finding out; a step yields
// at operation boundaries and keeps what is in flight. Every copy repair
// lands reaches the tickets that name its node pending.
//
// Admission: work on this node's devices holds DATA credit and a DATA slot
// of the device reserve across the device operation, never across a peer's
// answer; CONTROL objects and claims hold a control slot and never DATA
// credit (law 1).
//
// Every call that waits on a peer takes a deadline. The blocking forms wait
// on the network and the devices and must not run on one of the io
// context's threads; the asynchronous forms post their completion to the
// caller's executor, never inline. Thread-safe.
class DistributedStore final : public ControlObjectSource {
 public:
  using WriteDone = std::function<void(WriteEvidence)>;
  using ReadDone = std::function<void(ReadResult)>;
  using RetainDone = std::function<void(RetainOutcome)>;
  enum class ClaimScope { everywhere, here, every_node };

  struct Stores {
    StoragePool& data;
    LocalStore& control;
    PersistentBlockCache& cache;
    contract::ClaimStore& claims;
    // The node's DATA credit and device reserve; null admits everything.
    DataResourceArbiter* credit = nullptr;
    IoReserve* io = nullptr;
    // The ledger whose published inventory this node's half of an instant
    // delete checks. None: no inventory.
    const contract::ObjectLedger* ledger = nullptr;
    // When object work of each class was last present, in Macha's meaning: object
    // bytes this store moved, never every inbound request as Dannan's clocks count.
    // None: the node's.
    dannan::status::ActivityClocks* activity = nullptr;
  };

  struct PromptReplicationStats {
    uint64_t queued = 0;
    uint64_t copies = 0;
    uint64_t failures = 0;
    uint64_t skipped_no_room = 0;
    uint64_t dropped = 0;
  };

  // The store's own completions run on the node's executor and its retries
  // and ticket ages on the node's clock, both from `context.time()`.
  DistributedStore(dannan::Context& context, Stores stores, DistributedStoreOptions options = {});
  ~DistributedStore();
  DistributedStore(const DistributedStore&) = delete;
  DistributedStore& operator=(const DistributedStore&) = delete;

  // Writes. The blocking forms throw std::runtime_error for bytes that are
  // not the id's ("object hash mismatch") or larger than an extent.
  WriteEvidence write(const ObjectId& id, std::span<const uint8_t> data, WriteOptions options = {});
  void write(const ObjectId& id, Bytes data, WriteOptions options, boost::asio::any_io_executor executor, WriteDone done);
  // Macha's forms: the id-returning ones throw StorageUnavailable when the
  // write landed nowhere (or std::runtime_error when cancelled); the bool
  // ones return whether it landed anywhere. Named no level they seek
  // write_copies before they return, as Macha's do (dht.write_copies, whose
  // default of 1 is the facade's to pass: this store's default is Tom's 2).
  ObjectId put(std::span<const uint8_t> data, WriteOptions options = {});
  bool put(const ObjectId& id, std::span<const uint8_t> data, WriteOptions options = {});
  ObjectId put_deferred(std::span<const uint8_t> data, WriteOptions options = {});
  // Durable here and returned at once, the further copies owed to prompt
  // replication; as put() on a node that hosts no extents.
  bool put_here(const ObjectId& id, std::span<const uint8_t> data, dannan::sched::FrameType frame = dannan::sched::FrameType::loader);
  // Written here and made durable by a later wait() on the evidence's ticket
  // (Macha's put_deferred_here, whose batch the ticket replaces), the further
  // copies owed to prompt replication; on a node that hosts no extents, or
  // that refuses it, the first copy goes to the next in placement order.
  WriteEvidence put_deferred_here(const ObjectId& id, std::span<const uint8_t> data, dannan::sched::FrameType frame = dannan::sched::FrameType::loader);

  // Tickets. status() waits on nothing: nothing for a ticket forgotten.
  // wait() completes when the ticket settles or at the deadline, whichever
  // comes first, and may be called again; for a deferred write it first asks
  // each copy awaiting its barrier for it.
  std::optional<WriteEvidence> status(Ticket ticket) const;
  void wait(Ticket ticket, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, WriteDone done);
  std::optional<WriteEvidence> wait(Ticket ticket, std::chrono::milliseconds deadline);
  // A wait over many tickets (Macha's DurabilityBatch and its one
  // durability_barrier): one evidence for each ticket, in the order given, a
  // forgotten one's holding no ticket. Generations are cumulative within one
  // node, process epoch, domain and backend instance, so each such place is
  // asked for one barrier, at the highest generation any of the tickets names,
  // and a place whose process has changed is probed once with every id that
  // named it. Each ticket keeps its own level and evidence.
  using ManyDone = std::function<void(std::vector<WriteEvidence>)>;
  void wait(std::vector<Ticket> tickets, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, ManyDone done);
  std::vector<WriteEvidence> wait(const std::vector<Ticket>& tickets, std::chrono::milliseconds deadline);
  // As wait(), but until no member is pending at all (WriteEvidence::
  // complete): the report a ticket makes again once repair or prompt
  // replication has landed the last copy it was owed.
  void wait_complete(Ticket ticket, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, WriteDone done);
  std::optional<WriteEvidence> wait_complete(Ticket ticket, std::chrono::milliseconds deadline);

  // Reads.
  ReadResult read(const ObjectId& id, ReadOptions options = {});
  void read(const ObjectId& id, ReadOptions options, boost::asio::any_io_executor executor, ReadDone done);
  std::optional<Bytes> get(const ObjectId& id, ReadOptions options = {});
  // The local ground truth: this node's pool holds it. Asks no peer.
  bool has(const ObjectId& id) const;
  // In the pool or the cache.
  bool locally_available(const ObjectId& id) const;
  // Macha's activity forms over the clocks above.
  void foreground_activity(uint64_t bytes) { _activity.note(dannan::sched::FrameType::foreground, bytes); }
  void interactive_activity(uint64_t bytes) { _activity.note(dannan::sched::FrameType::read_ahead, bytes); }
  void loader_activity(uint64_t bytes) { _activity.note(dannan::sched::FrameType::loader, bytes); }
  uint64_t take_foreground_bytes() { return _activity.take_bytes(dannan::sched::FrameType::foreground); }
  uint64_t take_interactive_bytes() { return _activity.take_bytes(dannan::sched::FrameType::read_ahead); }
  uint64_t take_loader_bytes() { return _activity.take_bytes(dannan::sched::FrameType::loader); }
  std::chrono::milliseconds idle_for(dannan::sched::WorkClass work) const { return _activity.idle_for(work); }
  // Macha's forms the filesystem and the catalogue call. held_here: as has().
  // cache_local: a write through to this node's cache, if it has one; a failure
  // is only that the cache did not take it. hydration_available: the node has a
  // cache to fill. hydrate: true once the object is here, fetching it into the
  // cache if not.
  bool held_here(const ObjectId& id) const { return has(id); }
  bool cache_local(const ObjectId& id, std::span<const uint8_t> data);
  bool hydration_available() const;
  bool hydrate(const ObjectId& id, ReadOptions options = {});
  // Claims here whichever of `ids` this node holds, and returns the rest, sorted
  // and unique (Macha's retain_data_here).
  std::vector<ObjectId> retain_data_here(const std::vector<ObjectId>& ids, const contract::RetentionDot& dot);
  // Changes when what this node can reach may have: the membership, or how many of
  // its disks are online. A write parked as unreachable retries when it moves.
  uint64_t reachability_epoch() const noexcept;
  // Waits until every opportunistic local copy queued so far is written or
  // dropped.
  void wait_local_copies_settled();

  // Placement.
  bool holds_extents() const;
  bool should_own(const ObjectId& id) const;
  // Active hosting nodes in placement order: the first `replication` the
  // owners, the rest fallbacks.
  std::vector<dannan::membership::NodeRecord> ranked(const ObjectId& id) const;
  // The replication set over every hosting node known, dead ones included.
  std::vector<dannan::membership::NodeRecord> replication_set(const ObjectId& id) const;
  bool has_on(const dannan::membership::NodeRecord& target, const ObjectId& id);
  // Whether `target` holds each object in a copy that reads back intact
  // (have_valid_objects, at most 16 ids a request); an id it could not say
  // for now is left out.
  std::map<ObjectId, bool> valid_on(const dannan::membership::NodeRecord& target, const std::vector<ObjectId>& ids);
  // Puts the object on every hosting node present; how many took it.
  size_t replicate_all(const ObjectId& id, std::span<const uint8_t> data, bool foreground = false);
  // Removes the object from every node present that does not claim it
  // (delete_object), and from this node's cache.
  void erase_all(const ObjectId& id);
  // Makes a DATA object local: held and valid, else from the cache, else
  // fetched. The claim walk's restorer.
  bool ensure_local(const ObjectId& id, bool foreground = false);

  // One repair step: at most `operation_budget` calls (zero: no bound) and
  // `byte_budget` bytes moved. Single owner.
  RepairResult repair_step(const RepairLists& lists, uint64_t byte_budget, size_t operation_budget, const std::function<bool()>& should_yield = {});
  RepairDiagnostics repair_diagnostics() const;
  // The link's estimated rate, from transfers of half an extent or more.
  double estimated_network_bps() const { return _network_bps.load(std::memory_order_relaxed); }

  // Claims. retain_data claims each object on up to write_copies nodes that
  // hold it, the presence asked in batches; it returns, sorted, the objects
  // no node present holds. retain_control makes the objects local, offers
  // them and claims them on the nodes `scope` names, local first then
  // nearest; false only when the local claim did not hold. Each claiming
  // node checks `writer` as it checks a write (WriteScope); a node that
  // refuses it claims nothing.
  std::vector<ObjectId> retain_data(const std::vector<ObjectId>& ids, const contract::RetentionDot& dot, const WriteScope& writer = {});
  bool retain_control(const std::vector<ObjectId>& ids, const contract::RetentionDot& dot, ClaimScope scope = ClaimScope::everywhere,
                      const WriteScope& writer = {});
  // Either claim, run on the store's workers and its outcome posted to
  // `executor`, never inline. `scope` is read for CONTROL only. Waits on
  // what the blocking form waits on, each peer within control_call.
  void retain(contract::RetentionClass type, std::vector<ObjectId> ids, contract::RetentionDot dot, WriteScope writer, ClaimScope scope,
              boost::asio::any_io_executor executor, RetainDone done);

  // The instant delete of DATA objects (remove.h): every node present is
  // asked (remove_objects; this node through remove_here), each checks
  // `writer` as it checks a write, releases the claims `observed` dominates
  // and drops its copy where no claim remains and its inventory does not
  // refer to it; a member of an object's replication set that is not present
  // is named unreachable. Each peer is asked within min(deadline,
  // control_call). The blocking form runs on the caller's thread; the
  // asynchronous form on the store's workers, its evidence posted to
  // `executor`, never inline.
  using RemoveDone = std::function<void(RemoveEvidence)>;
  static constexpr dannan::sched::Waits remove_waits = remove_here_waits | dannan::sched::Waits::network | dannan::sched::Waits::locks;
  RemoveEvidence remove(const std::vector<ObjectId>& ids, const contract::RetentionClock& observed, const WriteScope& writer,
                        std::chrono::milliseconds deadline);
  void remove(std::vector<ObjectId> ids, contract::RetentionClock observed, WriteScope writer, std::chrono::milliseconds deadline,
              boost::asio::any_io_executor executor, RemoveDone done);
  // Offers CONTROL objects this node holds to every other node present: each
  // is asked what it lacks and sent only that, at speculative priority.
  ControlOffer offer_control(const std::vector<ObjectId>& ids);
  bool ensure_control_local(const ObjectId& id) override;

  PromptReplicationStats prompt_replication_stats() const;
  ReplicaTransferStats replica_stats(const dannan::identity::NodeId& node) const { return _selector.stats(node); }
  // The clocks moved other than with the steady clock (a test's stepped
  // time): retries due by them are looked at now.
  void time_advanced();

 private:
  struct Shared;
  struct TicketRecord;
  struct WriteRun;
  struct SharedFetch;
  struct Job;
  struct RepairState;

  WriteEvidence run_write(const ObjectId& id, std::span<const uint8_t> data, const WriteOptions& options, std::shared_ptr<TicketRecord> into = {});
  std::shared_ptr<TicketRecord> new_ticket(const ObjectId& id, Durability level, bool deferred);
  void check_write(const ObjectId& id, std::span<const uint8_t> data) const;
  std::vector<dannan::membership::NodeRecord> hosting(const std::vector<dannan::membership::NodeRecord>& nodes) const;
  // The principal and prefix as the replicas are told them: this node under
  // the domain's name where the caller named none.
  message::WritePrincipal writer_of(const std::optional<dannan::contract::Principal>& principal, const std::string& prefix) const;
  // This node's check of a writer before it stores; the refusal's text, or
  // nothing when allowed.
  std::optional<std::string> scope_refusal(const message::WritePrincipal& writer) const;

  // Queues a job for the workers; false when `ephemeral_bytes` would take the
  // ephemeral queue past its bound.
  bool submit(std::function<void()> job, size_t ephemeral_bytes = 0);
  void worker_loop(std::stop_token stop);
  // A copy acknowledged by `node` outside a write's own calls (prompt
  // replication, a ticket's catch-up): every ticket for `id` hears of it.
  void note_acknowledged(const ObjectId& id, const dannan::identity::NodeId& node);
  void on_roster(const dannan::RosterChange& change);
  void catch_up(const ObjectId& id, const dannan::membership::NodeRecord& node);
  // Asks for what the tickets' copies awaiting a barrier owe, one barrier a
  // place (wait(), over many tickets).
  void run_barriers(const std::vector<std::shared_ptr<TicketRecord>>& records, std::chrono::milliseconds deadline);
  void wait_until(Ticket ticket, bool complete, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, WriteDone done);
  // Sets the wait going; the ticket when its barriers are still to be asked for.
  std::shared_ptr<TicketRecord> arm_wait(Ticket ticket, bool complete, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                                         WriteDone done);

  // Admission for this node's devices: DATA credit and a DATA slot, as
  // store::admit_data takes `wait`; a control slot.
  std::optional<DataAdmission> admit_data(dannan::sched::FrameType frame, uint64_t bytes, std::optional<std::chrono::milliseconds> wait,
                                          std::atomic_bool* cancelled = nullptr) const;
  std::optional<IoReserve::Slot> admit_control() const;
  // How a peer's put or fetch is watched: it has no other limit past a caller's own.
  dannan::net::CallWatch peer_call_watch() const;
  // Logs a deferred write a barrier left short of its level.
  void note_unmet_barrier(const WriteEvidence& evidence) const;
  // A synchronous call other than a transfer: control_no_progress.
  dannan::net::CallWatch control_call_watch() const;
  void note_network(uint64_t bytes, std::chrono::steady_clock::duration elapsed);
  void trace_repair(const std::string& detail) const;
  void note_unsourceable(const ObjectId& id);
  void note_local_unreadable(const ObjectId& id);

  bool put_on(const dannan::membership::NodeRecord& target, const ObjectId& id, std::span<const uint8_t> data, dannan::sched::FrameType frame);
  ReadResult read_impl(const ObjectId& id, const ReadOptions& options);
  // `until`: the deadline a read's earlier stages already ran on.
  ReadResult get_remote(const ObjectId& id, const ReadOptions& options, bool opportunistic_persist,
                        std::optional<dannan::contract::TimePoint> until = std::nullopt);
  void fetch_next(const std::shared_ptr<SharedFetch>& fetch);
  void fetched(const std::shared_ptr<SharedFetch>& fetch, const dannan::membership::NodeRecord& target, ReplicaWorkClass work,
               std::chrono::steady_clock::time_point started, dannan::net::CallOutcome outcome);
  void finish_fetch(const std::shared_ptr<SharedFetch>& fetch, ObjectData result, FetchAnswer answer, std::optional<dannan::identity::NodeId> served_by);
  void enqueue_local_copy(const ObjectId& id, std::span<const uint8_t> data, bool promote);
  void local_writer_loop(std::stop_token stop);
  void queue_prompt_replication(const ObjectId& id);
  void prompt_replication_loop(std::stop_token stop);

  bool retain_on(const dannan::membership::NodeRecord& target, contract::RetentionClass object_class, const std::vector<ObjectId>& ids,
                 const contract::RetentionDot& dot, const message::WritePrincipal& writer);
  bool offer_control_on(const dannan::membership::NodeRecord& target, const std::vector<ObjectId>& ids, dannan::sched::FrameType frame, ControlOffer& out);
  std::map<ObjectId, std::vector<dannan::membership::NodeRecord>> select_present_batched(
      const std::map<ObjectId, std::vector<dannan::membership::NodeRecord>>& candidates_by_object, size_t floor);
  std::map<dannan::identity::NodeId, std::map<ObjectId, bool>> batched_have_objects(
      const std::map<dannan::identity::NodeId, dannan::membership::NodeRecord>& node_info,
      const std::map<dannan::identity::NodeId, std::vector<ObjectId>>& ids_by_node);

  dannan::Context& _context;
  dannan::status::ActivityClocks& _activity;
  const boost::asio::any_io_executor _executor;
  StoragePool& _data;
  LocalStore& _control;
  PersistentBlockCache& _cache;
  contract::ClaimStore& _claims;
  const contract::ObjectLedger* const _ledger;
  const DistributedStoreOptions _options;
  const dannan::contract::TimeSource& _time;
  const dannan::identity::NodeId _self;
  const dannan::status::ScopedLogger _logger;
  const dannan::status::NodeEvents::Kind _storage_event;
  DataResourceArbiter* const _credit;
  IoReserve* const _io;
  ControlObjectFetch _control_fetch;
  ReplicaSelector _selector;
  std::atomic<double> _network_bps{0.0};
  // Repair's cursors and counters: the step's single owner's, the counters
  // read by anyone.
  std::unique_ptr<RepairState> _repair;
  // What completions and timers reach after the store has gone: they hold it
  // weakly.
  std::shared_ptr<Shared> _shared;
  uint64_t _watch = 0;
  // Set as the store goes: a write in progress returns on what it has.
  std::atomic<bool> _stopping{false};

  // The workers' queue. Lock order: taken with no other lock held.
  mutable dannan::contract::Mutex _jobs_mutex;
  std::condition_variable_any _jobs_wake;
  std::deque<std::function<void()>> _jobs DANNAN_GUARDED_BY(_jobs_mutex);
  bool _jobs_stopped DANNAN_GUARDED_BY(_jobs_mutex) = false;
  size_t _ephemeral_bytes DANNAN_GUARDED_BY(_jobs_mutex) = 0;
  std::vector<std::jthread> _workers;

  // Readers of one object share its fetch.
  mutable dannan::contract::Mutex _fetch_mutex;
  std::map<ObjectId, std::weak_ptr<SharedFetch>> _fetches DANNAN_GUARDED_BY(_fetch_mutex);

  // Prompt second copy: objects that reached only their floor, pushed to the
  // next owner by one worker as speculative DATA work.
  mutable dannan::contract::Mutex _prompt_mutex;
  std::condition_variable_any _prompt_wake;
  std::deque<ObjectId> _prompt_queue DANNAN_GUARDED_BY(_prompt_mutex);
  std::set<ObjectId> _prompt_queued DANNAN_GUARDED_BY(_prompt_mutex);
  std::atomic<uint64_t> _prompt_copies{0};
  std::atomic<uint64_t> _prompt_failures{0};
  std::atomic<uint64_t> _prompt_skipped_no_room{0};
  std::atomic<uint64_t> _prompt_dropped{0};
  std::jthread _prompt_thread;

  // Opportunistic local copies of fetched objects, written by one worker so
  // a read never waits on them.
  struct LocalCopy {
    ObjectId id;
    Bytes data;
    bool promote = false;
  };
  dannan::contract::Mutex _local_copy_mutex;
  std::condition_variable_any _local_copy_wake;
  std::condition_variable_any _local_copy_settled;
  std::deque<LocalCopy> _local_copies DANNAN_GUARDED_BY(_local_copy_mutex);
  size_t _local_copy_bytes DANNAN_GUARDED_BY(_local_copy_mutex) = 0;
  // A dequeued copy not yet written: with the queue empty and this false,
  // every queued copy is settled.
  bool _local_copy_writing DANNAN_GUARDED_BY(_local_copy_mutex) = false;
  std::jthread _local_writer;
};

}  // namespace dagda::store
