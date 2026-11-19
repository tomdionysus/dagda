//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The lease logic (plan, "Store: leases and claimed collections";
// requirement E7) on stepped time: a lease holds while its holder is a
// member; after the absence window another node takes it at epoch + 1; a
// holder that returns and finds a higher epoch stands down, and the fence
// refuses what it then tries to do. acquire() through lease records held in
// memory (the head space is step 4's), its completions on the caller's
// executor.

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dagda/lease/fence.h"
#include "dagda/lease/keeper.h"
#include "dagda/test/fixtures/lease.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda::lease {
namespace {

using namespace std::chrono_literals;
using dagda::test::MemoryLeaseRecords;
using dagda::test::node_of;
using dagda::test::TestMembership;

const NodeId a = node_of(0x11);
const NodeId b = node_of(0x22);
const NodeId c = node_of(0x33);

struct Node {
  Node(NodeId self, dannan::test::SteppedTime& time, dannan::test::SteppedWall& wall, LeaseOptions options = {})
      : members(self, time), keeper(members, time, wall, options) {}
  TestMembership members;
  LeaseKeeper keeper;
};

class LeaseKeeperTest : public ::testing::Test {
 protected:
  void step(std::chrono::milliseconds by) {
    time.advance(by);
    wall.advance(by);
  }

  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
};

TEST_F(LeaseKeeperTest, ALeaseHoldsWhileItsHolderIsAMember) {
  Node node_b(b, time, wall);
  node_b.members.join(a);
  const Lease held{a, 1, wall.unix_ms()};
  for (int i = 0; i < 10; ++i) {
    const auto decision = node_b.keeper.decide("sweep", held);
    EXPECT_EQ(decision.kind, Decision::Kind::held_elsewhere);
    EXPECT_FALSE(decision.proposed.has_value());
    step(1h);
  }
  Node node_a(a, time, wall);
  EXPECT_EQ(node_a.keeper.decide("sweep", held).kind, Decision::Kind::held_here);
}

TEST_F(LeaseKeeperTest, NobodysLeaseIsTakenAtEpochOne) {
  Node node_b(b, time, wall);
  const auto decision = node_b.keeper.decide("sweep", std::nullopt);
  ASSERT_EQ(decision.kind, Decision::Kind::take);
  EXPECT_EQ(decision.proposed, (Lease{b, 1, wall.unix_ms()}));
}

// After absence, another node takes over at epoch + 1, and not a
// millisecond before the window.
TEST_F(LeaseKeeperTest, AnAbsentHoldersLeaseIsTakenAtTheNextEpoch) {
  Node node_b(b, time, wall);
  node_b.members.join(a);
  const Lease held{a, 4, wall.unix_ms()};
  EXPECT_EQ(node_b.keeper.decide("sweep", held).kind, Decision::Kind::held_elsewhere);
  node_b.members.leave(a);
  step(10min - 1ms);
  const auto early = node_b.keeper.decide("sweep", held);
  EXPECT_EQ(early.kind, Decision::Kind::held_elsewhere);
  EXPECT_NE(early.why.find("absent 599999 ms of 600000 ms"), std::string::npos) << early.why;
  step(1ms);
  const auto decision = node_b.keeper.decide("sweep", held);
  ASSERT_EQ(decision.kind, Decision::Kind::take);
  EXPECT_EQ(decision.proposed, (Lease{b, 5, wall.unix_ms()}));
  // A holder seen again before the takeover is written keeps it.
  node_b.members.join(a);
  EXPECT_EQ(node_b.keeper.decide("sweep", held).kind, Decision::Kind::held_elsewhere);
}

// A holder never seen at all is absent from when this node first asks.
TEST_F(LeaseKeeperTest, AHolderNeverSeenIsAbsentFromTheFirstLook) {
  Node node_b(b, time, wall);
  const Lease held{c, 1, 1};
  EXPECT_EQ(node_b.keeper.decide("sweep", held).kind, Decision::Kind::held_elsewhere);
  step(10min);
  EXPECT_EQ(node_b.keeper.decide("sweep", held).kind, Decision::Kind::take);
}

TEST_F(LeaseKeeperTest, RankedTakersWaitRankTimesTheStep) {
  Node node_b(b, time, wall, LeaseOptions{10min, 30s});
  EXPECT_EQ(node_b.keeper.decide("sweep", std::nullopt, 2).kind, Decision::Kind::waiting);
  step(59s);
  EXPECT_EQ(node_b.keeper.decide("sweep", std::nullopt, 2).kind, Decision::Kind::waiting);
  step(1s);
  EXPECT_EQ(node_b.keeper.decide("sweep", std::nullopt, 2).kind, Decision::Kind::take);
  // The wait starts again once the lease was taken or held.
  EXPECT_EQ(node_b.keeper.decide("sweep", std::nullopt, 1).kind, Decision::Kind::waiting);
  EXPECT_EQ(node_b.keeper.vacant("other", Lease{a, 3, 1}, 0).proposed, (Lease{b, 4, wall.unix_ms()}));
}

// The whole story on two nodes and a receiver: A holds; A drops out and B
// takes over at epoch 2 in its half of a partition; A returns, the halves
// merge, A finds a higher epoch and stands down, and the receiver refuses
// A's stale action.
TEST_F(LeaseKeeperTest, AHolderThatReturnsStandsDownAndIsFenced) {
  Node node_a(a, time, wall);
  Node node_b(b, time, wall);
  node_a.members.join(b);
  node_b.members.join(a);
  MemoryLeaseRecords head_a, head_b;
  EpochFence receiver;
  boost::asio::thread_pool pool(1);

  const auto first = dagda::test::acquire(node_a.keeper, head_a, "athena", "sweep", pool.get_executor());
  ASSERT_EQ(first.outcome, Acquisition::Outcome::taken) << first.why;
  const auto mine = *first.lease;
  EXPECT_EQ(mine, (Lease{a, 1, wall.unix_ms()}));
  MemoryLeaseRecords::reconcile(head_a, head_b);
  EXPECT_TRUE(receiver.admit("sweep", mine.epoch).admitted);

  // A is cut off. B, past the window, takes over.
  node_b.members.leave(a);
  node_a.members.leave(b);
  step(10min);
  const auto takeover = dagda::test::acquire(node_b.keeper, head_b, "athena", "sweep", pool.get_executor());
  ASSERT_EQ(takeover.outcome, Acquisition::Outcome::taken) << takeover.why;
  EXPECT_EQ(takeover.lease, (Lease{b, 2, wall.unix_ms()}));
  EXPECT_TRUE(receiver.admit("sweep", takeover.lease->epoch).admitted);
  // Both believe they hold it for the window: the soft claim.
  EXPECT_TRUE(node_a.keeper.standing(mine, head_a.read("athena", "sweep")).holds);
  EXPECT_TRUE(node_b.keeper.standing(*takeover.lease, head_b.read("athena", "sweep")).holds);
  // The stale holder cannot act.
  const auto stale = receiver.admit("sweep", mine.epoch);
  EXPECT_FALSE(stale.admitted);
  EXPECT_EQ(stale.highest, 2u);

  // A returns; the heads merge; A stands down.
  node_a.members.join(b);
  node_b.members.join(a);
  step(1s);
  MemoryLeaseRecords::reconcile(head_a, head_b);
  const auto standing = node_a.keeper.standing(mine, head_a.read("athena", "sweep"));
  EXPECT_FALSE(standing.holds);
  EXPECT_EQ(standing.current, takeover.lease);
  EXPECT_EQ(standing.why, "superseded by " + dannan::identity::to_string(b) + " at epoch 2");
  const auto again = dagda::test::acquire(node_a.keeper, head_a, "athena", "sweep", pool.get_executor());
  EXPECT_EQ(again.outcome, Acquisition::Outcome::held_elsewhere);
  EXPECT_EQ(again.lease, takeover.lease);
  EXPECT_EQ(head_a.joins(), 1);
  pool.join();
}

TEST_F(LeaseKeeperTest, AcquireSaysWhatHappened) {
  Node node_a(a, time, wall, LeaseOptions{10min, 30s});
  Node node_b(b, time, wall, LeaseOptions{10min, 30s});
  node_b.members.join(a);
  MemoryLeaseRecords records;
  boost::asio::thread_pool pool(1);
  const auto run = [&](Node& node, size_t rank = 0) { return dagda::test::acquire(node.keeper, records, "lir", "shard/7", pool.get_executor(), rank); };

  EXPECT_EQ(run(node_b, 1).outcome, Acquisition::Outcome::waiting);
  const auto taken = run(node_a);
  EXPECT_EQ(taken.outcome, Acquisition::Outcome::taken);
  EXPECT_EQ(run(node_a).outcome, Acquisition::Outcome::held);
  const auto elsewhere = run(node_b);
  EXPECT_EQ(elsewhere.outcome, Acquisition::Outcome::held_elsewhere);
  EXPECT_EQ(elsewhere.lease, taken.lease);
  EXPECT_EQ(records.joins(), 1);

  // A write that does not happen is a failure with its reason.
  node_b.members.leave(a);
  step(10min);
  records.fail_next("deadline passed");
  const auto failed = run(node_b);
  EXPECT_EQ(failed.outcome, Acquisition::Outcome::failed);
  EXPECT_NE(failed.why.find("deadline passed"), std::string::npos) << failed.why;
  EXPECT_EQ(records.read("lir", "shard/7"), taken.lease);
  EXPECT_EQ(run(node_b).outcome, Acquisition::Outcome::taken);
  pool.join();
}

// A node whose copy of the head lags proposes a claim the merged record
// does not keep: it is told it lost, and who holds the lease.
TEST_F(LeaseKeeperTest, AClaimTheRecordDoesNotKeepIsLost) {
  struct Lagging final : LeaseRecords {
    MemoryLeaseRecords& records;
    explicit Lagging(MemoryLeaseRecords& inner) : records(inner) {}
    std::optional<Lease> read(const std::string&, const std::string&) const override { return std::nullopt; }
    void join(const std::string& scope, const std::string& name, const Lease& proposed, std::chrono::milliseconds deadline,
              boost::asio::any_io_executor executor, JoinDone done) override {
      records.join(scope, name, proposed, deadline, std::move(executor), std::move(done));
    }
  };
  Node node_a(a, time, wall);
  Node node_b(b, time, wall);
  MemoryLeaseRecords records;
  boost::asio::thread_pool pool(1);
  ASSERT_EQ(dagda::test::acquire(node_a.keeper, records, "lir", "shard/1", pool.get_executor()).outcome, Acquisition::Outcome::taken);
  step(1ms);
  Lagging lagging(records);
  const auto lost = dagda::test::acquire(node_b.keeper, lagging, "lir", "shard/1", pool.get_executor());
  EXPECT_EQ(lost.outcome, Acquisition::Outcome::lost);
  EXPECT_EQ(lost.lease->holder, a);
  pool.join();
}

// The executor rule: the completion is posted to the caller's executor,
// never run inline, even when the answer is known at once.
TEST_F(LeaseKeeperTest, CompletionsArePostedToTheCallersExecutor) {
  Node node_a(a, time, wall);
  MemoryLeaseRecords records;
  boost::asio::io_context io;
  int completed = 0;
  node_a.keeper.acquire(records, "lir", "shard/1", 5s, io.get_executor(), [&](Acquisition out) {
    EXPECT_EQ(out.outcome, Acquisition::Outcome::taken);
    ++completed;
  });
  node_a.keeper.acquire(records, "lir", "shard/1", 5s, io.get_executor(), [&](Acquisition out) {
    EXPECT_EQ(out.outcome, Acquisition::Outcome::held);
    ++completed;
  });
  EXPECT_EQ(completed, 0);
  io.run_for(5s);
  EXPECT_EQ(completed, 2);
}

// Eight nodes racing for one vacant lease on one converged head: the record
// ends with the lease order's winner, every answer is the record as it stood
// after that node's join, and only the winner still stands.
TEST_F(LeaseKeeperTest, ConcurrentTakersLeaveOneStanding) {
  std::vector<std::unique_ptr<Node>> nodes;
  for (uint8_t i = 1; i <= 8; ++i) nodes.push_back(std::make_unique<Node>(node_of(static_cast<uint8_t>(0x10 * i)), time, wall));
  MemoryLeaseRecords records;
  boost::asio::thread_pool pool(4);
  std::vector<std::future<Acquisition>> answers;
  std::vector<std::thread> callers;
  std::vector<std::promise<Acquisition>> promises(nodes.size());
  for (auto& promise : promises) answers.push_back(promise.get_future());
  for (size_t i = 0; i < nodes.size(); ++i)
    callers.emplace_back([&, i] {
      nodes[i]->keeper.acquire(records, "lir", "shard/9", 5s, pool.get_executor(), [&promises, i](Acquisition out) { promises[i].set_value(std::move(out)); });
    });
  for (auto& caller : callers) caller.join();
  // Every proposal is {node, 1, now}: the lowest node that proposed wins.
  std::optional<Lease> winner;
  int proposals = 0;
  int still_standing = 0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    ASSERT_EQ(answers[i].wait_for(10s), std::future_status::ready);
    const auto out = answers[i].get();
    EXPECT_TRUE(out.outcome == Acquisition::Outcome::taken || out.outcome == Acquisition::Outcome::lost || out.outcome == Acquisition::Outcome::held_elsewhere)
        << outcome_name(out.outcome);
    if (out.outcome != Acquisition::Outcome::held_elsewhere) {
      ++proposals;
      winner = merge(winner, std::optional<Lease>(Lease{nodes[i]->members.self(), 1, wall.unix_ms()}));
    }
    if (out.outcome == Acquisition::Outcome::taken && nodes[i]->keeper.standing(*out.lease, records.read("lir", "shard/9")).holds) ++still_standing;
  }
  ASSERT_TRUE(winner.has_value());
  EXPECT_EQ(records.read("lir", "shard/9"), winner);
  EXPECT_EQ(still_standing, 1);
  EXPECT_EQ(records.joins(), proposals);
  pool.join();
}

// decide() from many threads on one keeper, over many leases.
TEST_F(LeaseKeeperTest, OneKeeperDecidesFromManyThreads) {
  Node node(b, time, wall, LeaseOptions{10min, 1ms});
  std::vector<std::thread> threads;
  std::atomic<int> takes{0};
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([&, t] {
      for (int i = 0; i < 500; ++i) {
        const auto name = "lease/" + std::to_string((t * 500 + i) % 64);
        if (node.keeper.decide(name, std::nullopt, static_cast<size_t>(i % 2)).kind == Decision::Kind::take) ++takes;
      }
    });
  for (auto& thread : threads) thread.join();
  // Stepped time stands still: rank 0 always takes, rank 1 always waits.
  EXPECT_EQ(takes.load(), 2000);
}

}  // namespace
}  // namespace dagda::lease
