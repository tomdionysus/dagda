//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/lease/fence.h"

#include <algorithm>

namespace dagda::lease {

using dannan::contract::Lock;

namespace {

FenceDecision decide(uint64_t highest, uint64_t epoch) {
  if (epoch < highest) return {false, highest, "epoch " + std::to_string(epoch) + " is below the lease's epoch " + std::to_string(highest)};
  return {true, std::max(highest, epoch), "epoch " + std::to_string(epoch) + " is the lease's"};
}

}  // namespace

FenceDecision fence(const std::optional<Lease>& known, uint64_t epoch) { return decide(known ? known->epoch : 0, epoch); }

FenceDecision EpochFence::admit(const std::string& name, uint64_t epoch) {
  Lock lock(_mutex);
  auto& highest = _highest[name];
  auto decision = decide(highest, epoch);
  highest = decision.highest;
  return decision;
}

void EpochFence::observe(const std::string& name, const Lease& lease) {
  Lock lock(_mutex);
  auto& highest = _highest[name];
  highest = std::max(highest, lease.epoch);
}

std::optional<uint64_t> EpochFence::highest(const std::string& name) const {
  Lock lock(_mutex);
  const auto found = _highest.find(name);
  if (found == _highest.end()) return std::nullopt;
  return found->second;
}

}  // namespace dagda::lease
