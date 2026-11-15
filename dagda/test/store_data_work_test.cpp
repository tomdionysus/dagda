//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Admission on a node's devices: the DATA credit arbiter's waits (Macha's
// tests/test_rpc_cluster.cpp, develop 5a06d3c:
// test_data_credit_wait_gives_up_when_nothing_is_moving,
// test_data_credit_wait_survives_genuine_contention, every assertion kept)
// and the device I/O reserve (new, requirements section 7 item 4: Macha has
// none): DATA work never takes the control reserve, control work never waits
// behind DATA, and without the reserve it would.

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "dagda/store/data_work.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using dannan::sched::FrameType;
using test::wait_until;

// Macha: test_data_credit_wait_gives_up_when_nothing_is_moving. A DATA
// credit wait with no deadline gives up only when the arbiter is wholly
// stalled; any release resets the window, so contention still waits.
TEST(StoreDataWork, ACreditWaitGivesUpWhenNothingIsMoving) {
  constexpr uint64_t capacity = 4 * 1024 * 1024;
  constexpr uint64_t reserve = 1024 * 1024;
  constexpr uint64_t chunk = 1024 * 1024;

  // Background concurrency 2: a third loader acquire cannot be admitted
  // while the first two are held.
  DataResourceArbiter arbiter(capacity, reserve, 2, 200ms);
  const DataWorkContext context(FrameType::loader, chunk);
  auto first = arbiter.acquire(context, chunk);
  auto second = arbiter.acquire(context, chunk);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  const auto started = Clock::now();
  const auto third = arbiter.acquire(context, chunk);
  const auto waited = Clock::now() - started;
  // It gives up after the no-progress window instead of hanging.
  EXPECT_FALSE(third.has_value());
  EXPECT_GE(waited, 200ms);
  EXPECT_LT(waited, 5s);
  EXPECT_EQ(arbiter.stats().no_progress_failures, 1u);

  // Releasing makes room again: giving up once does not poison the arbiter.
  first.reset();
  const auto fourth = arbiter.acquire(context, chunk);
  EXPECT_TRUE(fourth.has_value());
}

// Macha: test_data_credit_wait_survives_genuine_contention. A waiter does not
// give up while work flows: a release inside the no-progress window resets
// it, and the waiter is admitted.
TEST(StoreDataWork, ACreditWaitSurvivesGenuineContention) {
  constexpr uint64_t capacity = 4 * 1024 * 1024;
  constexpr uint64_t reserve = 1024 * 1024;
  constexpr uint64_t chunk = 1024 * 1024;

  DataResourceArbiter arbiter(capacity, reserve, 2, 400ms);
  const DataWorkContext context(FrameType::loader, chunk);
  auto first = arbiter.acquire(context, chunk);
  auto second = arbiter.acquire(context, chunk);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  std::jthread releaser([&] {
    std::this_thread::sleep_for(150ms);
    first.reset();
  });
  const auto third = arbiter.acquire(context, chunk);
  EXPECT_TRUE(third.has_value());
}

// A wait's deadline is on the injected time: a stepped clock moved past it
// ends the wait, with no real time passing.
TEST(StoreDataWork, ACreditDeadlineIsOnTheInjectedTime) {
  dannan::test::SteppedTime time;
  DataResourceArbiter arbiter(4 * 1024 * 1024, 1024 * 1024, 1, 1h, {}, std::nullopt, time);
  const auto held = arbiter.try_acquire(DataWorkContext(FrameType::loader, 1024), 1024);
  ASSERT_TRUE(held.has_value());
  auto waiter = std::async(std::launch::async, [&] { return arbiter.acquire(DataWorkContext(FrameType::loader, 1024, time.now() + 10s), 1024); });
  EXPECT_EQ(waiter.wait_for(50ms), std::future_status::timeout);
  time.advance(11s);
  arbiter.time_advanced();
  ASSERT_EQ(waiter.wait_for(5s), std::future_status::ready);
  EXPECT_FALSE(waiter.get().has_value());
  EXPECT_EQ(arbiter.stats().cancelled_waits, 1u);
  // The constructor refuses a wait that could be unbounded.
  EXPECT_THROW(DataResourceArbiter(4 * 1024 * 1024, 1024 * 1024, 1, 0ms), std::invalid_argument);
}

