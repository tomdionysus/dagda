//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dagda/contract/claim_store.h"
#include "dagda/domain/domain.h"
#include "dagda/heads/space.h"
#include "dagda/heads/spaces.h"
#include "dagda/lease/keeper.h"
#include "dagda/lease/lease.h"
#include "dagda/store/distributed_store.h"
#include "dagda/store/object_id.h"
#include "dannan/sched/walk.h"
#include "dannan/sched/work.h"

// What a hosted system holds of the store (requirements C1, C2): the
// design's nouns, built over a dagda::Node and handed to the system beside
// Dannan's Context. Written as a specification: every operation states what
// it waits on and its thread safety; every asynchronous operation takes a
// deadline and the caller's executor and posts its completion there, never
// inline, at the latest when the deadline passes; every blocking form is a
// wait on that completion, never another code path, and must not be called
// from one of the io context's threads; nothing that happened on more than
// one node is a bare boolean. Every noun is thread-safe.
//
// Version 1 is frozen at 0.1.4 (Tom, 2026-11-13): Domains, Objects (with the
// instant delete), Claims, Leases, Tickets and, on one node, Heads; it is what
// Macha's port builds on, and it stays served. Version 2 adds the three-systems
// design's nouns slice by slice, from slice 1 the change notice (Heads::watch,
// Heads::changed), and is frozen at slice 4: until then a hosted system built
// against it is built against a contract still growing, pinned to a slice's tag.
namespace dagda {

class Node;

// The contract's version. A hosted system states the version it was built
// against (check_version, require_version): this one, or the frozen one served
// beside it; anything else is refused.
inline constexpr uint32_t context_version = 2;
inline constexpr uint32_t context_version_frozen = 1;

// A version check's evidence: what the system was built against, what this
// library serves, and why it was refused.
struct VersionCheck {
  bool accepted = false;
  uint32_t built_against = 0;
  uint32_t served = 0;
  std::string why;
};

class ContextVersionMismatch final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

using store::Durability;
using store::ObjectId;
using store::ReadOptions;
using store::ReadResult;
using store::RemoveEvidence;
using store::RemoveState;
using store::Ticket;
using store::WriteEvidence;
using store::WriteOptions;
using store::WriteScope;

// What a claim came to. `finished`: it ran to its end before the deadline;
// then `unheld` (DATA) names, sorted, the objects no node present holds,
// and `held_here` (CONTROL) whether this node's own claim held. Otherwise
// `why` says what stopped it or that the deadline passed; a claim still
// running then may yet land, and claiming again is harmless.
struct ClaimEvidence {
  contract::RetentionClass type = contract::RetentionClass::data;
  size_t sought = 0;
  bool finished = false;
  std::vector<ObjectId> unheld;
  bool held_here = false;
  std::string why;
};

// What opening a head space came to. `opened`: it replayed and stands at
// `generation`. Otherwise `why` says what stopped it or that the deadline passed;
// an open still running then may yet land, and opening again says so.
struct SpaceOpened {
  bool opened = false;
  heads::SpaceId space{};
  uint64_t generation = 0;
  std::string why;
};

// How much a head read is asked to do: heads::Space's three.
enum class HeadRead : uint8_t { current, local, converged };

// What a head read came to. `finished`: it could happen, and `view` is what it found,
// empty for finding nothing. Otherwise `why` says why it could not, or that the
// deadline passed, and `stopped` that a long walk was told to stop.
struct HeadReadEvidence {
  bool finished = false;
  bool stopped = false;
  std::optional<heads::SpaceView> view;
  std::string why;
};

// What a mutation came to. `finished`: it ran to its end before the deadline, and
// `outcome` says what it did. Otherwise `why` says what refused it or that the deadline
// passed, in which case it may yet land, and the caller's identity will tell;
// `stopped` that a long walk was told to stop.
struct MutationEvidence {
  bool finished = false;
  bool stopped = false;
  std::optional<heads::CommitOutcome> outcome;
  std::string why;
};

// The heads of a space as this node holds them. `known`: the space is open here, and
// otherwise `why` says what stopped it.
struct HeadsEvidence {
  bool known = false;
  heads::HeadsReport report;
  // How it stands with the peers (dagda.heads.* carries the same).
  heads::ExchangeStatus status;
  heads::ConvergenceDemandDiagnostics convergence;
  heads::OwedBacklog owed;
  std::string why;
};

// A watch on a space (D4). `watching`: the space is open here, and `token` withdraws the
// watch; otherwise `why` says why not.
struct HeadWatch {
  bool watching = false;
  uint64_t token = 0;
  std::string why;
};

// The keys whose entries differ between two heads (D4). `finished`: they could be read,
// and `page` is a page of them, empty for none; otherwise `why` says why they could not.
struct HeadChanges {
  bool finished = false;
  dannan::sched::Page<std::string, std::string> page;
  std::string why;
};

class Context {
 public:
  // The domains this node holds. Until the catalogue (a head space, step 4)
  // a node holds the one domain it was opened with, v1 (Macha's) by
  // default. Waits on nothing.
  class Domains {
   public:
    static constexpr dannan::sched::Waits list_waits = dannan::sched::Waits::none;
    std::vector<domain::Domain> list() const;
    // Nothing for a domain this node does not hold.
    std::optional<domain::Domain> describe(const domain::DomainId& id) const;

