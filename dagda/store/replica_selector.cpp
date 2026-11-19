//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/replica_selector.h"

#include <algorithm>
#include <cmath>

namespace dagda::store {
namespace {

using dannan::contract::Lock;

constexpr double default_transfer_ms = 100.0;
constexpr double failure_penalty_ms = 500.0;
constexpr double ewma_alpha = 0.20;

}  // namespace

double ReplicaSelector::estimate_ms(const State& state, std::optional<std::chrono::milliseconds> round_trip) {
  if (state.ewma_transfer_ms > 0.0) return state.ewma_transfer_ms;
  // Unmeasured: the round trip is the least a transfer costs, and a nearer
  // peer is the better first guess.
  if (round_trip) return std::max(1.0, static_cast<double>(round_trip->count()));
  return default_transfer_ms;
}

double ReplicaSelector::score(const State& state, ReplicaWorkClass work, std::optional<std::chrono::milliseconds> round_trip) {
  const double transfer = estimate_ms(state, round_trip);
  const double failures = std::min<uint32_t>(state.consecutive_failures, 8) * failure_penalty_ms;
  if (work == ReplicaWorkClass::foreground) {
    // Optimise completion latency: in-flight foreground work weighs most,
    // but a clearly faster peer still wins when already busy.
    return transfer + transfer * 1.50 * static_cast<double>(state.foreground_in_flight) + transfer * 0.75 * static_cast<double>(state.speculative_in_flight) +
           failures;
  }
  // Speculation yields to any foreground work and prefers unused peers, which
  // stripes concurrent hydration over the replica set.
  if (state.foreground_in_flight) return 1.0e12 + transfer * static_cast<double>(state.foreground_in_flight) + failures;
  return transfer + transfer * 2.0 * static_cast<double>(state.speculative_in_flight) + failures;
}

std::vector<dannan::membership::NodeRecord> ReplicaSelector::order(const std::vector<dannan::membership::NodeRecord>& candidates, size_t stripe,
                                                                   ReplicaWorkClass work, const Latency& latency) const {
  if (candidates.size() < 2) return candidates;
  struct Ranked {
    dannan::membership::NodeRecord node;
    double score = 0;
    size_t tie = 0;
  };
  // Round trips are read before the lock: the latency source has locks of
  // its own.
  std::vector<std::optional<std::chrono::milliseconds>> round_trips;
  round_trips.reserve(candidates.size());
  for (const auto& candidate : candidates) round_trips.push_back(latency ? latency(candidate.id) : std::nullopt);

  std::vector<Ranked> ranked;
  ranked.reserve(candidates.size());
  {
    Lock lock(_mutex);
    const size_t rotation = stripe % candidates.size();
    for (size_t i = 0; i < candidates.size(); ++i) {
      const auto found = _states.find(candidates[i].id);
      const State empty;
      const State& state = found == _states.end() ? empty : found->second;
      ranked.push_back({candidates[i], score(state, work, round_trips[i]), (i + candidates.size() - rotation) % candidates.size()});
    }
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
    if (std::abs(a.score - b.score) > 0.001) return a.score < b.score;
    return a.tie < b.tie;
  });
  std::vector<dannan::membership::NodeRecord> out;
  out.reserve(ranked.size());
  for (auto& item : ranked) out.push_back(std::move(item.node));
  return out;
}

void ReplicaSelector::started(const dannan::identity::NodeId& node, ReplicaWorkClass work) {
  Lock lock(_mutex);
  auto& state = _states[node];
  ++state.selections;
  if (work == ReplicaWorkClass::foreground)
    ++state.foreground_in_flight;
  else
    ++state.speculative_in_flight;
}

void ReplicaSelector::promoted(const dannan::identity::NodeId& node) {
  Lock lock(_mutex);
  auto& state = _states[node];
  if (!state.speculative_in_flight) return;
  --state.speculative_in_flight;
  ++state.foreground_in_flight;
}

void ReplicaSelector::finished(const dannan::identity::NodeId& node, ReplicaWorkClass work, size_t, std::chrono::steady_clock::duration elapsed, bool success) {
  const double ms = std::max(0.001, std::chrono::duration<double, std::milli>(elapsed).count());
  Lock lock(_mutex);
  auto& state = _states[node];
  if (work == ReplicaWorkClass::foreground) {
    if (state.foreground_in_flight) --state.foreground_in_flight;
  } else if (state.speculative_in_flight) {
    --state.speculative_in_flight;
  }
  if (success) {
    ++state.successes;
    state.consecutive_failures = 0;
    state.ewma_transfer_ms = state.ewma_transfer_ms > 0.0 ? state.ewma_transfer_ms * (1.0 - ewma_alpha) + ms * ewma_alpha : ms;
  } else {
    ++state.failures;
    state.consecutive_failures = std::min<uint32_t>(state.consecutive_failures + 1, 8);
  }
}

ReplicaTransferStats ReplicaSelector::stats(const dannan::identity::NodeId& node) const {
  Lock lock(_mutex);
  const auto found = _states.find(node);
  if (found == _states.end()) return {};
  const auto& state = found->second;
  return {state.foreground_in_flight, state.speculative_in_flight, state.ewma_transfer_ms, state.successes, state.failures, state.selections};
}

}  // namespace dagda::store