// src/cluster/data_work.hpp at Macha 9a08846, acquire: a wait with no caller
// deadline runs in no-progress windows, and any release anywhere re-arms the
// window, so it outlasts any one window while anything moves. The release here
// is a foreground lease's, which admits nothing to the waiting loader.
TEST(StoreDataWork, AWaitWithNoDeadlineIsReArmedByAnyRelease) {
  dannan::test::SteppedTime time;
  DataResourceArbiter arbiter(4 * 1024 * 1024, 1024 * 1024, 1, 120s, {}, std::nullopt, time);
  auto held = arbiter.try_acquire(DataWorkContext(FrameType::loader, 1024), 1024);
  ASSERT_TRUE(held.has_value());
  auto waiter = std::async(std::launch::async, [&] { return admit_data(&arbiter, nullptr, FrameType::loader, 1024, no_caller_deadline); });
  ASSERT_TRUE(wait_until([&] { return arbiter.stats().loader_waits == 1; }));
  // Each release lands within a window of the last; together they span many.
  for (int i = 0; i < 5; ++i) {
    auto other = arbiter.try_acquire(DataWorkContext(FrameType::foreground, 1024), 1024);
    ASSERT_TRUE(other.has_value());
    time.advance(100s);
    arbiter.time_advanced();
    other.reset();
  }
  EXPECT_EQ(arbiter.stats().no_progress_failures, 0u);
  EXPECT_EQ(arbiter.stats().cancelled_waits, 0u);
  EXPECT_EQ(waiter.wait_for(0ms), std::future_status::timeout);
  held.reset();
  ASSERT_EQ(waiter.wait_for(10s), std::future_status::ready);
  const auto admitted = waiter.get();
  ASSERT_TRUE(admitted.has_value());
  EXPECT_TRUE(admitted->credit.has_value());
  EXPECT_EQ(arbiter.stats().no_progress_failures, 0u);
}

// The same code: a whole window with no release anywhere fails the wait, says
// so in Macha's words, counts a no-progress failure rather than a cancelled wait,
// and leaves the arbiter usable.
TEST(StoreDataWork, AWaitWithNoDeadlineFailsAfterAWholeWindowWithNoRelease) {
  dannan::test::SteppedTime time;
  const auto sink = std::make_shared<test::RecordingSink>();
  DataResourceArbiter arbiter(4 * 1024 * 1024, 1024 * 1024, 1, 120s, {}, dannan::status::ScopedLogger(sink, "credit"), time);
  auto held = arbiter.try_acquire(DataWorkContext(FrameType::loader, 1024), 1024);
  ASSERT_TRUE(held.has_value());
  auto waiter = std::async(std::launch::async, [&] { return admit_data(&arbiter, nullptr, FrameType::loader, 1024, no_caller_deadline); });
  ASSERT_TRUE(wait_until([&] { return arbiter.stats().loader_waits == 1; }));
  time.advance(121s);
  arbiter.time_advanced();
  ASSERT_EQ(waiter.wait_for(10s), std::future_status::ready);
  EXPECT_FALSE(waiter.get().has_value());
  const auto stats = arbiter.stats();
  EXPECT_EQ(stats.no_progress_failures, 1u);
  EXPECT_EQ(stats.cancelled_waits, 0u);
  EXPECT_TRUE(
      sink->saw("credit: DATA credit wait abandoned after 120000 ms with no release anywhere: class=loader bytes=1024 used=1024/4194304 "
                "lower_active=1/1 waiting_viewers=0 waiting_loaders=0"));
  held.reset();
  EXPECT_TRUE(admit_data(&arbiter, nullptr, FrameType::loader, 1024, no_caller_deadline).has_value());
}

// A caller's own deadline stays absolute: a release does not extend it, and
// its end is a cancelled wait, not a stall.
TEST(StoreDataWork, ACallersOwnDeadlineIsNotReArmedByARelease) {
  dannan::test::SteppedTime time;
  const auto sink = std::make_shared<test::RecordingSink>();
  DataResourceArbiter arbiter(4 * 1024 * 1024, 1024 * 1024, 1, 1h, {}, dannan::status::ScopedLogger(sink, "credit"), time);
  const auto held = arbiter.try_acquire(DataWorkContext(FrameType::loader, 1024), 1024);
  ASSERT_TRUE(held.has_value());
  auto waiter = std::async(std::launch::async, [&] { return admit_data(&arbiter, nullptr, FrameType::loader, 1024, 10s); });
  ASSERT_TRUE(wait_until([&] { return arbiter.stats().loader_waits == 1; }));
  {
    auto other = arbiter.try_acquire(DataWorkContext(FrameType::foreground, 1024), 1024);
    ASSERT_TRUE(other.has_value());
    time.advance(9s);
    arbiter.time_advanced();
  }
  time.advance(2s);
  arbiter.time_advanced();
  ASSERT_EQ(waiter.wait_for(10s), std::future_status::ready);
  EXPECT_FALSE(waiter.get().has_value());
  EXPECT_EQ(arbiter.stats().cancelled_waits, 1u);
  EXPECT_EQ(arbiter.stats().no_progress_failures, 0u);
  EXPECT_FALSE(sink->saw("abandoned"));
}

