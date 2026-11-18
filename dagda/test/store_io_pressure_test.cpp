//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement S21: DATA device pressure from service time, and the pool
// feeding it with every read and write, maintenance's included.
//
// Moved from Macha's tests/test_io_pressure.cpp (develop 5a06d3c), every
// assertion kept: test_an_ordinary_large_write_is_not_a_slow_device,
// test_pressure_onset_and_release_are_each_logged_once,
// test_a_device_far_slower_than_it_should_be_is_noticed,
// test_the_single_sample_trip_scales_with_the_operation,
// test_a_read_is_measured_with_the_bytes_it_returned,
// test_maintenance_reads_are_part_of_the_service_time_they_spend, and, with
// the DATA credit arbiter (cluster/data_work.hpp, step 3b), its seven:
// test_a_viewer_is_never_refused_because_the_disk_is_slow,
// test_the_loader_runs_freely_under_pressure_with_no_viewer,
// test_the_loader_yields_to_a_viewer_on_a_slow_device,
// test_a_viewer_between_two_extents_is_still_a_viewer,
// test_background_work_always_drains_on_a_pressured_device,
// test_control_cannot_enter_the_data_arbiter_at_all,
// test_with_no_device_admission_is_exactly_what_it_was. Macha's process
// logger is the monitor's own scoped logger; its viewer is Dannan's
// foreground class.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "dagda/store/data_work.h"
#include "dagda/store/io_pressure.h"
#include "dagda/store/object_id.h"
#include "dagda/store/storage_pool.h"
#include "dagda/test/fixtures/store.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using dannan::sched::FrameType;
using test::pattern;
using test::RecordingSink;
using test::StoreBed;

// The shipped defaults, so these tests exercise what the nodes run.
constexpr DiskServiceMonitor::Thresholds defaults{25ms, 120ms, 300, 150, 1000};

dannan::status::ScopedLogger quiet() { return dannan::status::ScopedLogger(std::make_shared<RecordingSink>(), "pressure"); }

size_t count(RecordingSink& sink, std::string_view what) {
  const auto lines = sink.lines();
  return static_cast<size_t>(std::count_if(lines.begin(), lines.end(), [&](const auto& line) { return line.find(what) != std::string::npos; }));
}

// Macha: test_an_ordinary_large_write_is_not_a_slow_device.
TEST(StoreIoPressure, AnOrdinaryLargeWriteIsNotASlowDevice) {
  // A 4 MiB write taking 200 ms is judged against what that size should
  // cost, and is unremarkable.
  DiskServiceMonitor monitor(quiet(), defaults);
  for (int i = 0; i < 100; ++i) monitor.note(200ms, 4 * 1024 * 1024);
  EXPECT_FALSE(monitor.pressured());
  // Expected for 4 MiB is 25 + 480 = 505 ms, so 200 ms is comfortably under.
  EXPECT_LT(monitor.sample().slowdown_percent, 100u);

  // Small operations are judged on their own scale: 4 KiB taking 200 ms is a
  // device in trouble.
  DiskServiceMonitor small(quiet(), defaults);
  for (int i = 0; i < 40; ++i) small.note(200ms, 4096);
  EXPECT_TRUE(small.pressured());
}

// Macha: test_pressure_onset_and_release_are_each_logged_once. Each
// transition logs one line; staying in a state logs nothing.
TEST(StoreIoPressure, OnsetAndReleaseAreEachLoggedOnce) {
  auto capture = std::make_shared<RecordingSink>();
  DiskServiceMonitor monitor(dannan::status::ScopedLogger(capture, "pressure"), defaults);
  for (int i = 0; i < 20; ++i) monitor.note(100ms, 4 * 1024 * 1024);
  EXPECT_EQ(count(*capture, "pressure"), 0u);

  for (int i = 0; i < 20; ++i) monitor.note(3000ms, 4 * 1024 * 1024);
  EXPECT_TRUE(monitor.pressured());
  EXPECT_EQ(count(*capture, "DATA device pressure onset"), 1u);

  for (int i = 0; i < 200; ++i) monitor.note(10ms, 4 * 1024 * 1024);
  EXPECT_FALSE(monitor.pressured());
  EXPECT_EQ(count(*capture, "DATA device pressure released"), 1u);
  EXPECT_EQ(count(*capture, "DATA device pressure onset"), 1u);
}

