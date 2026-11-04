//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <map>
#include <optional>
#include <vector>

#include "dagda/lease/lease.h"
#include "dannan/context.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

namespace dagda::lease {

// Who is a member now, as leases need it: a lease holds while its holder is
// a member, and absence is measured on this node's steady clock from when
// this node first knew the holder absent (requirement E7). A node's comes
// from Dannan's roster (RosterMembership); a test's is driven by the test.
class Membership {
 public:
  virtual ~Membership() = default;
  // This node. Waits on nothing; thread-safe.
  virtual NodeId self() const = 0;
  // Empty while `node` is an active member (this node always is); else when
  // this node first knew it absent, never later than now. Waits on nothing;
  // thread-safe.
  virtual std::optional<dannan::contract::TimePoint> absent_since(const NodeId& node) const = 0;
  // Every active member, this node included. Waits on nothing; thread-safe.
  virtual std::vector<NodeId> active() const = 0;
};

// Membership from Dannan's roster: active is `Nodes::active()` (heard of
// within dead_after), and a node's absence starts when the roster last heard
// of it (`Nodes::absent_since`, on the node's steady clock, kept across a
// restart), or, for a node the roster does not know, when this node first
// asks about it. A node active again is a member at once and its absence
// forgotten.
class RosterMembership final : public Membership {
 public:
  // `nodes` and `time` must outlive this; `time` is the clock the roster
  // measures absence on (`Context::time().source()`).
  RosterMembership(dannan::Nodes& nodes, const dannan::contract::TimeSource& time);
  RosterMembership(const RosterMembership&) = delete;
  RosterMembership& operator=(const RosterMembership&) = delete;

  NodeId self() const override;
  std::optional<dannan::contract::TimePoint> absent_since(const NodeId& node) const override;
  std::vector<NodeId> active() const override;

 private:
  dannan::Nodes& _nodes;
  const dannan::contract::TimeSource& _time;
  const NodeId _self;
  // Nodes the roster does not know, from the first look.
  mutable dannan::contract::Mutex _mutex;
  mutable std::map<NodeId, dannan::contract::TimePoint> _unknown_since DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dagda::lease