   private:
    friend class Context;
    explicit Domains(Node& node) : _node(node) {}
    Node& _node;
  };

  // Content-addressed objects in the node's domain, named by the SHA-256
  // of their bytes. A write is checked at every storing replica under
  // options.principal and options.prefix (this node and the domain's name
  // when unset) and waits for options.level; its evidence names every node
  // and carries the ticket that outlives the call. A read is local, then
  // the cache, then remote owners in latency order.
  class Objects {
   public:
    using WriteDone = std::function<void(WriteEvidence)>;
    using ReadDone = std::function<void(ReadResult)>;

    static constexpr dannan::sched::Waits write_waits =
        dannan::sched::Waits::state_device | dannan::sched::Waits::data_device | dannan::sched::Waits::network | dannan::sched::Waits::locks;
    static constexpr dannan::sched::Waits read_waits = write_waits;
    // Throws std::invalid_argument for a deadline that is not positive, and
    // std::runtime_error for bytes larger than an extent.
    void write(store::Bytes data, WriteOptions options, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, WriteDone done);
    WriteEvidence write(std::span<const uint8_t> data, WriteOptions options, std::chrono::milliseconds deadline);
    // Throws std::invalid_argument for a deadline that is not positive.
    void read(const ObjectId& id, ReadOptions options, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, ReadDone done);
    ReadResult read(const ObjectId& id, ReadOptions options, std::chrono::milliseconds deadline);

    // Contract version 2, Macha's forms (Tom, 2026-11-14). A write naming its id,
    // which is checked against the bytes, hashed once; throws std::runtime_error
    // when they differ or the bytes are larger than an extent. The blocking forms
    // run on the caller's own thread, no worker hop, and take their deadline from
    // the options: zero is none of its own, as Macha's, admission's window and
    // each peer's no-progress bound ending the wait.
    void write(const ObjectId& id, store::Bytes data, WriteOptions options, boost::asio::any_io_executor executor, WriteDone done);
    WriteEvidence write(const ObjectId& id, std::span<const uint8_t> data, WriteOptions options);
    ReadResult read(const ObjectId& id, ReadOptions options);
    // Macha's filesystem forms, on the caller's own thread. put_here: durable here
    // and returned at once, the further copies owed to prompt replication; a
    // put() on a node that hosts no extents. put_deferred_here: written here, made
    // durable by a wait on the evidence's ticket. Both throw std::runtime_error
    // when the bytes are not the id's.
    bool put_here(const ObjectId& id, std::span<const uint8_t> data, dannan::sched::FrameType frame = dannan::sched::FrameType::loader);
    WriteEvidence put_deferred_here(const ObjectId& id, std::span<const uint8_t> data, dannan::sched::FrameType frame = dannan::sched::FrameType::loader);
    // In this node's pool or its cache. Asks no peer.
    bool locally_available(const ObjectId& id) const;
    // Written through to this node's cache, when it has one; false is only that
    // the cache did not take it.
    bool cache_local(const ObjectId& id, std::span<const uint8_t> data);
    // This node has a cache to hydrate into.
    bool hydration_available() const;
    // True once the object is here, fetching it into the cache if not.
    bool hydrate(const ObjectId& id, ReadOptions options = {});
    // Made local: held and valid, else from the cache, else fetched.
    bool ensure_local(const ObjectId& id, bool foreground = false);
    // Moves when what this node can reach may have (the membership, its disks
    // online): a write parked as unreachable retries when it does.
    uint64_t reachability_epoch() const;
    // This node's pool holds it: the local ground truth. Asks no peer.
    static constexpr dannan::sched::Waits has_waits = dannan::sched::local_waits;
    bool has(const ObjectId& id) const;