// Macha: test_a_device_far_slower_than_it_should_be_is_noticed.
TEST(StoreIoPressure, ADeviceFarSlowerThanItShouldBeIsNoticed) {
  DiskServiceMonitor monitor(quiet(), defaults);
  EXPECT_FALSE(monitor.pressured());

  for (int i = 0; i < 50; ++i) monitor.note(100ms, 4 * 1024 * 1024);
  EXPECT_FALSE(monitor.pressured());

  // After fifty healthy samples a 17.7 s write lifts the moving average only
  // to about 237%, under the 300% line; at 3,505% of its own expectation it
  // trips the single-sample outlier instead.
  monitor.note(17700ms, 4 * 1024 * 1024);
  EXPECT_TRUE(monitor.pressured());
  EXPECT_EQ(monitor.pressure_onsets(), 1u);
  EXPECT_GE(monitor.sample().worst_us, 17000000u);

  // Sustained degradation keeps it there.
  for (int i = 0; i < 20; ++i) monitor.note(3000ms, 4 * 1024 * 1024);
  EXPECT_TRUE(monitor.pressured());

  // Recovery needs a run of healthy operations, not one.
  monitor.note(100ms, 4 * 1024 * 1024);
  EXPECT_TRUE(monitor.pressured());
  for (int i = 0; i < 80; ++i) monitor.note(80ms, 4 * 1024 * 1024);
  EXPECT_FALSE(monitor.pressured());
}

// Macha: test_the_single_sample_trip_scales_with_the_operation.
TEST(StoreIoPressure, TheSingleSampleTripScalesWithTheOperation) {
  // The outlier trip is a ratio of expectation, so it means the same at
  // every size. 2 s moving 32 MiB is 51% of expectation and must not trip.
  DiskServiceMonitor large(quiet(), defaults);
  for (int i = 0; i < 10; ++i) large.note(2000ms, 32 * 1024 * 1024);
  EXPECT_FALSE(large.pressured());

  // 2 s for a 4 KiB read is 8,000% and trips on the first sample.
  DiskServiceMonitor tiny(quiet(), defaults);
  tiny.note(2000ms, 4096);
  EXPECT_TRUE(tiny.pressured());
  EXPECT_EQ(tiny.pressure_onsets(), 1u);

  // A 17.7 s write after a healthy run trips on its first sample.
  DiskServiceMonitor founding(quiet(), defaults);
  for (int i = 0; i < 50; ++i) founding.note(100ms, 4 * 1024 * 1024);
  ASSERT_FALSE(founding.pressured());
  founding.note(17700ms, 4 * 1024 * 1024);
  EXPECT_TRUE(founding.pressured());

  // Zero disables the trip and leaves the moving average alone in charge.
  auto no_trip = defaults;
  no_trip.outlier_percent = 0;
  DiskServiceMonitor averaged(quiet(), no_trip);
  for (int i = 0; i < 50; ++i) averaged.note(100ms, 4 * 1024 * 1024);
  averaged.note(17700ms, 4 * 1024 * 1024);
  EXPECT_FALSE(averaged.pressured());
}

