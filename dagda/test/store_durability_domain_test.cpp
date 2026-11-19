//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirements S13 and S14. Time is a stepped source, so a batch window ends
// when the test moves it and never by waiting; the bounded future waits are
// a safety net against a hang, not a timing.
//
// Moved from Macha's tests/test_invariants.cpp (develop 5a06d3c):
// test_a_durability_barrier_is_timed_per_filesystem. Its other durability
// cases drive a LocalStore or a StoragePool and move with them.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <thread>

#include "dagda/store/durability_domain.h"
#include "dannan/status/observation.h"
#include "dannan/supervise/supervised.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;

constexpr auto bound = 10s;

// What a domain runs on in a test.
struct Bed {
  dannan::test::TempDir dir;
  std::filesystem::path root = dir.path() / "objects";
  dannan::supervise::SupervisedThreads threads;
  dannan::status::Observations observations;
  dannan::test::SteppedTime time;

  Bed() { std::filesystem::create_directories(root); }
  DurabilityDomain::Services services() { return {threads, observations, time}; }
  std::filesystem::path file(const char* name) {
    const auto path = root / name;
    std::ofstream(path) << "bytes";
    return path;
  }
};

bool done(std::future<void>& future) { return future.wait_for(0s) == std::future_status::ready; }

// Spins until another thread has read the stepped time past `seen`.
void await_reading(const dannan::test::SteppedTime& time, uint64_t seen) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (time.reads() <= seen && std::chrono::steady_clock::now() < until) std::this_thread::yield();
  ASSERT_GT(time.reads(), seen);
}

// Macha: test_a_durability_barrier_is_timed_per_filesystem. Each barrier's
// time is recorded under the directory its domain was made for, so
// control's commits and a DATA disk's are told apart.
TEST(StoreDurabilityDomain, ABarrierIsTimedPerFilesystem) {
  dannan::test::TempDir t;
  const auto root = t.path() / "barrier-timed-objects";
  std::filesystem::create_directories(root);
  dannan::supervise::SupervisedThreads threads;
  dannan::status::Observations observations;
  auto& series = observations.histogram("durability.barrier_us.barrier-timed-objects");
  const auto before = series.snapshot().count;
  auto domain = std::make_shared<DurabilityDomain>(1, root, DurabilityDomain::Services{threads, observations}, 1ms);
  const auto file = root / "object";
  std::ofstream(file) << "bytes";
  domain->await_durable(domain->complete_mutation(file, root), DurabilityUrgency::immediate);
  EXPECT_GT(series.snapshot().count, before);
}

TEST(StoreDurabilityDomain, RefusesAZeroIdOrANegativeWindow) {
  Bed bed;
  EXPECT_THROW(DurabilityDomain(0, bed.root, bed.services()), std::invalid_argument);
  EXPECT_THROW(DurabilityDomain(1, bed.root, bed.services(), -1ms), std::invalid_argument);
}

TEST(StoreDurabilityDomain, IssuesGenerationsInOrderFromOne) {
  Bed bed;
  DurabilityDomain domain(7, bed.root, bed.services());
  EXPECT_EQ(domain.id(), 7u);
  EXPECT_EQ(domain.current_generation(), 0u);
  EXPECT_EQ(domain.complete_mutation(bed.file("a"), bed.root), 1u);
  EXPECT_EQ(domain.complete_mutation(bed.file("b"), bed.root), 2u);
  EXPECT_EQ(domain.complete_mutation(), 3u);
  EXPECT_EQ(domain.current_generation(), 3u);
  EXPECT_EQ(domain.durable_generation(), 0u);
  EXPECT_EQ(domain.physical_barriers(), 0u);
}

TEST(StoreDurabilityDomain, GenerationZeroIsDurableAndOneNeverIssuedIsRefused) {
  Bed bed;
  DurabilityDomain domain(1, bed.root, bed.services(), 1h);
  domain.await_durable(0);
  EXPECT_THROW(domain.await_durable(1), std::runtime_error);
  domain.flush();
  EXPECT_EQ(domain.physical_barriers(), 0u);
}

TEST(StoreDurabilityDomain, ImmediateDoesNotWaitForTheWindow) {
  Bed bed;
  DurabilityDomain domain(1, bed.root, bed.services(), 1h);
  const auto generation = domain.complete_mutation(bed.file("a"), bed.root);
  domain.await_durable(generation, DurabilityUrgency::immediate);
  EXPECT_EQ(domain.durable_generation(), 1u);
  EXPECT_EQ(domain.physical_barriers(), 1u);
  // Durable already: no second barrier.
  domain.await_durable(generation, DurabilityUrgency::immediate);
  EXPECT_EQ(domain.physical_barriers(), 1u);
}