    // The instant delete (Tom, 2026-11-04 night, "Claims and space"; the
    // message and the replica's rule are store/remove.h). Every node present
    // checks `writer` as it checks a write, releases on each object only the
    // claims `observed` dominates (a claim written where the caller has not
    // seen survives, partition or not), and deletes its copy at once where no
    // claim remains and its current inventory does not refer to it; it keeps
    // it otherwise. The evidence names, node by node and object by object,
    // what was removed, which claim kept a copy, what the inventory kept,
    // which node refused the scope and why, and which could not be reached
    // (a member of the replication set not present among them). Idempotent:
    // asking again changes nothing more. What a delete keeps the collector
    // frees later, once its claims go. Throws std::invalid_argument for a
    // deadline that is not positive.
    using RemoveDone = std::function<void(RemoveEvidence)>;
    static constexpr dannan::sched::Waits remove_waits = store::DistributedStore::remove_waits;
    void remove(std::vector<ObjectId> ids, contract::RetentionClock observed, WriteScope writer, std::chrono::milliseconds deadline,
                boost::asio::any_io_executor executor, RemoveDone done);
    RemoveEvidence remove(std::vector<ObjectId> ids, contract::RetentionClock observed, WriteScope writer, std::chrono::milliseconds deadline);

   private:
    friend class Context;
    explicit Objects(Node& node) : _node(node) {}
    Node& _node;
  };

  // Claims: this node's durable promise, and its peers', to keep objects
  // (requirement L12), each by causal dot. retain() claims DATA objects on
  // up to the store's write_copies nodes that hold them, or makes CONTROL
  // objects local, offers them and claims them on the nodes `scope` names;
  // each claiming node checks `writer` as it checks a write. There is no
  // release call, as in Macha: a claim goes when the owner's release
  // horizon (ReachabilityProvider::release) no longer refers to it and its
  // clock has observed the claim, in the node's collector, or when an
  // instant delete (Objects::remove) whose clock has observed it names its
  // object.
  class Claims {
   public:
    using Done = std::function<void(ClaimEvidence)>;
    using Scope = store::DistributedStore::ClaimScope;

    static constexpr dannan::sched::Waits retain_waits = dannan::sched::Waits::state_device | dannan::sched::Waits::network | dannan::sched::Waits::locks;
    // Throws std::invalid_argument for a deadline that is not positive or a
    // dot with no origin or sequence.
    void retain(contract::RetentionClass type, std::vector<ObjectId> ids, contract::RetentionDot dot, WriteScope writer, std::chrono::milliseconds deadline,
                boost::asio::any_io_executor executor, Done done, Scope scope = Scope::everywhere);
    ClaimEvidence retain(contract::RetentionClass type, std::vector<ObjectId> ids, contract::RetentionDot dot, WriteScope writer,
                         std::chrono::milliseconds deadline, Scope scope = Scope::everywhere);
    // This node holds a claim on it.
    static constexpr dannan::sched::Waits retained_waits = dannan::sched::local_waits;
    bool retained(contract::RetentionClass type, const ObjectId& id) const;
    // Contract version 2, Macha's retain_data_here: claims here whichever of
    // `ids` this node holds, on the caller's own thread, and returns the rest,
    // sorted and unique.
    std::vector<ObjectId> retain_data_here(const std::vector<ObjectId>& ids, const contract::RetentionDot& dot);