// Macha: test_a_read_is_measured_with_the_bytes_it_returned. A read's size is
// known only once it succeeds; the pool's get() must still charge it, or
// reads are judged on per-operation overhead alone.
TEST(StoreIoPressure, AReadIsMeasuredWithTheBytesItReturned) {
  StoreBed bed;
  const auto backend = bed.dir.path() / "backend";
  std::filesystem::create_directories(backend);
  auto pool = bed.pool(bed.dir.path() / "state", {{backend, 64ULL * 1024 * 1024, 0}}, 0ms);
  pool->configure_service_monitor(defaults);

  std::vector<ObjectId> ids;
  uint64_t written = 0;
  for (size_t i = 0; i < 8; ++i) {
    auto data = pattern(256 * 1024 + i);
    data[0] ^= static_cast<uint8_t>(i);
    const auto id = object_id(data);
    ASSERT_TRUE(pool->put(id, data));
    written += data.size();
    ids.push_back(id);
  }

  const auto after_writes = pool->service_monitor().sample();
  EXPECT_EQ(after_writes.bytes, written);

  uint64_t read_back = 0;
  for (const auto& id : ids) {
    const auto data = pool->get(id);
    ASSERT_TRUE(data.has_value());
    read_back += data->size();
  }

  const auto after_reads = pool->service_monitor().sample();
  // The reads were charged their real size.
  EXPECT_EQ(after_reads.bytes - after_writes.bytes, read_back);
  EXPECT_GE(after_reads.operations, after_writes.operations + ids.size());
  // A temporary-directory store serving quarter-megabyte objects is not under
  // pressure.
  EXPECT_FALSE(pool->service_monitor().pressured());
}

// Macha: test_maintenance_reads_are_part_of_the_service_time_they_spend.
// Maintenance reads backends directly rather than through get(); the monitor
// must still see them.
TEST(StoreIoPressure, MaintenanceReadsArePartOfTheServiceTimeTheySpend) {
  StoreBed bed;
  const auto backend = bed.dir.path() / "backend";
  std::filesystem::create_directories(backend);
  auto pool = bed.pool(bed.dir.path() / "state", {{backend, 64ULL * 1024 * 1024, 0}}, 0ms);
  pool->configure_service_monitor(defaults);

  uint64_t stored = 0;
  for (size_t i = 0; i < 8; ++i) {
    auto data = pattern(256 * 1024 + i);
    data[0] ^= static_cast<uint8_t>(i);
    ASSERT_TRUE(pool->put(object_id(data), data));
    stored += data.size();
  }

  const auto before = pool->service_monitor().sample();
  const auto scrub = pool->scrub_step(0, 64, {});
  ASSERT_EQ(scrub.bytes, stored);
  const auto after = pool->service_monitor().sample();
  EXPECT_GT(after.operations, before.operations);
  EXPECT_EQ(after.bytes - before.bytes, stored);
}

// The timer reads the injected time, so a stepped source measures exactly
// what it was moved by.
TEST(StoreIoPressure, TheTimerMeasuresOnTheInjectedTime) {
  dannan::test::SteppedTime time;
  DiskServiceMonitor monitor(quiet(), defaults);
  {
    DiskServiceTimer timer(&monitor, 4096, time);
    time.advance(2000ms);
  }
  EXPECT_EQ(monitor.sample().operations, 1u);
  EXPECT_EQ(monitor.sample().worst_us, 2'000'000u);
  EXPECT_TRUE(monitor.pressured());
  // A null monitor measures nothing.
  DiskServiceTimer idle(nullptr, 4096, time);
}

// ---- The DATA credit arbiter ------------------------------------------------

DataWorkContext work(const DataResourceArbiter& arbiter, FrameType frame) { return DataWorkContext(frame, 0, arbiter.now() + 2s); }