TEST(StoreDurabilityDomain, BatchableWaitsOutItsWindow) {
  Bed bed;
  DurabilityDomain domain(1, bed.root, bed.services(), 500ms);
  const auto generation = domain.complete_mutation(bed.file("a"), bed.root);
  const auto seen = bed.time.reads();
  auto waiter = std::async(std::launch::async, [&] { domain.await_durable(generation); });
  await_reading(bed.time, seen);
  EXPECT_FALSE(done(waiter));
  EXPECT_EQ(domain.physical_barriers(), 0u);

  bed.time.advance(499ms);
  domain.time_advanced();
  EXPECT_FALSE(done(waiter));
  EXPECT_EQ(domain.durable_generation(), 0u);

  bed.time.advance(1ms);
  domain.time_advanced();
  ASSERT_EQ(waiter.wait_for(bound), std::future_status::ready);
  waiter.get();
  EXPECT_EQ(domain.durable_generation(), 1u);
  EXPECT_EQ(domain.physical_barriers(), 1u);
}

// Macha: test_durability_domain_group_commits_independent_publications, at
// the domain: three independent completions inside one window are made
// durable by exactly one barrier.
TEST(StoreDurabilityDomain, OneBarrierServesEveryWaiterInAWindow) {
  Bed bed;
  DurabilityDomain domain(1, bed.root, bed.services(), 120ms);
  const auto seen = bed.time.reads();
  const auto a = domain.complete_mutation(bed.file("a"), bed.root);
  auto first = std::async(std::launch::async, [&] { domain.await_durable(a); });
  await_reading(bed.time, seen);
  const auto b = domain.complete_mutation(bed.file("b"), bed.root);
  auto second = std::async(std::launch::async, [&] { domain.await_durable(b); });
  const auto c = domain.complete_mutation(bed.file("c"), bed.root);
  auto third = std::async(std::launch::async, [&] { domain.await_durable(c); });
  EXPECT_FALSE(done(first));

  bed.time.advance(120ms);
  domain.time_advanced();
  for (auto* waiter : {&first, &second, &third}) {
    ASSERT_EQ(waiter->wait_for(bound), std::future_status::ready);
    waiter->get();
  }
  EXPECT_EQ(domain.physical_barriers(), 1u);
  EXPECT_GE(domain.durable_generation(), c);
}

// An immediate request ends a batch already waiting: both callers return
// on one barrier and the window never ends.
TEST(StoreDurabilityDomain, AnImmediateRequestCutsAWaitingBatch) {
  Bed bed;
  DurabilityDomain domain(1, bed.root, bed.services(), 1h);
  const auto seen = bed.time.reads();
  const auto a = domain.complete_mutation(bed.file("a"), bed.root);
  auto batched = std::async(std::launch::async, [&] { domain.await_durable(a); });
  await_reading(bed.time, seen);
  const auto b = domain.complete_mutation(bed.file("b"), bed.root);
  domain.await_durable(b, DurabilityUrgency::immediate);
  ASSERT_EQ(batched.wait_for(bound), std::future_status::ready);
  batched.get();
  EXPECT_EQ(domain.physical_barriers(), 1u);
  EXPECT_EQ(domain.durable_generation(), 2u);
}

// The generation invariant: a cut claims only what registered before it.
TEST(StoreDurabilityDomain, AMutationAfterACutIsNotClaimedByIt) {
  Bed bed;
  DurabilityDomain domain(1, bed.root, bed.services(), 1h);
  domain.complete_mutation(bed.file("a"), bed.root);
  domain.flush();
  EXPECT_EQ(domain.durable_generation(), 1u);
  const auto later = domain.complete_mutation(bed.file("b"), bed.root);
  EXPECT_EQ(domain.durable_generation(), 1u);
  domain.flush();
  EXPECT_GE(domain.durable_generation(), later);
  EXPECT_EQ(domain.physical_barriers(), 2u);
}

// A file removed since its mutation leaves nothing to make durable.
TEST(StoreDurabilityDomain, AFileGoneSinceItsMutationIsNoFailure) {
  Bed bed;
  DurabilityDomain domain(1, bed.root, bed.services(), 1h);
  const auto file = bed.file("a");
  const auto generation = domain.complete_mutation(file, bed.root);
  std::filesystem::remove(file);
  domain.await_durable(generation, DurabilityUrgency::immediate);
  EXPECT_FALSE(domain.failed());
}

// Requirement S14: a failed barrier is sticky. Every later mutation and
// wait rethrows it, and nothing issued is ever reported durable.
TEST(StoreDurabilityDomain, AFailedBarrierIsSticky) {
  Bed bed;
  DurabilityDomain domain(1, bed.root, bed.services(), 1h);
#if defined(__linux__)
  // syncfs has no representative left to go through.
  const auto generation = domain.complete_mutation();
  std::filesystem::remove_all(bed.root);
#else
  // The per-file fallback cannot open the file it must sync.
  const auto file = bed.file("a");
  std::filesystem::permissions(file, std::filesystem::perms::none);
  const auto generation = domain.complete_mutation(file, bed.root);
#endif
  EXPECT_THROW(domain.await_durable(generation, DurabilityUrgency::immediate), std::runtime_error);
  EXPECT_TRUE(domain.failed());
  EXPECT_EQ(domain.durable_generation(), 0u);
  EXPECT_THROW(domain.complete_mutation(), std::runtime_error);
  EXPECT_THROW(domain.await_durable(generation), std::runtime_error);
  EXPECT_THROW(domain.flush(), std::runtime_error);
  EXPECT_EQ(domain.physical_barriers(), 0u);
}

}  // namespace
}  // namespace dagda::store
