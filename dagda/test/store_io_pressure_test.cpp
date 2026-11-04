//
// Dagda - The Store
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
// test_maintenance_reads_are_part_of_the_service_time_they_spend. The other
// seven cases there are the DATA credit arbiter's (cluster/data_work.hpp),
// which moves at step 3. Macha's process logger is the monitor's own scoped
// logger.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string_view>
#include <vector>

#include "dagda/store/io_pressure.h"
#include "dagda/store/object_id.h"
#include "dagda/store/storage_pool.h"
#include "dagda/test/fixtures/store.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
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

}  // namespace
}  // namespace dagda::store
