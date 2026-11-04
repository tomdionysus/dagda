//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/ledger/retention_ledger.h"

#include <utility>

namespace dagda::ledger {

using contract::RetentionClass;
using dannan::sched::Stop;

dannan::sched::Page<ObjectId, ObjectId> RetentionLedger::claimed(RetentionClass type, dannan::sched::Cursor<ObjectId> from,
                                                                 dannan::sched::Budget& budget) const {
  dannan::sched::Page<ObjectId, ObjectId> page;
  page.next = from;
  std::optional<ObjectId> position = from.after;
  for (;;) {
    if (const auto stop = budget.must_stop(_time.now())) {
      page.stopped = *stop;
      return page;
    }
    if (!budget.take_operation()) {
      page.stopped = Stop::budget;
      return page;
    }
    bool complete = false;
    const auto id = _claims.next_retained(type, position, complete);
    if (!id) {
      page.next = {};
      page.stopped = Stop::end;
      return page;
    }
    page.items.push_back(*id);
    page.next.after = *id;
  }
}

bool RetentionLedger::held(RetentionClass type, const ObjectId& id) const { return type == RetentionClass::data ? _data.has(id) : _control.has(id); }

void RetentionLedger::publish(InventoryHandle inventory) { _inventory.publish(std::move(inventory)); }

size_t RetentionLedger::release_unreferenced(RetentionClass type, const contract::ReleaseHorizon& release, size_t operation_budget) {
  return _claims.release_unreferenced(type, release.lookup(type), release.clock(), operation_budget);
}

size_t RetentionLedger::prune_unclaimed(RetentionClass type, size_t operation_budget) {
  return _claims.prune_unclaimed(type, [this, type](const ObjectId& id) { return held(type, id); }, operation_budget);
}

bool RetentionLedger::publish(contract::ReleaseBuild build) {
  if (!build.complete) return false;
  _release.publish(std::move(build.horizon));
  return true;
}

}  // namespace dagda::ledger
