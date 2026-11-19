//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/remove.h"

#include <algorithm>
#include <utility>

namespace dagda::store {

const char* remove_state_name(RemoveState state) noexcept {
  switch (state) {
    case RemoveState::removed:
      return "removed";
    case RemoveState::absent:
      return "absent";
    case RemoveState::kept_by_claim:
      return "kept_by_claim";
    case RemoveState::kept_referenced:
      return "kept_referenced";
    case RemoveState::refused:
      return "refused";
    case RemoveState::unreachable:
      return "unreachable";
  }
  return "unknown";
}

const RemovedObject* NodeRemoval::object(const ObjectId& id) const noexcept {
  for (const auto& entry : objects)
    if (entry.id == id) return &entry;
  return nullptr;
}

const NodeRemoval* RemoveEvidence::on(const dannan::identity::NodeId& node) const noexcept {
  for (const auto& entry : nodes)
    if (entry.node == node) return &entry;
  return nullptr;
}

RemoveState RemoveEvidence::state(const dannan::identity::NodeId& node, const ObjectId& id) const noexcept {
  const auto* found = on(node);
  const auto* entry = found ? found->object(id) : nullptr;
  return entry ? entry->state : RemoveState::unreachable;
}

size_t RemoveEvidence::removed() const noexcept {
  size_t out = 0;
  for (const auto& node : nodes)
    out += static_cast<size_t>(
        std::count_if(node.objects.begin(), node.objects.end(), [](const RemovedObject& entry) { return entry.state == RemoveState::removed; }));
  return out;
}

std::vector<RemovedObject> remove_here(const RemoveReplica& replica, std::vector<ObjectId> ids, const contract::RetentionClock& observed) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::vector<RemovedObject> out;
  if (ids.empty()) return out;
  out.reserve(ids.size());

  // No claim lands between the check below and the delete: under the guard a
  // claim takes for its presence check and journal write, each object to go is
  // marked, and it unlinks under the mark alone, holding no stripe.
  std::vector<std::pair<size_t, ClaimDeletion>> going;
  {
    const auto guard = replica.data.claim_locks().lock(ids);
    (void)replica.claims.release_observed(contract::RetentionClass::data, ids, observed);
    // The inventory as published now: what the owner's state refers to on this
    // node. One that could not be read whole may refer to anything, and so may
    // one not yet published (the maintainer, 2026-11-16).
    const auto inventory = replica.ledger ? replica.ledger->inventory() : nullptr;
    for (const auto& id : ids) {
      RemovedObject entry;
      entry.id = id;
      const auto claims = replica.claims.claims(contract::RetentionClass::data, id);
      if (!claims.adds.empty()) {
        entry.state = RemoveState::kept_by_claim;
        for (const auto& [origin, sequence] : claims.adds) entry.claims.push_back({origin, sequence});
      } else if (!inventory || !inventory->complete() || inventory->referenced(contract::RetentionClass::data, id)) {
        entry.state = RemoveState::kept_referenced;
      } else {
        going.emplace_back(out.size(), replica.data.claim_locks().begin_deleting(id));
      }
      out.push_back(std::move(entry));
    }
  }
  for (const auto& [index, deletion] : going) {
    auto& entry = out[index];
    entry.state = replica.data.remove(entry.id) ? RemoveState::removed : RemoveState::absent;
    if (replica.cache) (void)replica.cache->remove(entry.id);
  }
  return out;
}

}  // namespace dagda::store
