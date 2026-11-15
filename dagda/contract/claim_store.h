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
#include <map>
#include <optional>
#include <vector>

#include "dagda/contract/id_lookup.h"
#include "dagda/store/object_id.h"
#include "dannan/identity/node_id.h"
#include "dannan/sched/work.h"

// The claims a node holds: the objects it has durably promised to keep, each
// by causal dot (requirement L12). Owned by the store; above the ledger it is
// reached only through the ledger. Implemented by ledger::RetentionStore.
namespace dagda::contract {

using ObjectId = store::ObjectId;
using NodeId = dannan::identity::NodeId;

// The wire and disk values are layout v18's.
enum class RetentionClass : uint8_t {
  data = 1,
  control = 2,
};

// One claim's causal identity: the writing node and its sequence.
struct RetentionDot {
  NodeId origin{};
  uint64_t sequence = 0;
  auto operator<=>(const RetentionDot&) const = default;
};

// What a head has observed of each node's claim writes.
using RetentionClock = std::map<NodeId, uint64_t>;

class ClaimStore {
 public:
  virtual ~ClaimStore() = default;

  // Every operation takes the store's lock, which writes, lookups (a ledger
  // node read) and compaction hold across their state-device I/O and nothing
  // holds across DATA or network I/O.

  // Claim writes: one durable journal frame per 65,536 ids. A dot with no
  // origin or a zero sequence throws std::invalid_argument.
  static constexpr dannan::sched::Waits write_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::ThreadSafety write_safety = dannan::sched::ThreadSafety::thread_safe;
  virtual void retain(RetentionClass, const ObjectId&, const RetentionDot&) = 0;
  virtual void retain_batch(RetentionClass, const std::vector<ObjectId>&, const RetentionDot&) = 0;

  // Claim reads, from the class's ledger; `retained_ids` and `claim_objects`
  // are linear in the class's claims.
  static constexpr dannan::sched::Waits read_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::ThreadSafety read_safety = dannan::sched::ThreadSafety::thread_safe;
  virtual bool retained(RetentionClass, const ObjectId&) const = 0;
  // The next claimed object after `cursor`, which it moves; none, with
  // `complete` set and the cursor back at the start, at the end.
  virtual std::optional<ObjectId> next_retained(RetentionClass, std::optional<ObjectId>& cursor, bool& complete) const = 0;
  virtual std::vector<ObjectId> retained_ids(RetentionClass) const = 0;
  virtual size_t claim_objects(RetentionClass) const = 0;
  // Diagnostic: the observed-remove state of one object, `adds` and
  // `removed` keyed by origin. Empty when the object has no state.
  struct Claims {
    std::map<NodeId, uint64_t> adds;
    std::map<NodeId, uint64_t> removed;
  };
  virtual Claims claims(RetentionClass, const ObjectId&) const = 0;

  // Releases the claims a complete horizon no longer refers to and whose
  // writes its clock has observed: a claim from a dot the clock does not
  // dominate survives. Bounded; one journal frame a slice. Single owner (the
  // collector): the store keeps the walk's position.
  static constexpr dannan::sched::Waits release_waits = write_waits;
  static constexpr dannan::sched::ThreadSafety release_safety = dannan::sched::ThreadSafety::single_owner;
  virtual size_t release_unreferenced(RetentionClass, const IdLookup& live, const RetentionClock& observed, size_t operation_budget) = 0;
  // Releases, on the named objects only, the claims `observed` dominates, by
  // the same observed-remove rule and journal frame as release_unreferenced:
  // a claim from a dot the clock does not dominate survives, and an add the
  // clock has already seen removed is refused when it arrives late. The
  // instant delete's release. Returns the objects whose state changed.
  static constexpr dannan::sched::Waits release_observed_waits = write_waits;
  static constexpr dannan::sched::ThreadSafety release_observed_safety = dannan::sched::ThreadSafety::thread_safe;
  virtual size_t release_observed(RetentionClass, const std::vector<ObjectId>& ids, const RetentionClock& observed) = 0;
  // Forgets causality tombstones once no claim remains and the object is
  // absent. Bounded; one journal frame. `exists` is called without the lock,
  // so this also waits on whatever `exists` waits on. Single owner (the
  // collector): the store keeps the walk's position.
  static constexpr dannan::sched::Waits prune_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::ThreadSafety prune_safety = dannan::sched::ThreadSafety::single_owner;
  virtual size_t prune_unclaimed(RetentionClass, const std::function<bool(const ObjectId&)>& exists, size_t operation_budget) = 0;
  // Checkpoints the ledger and empties the journal past a threshold.
  static constexpr dannan::sched::Waits compact_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::ThreadSafety compact_safety = dannan::sched::ThreadSafety::thread_safe;
  virtual bool compact_if_needed(size_t record_threshold = 4096) = 0;
};

}  // namespace dagda::contract