// Macha: test_a_viewer_is_never_refused_because_the_disk_is_slow. Pressure
// may only refuse work nobody is waiting for.
TEST(StoreIoPressure, AForegroundRequestIsNeverRefusedBecauseTheDiskIsSlow) {
  DiskServiceMonitor monitor(quiet(), defaults);
  DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
  arbiter.observe_device(&monitor, 1);
  for (int i = 0; i < 30; ++i) monitor.note(9000ms, 4 * 1024 * 1024);
  ASSERT_TRUE(monitor.pressured());

  std::vector<DataResourceArbiter::Lease> foreground;
  for (int i = 0; i < 3; ++i) {
    auto lease = arbiter.try_acquire(work(arbiter, FrameType::foreground), 1024 * 1024);
    ASSERT_TRUE(lease.has_value());
    foreground.push_back(std::move(*lease));
  }
  const auto read_ahead = arbiter.try_acquire(work(arbiter, FrameType::read_ahead), 1024 * 1024);
  EXPECT_TRUE(read_ahead.has_value());

  const auto stats = arbiter.stats();
  EXPECT_TRUE(stats.device_pressured);
  EXPECT_GT(stats.device_service_us, 50000u);
  EXPECT_EQ(stats.pressure_refusals, 0u);
}

// Macha: test_the_loader_runs_freely_under_pressure_with_no_viewer. Law 3:
// the loader waits only if it would make a foreground user wait.
TEST(StoreIoPressure, TheLoaderRunsFreelyUnderPressureWithNoForegroundUser) {
  DiskServiceMonitor monitor(quiet(), defaults);
  DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms, [] { return false; });
  arbiter.observe_device(&monitor, 1);
  for (int i = 0; i < 30; ++i) monitor.note(9000ms, 4 * 1024 * 1024);
  ASSERT_TRUE(monitor.pressured());

  std::vector<DataResourceArbiter::Lease> loaders;
  for (int i = 0; i < 4; ++i) {
    auto lease = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
    ASSERT_TRUE(lease.has_value());
    loaders.push_back(std::move(*lease));
  }
  EXPECT_EQ(arbiter.stats().pressure_refusals, 0u);

  // Speculative work, below the loader, still stands aside.
  const auto speculative = arbiter.try_acquire(work(arbiter, FrameType::speculative), 1024 * 1024);
  EXPECT_FALSE(speculative.has_value());
  // Refusals are counted so the throttle's cost is visible.
  EXPECT_EQ(arbiter.stats().pressure_refusals, 1u);
}

// Macha: test_the_loader_yields_to_a_viewer_on_a_slow_device. With a
// foreground user present the loader yields to a trickle, not a stop.
TEST(StoreIoPressure, TheLoaderYieldsToAForegroundUserOnASlowDevice) {
  DiskServiceMonitor monitor(quiet(), defaults);
  DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms, [] { return false; });
  arbiter.observe_device(&monitor, 1);
  for (int i = 0; i < 30; ++i) monitor.note(9000ms, 4 * 1024 * 1024);
  ASSERT_TRUE(monitor.pressured());

  // A foreground request holding credit is a foreground user present.
  auto foreground = arbiter.try_acquire(work(arbiter, FrameType::foreground), 1024 * 1024);
  ASSERT_TRUE(foreground.has_value());

  auto first = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
  ASSERT_TRUE(first.has_value());
  const auto second = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
  EXPECT_FALSE(second.has_value());
  EXPECT_EQ(arbiter.stats().pressure_refusals, 1u);

  // Releasing the trickle lease lets the next unit of loader work through.
  first.reset();
  const auto next = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
  EXPECT_TRUE(next.has_value());

  auto another = arbiter.try_acquire(work(arbiter, FrameType::read_ahead), 1024 * 1024);
  EXPECT_TRUE(another.has_value());

  // With the user gone, the loader gets its concurrency back on the still
  // slow device.
  foreground.reset();
  another.reset();
  const auto unblocked = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
  EXPECT_TRUE(unblocked.has_value());
}