// Macha's zero waits for ever; Dagda's arbiter refuses zero, and a facade passes
// this instead (TODO/2026-11-05-second-cutover.md, S.3). A window that long neither
// overflows nor ends early, and still ends: it is finite.
TEST(StoreDataWork, AHundredYearWindowDoesNotOverflow) {
  constexpr std::chrono::milliseconds century = 24h * 365 * 100;
  ASSERT_EQ(century.count(), 3153600000000);
  dannan::test::SteppedTime time;
  const auto sink = std::make_shared<test::RecordingSink>();
  DataResourceArbiter arbiter(4 * 1024 * 1024, 1024 * 1024, 1, century, {}, dannan::status::ScopedLogger(sink, "credit"), time);
  const auto held = arbiter.try_acquire(DataWorkContext(FrameType::loader, 1024), 1024);
  ASSERT_TRUE(held.has_value());
  auto waiter = std::async(std::launch::async, [&] { return admit_data(&arbiter, nullptr, FrameType::loader, 1024, no_caller_deadline); });
  ASSERT_TRUE(wait_until([&] { return arbiter.stats().loader_waits == 1; }));
  EXPECT_EQ(waiter.wait_for(50ms), std::future_status::timeout);
  time.advance(24h * 365 * 99);
  arbiter.time_advanced();
  EXPECT_EQ(waiter.wait_for(50ms), std::future_status::timeout);
  EXPECT_EQ(arbiter.stats().no_progress_failures, 0u);
  time.advance(24h * 365 * 2);
  arbiter.time_advanced();
  ASSERT_EQ(waiter.wait_for(10s), std::future_status::ready);
  EXPECT_FALSE(waiter.get().has_value());
  EXPECT_EQ(arbiter.stats().no_progress_failures, 1u);
  EXPECT_TRUE(sink->saw("abandoned after 3153600000000 ms"));
}

// Macha has no reserve. Where a node has one, a request with no deadline waits
// for its slot as long as the window: never without a bound.
TEST(StoreDataWork, ASlotWaitWithNoDeadlineEndsAtTheWindow) {
  dannan::test::SteppedTime time;
  DataResourceArbiter arbiter(4 * 1024 * 1024, 1024 * 1024, 0, 120s, {}, std::nullopt, time);
  IoReserve io(IoReserveOptions{1, 0}, time);
  const auto slot = io.try_data(DataWorkContext(FrameType::loader, 1024));
  ASSERT_TRUE(slot.has_value());
  auto waiter = std::async(std::launch::async, [&] { return admit_data(&arbiter, &io, FrameType::loader, 1024, no_caller_deadline); });
  ASSERT_TRUE(wait_until([&] { return io.stats().data_waits == 1; }));
  time.advance(121s);
  io.time_advanced();
  ASSERT_EQ(waiter.wait_for(10s), std::future_status::ready);
  EXPECT_FALSE(waiter.get().has_value());
  EXPECT_EQ(io.stats().data_refusals, 1u);
}

// The reserve: with DATA holding every slot it may and more DATA waiting,
// control work is admitted at once, up to the reserve, and DATA can never
// take the reserved slots.
TEST(StoreDataWork, ControlWorkIsAdmittedWhileDataSaturatesTheDevice) {
  IoReserve io(IoReserveOptions{4, 2});
  ASSERT_EQ(io.data_share(), 2u);
  const DataWorkContext loader(FrameType::loader, 1024 * 1024);
  auto first = io.try_data(loader);
  auto second = io.try_data(loader);
  ASSERT_TRUE(first && second);
  // The reserve is not DATA's, however long it waits.
  auto waiting = std::async(std::launch::async, [&] { return io.data(loader, 300ms); });
  EXPECT_FALSE(io.try_data(DataWorkContext(FrameType::foreground, 4096)).has_value());

  const auto started = Clock::now();
  auto commit = io.try_control();
  auto claim = io.try_control();
  EXPECT_LT(Clock::now() - started, 100ms);
  EXPECT_TRUE(commit && claim);
  EXPECT_FALSE(waiting.get().has_value());

  const auto stats = io.stats();
  EXPECT_EQ(stats.control_waits, 0u);
  EXPECT_EQ(stats.control_admissions, 2u);
  EXPECT_EQ(stats.peak_data_in_flight, 2u);
  EXPECT_GE(stats.data_waits, 1u);
  EXPECT_GE(stats.data_refusals, 2u);
}

