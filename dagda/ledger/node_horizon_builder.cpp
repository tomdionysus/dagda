//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/ledger/node_horizon_builder.h"

#include <string>
#include <utility>

namespace dagda::ledger {

std::shared_ptr<const contract::InventoryHorizon> build_inventory(contract::Reachability answer) {
  // A batch is a CONTROL object the state names: without it the collector
  // would delete the retirements it holds.
  for (const auto& batch : answer.batches) answer.control.push_back(batch.id);
  return std::make_shared<const contract::InventoryHorizon>(
      answer.generation, answer.complete, contract::ReferencedSets(std::move(answer.views), std::move(answer.data), std::move(answer.control)), answer.garbage,
      std::move(answer.batches));
}

contract::ReleaseBuild build_release(contract::Reachability answer) {
  for (const auto& batch : answer.batches) answer.control.push_back(batch.id);
  return {std::make_shared<const contract::ReleaseHorizon>(
              answer.head, std::move(answer.clock), contract::ReferencedSets(std::move(answer.views), std::move(answer.data), std::move(answer.control))),
          answer.complete};
}

NodeHorizonBuilder::NodeHorizonBuilder(contract::ReachabilityProvider& provider, dannan::status::ScopedLogger logger) noexcept
    : _provider(provider), _logger(std::move(logger)) {}

std::shared_ptr<const contract::InventoryHorizon> NodeHorizonBuilder::inventory() { return build_inventory(_provider.inventory()); }

contract::ReleaseBuild NodeHorizonBuilder::release(const contract::Hash256& head) {
  try {
    auto answer = _provider.release(head);
    // A build names the head it was asked for, whatever the answer says.
    answer.head = head;
    return build_release(std::move(answer));
  } catch (const std::exception& error) {
    _logger.debug(std::string("retention release horizon unavailable: ") + error.what());
    contract::Reachability none;
    none.head = head;
    return build_release(std::move(none));
  }
}

}  // namespace dagda::ledger
