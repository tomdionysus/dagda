//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <functional>
#include <memory>

#include "dagda/contract/horizon.h"
#include "dagda/contract/reachability.h"
#include "dannan/status/log.h"

// The horizon builder (requirement L24): the horizons a node's collector
// publishes, made from its owner's ReachabilityProvider. Two builds that are
// pure functions of an answer, and the builder that asks for one.
namespace dagda::ledger {

// The inventory an answer describes: its views and ids, every batch as
// referenced CONTROL, its retirements split into stale and collectable,
// stamped with its generation and completeness.
std::shared_ptr<const contract::InventoryHorizon> build_inventory(contract::Reachability answer);

// The release horizon an answer describes, stamped with its head and clock,
// every batch as referenced CONTROL; complete only if the answer is.
contract::ReleaseBuild build_release(contract::Reachability answer);

// Single owner (the collector's thread), background class.
class NodeHorizonBuilder {
 public:
  NodeHorizonBuilder(contract::ReachabilityProvider& provider, dannan::status::ScopedLogger logger) noexcept;

  // At the provider's current generation. Throws what the provider throws:
  // an inventory that cannot be answered is not built.
  static constexpr dannan::sched::Waits inventory_waits = contract::ReachabilityProvider::answer_waits;
  std::shared_ptr<const contract::InventoryHorizon> inventory(const std::function<void()>& pause = {});
  // At `head`. A provider that cannot answer at all gives an empty,
  // incomplete build, which is never published.
  static constexpr dannan::sched::Waits release_waits = contract::ReachabilityProvider::answer_waits;
  contract::ReleaseBuild release(const contract::Hash256& head);

 private:
  contract::ReachabilityProvider& _provider;
  const dannan::status::ScopedLogger _logger;
};

}  // namespace dagda::ledger