   private:
    friend class Context;
    explicit Claims(Node& node) : _node(node) {}
    Node& _node;
  };

  // Leases (requirement E7): acquire(scope, name) -> {holder, epoch, since}
  // through the node's lease keeper, absence from Dannan's roster. The
  // records live in a head space the owner owns from step 4. Until then
  // they are held in this node's memory alone (lease::LocalLeaseRecords):
  // another node does not see them and a restart forgets them, so a lease
  // here orders work on one node only.
  class Leases {
   public:
    using Done = std::function<void(lease::Acquisition)>;

    // Until heads: memory only. From step 4: the head space's write.
    static constexpr dannan::sched::Waits acquire_waits = dannan::sched::Waits::locks;
    // Throws std::invalid_argument for a deadline that is not positive.
    void acquire(const std::string& scope, const std::string& name, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Done done,
                 size_t rank = 0);
    lease::Acquisition acquire(const std::string& scope, const std::string& name, std::chrono::milliseconds deadline, size_t rank = 0);
    // The record as this node holds it, and whether `mine` still stands
    // against it. Wait on nothing.
    std::optional<lease::Lease> read(const std::string& scope, const std::string& name) const;
    lease::Standing standing(const std::string& scope, const std::string& name, const lease::Lease& mine) const;

   private:
    friend class Context;
    explicit Leases(Node& node) : _node(node) {}
    Node& _node;
  };

  // Head spaces (requirements H14, H20, H21): an owner's opaque state as a set of
  // immutable commits, each named by its hash. open() names the owner's codec and
  // merge and the policy; one every-node space is Macha's, and `replicas: N` is refused
  // naming L2. A read is as Space's three say. mutate_delta() commits on this node
  // alone with a change the caller declares and mutate() with what the codec's
  // encode_delta makes of the two states; the peers are owed it.
  class Heads {
   public:
    using Opened = std::function<void(SpaceOpened)>;
    using Read = std::function<void(HeadReadEvidence)>;
    using Committed = std::function<void(MutationEvidence)>;

    static constexpr dannan::sched::Waits open_waits = heads::Spaces::open_waits;
    static constexpr dannan::sched::Waits current_waits = heads::Space::current_waits;
    static constexpr dannan::sched::Waits local_waits = heads::Space::local_waits;
    static constexpr dannan::sched::Waits converged_waits = heads::Space::converged_waits;
    static constexpr dannan::sched::Waits mutate_waits = heads::Space::mutate_waits;
    static constexpr dannan::sched::Waits heads_waits = heads::Space::heads_waits;

    // The id a name has in this node's domain. Waits on nothing.
    heads::SpaceId id_of(std::string_view name) const;

    // Throws std::invalid_argument for a deadline that is not positive.
    void open(heads::SpaceSpec spec, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Opened done);
    SpaceOpened open(heads::SpaceSpec spec, std::chrono::milliseconds deadline);

    // current_waits: nothing is read, and an open space with no head decoded yet
    // answers an empty view.
    HeadReadEvidence current(const heads::SpaceId& space) const;
    // Throws std::invalid_argument for a deadline that is not positive. The wait guard
    // checks `work` against what `how` declares.
    void read(const heads::SpaceId& space, HeadRead how, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Read done,
              dannan::sched::WorkContext work = dannan::sched::WorkContext{});
    HeadReadEvidence read(const heads::SpaceId& space, HeadRead how, std::chrono::milliseconds deadline,
                          dannan::sched::WorkContext work = dannan::sched::WorkContext{});

