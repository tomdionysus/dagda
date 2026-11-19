//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/lease/membership.h"

#include <algorithm>

namespace dagda::lease {

using dannan::contract::Lock;

RosterMembership::RosterMembership(dannan::Nodes& nodes, const dannan::contract::TimeSource& time) : _nodes(nodes), _time(time), _self(nodes.self().id) {}

NodeId RosterMembership::self() const { return _self; }

std::optional<dannan::contract::TimePoint> RosterMembership::absent_since(const NodeId& node) const {
  if (node == _self) return std::nullopt;
  const auto active = _nodes.active();
  const bool member = std::any_of(active.begin(), active.end(), [&](const dannan::membership::NodeRecord& record) { return record.id == node; });
  const auto since = member ? std::nullopt : _nodes.absent_since(node);
  Lock lock(_mutex);
  if (member || since) {
    // Known to the roster: its answer, and any first look forgotten.
    _unknown_since.erase(node);
    return since;
  }
  return _unknown_since.try_emplace(node, _time.now()).first->second;
}

std::vector<NodeId> RosterMembership::active() const {
  std::vector<NodeId> out{_self};
  for (const auto& record : _nodes.active())
    if (record.id != _self) out.push_back(record.id);
  return out;
}

}  // namespace dagda::lease
