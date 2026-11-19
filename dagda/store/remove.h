//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dagda/contract/claim_store.h"
#include "dagda/contract/object_ledger.h"
#include "dagda/store/object_id.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/store/storage_pool.h"
#include "dannan/identity/node_id.h"
#include "dannan/sched/work.h"

// The instant delete (the maintainer, 2026-11-04 night, "Claims and space"): a caller
// that has observed the claims on some DATA objects asks every node that may
// hold them to release those claims and drop its copy at once. Safe across a
// partition because a replica releases only the claims the caller's observed
// clock dominates (the claims' observed-remove rule, ClaimStore::
// release_observed) and keeps its copy while any other claim on it remains or
// its current inventory refers to it. What is kept is named, node by node.
namespace dagda::store {

// What one node did with one object.
enum class RemoveState : uint8_t {
  // Its copy was deleted now: no claim on it remained and its inventory did
  // not refer to it.
  removed = 0,
  // It held no copy; the claims the clock dominates were released and none
  // remains.
  absent = 1,
  // A claim the caller's clock does not dominate remains (`claims` names
  // each): the copy stays until the collector releases it against the
  // owner's head.
  kept_by_claim = 2,
  // The node's current inventory refers to it, is incomplete, or has not
  // been published: the copy stays and the collector decides.
  kept_referenced = 3,
  // The node refused the caller's scope and changed nothing.
  refused = 4,
  // The node did not answer, or is a member of the object's replication set
  // that is not present: nothing is known of its copy.
  unreachable = 5,
};
const char* remove_state_name(RemoveState) noexcept;

struct RemovedObject {
  ObjectId id{};
  RemoveState state = RemoveState::unreachable;
  // For kept_by_claim: the claims that remain, by origin and sequence.
  std::vector<contract::RetentionDot> claims;
};

// One node's answer. For refused, `why` is the scope source's reason and
// `scope_generation` the generation of the scope data it read; for
// unreachable, why no answer came.
struct NodeRemoval {
  dannan::identity::NodeId node{};
  std::vector<RemovedObject> objects;
  std::string why;
  uint64_t scope_generation = 0;
  const RemovedObject* object(const ObjectId& id) const noexcept;
};

// What an instant delete did, on every node it concerns: every node present
// (the local one first, then in id order), then the members of an object's
// replication set that are not present. `finished`: it ran to its end before
// the deadline; otherwise `why` says what stopped it, and a delete still
// running may yet land on some nodes (asking again is harmless: the delete is
// idempotent). Never a bare boolean.
struct RemoveEvidence {
  std::vector<ObjectId> sought;
  bool finished = false;
  std::string why;
  std::vector<NodeRemoval> nodes;

  const NodeRemoval* on(const dannan::identity::NodeId& node) const noexcept;
  // The object's state on `node`; unreachable when the evidence does not name
  // the pair.
  RemoveState state(const dannan::identity::NodeId& node, const ObjectId& id) const noexcept;
  // Copies deleted by this call, over every node.
  size_t removed() const noexcept;
};

// A node's answer as its replica gives it: refused (its scope check said no,
// nothing changed), or one entry per object.
struct ReplicaRemoval {
  bool refused = false;
  uint64_t scope_generation = 0;
  std::string why;
  std::vector<RemovedObject> objects;
};

// What a replica works on: this node's DATA pool, its claims, the ledger
// whose published inventory says what the owner's state refers to (none, or
// none published: every copy is kept), and the cache, whose copy goes with
// the pool's.
struct RemoveReplica {
  StoragePool& data;
  contract::ClaimStore& claims;
  const contract::ObjectLedger* ledger = nullptr;
  PersistentBlockCache* cache = nullptr;
};

// The replica's half, after its scope check: under each object's claim guard
// (StoragePool::claim_locks), releases the DATA claims `observed` dominates,
// then removes the local copy of each object no claim remains on and a
// complete published inventory does not refer to; one entry per distinct id,
// in id order.
// Idempotent. Waits on the state device (the claims' journal) and the DATA
// device; the caller holds the device's admission. Thread-safe.
inline constexpr dannan::sched::Waits remove_here_waits = dannan::sched::Waits::state_device | dannan::sched::Waits::data_device;
std::vector<RemovedObject> remove_here(const RemoveReplica& replica, std::vector<ObjectId> ids, const contract::RetentionClock& observed);

}  // namespace dagda::store
