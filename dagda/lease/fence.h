//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "dagda/lease/lease.h"
#include "dannan/contract/thread_safety.h"

// The epoch fence (plan, "Store: leases and claimed collections"; requirement
// E9, new: Macha never checks one). Every action taken under a lease carries
// the lease's epoch, and the receiver refuses one lower than the highest it
// knows for that lease. Two holders can coexist for one absence window; the
// fence is what keeps the stale one from acting. An equal epoch is the
// holder's own and is admitted.
namespace dagda::lease {

// A receiver's answer to an action, as evidence: admitted or not, the
// highest epoch the receiver knew for the lease when it decided, and why.
struct FenceDecision {
  bool admitted = false;
  uint64_t highest = 0;
  std::string why;
};

// The pure check: an action under `epoch` against the lease record the
// receiver holds (none: anything is admitted).
FenceDecision fence(const std::optional<Lease>& known, uint64_t epoch);

// A receiver's fence over many leases: the highest epoch it has admitted or
// seen in a record, per lease name. It forgets on restart; a receiver that
// restarts raises it again from the records it holds (observe). Thread-safe;
// waits on nothing.
class EpochFence {
 public:
  // Admits an action under `epoch` unless a higher one is known; an admitted
  // epoch raises the floor.
  FenceDecision admit(const std::string& name, uint64_t epoch);
  // Raises the floor from a lease record this node holds (a takeover it has
  // merged), before any action under it arrives.
  void observe(const std::string& name, const Lease& lease);
  // The floor: empty for a lease nothing is known of.
  std::optional<uint64_t> highest(const std::string& name) const;

 private:
  mutable dannan::contract::Mutex _mutex;
  std::map<std::string, uint64_t, std::less<>> _highest DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dagda::lease
