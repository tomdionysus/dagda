//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// An owner's ReachabilityProvider for the ledger's tests: what Macha's tests
// proved by driving the catalogue and the filesystem, expressed as the
// answers those gave the ledger.

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

#include "dagda/contract/reachability.h"

namespace dagda::test {

class TestProvider final : public contract::ReachabilityProvider {
 public:
  // The state now: its generation, the newest known, what it refers to.
  uint64_t current_generation = 1;
  uint64_t known = 1;
  contract::Reachability answer;
  bool inventory_throws = false;
  // The sole accepted head, and what each head refers to; a head with no
  // answer cannot be read at all.
  std::optional<contract::Hash256> head;
  std::map<contract::Hash256, contract::Reachability> heads;
  std::optional<contract::ControlEpoch> epoch;
  // Run as a removal is about to be held in its epoch: where a new epoch
  // may begin.
  std::function<void()> before_hold;
  std::vector<contract::GarbageCollection> collected;
  int inventories = 0;
  int releases = 0;

  uint64_t generation() override { return current_generation; }
  uint64_t known_generation() override { return known; }
  contract::Reachability inventory() override {
    ++inventories;
    if (inventory_throws) throw std::runtime_error("a node of the state is unreadable");
    auto out = answer;
    out.generation = current_generation;
    return out;
  }
  std::optional<contract::Hash256> release_head() override { return head; }
  contract::Reachability release(const contract::Hash256& at) override {
    ++releases;
    const auto found = heads.find(at);
    if (found == heads.end()) throw std::runtime_error("head unreadable");
    return found->second;
  }
  std::optional<contract::ControlEpoch> control_epoch() override { return epoch; }
  bool within_control_epoch(uint64_t sequence, const std::function<void()>& removal) override {
    if (before_hold) before_hold();
    if (!epoch || epoch->sequence != sequence) return false;
    removal();
    return true;
  }
  void collect_garbage(const contract::GarbageCollection& collection) override { collected.push_back(collection); }
};

}  // namespace dagda::test
