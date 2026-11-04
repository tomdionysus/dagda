//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/identity/node_id.h"
#include "dannan/membership/node_record.h"

namespace dagda::store {

enum class ReplicaWorkClass : uint8_t {
  foreground,
  speculative,
};

struct ReplicaTransferStats {
  size_t foreground_in_flight = 0;
  size_t speculative_in_flight = 0;
  double ewma_transfer_ms = 0;
  uint64_t successes = 0;
  uint64_t failures = 0;
  uint64_t selections = 0;
};

// Where a read goes (requirement S27): Macha's read-source policy for
// foreground fetches and speculative hydration (cluster/replica_selector.*).
// No network code: callers supply candidates, call started(), then
// finished(). A peer this node has fetched nothing from yet is estimated by
// Dannan's measured CONTROL round trip when the caller gives one (Macha
// assumed 100 ms for every unmeasured peer, so its first read went to
// whichever candidate came first). Thread-safe; waits on nothing.
class ReplicaSelector {
 public:
  using Latency = std::function<std::optional<std::chrono::milliseconds>(const dannan::identity::NodeId&)>;

  // All candidates, most suitable first. `stripe` rotates equal-scored
  // candidates deterministically.
  std::vector<dannan::membership::NodeRecord> order(const std::vector<dannan::membership::NodeRecord>& candidates, size_t stripe, ReplicaWorkClass work,
                                                    const Latency& latency = {}) const;

  void started(const dannan::identity::NodeId& node, ReplicaWorkClass work);
  // A speculative transfer became foreground.
  void promoted(const dannan::identity::NodeId& node);
  void finished(const dannan::identity::NodeId& node, ReplicaWorkClass work, size_t bytes, std::chrono::steady_clock::duration elapsed, bool success);

  ReplicaTransferStats stats(const dannan::identity::NodeId& node) const;

 private:
  struct State {
    size_t foreground_in_flight = 0;
    size_t speculative_in_flight = 0;
    double ewma_transfer_ms = 0;
    uint64_t successes = 0;
    uint64_t failures = 0;
    uint32_t consecutive_failures = 0;
    uint64_t selections = 0;
  };

  static double estimate_ms(const State& state, std::optional<std::chrono::milliseconds> round_trip);
  static double score(const State& state, ReplicaWorkClass work, std::optional<std::chrono::milliseconds> round_trip);

  mutable dannan::contract::Mutex _mutex;
  std::map<dannan::identity::NodeId, State> _states DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dagda::store