// Macha: test_a_viewer_between_two_extents_is_still_a_viewer. Playback holds
// no credit between extents, so presence comes from the activity signal the
// arbiter is given, not from held credit.
TEST(StoreIoPressure, AForegroundUserBetweenTwoExtentsIsStillPresent) {
  DiskServiceMonitor monitor(quiet(), defaults);
  std::atomic_bool watching{true};
  DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms, [&] { return watching.load(); });
  arbiter.observe_device(&monitor, 1);
  for (int i = 0; i < 30; ++i) monitor.note(9000ms, 4 * 1024 * 1024);
  ASSERT_TRUE(monitor.pressured());

  // No foreground credit is held, yet the loader still yields to its trickle.
  const auto first = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
  ASSERT_TRUE(first.has_value());
  const auto second = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
  EXPECT_FALSE(second.has_value());

  // A foreground request arriving mid-gap is still never refused.
  auto foreground = arbiter.try_acquire(work(arbiter, FrameType::foreground), 1024 * 1024);
  EXPECT_TRUE(foreground.has_value());

  // Once the stream has stopped, the loader takes the disk back.
  watching = false;
  foreground.reset();
  const auto resumed = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
  EXPECT_TRUE(resumed.has_value());
}

// A loader waiting because a foreground user is present on a slow device is let through
// when the user goes, though nothing is released: the user's going notifies nobody, and
// the wait otherwise looked again only at a release or at the end of its window. Macha's
// arbiter waits the same way (data_work.hpp:239-299 at main); found as
// StoreRepair.ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit failing on a
// slow Pi (2026-11-17, and under ASan 2026-11-18), its other holders never releasing.
TEST(StoreIoPressure, AWaitingLoaderSeesTheForegroundUserGoWithNothingReleased) {
  DiskServiceMonitor monitor(quiet(), defaults);
  std::atomic_bool watching{true};
  DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 1h, [&] { return watching.load(); });
  arbiter.observe_device(&monitor, 1);
  for (int i = 0; i < 30; ++i) monitor.note(9000ms, 4 * 1024 * 1024);
  ASSERT_TRUE(monitor.pressured());

  const auto trickle = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
  ASSERT_TRUE(trickle.has_value());
  auto waiting = std::async(std::launch::async, [&] { return arbiter.acquire(work(arbiter, FrameType::loader), 1024 * 1024); });
  ASSERT_TRUE(test::wait_until([&] { return arbiter.stats().loader_waits >= 1; }, 5s));
  EXPECT_EQ(waiting.wait_for(50ms), std::future_status::timeout);

  watching = false;
  ASSERT_EQ(waiting.wait_for(2s), std::future_status::ready) << "the loader slept on after the foreground user went";
  EXPECT_TRUE(waiting.get().has_value());
}

// The node's arbiter knows when the user's window closes, and looks again exactly then;
// and work turned away for the pressure alone is woken by the device leaving it, though
// nothing is released and no window is open: nothing polls (the Macha session's word,
// 2026-11-18: park on what can change the answer).
TEST(StoreIoPressure, AWaiterWakesWhenTheWindowClosesOrThePressureEnds) {
  DiskServiceMonitor monitor(quiet(), defaults);
  std::atomic<int64_t> present_for_ms{300};
  DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 1h, [&] { return present_for_ms.load() > 0; });
  arbiter.foreground_presence_ends([&]() -> std::optional<std::chrono::milliseconds> {
    const auto left = present_for_ms.load();
    return left > 0 ? std::optional(std::chrono::milliseconds(left)) : std::nullopt;
  });
  arbiter.observe_device(&monitor, 1);
  for (int i = 0; i < 30; ++i) monitor.note(9000ms, 4 * 1024 * 1024);
  ASSERT_TRUE(monitor.pressured());
  const auto trickle = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
  ASSERT_TRUE(trickle.has_value());

  // The user's window: the loader looks again when it closes, told nothing.
  auto loader = std::async(std::launch::async, [&] { return arbiter.acquire(work(arbiter, FrameType::loader), 1024 * 1024); });
  ASSERT_TRUE(test::wait_until([&] { return arbiter.stats().loader_waits >= 1; }, 5s));
  present_for_ms = 0;
  ASSERT_EQ(loader.wait_for(2s), std::future_status::ready) << "the loader slept past the window's end";
  auto second = loader.get();
  EXPECT_TRUE(second.has_value());

  // Speculative work stands aside for the pressure itself: woken when it ends.
  auto speculative = std::async(std::launch::async, [&] { return arbiter.acquire(work(arbiter, FrameType::speculative), 1024 * 1024); });
  ASSERT_TRUE(test::wait_until([&] { return arbiter.stats().speculative_waits >= 1; }, 5s));
  EXPECT_EQ(speculative.wait_for(300ms), std::future_status::timeout);
  for (int i = 0; i < 200 && monitor.pressured(); ++i) monitor.note(1ms, 4 * 1024 * 1024);
  ASSERT_FALSE(monitor.pressured());
  ASSERT_EQ(speculative.wait_for(2s), std::future_status::ready) << "the speculative work slept on after the pressure ended";
  EXPECT_TRUE(speculative.get().has_value());
}

