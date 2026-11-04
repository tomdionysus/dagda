//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Membership from Dannan's roster (Nodes::active, Nodes::watch, Dannan
// 0.2.0): a node active is a member; a node's absence starts when the watch
// reports it left, or for one never seen, at the first look; seen again, it
// is a member at once.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <functional>
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

// Dannan's Nodes as far as membership reads it; the test delivers the
// watch's changes itself, as Dannan posts them.
class RosterNodes final : public dannan::Nodes {
 public:
  explicit RosterNodes(NodeId self) : _self(record(self)) { members.push_back(_self); }

  std::vector<NodeRecord> members;
  std::function<void(dannan::RosterChange)> handler;
  std::vector<uint64_t> unwatched;

  void change(dannan::RosterChange::Kind kind, const NodeId& id) { handler(dannan::RosterChange{kind, record(id)}); }

  uint64_t watch(boost::asio::any_io_executor, std::function<void(dannan::RosterChange)> watcher) override {
    handler = std::move(watcher);
    return 7;
  }
  void unwatch(uint64_t id) override { unwatched.push_back(id); }
  NodeRecord self() const override { return _self; }
  std::vector<NodeRecord> roster() const override { return members; }
  std::vector<NodeRecord> active() const override { return members; }
  dannan::RosterSnapshot snapshot() const override { return {members, members}; }
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
  boost::asio::io_context io;
  const auto self = node_of(1), peer = node_of(2), stranger = node_of(3);
  RosterNodes nodes(self);
  nodes.members.push_back(record(peer));
  {
    RosterMembership membership(nodes, time, io.get_executor());
    EXPECT_EQ(membership.self(), self);
    EXPECT_EQ(membership.active(), (std::vector<NodeId>{self, peer}));
    EXPECT_FALSE(membership.absent_since(self).has_value());
    EXPECT_FALSE(membership.absent_since(peer).has_value());

    // The peer leaves: absent from when the watch said so, not from when
    // this node asks.
    const auto left_at = time.now();
    nodes.members.pop_back();
    nodes.change(dannan::RosterChange::Kind::left, peer);
    time.advance(5min);
    EXPECT_EQ(membership.absent_since(peer), left_at);
    EXPECT_EQ(membership.active(), std::vector<NodeId>{self});

    // A node never seen: absent from the first look.
    const auto first_look = time.now();
    EXPECT_EQ(membership.absent_since(stranger), first_look);
    time.advance(1min);
    EXPECT_EQ(membership.absent_since(stranger), first_look);

    // Back in the roster before the watch says so: a member at once, its
    // absence forgotten; a later departure starts afresh.
    nodes.members.push_back(record(peer));
    EXPECT_FALSE(membership.absent_since(peer).has_value());
    nodes.change(dannan::RosterChange::Kind::joined, peer);
    nodes.members.pop_back();
    time.advance(1min);
    const auto again = time.now();
    nodes.change(dannan::RosterChange::Kind::left, peer);
    EXPECT_EQ(membership.absent_since(peer), again);
  }
  // Destroyed: unwatched, and a change Dannan had already posted is
  // harmless.
  EXPECT_EQ(nodes.unwatched, std::vector<uint64_t>{7});
  nodes.change(dannan::RosterChange::Kind::left, peer);
}

// The keeper over Dannan's roster: a peer that left is taken over after the
// window.
TEST(LeaseMembership, TheKeeperTakesOverFromAPeerTheRosterSaysLeft) {
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  boost::asio::io_context io;
  const auto self = node_of(1), peer = node_of(2);
  RosterNodes nodes(self);
  nodes.members.push_back(record(peer));
  RosterMembership membership(nodes, time, io.get_executor());
  LeaseKeeper keeper(membership, time, wall);
  const Lease held{peer, 1, 1};
  EXPECT_EQ(keeper.decide("sweep", held).kind, Decision::Kind::held_elsewhere);
  nodes.members.pop_back();
  nodes.change(dannan::RosterChange::Kind::left, peer);
  time.advance(10min);
  const auto decision = keeper.decide("sweep", held);
  ASSERT_EQ(decision.kind, Decision::Kind::take);
  EXPECT_EQ(decision.proposed->epoch, 2u);
}

}  // namespace
}  // namespace dagda::lease