    // Throws std::invalid_argument for a deadline that is not positive.
    void mutate(const heads::SpaceId& space, heads::MutationFactory mutation, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                Committed done, std::optional<heads::ExternalIdentity> identity = {}, size_t retries = heads::Space::default_retries,
                dannan::sched::WorkContext work = dannan::sched::WorkContext{});
    MutationEvidence mutate(const heads::SpaceId& space, heads::MutationFactory mutation, std::chrono::milliseconds deadline,
                            std::optional<heads::ExternalIdentity> identity = {}, size_t retries = heads::Space::default_retries,
                            dannan::sched::WorkContext work = dannan::sched::WorkContext{});
    void mutate_delta(const heads::SpaceId& space, heads::MutationFactory mutation, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                      Committed done, std::optional<heads::ExternalIdentity> identity = {}, size_t retries = heads::Space::default_retries,
                      dannan::sched::WorkContext work = dannan::sched::WorkContext{});
    MutationEvidence mutate_delta(const heads::SpaceId& space, heads::MutationFactory mutation, std::chrono::milliseconds deadline,
                                  std::optional<heads::ExternalIdentity> identity = {}, size_t retries = heads::Space::default_retries,
                                  dannan::sched::WorkContext work = dannan::sched::WorkContext{});

    // heads_waits.
    HeadsEvidence heads(const heads::SpaceId& space) const;

    // The change notice (D4), heads::Space::watch on this node's space: `notice` is
    // posted to `executor`, never called inline, each time this node's committed head
    // moves, a peer's commit arriving included. The executor must outlive the watch.
    // Waits on nothing.
    HeadWatch watch(const heads::SpaceId& space, boost::asio::any_io_executor executor, std::function<void(heads::HeadMoved)> notice);
    // Nothing is posted for the token after it returns. Waits on nothing.
    void unwatch(const heads::SpaceId& space, uint64_t token);
    // The keys whose entries differ between two heads this node holds, a page at a time
    // in key order, with the owner's tree: heads::Space::changed. Reads only where the
    // trees differ, at most until the budget's deadline. What a notice names as `before`
    // and `after` is held here when it is told.
    template <class Value>
    HeadChanges changed(const heads::Tree<Value>& tree, const heads::SpaceId& space, const heads::HeadIdentity& before, const heads::HeadIdentity& after,
                        dannan::sched::Cursor<std::string> from, dannan::sched::Budget& budget) const {
      HeadChanges out;
      auto* found = find(space, out.why);
      if (!found) return out;
      try {
        out.page = found->changed(tree, before, after, from, budget);
        out.finished = true;
      } catch (const std::exception& error) {
        out.why = error.what();
      }
      return out;
    }

   private:
    friend class Context;
    explicit Heads(Node& node) : _node(node) {}
    // This node's open space, or none with `why` saying so.
    heads::Space* find(const heads::SpaceId& space, std::string& why) const;
    void commit(const heads::SpaceId& space, heads::MutationFactory mutation, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                Committed done, std::optional<heads::ExternalIdentity> identity, size_t retries, dannan::sched::WorkContext work, bool supplied);
    Node& _node;
  };

  // A write's ticket, after the write has returned: status() waits on
  // nothing (nothing for a ticket forgotten); wait() completes when it
  // settles, wait_complete() when no member is pending at all, each at the
  // latest at the deadline, with the evidence as it then stands (nothing
  // for a ticket forgotten).
  class Tickets {
   public:
    using Done = std::function<void(std::optional<WriteEvidence>)>;

    static constexpr dannan::sched::Waits status_waits = dannan::sched::Waits::none;
    static constexpr dannan::sched::Waits wait_waits = dannan::sched::Waits::data_device | dannan::sched::Waits::network;
    std::optional<WriteEvidence> status(Ticket ticket) const;
    // Throw std::invalid_argument for a deadline that is not positive.
    void wait(Ticket ticket, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Done done);
    std::optional<WriteEvidence> wait(Ticket ticket, std::chrono::milliseconds deadline);
    void wait_complete(Ticket ticket, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Done done);
    std::optional<WriteEvidence> wait_complete(Ticket ticket, std::chrono::milliseconds deadline);
    // Contract version 2: one wait over many tickets (a publication's extents),
    // one barrier a place whichever tickets name it; the evidence in the order
    // asked, a forgotten ticket's empty. Throws std::invalid_argument for a
    // deadline that is not positive.
    using ManyDone = std::function<void(std::vector<WriteEvidence>)>;
    void wait(std::vector<Ticket> tickets, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, ManyDone done);
    std::vector<WriteEvidence> wait(const std::vector<Ticket>& tickets, std::chrono::milliseconds deadline);