// Macha: test_background_work_always_drains_on_a_pressured_device. Law 4:
// min_background always admits one lease when nothing is active, so
// background work keeps feeding the monitor and the average can fall.
TEST(StoreIoPressure, BackgroundWorkAlwaysDrainsOnAPressuredDevice) {
  DiskServiceMonitor monitor(quiet(), defaults);
  DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms, [] { return true; });
  arbiter.observe_device(&monitor, 1);
  for (int i = 0; i < 200; ++i) monitor.note(30000ms, 4 * 1024 * 1024);
  ASSERT_TRUE(monitor.pressured());

  // Under sustained overload with a foreground user present, the trickle
  // still turns over one lease at a time.
  for (int i = 0; i < 50; ++i) {
    const auto lease = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
    ASSERT_TRUE(lease.has_value());
  }

  // A recovered device releases pressure.
  for (int i = 0; i < 100; ++i) monitor.note(80ms, 4 * 1024 * 1024);
  EXPECT_FALSE(monitor.pressured());
  std::vector<DataResourceArbiter::Lease> loaders;
  for (int i = 0; i < 4; ++i) {
    auto lease = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
    ASSERT_TRUE(lease.has_value());
    loaders.push_back(std::move(*lease));
  }
}

// Macha: test_control_cannot_enter_the_data_arbiter_at_all. Law 1: control
// work never enters the DATA arbiter: a DATA work context for it throws, and
// so does asking the arbiter or the device's DATA share in its name.
TEST(StoreIoPressure, ControlCannotEnterTheDataArbiterAtAll) {
  DiskServiceMonitor monitor(quiet(), defaults);
  DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
  arbiter.observe_device(&monitor, 1);
  for (int i = 0; i < 30; ++i) monitor.note(9000ms, 4 * 1024 * 1024);
  ASSERT_TRUE(monitor.pressured());

  EXPECT_THROW(DataWorkContext{FrameType::control}, std::invalid_argument);
  IoReserve io;
  EXPECT_THROW(admit_data(&arbiter, &io, FrameType::control, 4096, 100ms), std::invalid_argument);
  EXPECT_THROW(admit_data(nullptr, nullptr, FrameType::control, 4096, 100ms), std::invalid_argument);
  EXPECT_EQ(arbiter.stats().used_bytes, 0u);
}

// Macha: test_with_no_device_admission_is_exactly_what_it_was. Without an
// observed device, pressure never refuses anything.
TEST(StoreIoPressure, WithNoDeviceAdmissionIsExactlyWhatItWas) {
  DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
  std::vector<DataResourceArbiter::Lease> loaders;
  for (int i = 0; i < 6; ++i) {
    auto lease = arbiter.try_acquire(work(arbiter, FrameType::loader), 1024 * 1024);
    ASSERT_TRUE(lease.has_value());
    loaders.push_back(std::move(*lease));
  }
  const auto stats = arbiter.stats();
  EXPECT_FALSE(stats.device_pressured);
  EXPECT_EQ(stats.device_service_us, 0u);
  EXPECT_EQ(stats.pressure_refusals, 0u);
}

}  // namespace
}  // namespace dagda::store
