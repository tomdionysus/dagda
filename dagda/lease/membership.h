//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <cstdint>
#include <map>
#include <memory>
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

// TODO(Dannan: absence on the roster): when a node left, on the steady
// clock, from Nodes (a RosterChange's time, or Nodes::absent_since), so a
// hosted system does not rebuild it from watch() as this does.
//
// Membership from Dannan's roster: active is `Nodes::active()` (heard of
// within dead_after), and a node's absence starts when `Nodes::watch()`
// reports it left, or, for a node never seen to leave (one this node never
// knew active), when this node first asks about it. A node seen again is
// active at once and its absence forgotten.
class RosterMembership final : public Membership {
 public:
  // `nodes` and `time` must outlive this. Watches the roster on `executor`.
  RosterMembership(dannan::Nodes& nodes, const dannan::contract::TimeSource& time, boost::asio::any_io_executor executor);
  ~RosterMembership() override;
  RosterMembership(const RosterMembership&) = delete;
  RosterMembership& operator=(const RosterMembership&) = delete;

  NodeId self() const override;
  std::optional<dannan::contract::TimePoint> absent_since(const NodeId& node) const override;
  std::vector<NodeId> active() const override;

 private:
  // Shared with the watch handler, which may run once more after unwatch:
  // once closed it touches nothing else.
  struct Absences {
    dannan::contract::Mutex mutex;
    std::map<NodeId, dannan::contract::TimePoint> since DANNAN_GUARDED_BY(mutex);
    bool closed DANNAN_GUARDED_BY(mutex) = false;
  };

  dannan::Nodes& _nodes;
  const dannan::contract::TimeSource& _time;
  const NodeId _self;
  std::shared_ptr<Absences> _absences;
  uint64_t _watch = 0;
};

}  // namespace dagda::lease