   private:
    friend class Context;
    explicit Tickets(Node& node) : _node(node) {}
    Node& _node;
  };

  // CONTROL objects (a head space's nodes, Macha's catalogue and metadata trees)
  // on this node's control store, outside DATA credit (law 1). Contract version 2,
  // Macha's bare forms: none claims anything; a claim is Claims::retain(control).
  // Each blocks on the caller's own thread.
  class Control {
   public:
    static constexpr dannan::sched::Waits local_waits = dannan::sched::local_waits;
    static constexpr dannan::sched::Waits fetch_waits = dannan::sched::Waits::state_device | dannan::sched::Waits::network | dannan::sched::Waits::locks;
    // Held here: the control store only, no peer asked.
    bool has(const ObjectId& id) const;
    // Opened and checked against its id; nothing when not held here.
    std::optional<store::Bytes> get(const ObjectId& id) const;
    // Durable here before it returns; nothing offered, nothing claimed. Throws
    // std::runtime_error when the bytes are not the id's. False when this node's
    // store could not take it.
    bool put(const ObjectId& id, std::span<const uint8_t> data);
    // Made local from a peer that holds it, verified by its id; true at once when
    // held here, false when no peer could supply it.
    bool ensure_local(const ObjectId& id);
    // Offered to every active node that lacks them, writing no claim: only when
    // every one is held here.
    store::ControlOffer offer(const std::vector<ObjectId>& ids);

   private:
    friend class Context;
    explicit Control(Node& node) : _node(node) {}
    Node& _node;
  };

  // When work of each class was last present here, on the one clock the store,
  // repair, the local stages and DATA admission read (contract version 2). The
  // store notes its own object work; a hosted system notes what only it sees, as
  // Macha's HTTP server notes every API request and every response body still
  // draining as a viewer present (Macha's law 2). Lock-free; waits on nothing;
  // safe from any thread.
  class Activity {
   public:
    static constexpr dannan::sched::Waits waits = dannan::sched::Waits::none;
    // A class present now, with no bytes: a request.
    void note(dannan::sched::WorkClass work);
    // Bytes a frame class moved, and its class present.
    void note(dannan::sched::FrameType frame, uint64_t bytes);
    // 24 h when the class has never been present.
    std::chrono::milliseconds idle_for(dannan::sched::WorkClass work) const;
    // The bytes since the last take, for foreground, read-ahead and loader.
    uint64_t take_bytes(dannan::sched::FrameType frame);

   private:
    friend class Context;
    explicit Activity(Node& node) : _node(node) {}
    Node& _node;
  };

  // Made by the node, for the node's life.
  explicit Context(Node& node);
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  uint32_t version() const noexcept;
  // Whether a hosted system built against `built_against` (its
  // dagda::context_version) is served, with the reason when not. Waits on
  // nothing.
  VersionCheck check_version(uint32_t built_against) const noexcept;
  // As check_version, throwing ContextVersionMismatch with the reason.
  void require_version(uint32_t built_against) const;

  Domains& domains() noexcept { return _domains; }
  Objects& objects() noexcept { return _objects; }
  Claims& claims() noexcept { return _claims; }
  Leases& leases() noexcept { return _leases; }
  Heads& heads() noexcept { return _heads; }
  Tickets& tickets() noexcept { return _tickets; }
  Activity& activity() noexcept { return _activity; }
  Control& control() noexcept { return _control; }

 private:
  Domains _domains;
  Objects _objects;
  Claims _claims;
  Leases _leases;
  Heads _heads;
  Tickets _tickets;
  Activity _activity;
  Control _control;
};

}  // namespace dagda
