//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>

#include "dagda/contract/object_ledger.h"
#include "dannan/sched/walk.h"
#include "dannan/status/observation.h"

namespace dagda::ledger {

// What the claim walk asks of its caller for a claimed object this node does
// not hold. The caller owns the network credit and the fetch.
class ClaimRestorer {
 public:
  virtual ~ClaimRestorer() = default;
  enum class Outcome : uint8_t { restored, not_restored, waiting_for_credit };
  virtual Outcome restore(contract::RetentionClass, const ObjectId&) = 0;
};

struct ClaimWalkStep {
  size_t examined = 0;
  size_t missing = 0;
  size_t restored = 0;
  // A missing claim met a caller out of credit; the cursor stays before it.
  bool waiting_for_credit = false;
  // The step reached its bound with claims left.
  bool unfinished = false;
};

// The claim walk (requirement L23). A durable claim is a promise about this
// node: a claimed copy lost to scrub or corruption, and belonging only to a
// branch this node has not seen, is invisible to repair by what is
// referenced. Each step examines a bounded page of one class's claims from a
// cursor it keeps, and asks the restorer for each claim not held. Presence
// costs nothing; only a missing claim may spend. Single owner.
class ClaimWalk {
 public:
  // Claims examined per step.
  static constexpr size_t step_bound = 16;

  // Counts what it examines and finds missing into `observations`
  // (maintenance.claim_walk.examined and .missing), when given.
  explicit ClaimWalk(contract::RetentionClass type, dannan::status::Observations* observations = nullptr) noexcept : _type(type), _observations(observations) {}

  ClaimWalkStep step(const contract::ObjectLedger&, ClaimRestorer&);

  contract::RetentionClass type() const noexcept { return _type; }
  const dannan::sched::Cursor<ObjectId>& cursor() const noexcept { return _cursor; }

 private:
  contract::RetentionClass _type;
  dannan::status::Observations* _observations;
  dannan::sched::Cursor<ObjectId> _cursor;
};

}  // namespace dagda::ledger
