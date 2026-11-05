//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement S18 at one store: has() never reports an object a put is
// still writing, never waits for that put, never consults the device after
// warm-up, and is exact across a restart.
//
// Moved from Macha's tests/test_presence.cpp (develop 5a06d3c), every
// assertion kept: test_has_answers_a_put_in_progress_without_waiting_for_it,
// test_a_put_that_fails_midway_is_never_present,
// test_has_does_not_consult_the_device_after_warm_up,
// test_presence_is_exact_across_a_restart. Its PresenceIndex cases moved in
// 1a; its StoragePool cases move with the pool (1d). Macha bounded the
// "without waiting" with a 500 ms real-time check; here the put is held at
// its write by the interposed device, and has() answering while it is held
// is the proof.

#include <gtest/gtest.h>

#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "dagda/store/local_store.h"
#include "dagda/test/fixtures/store.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using test::InterposedLocalStoreFiles;
using test::numbered;
using test::StoreBed;
using test::wait_until;

const auto loose_only = LocalStoreOptions::loose_only(64ULL << 20);

// Macha: test_has_answers_a_put_in_progress_without_waiting_for_it. The put
// holds the object's lock across its write; has() neither waits for it nor
// sees the object before the write completes.
TEST(StoreLocalStorePresence, HasAnswersAPutInProgressWithoutWaitingForIt) {
  StoreBed bed;
  InterposedLocalStoreFiles files;
  auto store = bed.open(bed.dir.path() / "store", loose_only, LocalStoreMode::authoritative, {}, files);
  ASSERT_TRUE(wait_until([&] { return store->presence_authoritative(); }));
  const auto bytes = numbered(1);
  const auto id = object_id(bytes);

  std::mutex mutex;
  std::condition_variable cv;
  bool writing = false;
  bool release = false;
  files.before([&](InterposedLocalStoreFiles::Op op, const std::filesystem::path&) {
    if (op != InterposedLocalStoreFiles::Op::install) return;
    std::unique_lock lock(mutex);
    writing = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
  });
  auto put = std::async(std::launch::async, [&] { return store->put(id, bytes); });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 10s, [&] { return writing; }));
  }
  // Answered while the put is provably still held at its write.
  auto asked = std::async(std::launch::async, [&] { return store->has(id); });
  const bool answered = asked.wait_for(10s) == std::future_status::ready;
  EXPECT_TRUE(answered);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  cv.notify_all();
  ASSERT_TRUE(put.get());
  if (answered) {
    EXPECT_FALSE(asked.get());
  }
  EXPECT_TRUE(store->has(id));
}

// Macha: test_a_put_that_fails_midway_is_never_present.
TEST(StoreLocalStorePresence, APutThatFailsMidwayIsNeverPresent) {
  StoreBed bed;
  InterposedLocalStoreFiles files;
  auto store = bed.open(bed.dir.path() / "store", loose_only, LocalStoreMode::authoritative, {}, files);
  ASSERT_TRUE(wait_until([&] { return store->presence_authoritative(); }));
  const auto bytes = numbered(1);
  const auto id = object_id(bytes);
  files.before([](InterposedLocalStoreFiles::Op op, const std::filesystem::path&) {
    if (op == InterposedLocalStoreFiles::Op::install) throw std::runtime_error("device failed mid-write");
  });
  bool threw = false;
  try {
    (void)store->put(id, bytes);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  EXPECT_TRUE(threw);
  EXPECT_FALSE(store->has(id));
  // And the store is still usable: the next put of it succeeds.
  files.before({});
  ASSERT_TRUE(store->put(id, bytes));
  EXPECT_TRUE(store->has(id));
}

// Macha: test_has_does_not_consult_the_device_after_warm_up. Behind the
// store's back, where a device check would notice, the held object's file
// disappears and another object's file appears: has() reports what the store
// knows, which is what it wrote.
TEST(StoreLocalStorePresence, HasDoesNotConsultTheDeviceAfterWarmUp) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  auto store = bed.open(root, loose_only);
  ASSERT_TRUE(wait_until([&] { return store->presence_authoritative(); }));
  const auto held_bytes = numbered(1);
  const auto held = object_id(held_bytes);
  ASSERT_TRUE(store->put(held, held_bytes));
  const auto planted = object_id(numbered(2));

  std::filesystem::create_directories(store->object_path(planted).parent_path());
  std::filesystem::copy_file(store->object_path(held), store->object_path(planted));
  std::filesystem::remove(store->object_path(held));
  EXPECT_TRUE(store->has(held));
  EXPECT_FALSE(store->has(planted));
}

// Macha: test_presence_is_exact_across_a_restart.
TEST(StoreLocalStorePresence, PresenceIsExactAcrossARestart) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  std::vector<ObjectId> held;
  {
    auto store = bed.open(root, loose_only);
    for (uint64_t n = 0; n < 20; ++n) {
      const auto bytes = numbered(n);
      held.push_back(object_id(bytes));
      ASSERT_TRUE(store->put(held.back(), bytes));
    }
    ASSERT_TRUE(store->remove(held[3]));
  }
  auto store = bed.open(root, loose_only);
  ASSERT_TRUE(wait_until([&] { return store->presence_authoritative(); }));
  for (size_t n = 0; n < held.size(); ++n) EXPECT_EQ(store->has(held[n]), n != 3);
  EXPECT_FALSE(store->has(object_id(numbered(100))));
}

}  // namespace
}  // namespace dagda::store
