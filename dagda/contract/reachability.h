//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "dagda/contract/claim_store.h"
#include "dagda/contract/horizon.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/work.h"

// What an owner's state refers to, as the owner answers it (requirements
// 5.3, C3): the seam that keeps the ledger and the collector free of any
// owner's types. Dagda builds its horizons from the answers and collects
// garbage against them; what the state is, how it is walked and what a
// retirement means are the owner's.
namespace dagda::contract {

// One answer for one purpose. The ids are in any order, with duplicates;
// `views`, when the owner keeps counted tries (ledger::ReferenceCounts), are
// held beside them. A batch's id is referenced CONTROL: Dagda adds it.
struct Reachability {
  ReferencedSets::Views views;
  std::vector<ObjectId> data;
  std::vector<ObjectId> control;
  // Inventory only: every retirement the state holds, and its batches.
  std::vector<Garbage> garbage;
  std::vector<GarbageBatch> batches;
  // Every part could be read. Never a silently shorter set: a part the owner
  // could not read makes the answer incomplete. An incomplete release is
  // never published; an incomplete inventory shuts every destructive gate.
  bool complete = false;
  // The inventory's stamp: the generation answered for.
  uint64_t generation = 0;
  // The release's stamp: the head answered for, and the clock of the claim
  // writes it has observed.
  Hash256 head{};
  RetentionClock clock;
};

// The owner's CONTROL publication epoch. An owner that writes CONTROL objects
// before its state names them (data before metadata) leaves them briefly
// unreferenced; an object first seen unreferenced in an epoch survives that
// epoch, and only a later one orphans it. `since` is when the epoch began on
// the node's time source (the one the collector is given): an object touched
// since then is kept too. An owner that stages nothing begins a new epoch at
// every change of its state.
struct ControlEpoch {
  uint64_t sequence = 0;
  dannan::contract::TimePoint since{};
};

// What tombstone collection decided, handed to the owner to commit to its
// state: retirements to erase (stale ones, referenced again, and those whose
// grace has run), retirements never stamped (stamp them now, a full grace
// from here), and batches whose grace has run (drop them whole).
struct GarbageCollection {
  std::vector<Garbage> erase;
  std::vector<Garbage> stamp;
  std::vector<ObjectId> matured_batches;
};

// Every operation is called by the collector alone, on its thread.
class ReachabilityProvider {
 public:
  virtual ~ReachabilityProvider() = default;
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::single_owner;

  // The generation an inventory would be answered at now; cheap. And the
  // newest this node knows of, which it may not yet hold: an inventory
  // behind it is never used destructively.
  static constexpr dannan::sched::Waits generation_waits = dannan::sched::local_waits;
  virtual uint64_t generation() = 0;
  virtual uint64_t known_generation() = 0;

  // What the state at generation() refers to, its retirements and their
  // batches. Throws when it cannot answer at all. May read the owner's
  // CONTROL objects and fetch what it lacks.
  static constexpr dannan::sched::Waits answer_waits = dannan::sched::Waits::state_device | dannan::sched::Waits::network | dannan::sched::Waits::locks;
  virtual Reachability inventory() = 0;

  // The head claims are released against: the owner's sole accepted head,
  // none while it has several or none.
  virtual std::optional<Hash256> release_head() = 0;
  // What `head` refers to, stamped with it and its clock. Incomplete, never
  // shorter, when a part cannot be read.
  virtual Reachability release(const Hash256& head) = 0;

  // The CONTROL publication epoch; none until the owner has one, and then
  // CONTROL GC removes nothing.
  static constexpr dannan::sched::Waits epoch_waits = dannan::sched::Waits::locks;
  virtual std::optional<ControlEpoch> control_epoch() = 0;
  // Runs `removal` holding the epoch where it is, if it is still `sequence`,
  // and returns whether it ran: a CONTROL object is removed only while the
  // epoch it was judged in stands, so GC never races a new epoch and
  // deletes what it staged. `removal` waits on the CONTROL store.
  virtual bool within_control_epoch(uint64_t sequence, const std::function<void()>& removal) = 0;

  // Commits what tombstone collection decided. Called on every pass the
  // tombstone gate opens, with an inventory, whatever it holds.
  virtual void collect_garbage(const GarbageCollection&) = 0;
};

}  // namespace dagda::contract