// Without the reserve, the same saturating loader holds a commit back until
// a DATA slot frees: what the reserve prevents.
TEST(StoreDataWork, WithoutTheReserveControlQueuesBehindData) {
  IoReserve io(IoReserveOptions{4, 0});
  const DataWorkContext loader(FrameType::loader, 1024 * 1024);
  std::vector<IoReserve::Slot> held;
  for (int i = 0; i < 4; ++i) held.push_back(std::move(*io.try_data(loader)));
  EXPECT_FALSE(io.control(100ms).has_value());
  EXPECT_EQ(io.stats().control_waits, 1u);
  EXPECT_EQ(io.stats().control_refusals, 1u);
}

// A control request waiting closes DATA admission until it is served: the
// slot DATA frees goes to control, never to the DATA waiter beside it.
TEST(StoreDataWork, AWaitingControlRequestIsServedBeforeData) {
  IoReserve io(IoReserveOptions{3, 1});
  const DataWorkContext loader(FrameType::loader, 1024);
  auto data_a = io.try_data(loader);
  auto data_b = io.try_data(loader);
  auto control_a = io.try_control();
  ASSERT_TRUE(data_a && data_b && control_a);
  auto control_waiter = std::async(std::launch::async, [&] { return io.control(5s); });
  ASSERT_TRUE([&] {
    for (int i = 0; i < 5000 && io.stats().control_waits == 0; ++i) std::this_thread::sleep_for(1ms);
    return io.stats().control_waits == 1;
  }());
  auto data_waiter = std::async(std::launch::async, [&] { return io.data(loader, 300ms); });
  data_a.reset();
  ASSERT_EQ(control_waiter.wait_for(5s), std::future_status::ready);
  // Held while the DATA waiter waits on.
  const auto granted = control_waiter.get();
  EXPECT_TRUE(granted.has_value());
  EXPECT_FALSE(data_waiter.get().has_value());
  // A reserve that leaves DATA nothing is refused.
  EXPECT_THROW(IoReserve(IoReserveOptions{2, 2}), std::invalid_argument);
  EXPECT_THROW(IoReserve(IoReserveOptions{0, 0}), std::invalid_argument);
}

