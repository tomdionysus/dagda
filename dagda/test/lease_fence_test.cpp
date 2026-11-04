//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The epoch fence (plan, "Store: leases and claimed collections";
// requirement E9, new: Macha never checks one): every action under a lease
// carries its epoch and the receiver refuses a lower one.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "dagda/lease/fence.h"
#include "dagda/test/fixtures/lease.h"

namespace dagda::lease {
namespace {

using dagda::test::node_of;

TEST(LeaseFence, ALowerEpochsActionIsRefused) {
  EpochFence fence;
  EXPECT_FALSE(fence.highest("shard/1").has_value());
  EXPECT_TRUE(fence.admit("shard/1", 1).admitted);
  EXPECT_TRUE(fence.admit("shard/1", 2).admitted);
  const auto stale = fence.admit("shard/1", 1);
  EXPECT_FALSE(stale.admitted);
  EXPECT_EQ(stale.highest, 2u);
  EXPECT_EQ(stale.why, "epoch 1 is below the lease's epoch 2");
  // The holder's own epoch, again: admitted.
  EXPECT_TRUE(fence.admit("shard/1", 2).admitted);
  // Each lease has its own floor.
  EXPECT_TRUE(fence.admit("shard/2", 1).admitted);
  EXPECT_EQ(fence.highest("shard/1"), 2u);
}

// A receiver that has merged a takeover refuses the old holder before the
// new one has acted at all.
TEST(LeaseFence, AMergedTakeoverRaisesTheFloor) {
  EpochFence fence;
  EXPECT_TRUE(fence.admit("sweep", 1).admitted);
  fence.observe("sweep", Lease{node_of(2), 2, 900});
  EXPECT_FALSE(fence.admit("sweep", 1).admitted);
  EXPECT_TRUE(fence.admit("sweep", 2).admitted);
  // Observing an older record lowers nothing.
  fence.observe("sweep", Lease{node_of(1), 1, 100});
  EXPECT_EQ(fence.highest("sweep"), 2u);
}

TEST(LeaseFence, ThePureCheckAgainstARecord) {
  EXPECT_TRUE(lease::fence(std::nullopt, 1).admitted);
  EXPECT_TRUE(lease::fence(Lease{node_of(1), 2, 1}, 2).admitted);
  EXPECT_TRUE(lease::fence(Lease{node_of(1), 2, 1}, 3).admitted);
  const auto refused = lease::fence(Lease{node_of(1), 2, 1}, 1);
  EXPECT_FALSE(refused.admitted);
  EXPECT_EQ(refused.highest, 2u);
}

// Concurrent senders with mixed epochs: the floor only rises, it ends at the
// highest epoch sent, and every refusal names a floor above its epoch.
TEST(LeaseFence, ConcurrentActionsNeverLowerTheFloor) {
  EpochFence fence;
  std::atomic<uint64_t> highest_sent{0};
  std::atomic<int> violations{0};
  std::vector<std::thread> senders;
  for (int t = 0; t < 8; ++t) {
    senders.emplace_back([&, t] {
      std::mt19937_64 random(static_cast<uint64_t>(t));
      uint64_t floor_seen = 0;
      for (int i = 0; i < 2000; ++i) {
        const uint64_t epoch = 1 + random() % 50;
        uint64_t sent = highest_sent.load();
        while (epoch > sent && !highest_sent.compare_exchange_weak(sent, epoch)) {
        }
        const auto decision = fence.admit(t % 2 ? "a" : "b", epoch);
        if (t % 2) {
          if (decision.highest < floor_seen) ++violations;
          floor_seen = decision.highest;
        }
        if (!decision.admitted && decision.highest <= epoch) ++violations;
        if (decision.admitted && decision.highest < epoch) ++violations;
      }
    });
  }
  for (auto& sender : senders) sender.join();
  EXPECT_EQ(violations.load(), 0);
  EXPECT_EQ(std::max(*fence.highest("a"), *fence.highest("b")), highest_sent.load());
}

}  // namespace
}  // namespace dagda::lease
