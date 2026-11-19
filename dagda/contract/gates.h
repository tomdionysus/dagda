//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string>

#include "dagda/contract/horizon.h"

// The three destructive gates (requirement L19), one per row of the gate
// table: whether a collection pass may proceed, and why not. A gate reads
// the inventory's stamp and the pass's facts; it fetches nothing and waits on
// nothing. Every gate reads this node's own state only: no gate waits for
// another node. What protects an absent node's references is the grace the
// sweeps apply on this node's clock.
//
// The reasons and conditions are Macha's words, unchanged, so its decision
// traces read the same: "catalogue" in them names the owner's inventory,
// complete when every part of the owner's answer was.
namespace dagda::contract {

// What the pass knows when it reaches the gates.
struct PassFacts {
  // Tombstone collection is due: no foreground work, the owner's state not
  // dirty.
  bool garbage_due = false;
  // Garbage collection is due: no foreground work, past its quiet window.
  bool gc_due = false;
  // Why GC is not due: foreground work, or GC finished and waiting for an
  // event (its quiet window unbounded).
  bool busy = false;
  bool gc_waiting_for_event = false;
  // The inventory was rebuilt in this pass: not yet used destructively.
  bool rebuilt_inventory = false;
  // A sole accepted head to release against.
  bool release_view = false;
  // The generation of the owner's state this node knows of.
  uint64_t known_generation = 0;
};

struct GateVerdict {
  bool permitted = false;
  // Empty when permitted: why the gate is shut, for the log.
  std::string reason;
  // Every condition the gate read, as `name=0|1` flags, for the trace.
  std::string conditions;
};

// Before the first inventory the owner's inventory counts as complete and
// its generation as zero.

// Tombstone collection: due, the inventory not rebuilt this pass, the
// inventory complete.
GateVerdict tombstone_gate(const PassFacts&, const InventoryHorizon*);

// CONTROL release and GC: due, not rebuilt, a release view, the inventory
// complete, an inventory, at or past the known generation.
GateVerdict control_gate(const PassFacts&, const InventoryHorizon*);

// DATA release and sweep: the control gate's conditions; the reason is the
// first that fails, in the pass's log order.
GateVerdict data_gate(const PassFacts&, const InventoryHorizon*);

}  // namespace dagda::contract
