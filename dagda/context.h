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
#include "dagda/lease/keeper.h"
#include "dagda/lease/lease.h"
#include "dagda/store/distributed_store.h"
#include "dagda/store/object_id.h"
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
// Version 1 as it stands carries Domains, Objects (with the instant delete,
// step 7b), Claims, Leases and Tickets. Heads, Retained and node-local spaces are added at step 4, with
// the domain catalogue, and version 1 is frozen only then: until it is, a
// hosted system built against it is built against a contract still
// growing.
namespace dagda {

class Node;

// The contract's version. A hosted system states the version it was built
// against (check_version, require_version), and a mismatch is refused.
inline constexpr uint32_t context_version = 1;

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

   private:
    friend class Context;
    explicit Tickets(Node& node) : _node(node) {}
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
  Tickets& tickets() noexcept { return _tickets; }

 private:
  Domains _domains;
  Objects _objects;
  Claims _claims;
  Leases _leases;
  Tickets _tickets;
};

}  // namespace dagda
