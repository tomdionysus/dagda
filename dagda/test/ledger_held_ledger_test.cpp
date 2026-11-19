//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement L3, at the ledger. Its other Macha cases drive a LocalStore
// and move with it (1c).
//
// Moved from Macha's tests/test_object_ledger.cpp (develop 5a06d3c):
// test_a_flush_does_not_wait_for_the_seed. The bounded future waits are a
// safety net against a hang, not a timing.

#include "dagda/test/fixtures/ledger.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <future>
#include <random>
#include <string>
#include <vector>

#include "dagda/ledger/held_ledger.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

using namespace std::chrono_literals;
using test::id_of;
using test::key_of;
using test::quiet_logger;
using test::small_options;

// Macha: test_a_flush_does_not_wait_for_the_seed. A write's flush does not
// wait for a seed being built, and what it queued after the snapshot is
// applied over the seed.
TEST(LedgerHeldLedger, AFlushDoesNotWaitForTheSeed) {
  dannan::test::TempDir dir;
  HeldLedger ledger(dir.path(), key_of(16), small_options(), quiet_logger());
  std::mt19937_64 random(21);
  const auto seeded_id = id_of(random);
  const auto later_id = id_of(random);
  std::promise<void> in_snapshot;
  std::promise<void> release;
  auto released = release.get_future().share();
  auto seeding = std::async(std::launch::async, [&] {
    ledger.seed([&] {
      ledger.drop_queued();
      in_snapshot.set_value();
      // The slow part of a seed, held open.
      released.wait();
      return std::vector<ObjectId>{seeded_id};
    });
  });
  in_snapshot.get_future().wait();
  // While the seed is held open: a write records and flushes, and returns.
  auto writing = std::async(std::launch::async, [&] {
    ledger.record(later_id, true);
    ledger.flush();
  });
  const bool flushed = writing.wait_for(5s) == std::future_status::ready;
  EXPECT_TRUE(flushed);
  EXPECT_FALSE(ledger.seeded());
  release.set_value();
  seeding.get();
  if (!flushed) writing.get();
  // Queued after the snapshot: applied over the seed by the seed itself.
  EXPECT_EQ(ledger.held(seeded_id), std::optional<bool>(true));
  EXPECT_EQ(ledger.held(later_id), std::optional<bool>(true));
  EXPECT_EQ(ledger.size(), 2u);
}

// Unseeded it answers nothing and keeps what is queued; seeded, every
// install that marks an id not held counts a removal, and a seed counts one.
TEST(LedgerHeldLedger, AnswersOnceSeededAndCountsRemovals) {
  dannan::test::TempDir dir;
  HeldLedger ledger(dir.path(), key_of(17), small_options(), quiet_logger());
  std::mt19937_64 random(31);
  const auto a = id_of(random);
  const auto b = id_of(random);
  EXPECT_FALSE(ledger.seeded());
  EXPECT_FALSE(ledger.held(a).has_value());
  ledger.record(a, true);
  ledger.flush();
  EXPECT_EQ(ledger.size(), 0u);
  EXPECT_EQ(ledger.removals(), 0u);

  ledger.seed([&] { return std::vector<ObjectId>{}; });
  EXPECT_TRUE(ledger.seeded());
  // Queued before the seed and not dropped by its snapshot: applied over it.
  EXPECT_EQ(ledger.held(a), std::optional<bool>(true));
  EXPECT_EQ(ledger.held(b), std::optional<bool>(false));
  EXPECT_EQ(ledger.removals(), 1u);

  // The last change to an id wins, and only a not-held install counts.
  ledger.record(b, true);
  ledger.record(a, false);
  ledger.record(b, false);
  ledger.record(b, true);
  ledger.flush();
  EXPECT_EQ(ledger.held(a), std::optional<bool>(false));
  EXPECT_EQ(ledger.held(b), std::optional<bool>(true));
  EXPECT_EQ(ledger.removals(), 2u);
  const auto view = ledger.view();
  EXPECT_EQ(view.removals, 2u);
  EXPECT_EQ(view.records.ids(), std::vector<ObjectId>{b});
  // The record of a held object is 1 as eight big-endian bytes.
  EXPECT_EQ(view.records.get(b), std::optional<Bytes>(Bytes{0, 0, 0, 0, 0, 0, 0, 1}));
}

// A reopened ledger is seeded and holds what it held, with no walk; its
// removals start again from zero, and its check resumes where it stopped.
TEST(LedgerHeldLedger, AReopenedLedgerKnowsWhatItHeld) {
  dannan::test::TempDir dir;
  std::mt19937_64 random(32);
  std::vector<ObjectId> ids;
  for (int i = 0; i < 300; ++i) ids.push_back(id_of(random));
  {
    HeldLedger ledger(dir.path(), key_of(18), small_options(), quiet_logger());
    ledger.seed([&] {
      auto sorted = ids;
      std::sort(sorted.begin(), sorted.end());
      return sorted;
    });
    ledger.record(ids[0], false);
    ledger.flush();
    ledger.set_verify_next(0x1234);
    EXPECT_EQ(ledger.verify_next(), 0x1234);
  }
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "seeded"));
  std::ifstream verify(dir.path() / "verify-next");
  std::string text;
  std::getline(verify, text);
  EXPECT_EQ(text, std::to_string(0x1234));

  HeldLedger reopened(dir.path(), key_of(18), small_options(), quiet_logger());
  EXPECT_TRUE(reopened.seeded());
  EXPECT_EQ(reopened.size(), ids.size() - 1);
  EXPECT_EQ(reopened.held(ids[0]), std::optional<bool>(false));
  for (size_t i = 1; i < ids.size(); ++i) EXPECT_EQ(reopened.held(ids[i]), std::optional<bool>(true));
  EXPECT_EQ(reopened.removals(), 0u);
  EXPECT_EQ(reopened.verify_next(), 0x1234);
}

// The ids under one two-byte prefix, in order, and none of its neighbours'.
TEST(LedgerHeldLedger, ListsTheIdsUnderATwoBytePrefix) {
  dannan::test::TempDir dir;
  HeldLedger ledger(dir.path(), key_of(19), small_options(), quiet_logger());
  std::mt19937_64 random(33);
  std::vector<ObjectId> all;
  std::vector<ObjectId> wanted;
  for (int i = 0; i < 600; ++i) {
    auto id = id_of(random);
    // Around 0x1234: the one before, itself, the one after.
    id.bytes[0] = 0x12;
    id.bytes[1] = static_cast<uint8_t>(0x33 + i % 3);
    if (i % 3 == 1) wanted.push_back(id);
    all.push_back(id);
  }
  // At the ends of the id space too.
  ObjectId first{};
  ObjectId last{};
  last.bytes.fill(0xff);
  all.push_back(first);
  all.push_back(last);
  std::sort(all.begin(), all.end());
  std::sort(wanted.begin(), wanted.end());
  ledger.seed([&] { return all; });
  EXPECT_EQ(ledger.held_with_prefix(0x1234), wanted);
  EXPECT_EQ(ledger.held_with_prefix(0x0000), std::vector<ObjectId>{first});
  EXPECT_EQ(ledger.held_with_prefix(0xffff), std::vector<ObjectId>{last});
  EXPECT_TRUE(ledger.held_with_prefix(0x5555).empty());
}

}  // namespace
}  // namespace dagda::ledger