// Stopping wakes every waiter, and nothing is admitted after.
TEST(StoreDataWork, StoppingRefusesEveryWaiter) {
  IoReserve io(IoReserveOptions{1, 0});
  DataResourceArbiter arbiter(2 * 1024 * 1024, 1024 * 1024, 1, 30s);
  const DataWorkContext loader(FrameType::loader, 1024);
  auto slot = io.try_data(loader);
  auto lease = arbiter.try_acquire(loader, 1024);
  ASSERT_TRUE(slot && lease);
  auto slot_waiter = std::async(std::launch::async, [&] { return io.control(30s); });
  auto credit_waiter = std::async(std::launch::async, [&] { return arbiter.acquire(loader, 1024); });
  // Before or after they wait, the answer is the same.
  io.stop();
  arbiter.stop();
  ASSERT_EQ(slot_waiter.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(credit_waiter.wait_for(5s), std::future_status::ready);
  EXPECT_FALSE(slot_waiter.get().has_value());
  EXPECT_FALSE(credit_waiter.get().has_value());
}

// Macha (tests/test_contract.cpp): test_data_work_context_is_the_data_
// specialisation. Through the base it is the same work; its quantum is its own;
// control is refused at construction.
TEST(StoreDataWork, TheContextIsTheDataSpecialisation) {
  const DataWorkContext data(FrameType::speculative, 4096);
  const dannan::sched::WorkContext& general = data;
  EXPECT_EQ(general.frame_type(), FrameType::speculative);
  EXPECT_EQ(data.quantum_bytes(), 4096u);
  EXPECT_THROW((void)DataWorkContext(FrameType::control), std::invalid_argument);
}

// Macha (tests/test_foundations.cpp):
// test_data_resource_arbiter_background_effort_ceiling. Byte capacity would
// admit four loader leases; the effort ceiling admits two at a time, counts
// speculative work with them and never counts foreground work.
TEST(StoreDataWork, TheBackgroundCeilingCountsLoaderAndSpeculativeNotForeground) {
  DataResourceArbiter arbiter(64, 8, 2, 1h);
  const DataWorkContext loader(FrameType::loader, 1);
  auto first = arbiter.try_acquire(loader, 1);
  auto second = arbiter.try_acquire(loader, 1);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(arbiter.try_acquire(loader, 1).has_value());
  EXPECT_FALSE(arbiter.try_acquire(DataWorkContext(FrameType::speculative, 1), 1).has_value());
  const auto foreground = arbiter.try_acquire(DataWorkContext(FrameType::foreground, 1), 1);
  EXPECT_TRUE(foreground.has_value());
  auto stats = arbiter.stats();
  EXPECT_EQ(stats.background_limit, 2u);
  EXPECT_EQ(stats.background_active, 2u);
  EXPECT_EQ(stats.peak_background_active, 2u);
  first.reset();
  const auto third = arbiter.try_acquire(loader, 1);
  EXPECT_TRUE(third.has_value());
  EXPECT_EQ(arbiter.stats().background_active, 2u);
}

// Macha (tests/test_foundations.cpp):
// test_data_resource_arbiter_reserves_viewer_headroom. What is larger than a
// class may ever hold is refused at once. Lower classes cannot consume the
// reserved byte, so a late foreground request starts at once. A deadline ends a
// wait on the injected time. A release serves the waiting loader before
// speculative work. The waits are seen in the arbiter's own figures, never
// waited out.
TEST(StoreDataWork, TheForegroundReserveHoldsAndALoaderIsServedBeforeSpeculation) {
  dannan::test::SteppedTime time;
  DataResourceArbiter arbiter(4, 1, 0, 1h, {}, std::nullopt, time);
  EXPECT_FALSE(arbiter.acquire(DataWorkContext(FrameType::loader), 4).has_value());
  EXPECT_FALSE(arbiter.acquire(DataWorkContext(FrameType::foreground), 5).has_value());
  const DataWorkContext loader(FrameType::loader, 1);
  auto first = arbiter.acquire(loader, 1);
  auto second = arbiter.acquire(loader, 1);
  auto third = arbiter.acquire(loader, 1);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());

  auto blocked_loader = std::async(std::launch::async, [&] { return arbiter.acquire(loader, 1); });
  ASSERT_TRUE(wait_until([&] { return arbiter.stats().loader_waits == 1; }));
  auto blocked_speculative = std::async(std::launch::async, [&] { return arbiter.acquire(DataWorkContext(FrameType::speculative, 1), 1); });
  ASSERT_TRUE(wait_until([&] { return arbiter.stats().speculative_waits == 1; }));

  auto foreground = arbiter.acquire(DataWorkContext(FrameType::foreground, 1), 1);
  ASSERT_TRUE(foreground.has_value());
  EXPECT_EQ(blocked_loader.wait_for(0ms), std::future_status::timeout);
  foreground.reset();

  auto expiring = std::async(std::launch::async, [&] { return arbiter.acquire(DataWorkContext(FrameType::loader, 1, time.now() + 20ms), 1); });
  ASSERT_TRUE(wait_until([&] { return arbiter.stats().loader_waits == 2; }));
  time.advance(21ms);
  arbiter.time_advanced();
  ASSERT_EQ(expiring.wait_for(10s), std::future_status::ready);
  EXPECT_FALSE(expiring.get().has_value());

  first.reset();
  ASSERT_EQ(blocked_loader.wait_for(10s), std::future_status::ready);
  auto admitted_loader = blocked_loader.get();
  ASSERT_TRUE(admitted_loader.has_value());
  EXPECT_EQ(blocked_speculative.wait_for(0ms), std::future_status::timeout);
  second.reset();
  ASSERT_EQ(blocked_speculative.wait_for(10s), std::future_status::ready);
  auto admitted_speculative = blocked_speculative.get();
  ASSERT_TRUE(admitted_speculative.has_value());

  const auto stats = arbiter.stats();
  EXPECT_EQ(stats.capacity_bytes, 4u);
  EXPECT_EQ(stats.foreground_reserve_bytes, 1u);
  EXPECT_EQ(stats.peak_used_bytes, 4u);
  EXPECT_EQ(stats.foreground_admissions, 1u);
  EXPECT_EQ(stats.loader_admissions, 4u);
  EXPECT_EQ(stats.loader_waits, 2u);
  EXPECT_EQ(stats.speculative_admissions, 1u);
  EXPECT_EQ(stats.speculative_waits, 1u);
  EXPECT_EQ(stats.cancelled_waits, 1u);
}

}  // namespace
}  // namespace dagda::store
