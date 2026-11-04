//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Membership from Dannan's roster (Nodes::active, Nodes::absent_since, Dannan
// 0.3.0): a node active is a member; a node's absence starts when the roster
// last heard of it, or for one the roster does not know, at the first look;
// active again, it is a member at once.

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <optional>
#include <vector>

#include "dagda/lease/membership.h"
#include "dagda/test/fixtures/lease.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda::lease {
namespace {

using namespace std::chrono_literals;
using dagda::test::node_of;
using dannan::membership::NodeRecord;

NodeRecord record(const NodeId& id) {
  NodeRecord out;
  out.id = id;
  return out;
}

// Dannan's Nodes as far as membership reads it: the active roster and when
// an absent node was last heard of.
class RosterNodes final : public dannan::Nodes {
 public:
  explicit RosterNodes(NodeId self) : _self(record(self)) { members.push_back(_self); }

  std::vector<NodeRecord> members;
  std::map<NodeId, dannan::contract::TimePoint> absent;

  void leave(const NodeId& id, dannan::contract::TimePoint last_heard) {
    std::erase_if(members, [&](const NodeRecord& member) { return member.id == id; });
    absent[id] = last_heard;
  }
  void join(const NodeId& id) {
    members.push_back(record(id));
    absent.erase(id);
  }

  uint64_t watch(boost::asio::any_io_executor, std::function<void(dannan::RosterChange)>) override { return 0; }
  void unwatch(uint64_t) override {}
  NodeRecord self() const override { return _self; }
  std::vector<NodeRecord> roster() const override { return members; }
  std::vector<NodeRecord> active() const override { return members; }
  dannan::RosterSnapshot snapshot() const override { return {members, members}; }
  std::optional<dannan::contract::TimePoint> absent_since(const dannan::identity::NodeId& id) const override {
    const auto found = absent.find(id);
    return found == absent.end() ? std::nullopt : std::optional(found->second);
  }
  bool directly_reachable(const dannan::identity::NodeId&) const override { return false; }
  bool all_known_reachable() const override { return false; }
  bool inbound_capable(const dannan::identity::NodeId&) const override { return false; }
  std::optional<dannan::net::Purpose> path(const dannan::identity::NodeId&, dannan::net::Lane) const override { return std::nullopt; }
  std::optional<std::chrono::milliseconds> latency(const dannan::identity::NodeId&) const override { return std::nullopt; }
  std::map<dannan::identity::NodeId, std::chrono::milliseconds> latencies() const override { return {}; }
  std::vector<dannan::membership::IdentityReset> identity_resets() const override { return {}; }
  bool apply_identity_reset(const dannan::membership::IdentityReset&) override { return false; }
  void propagate_identity_reset(const dannan::membership::IdentityReset&, std::chrono::milliseconds) override {}
  std::vector<NodeRecord> placement(std::span<const uint8_t>, size_t, const dannan::placement::Scheme&, const std::string&) const override { return {}; }
  void advertise(const std::string&, dannan::net::Bytes) override {}
  void withdraw(const std::string&) override {}

 private:
  NodeRecord _self;
};

TEST(LeaseMembership, AbsenceComesFromTheRoster) {
  dannan::test::SteppedTime time;
  const auto self = node_of(1), peer = node_of(2), stranger = node_of(3);
  RosterNodes nodes(self);
  nodes.join(peer);
  RosterMembership membership(nodes, time);
  EXPECT_EQ(membership.self(), self);
  EXPECT_EQ(membership.active(), (std::vector<NodeId>{self, peer}));
  EXPECT_FALSE(membership.absent_since(self).has_value());
  EXPECT_FALSE(membership.absent_since(peer).has_value());

  // The peer goes quiet: absent from when the roster last heard of it, not
  // from when the roster gave it up or this node asks.
  const auto last_heard = time.now();
  time.advance(2s);
  nodes.leave(peer, last_heard);
  time.advance(5min);
  EXPECT_EQ(membership.absent_since(peer), last_heard);
  EXPECT_EQ(membership.active(), std::vector<NodeId>{self});

  // A node the roster does not know: absent from the first look.
  const auto first_look = time.now();
  EXPECT_EQ(membership.absent_since(stranger), first_look);
  time.advance(1min);
  EXPECT_EQ(membership.absent_since(stranger), first_look);
  // Once the roster knows it, the roster's answer replaces the first look.
  nodes.join(stranger);
  EXPECT_FALSE(membership.absent_since(stranger).has_value());
  const auto stranger_heard = time.now();
  time.advance(1min);
  nodes.leave(stranger, stranger_heard);
  EXPECT_EQ(membership.absent_since(stranger), stranger_heard);

  // Back: a member at once, its absence forgotten; a later departure starts
  // afresh.
  nodes.join(peer);
  EXPECT_FALSE(membership.absent_since(peer).has_value());
  time.advance(1min);
  const auto again = time.now();
  nodes.leave(peer, again);
  EXPECT_EQ(membership.absent_since(peer), again);
}

// The keeper over Dannan's roster: a peer the roster lost is taken over after
// the window, measured from when it was last heard of.
TEST(LeaseMembership, TheKeeperTakesOverFromAPeerTheRosterLost) {
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  const auto self = node_of(1), peer = node_of(2);
  RosterNodes nodes(self);
  nodes.join(peer);
  RosterMembership membership(nodes, time);
  LeaseKeeper keeper(membership, time, wall);
  const Lease held{peer, 1, 1};
  EXPECT_EQ(keeper.decide("sweep", held).kind, Decision::Kind::held_elsewhere);
  nodes.leave(peer, time.now());
  time.advance(10min);
  const auto decision = keeper.decide("sweep", held);
  ASSERT_EQ(decision.kind, Decision::Kind::take);
  EXPECT_EQ(decision.proposed->epoch, 2u);
}

}  // namespace
}  // namespace dagda::lease
