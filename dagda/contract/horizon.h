//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "dagda/contract/claim_store.h"
#include "dagda/contract/id_lookup.h"
#include "dagda/ledger/object_trie.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/node_id.h"

// The ledger's two reachability horizons (requirements L16 to L18): what an
// owner's state refers to, per retention class, as an immutable snapshot
// published whole. A refresh publishes a new snapshot, never changing one in
// use. What is referenced, and what has been retired, is the owner's answer
// (reachability.h); the horizons are Dagda's.
namespace dagda::contract {

using Hash256 = dannan::identity::Hash256;

// One object the owner has retired: it stopped referring to `id` at
// `retired_at_ns` on the wall clock, or has not yet stamped when (zero),
// which keeps it uncollectable until it does. `token` is the owner's, handed
// back as given, so a decision taken on an older inventory names the
// retirement it saw and no later one of the same object (Macha's GarbageRef
// and its retirement id).
struct Garbage {
  ObjectId id{};
  int64_t retired_at_ns = 0;
  dannan::identity::NodeId token{};
  auto operator<=>(const Garbage&) const = default;
};

// A set of retirements kept as one CONTROL object `id`, all retired at
// `retired_at_ns`, dropped whole once its grace has run (Macha's tombstone
// batch). The state names it, so it is referenced CONTROL while it stands.
struct GarbageBatch {
  ObjectId id{};
  int64_t retired_at_ns = 0;
  uint32_t count = 0;
  auto operator<=>(const GarbageBatch&) const = default;
};

// The objects a state refers to, per class: optional frozen views of counted
// tries (ledger::ReferenceCounts, on disk, read through their cache) and a
// sorted set of others beside them, or the set alone. Macha's name, kept as
// the value type a ReachabilityProvider's answer becomes. Thread-safe; it
// must outlive the lookups it makes.
class ReferencedSets {
 public:
  struct Views {
    std::optional<ledger::ObjectTrie::Snapshot> data;
    std::optional<ledger::ObjectTrie::Snapshot> control;
  };
  // Takes ids in any order, with duplicates; keeps them sorted and unique.
  ReferencedSets(std::vector<ObjectId> data, std::vector<ObjectId> control);
  ReferencedSets(Views views, std::vector<ObjectId> data, std::vector<ObjectId> control);

  bool referenced(RetentionClass, const ObjectId&) const;
  // referenced() as a lookup; it reads this set, which must outlive it.
  IdLookup lookup(RetentionClass) const;
  // Up to `limit` ids after `after` (from the first when none), in order.
  std::vector<ObjectId> next(RetentionClass, const std::optional<ObjectId>& after, size_t limit) const;
  // Every id, in order: linear, for tests and the pull list's fallback.
  std::vector<ObjectId> all(RetentionClass) const;
  // The ids none of `held` holds, in order: the view joined against `held`
  // in step a page at a time, and each of the others looked up. `pause`, if
  // given, is called between pages.
  std::vector<ObjectId> not_held(RetentionClass, std::span<const ledger::ObjectTrie::Snapshot> held, const std::function<void()>& pause = {}) const;
  size_t size(RetentionClass) const;

 private:
  struct Set {
    std::optional<ledger::ObjectTrie::Snapshot> view;
    // Sorted and unique.
    std::vector<ObjectId> others;
    size_t size = 0;
  };
  const Set& of(RetentionClass) const;
  static Set make(std::optional<ledger::ObjectTrie::Snapshot>, std::vector<ObjectId>);

  Set _data;
  Set _control;
};

// The inventory at a generation of the owner's state: what repair,
// tombstone collection, CONTROL GC and the DATA sweep act on. Never used
// destructively in the pass that built it.
class InventoryHorizon : public ReferencedSets {
 public:
  // `garbage` is every retirement the owner holds; one whose id is still
  // referenced as DATA is stale (its object came back to life), the rest
  // collectable. Both lists keep the given order. `complete` is whether every
  // part of the owner's answer could be read.
  InventoryHorizon(uint64_t generation, bool complete, std::vector<ObjectId> data, std::vector<ObjectId> control, const std::vector<Garbage>& garbage);
  InventoryHorizon(uint64_t generation, bool complete, ReferencedSets sets, const std::vector<Garbage>& garbage, std::vector<GarbageBatch> batches = {});

  // The stamp: the generation the inventory was built at.
  uint64_t generation() const noexcept { return _generation; }
  bool complete() const noexcept { return _complete; }
  const std::vector<Garbage>& garbage() const noexcept { return _garbage; }
  const std::vector<Garbage>& stale_garbage() const noexcept { return _stale_garbage; }
  // The state's garbage batches, as given: dropped whole once matured.
  const std::vector<GarbageBatch>& batches() const noexcept { return _batches; }

 private:
  uint64_t _generation;
  bool _complete;
  std::vector<Garbage> _garbage;
  std::vector<Garbage> _stale_garbage;
  std::vector<GarbageBatch> _batches;
};

// The release horizon at the owner's sole accepted head: what claims are
// released against. Only a complete horizon is published.
class ReleaseHorizon : public ReferencedSets {
 public:
  ReleaseHorizon(Hash256 head, RetentionClock clock, std::vector<ObjectId> data, std::vector<ObjectId> control);
  ReleaseHorizon(Hash256 head, RetentionClock clock, ReferencedSets sets);

  // The stamp: the head's hash and the clock of the claim writes it saw.
  const Hash256& head() const noexcept { return _head; }
  const RetentionClock& clock() const noexcept { return _clock; }

 private:
  Hash256 _head;
  RetentionClock _clock;
};

// A release horizon as built; complete when every part it needed was
// readable.
struct ReleaseBuild {
  std::shared_ptr<const ReleaseHorizon> horizon;
  bool complete = false;
};

}  // namespace dagda::contract
