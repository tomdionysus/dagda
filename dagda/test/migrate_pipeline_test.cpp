//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The ordered pipeline the migration's passes run on (TODO/2026-11-19-migrate-speed.md):
// what a serial pass would consume, in the same order, whatever order the workers
// finish in; never more than its window ahead; and an exception at its item's turn.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dagda/migrate/pipeline.h"

namespace dagda::migrate {
namespace {

using Produce = std::function<std::optional<std::pair<int, uint64_t>>()>;

Produce counting(int count, std::atomic<int>* produced = nullptr) {
  auto next = std::make_shared<int>(0);
  return [next, count, produced]() -> std::optional<std::pair<int, uint64_t>> {
    if (*next >= count) return std::nullopt;
    if (produced) produced->fetch_add(1);
    return std::pair{(*next)++, uint64_t{1}};
  };
}

// Workers that finish out of order: the later items quickest. The caller sees them in
// order, every one once.
TEST(MigratePipeline, TheCallerConsumesInOrderWhateverOrderTheWorkersFinishIn) {
  std::vector<int> seen;
  PipelineLimits limits;
  limits.workers = 4;
  limits.items = 8;
  run_ordered<int, int>(
      limits, counting(200),
      [](int& item) {
        std::this_thread::sleep_for(std::chrono::microseconds((200 - item) % 7 * 300));
        return item * 3;
      },
      [&](int& result) { seen.push_back(result); });
  ASSERT_EQ(seen.size(), 200u);
  for (int i = 0; i < 200; ++i) EXPECT_EQ(seen[static_cast<size_t>(i)], i * 3);
}

// The producer never runs more than the window ahead of the caller.
TEST(MigratePipeline, TheProducerKeepsWithinTheWindow) {
  std::atomic<int> produced{0};
  int consumed = 0;
  int furthest = 0;
  PipelineLimits limits;
  limits.workers = 3;
  limits.items = 5;
  run_ordered<int, int>(
      limits, counting(100, &produced), [](int& item) { return item; },
      [&](int&) {
        ++consumed;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        furthest = std::max(furthest, produced.load() - consumed);
      });
  EXPECT_EQ(consumed, 100);
  EXPECT_LE(furthest, 5);
}

// And within its bytes: items of 10 bytes against a bound of 25 hold three at most.
TEST(MigratePipeline, TheProducerKeepsWithinTheBytes) {
  std::atomic<int> produced{0};
  int consumed = 0;
  int furthest = 0;
  auto next = std::make_shared<int>(0);
  PipelineLimits limits;
  limits.workers = 2;
  limits.items = 100;
  limits.bytes = 25;
  run_ordered<int, int>(
      limits,
      [&]() -> std::optional<std::pair<int, uint64_t>> {
        if (*next >= 50) return std::nullopt;
        produced.fetch_add(1);
        return std::pair{(*next)++, uint64_t{10}};
      },
      [](int& item) { return item; },
      [&](int&) {
        ++consumed;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        furthest = std::max(furthest, produced.load() - consumed);
      });
  EXPECT_EQ(consumed, 50);
  EXPECT_LE(furthest, 3);
}

// A worker's exception is thrown at its item's turn: everything before it consumed,
// nothing after.
TEST(MigratePipeline, AWorkersExceptionComesAtItsItemsTurn) {
  std::vector<int> seen;
  PipelineLimits limits;
  limits.workers = 4;
  EXPECT_THROW(run_ordered<int, int>(
                   limits, counting(100),
                   [](int& item) {
                     if (item == 37) throw std::runtime_error("item 37");
                     return item;
                   },
                   [&](int& result) { seen.push_back(result); }),
               std::runtime_error);
  ASSERT_EQ(seen.size(), 37u);
  for (int i = 0; i < 37; ++i) EXPECT_EQ(seen[static_cast<size_t>(i)], i);
}

// So is the producer's, after what it produced before it.
TEST(MigratePipeline, AProducersExceptionComesAfterWhatItProduced) {
  std::vector<int> seen;
  auto next = std::make_shared<int>(0);
  PipelineLimits limits;
  limits.workers = 2;
  EXPECT_THROW(run_ordered<int, int>(
                   limits,
                   [&]() -> std::optional<std::pair<int, uint64_t>> {
                     if (*next == 20) throw std::runtime_error("cannot read");
                     return std::pair{(*next)++, uint64_t{1}};
                   },
                   [](int& item) { return item; }, [&](int& result) { seen.push_back(result); }),
               std::runtime_error);
  EXPECT_EQ(seen.size(), 20u);
}

// The caller's own exception stops the threads and comes back out.
TEST(MigratePipeline, TheCallersExceptionStopsEverything) {
  int consumed = 0;
  PipelineLimits limits;
  limits.workers = 3;
  EXPECT_THROW(run_ordered<int, int>(
                   limits, counting(1000), [](int& item) { return item; },
                   [&](int&) {
                     if (++consumed == 10) throw std::logic_error("refused");
                   }),
               std::logic_error);
  EXPECT_EQ(consumed, 10);
}

// Nothing to do is done at once.
TEST(MigratePipeline, AnEmptyPassEnds) {
  int consumed = 0;
  run_ordered<int, int>({}, counting(0), [](int& item) { return item; }, [&](int&) { ++consumed; });
  EXPECT_EQ(consumed, 0);
}

}  // namespace
}  // namespace dagda::migrate
