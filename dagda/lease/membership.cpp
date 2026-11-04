//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/lease/membership.h"

#include <algorithm>

namespace dagda::lease {

using dannan::contract::Lock;

RosterMembership::RosterMembership(dannan::Nodes& nodes, const dannan::contract::TimeSource& time, boost::asio::any_io_executor executor)
    : _nodes(nodes), _time(time), _self(nodes.self().id), _absences(std::make_shared<Absences>()) {
  _watch = _nodes.watch(std::move(executor), [absences = _absences, time = &time](dannan::RosterChange change) {
    Lock lock(absences->mutex);
    if (absences->closed) return;
    if (change.kind == dannan::RosterChange::Kind::left)
      absences->since.try_emplace(change.node.id, time->now());
    else
      absences->since.erase(change.node.id);
  });
}

RosterMembership::~RosterMembership() {
  _nodes.unwatch(_watch);
  Lock lock(_absences->mutex);
  _absences->closed = true;
}

NodeId RosterMembership::self() const { return _self; }

std::optional<dannan::contract::TimePoint> RosterMembership::absent_since(const NodeId& node) const {
  if (node == _self) return std::nullopt;
  const auto active = _nodes.active();
  const bool member = std::any_of(active.begin(), active.end(), [&](const dannan::membership::NodeRecord& record) { return record.id == node; });
  Lock lock(_absences->mutex);
  if (member) {
    // Active again before the watch said so.
    _absences->since.erase(node);
    return std::nullopt;
  }
  return _absences->since.try_emplace(node, _time.now()).first->second;
}

std::vector<NodeId> RosterMembership::active() const {
  std::vector<NodeId> out{_self};
  for (const auto& record : _nodes.active())
    if (record.id != _self) out.push_back(record.id);
  return out;
}

}  // namespace dagda::lease
