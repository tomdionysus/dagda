//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/ledger/claim_walk.h"

#include <optional>

namespace dagda::ledger {

ClaimWalkStep ClaimWalk::step(const contract::ObjectLedger& ledger, ClaimRestorer& restorer) {
  ClaimWalkStep step;
  dannan::sched::Budget budget;
  budget.operations(step_bound);
  const auto page = ledger.claimed(_type, _cursor, budget);
  step.unfinished = page.stopped == dannan::sched::Stop::budget;
  std::optional<ObjectId> resume = _cursor.after;
  for (const auto& id : page.items) {
    ++step.examined;
    if (_observations) _observations->add("maintenance.claim_walk.examined");
    if (!ledger.held(_type, id)) {
      ++step.missing;
      if (_observations) _observations->add("maintenance.claim_walk.missing");
      const auto outcome = restorer.restore(_type, id);
      if (outcome == ClaimRestorer::Outcome::waiting_for_credit) {
        // Resume at this claim when credit returns.
        _cursor.after = resume;
        step.waiting_for_credit = true;
        step.unfinished = false;
        return step;
      }
      if (outcome == ClaimRestorer::Outcome::restored) ++step.restored;
    }
    resume = id;
  }
  _cursor = page.next;
  return step;
}

}  // namespace dagda::ledger
