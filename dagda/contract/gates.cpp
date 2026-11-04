//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/contract/gates.h"

#include <string_view>

namespace dagda::contract {
namespace {

std::string flag(std::string_view name, bool value) { return std::string(name) + (value ? "=1" : "=0"); }

bool inventory_complete(const InventoryHorizon* inventory) { return !inventory || inventory->complete(); }

bool generation_current(const PassFacts& facts, const InventoryHorizon* inventory) {
  return (inventory ? inventory->generation() : 0) >= facts.known_generation;
}

std::string not_due_reason(const PassFacts& facts) {
  return facts.busy ? "foreground busy" : facts.gc_waiting_for_event ? "complete; waiting for an event" : "quiet window";
}

}  // namespace

GateVerdict tombstone_gate(const PassFacts& facts, const InventoryHorizon* inventory) {
  GateVerdict verdict;
  if (!facts.garbage_due)
    verdict.reason = "not due";
  else if (facts.rebuilt_inventory)
    verdict.reason = "inventory rebuilt this pass";
  else if (!inventory_complete(inventory))
    verdict.reason = "catalogue inventory incomplete";
  verdict.permitted = verdict.reason.empty();
  verdict.conditions =
      flag("due", facts.garbage_due) + " " + flag("rebuilt", facts.rebuilt_inventory) + " " + flag("catalogue_complete", inventory_complete(inventory));
  return verdict;
}

GateVerdict control_gate(const PassFacts& facts, const InventoryHorizon* inventory) {
  GateVerdict verdict;
  if (!facts.gc_due)
    verdict.reason = not_due_reason(facts);
  else if (facts.rebuilt_inventory)
    verdict.reason = "inventory rebuilt this pass";
  else if (!facts.release_view)
    verdict.reason = "no sole accepted head for retention release";
  else if (!inventory_complete(inventory))
    verdict.reason = "catalogue inventory incomplete";
  else if (!inventory)
    verdict.reason = "no reachability inventory";
  else if (!generation_current(facts, inventory))
    verdict.reason = "inventory generation behind known";
  verdict.permitted = verdict.reason.empty();
  verdict.conditions = flag("due", facts.gc_due) + " " + flag("rebuilt", facts.rebuilt_inventory) + " " + flag("release_view", facts.release_view) + " " +
                       flag("catalogue_complete", inventory_complete(inventory)) + " " + flag("control_live", inventory != nullptr) + " " +
                       flag("generation_current", generation_current(facts, inventory));
  return verdict;
}

GateVerdict data_gate(const PassFacts& facts, const InventoryHorizon* inventory) {
  GateVerdict verdict;
  if (!facts.gc_due)
    verdict.reason = not_due_reason(facts);
  else if (facts.rebuilt_inventory)
    verdict.reason = "inventory rebuilt this pass";
  else if (!facts.release_view)
    verdict.reason = "no sole accepted head for retention release";
  else if (!inventory_complete(inventory))
    verdict.reason = "catalogue inventory incomplete";
  else if (!inventory)
    verdict.reason = "no reachability inventory";
  else if (!generation_current(facts, inventory))
    verdict.reason = "inventory generation " + std::to_string(inventory->generation()) + " behind known " + std::to_string(facts.known_generation);
  verdict.permitted = verdict.reason.empty();
  verdict.conditions = flag("due", facts.gc_due) + " " + flag("rebuilt", facts.rebuilt_inventory) + " " + flag("release_view", facts.release_view) + " " +
                       flag("catalogue_complete", inventory_complete(inventory)) + " " + flag("live", inventory != nullptr) + " " +
                       flag("generation_current", generation_current(facts, inventory));
  return verdict;
}

}  // namespace dagda::contract
