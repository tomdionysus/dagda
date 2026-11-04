//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <memory>

#include "dagda/contract/claim_store.h"
#include "dagda/contract/horizon.h"
#include "dagda/contract/object_store.h"
#include "dannan/sched/walk.h"
#include "dannan/sched/work.h"

// Edges by child: which objects this node has promised to keep (claimed),
// whether it holds them (held), and what the two horizons refer to.
// Implemented by ledger::RetentionLedger over a ClaimStore and the two
// classes' object stores. The horizon builder derives horizons, the
// collector publishes them, the ledger holds them.
namespace dagda::contract {

class ObjectLedger {
 public:
  virtual ~ObjectLedger() = default;

  // The claimed objects of one class after `from`, in id order, one budget
  // operation per claim. At the end the next cursor is the start and the
  // page is complete. A claim read per item.
  static constexpr dannan::sched::Waits claimed_waits = ClaimStore::read_waits;
  static constexpr dannan::sched::ThreadSafety claimed_safety = dannan::sched::ThreadSafety::thread_safe;
  virtual dannan::sched::Page<ObjectId, ObjectId> claimed(RetentionClass, dannan::sched::Cursor<ObjectId> from, dannan::sched::Budget&) const = 0;

  // Whether this node holds the object, from the class's object store.
  static constexpr dannan::sched::Waits held_waits = ObjectStore::has_waits;
  static constexpr dannan::sched::ThreadSafety held_safety = dannan::sched::ThreadSafety::thread_safe;
  virtual bool held(RetentionClass, const ObjectId&) const = 0;
  // The class's object store's held_view().
  static constexpr dannan::sched::Waits held_view_waits = ObjectStore::held_view_waits;
  static constexpr dannan::sched::ThreadSafety held_view_safety = dannan::sched::ThreadSafety::thread_safe;
  virtual HeldView held_view(RetentionClass) const = 0;

  // The published horizons, null until first published. A held handle's
  // snapshot never changes. A pointer copy.
  static constexpr dannan::sched::Waits horizon_waits = dannan::sched::Waits::none;
  static constexpr dannan::sched::ThreadSafety horizon_safety = dannan::sched::ThreadSafety::thread_safe;
  using InventoryHandle = std::shared_ptr<const InventoryHorizon>;
  using ReleaseHandle = std::shared_ptr<const ReleaseHorizon>;
  virtual InventoryHandle inventory() const = 0;
  virtual ReleaseHandle release() const = 0;

  // Publishes a horizon. An incomplete release build is refused and the
  // previous horizon kept; returns whether it was published. A null handle
  // throws std::invalid_argument. Single owner (the collector).
  static constexpr dannan::sched::Waits publish_waits = dannan::sched::Waits::none;
  static constexpr dannan::sched::ThreadSafety publish_safety = dannan::sched::ThreadSafety::single_owner;
  virtual void publish(InventoryHandle) = 0;
  virtual bool publish(ReleaseBuild) = 0;

  // Claims, forwarded to the node's ClaimStore.
  // Whether the object is claimed.
  static constexpr dannan::sched::Waits retained_waits = ClaimStore::read_waits;
  static constexpr dannan::sched::ThreadSafety retained_safety = dannan::sched::ThreadSafety::thread_safe;
  virtual bool retained(RetentionClass, const ObjectId&) const = 0;
  // Releases claims `release` no longer refers to and whose writes its clock
  // has observed. Bounded. Single owner (the collector).
  static constexpr dannan::sched::Waits release_waits = ClaimStore::release_waits;
  static constexpr dannan::sched::ThreadSafety release_safety = ClaimStore::release_safety;
  virtual size_t release_unreferenced(RetentionClass, const ReleaseHorizon& release, size_t operation_budget) = 0;
  // Forgets causality tombstones once no claim remains and the object is not
  // held. Bounded. Single owner (the collector).
  static constexpr dannan::sched::Waits prune_waits = held_waits | ClaimStore::prune_waits;
  static constexpr dannan::sched::ThreadSafety prune_safety = ClaimStore::prune_safety;
  virtual size_t prune_unclaimed(RetentionClass, size_t operation_budget) = 0;
  // Checkpoints the claims and empties their journal past a threshold.
  static constexpr dannan::sched::Waits compact_waits = ClaimStore::compact_waits;
  static constexpr dannan::sched::ThreadSafety compact_safety = ClaimStore::compact_safety;
  virtual bool compact_if_needed(size_t record_threshold) = 0;
};

}  // namespace dagda::contract
