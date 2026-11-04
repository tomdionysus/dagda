//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement L22: deletion waits on this node's own clock, from its own
// first sighting of an object unreferenced, kept across restarts in layout
// v18's sightings file.
//
// Moved from Macha's tests/test_absent_nodes.cpp (develop 5a06d3c), every
// assertion kept: test_an_object_matures_a_grace_after_its_first_sighting,
// test_an_object_referenced_again_starts_its_wait_afresh,
// test_a_clock_set_back_restarts_the_wait,
// test_a_completed_sweep_drops_sightings_of_objects_no_longer_held,
// test_sightings_survive_a_restart,
// test_the_sightings_codec_round_trips_and_refuses_damage. The file's other
// two cases are the roster's forgetting horizon, which is Dannan's.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>

#include "dagda/ledger/unreferenced_since.h"
#include "dagda/test/fixtures/ledger.h"
#include "dannan/identity/crypto.h"
#include "dannan/net/codec.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

using namespace std::chrono_literals;

ObjectId id(uint8_t n) {
  ObjectId out{};
  out.bytes[0] = n;
  return out;
}

dannan::status::ScopedLogger quiet() { return test::quiet_logger(); }

// Macha: test_an_object_matures_a_grace_after_its_first_sighting. The wait
// runs from this node's first sighting, whatever the time is when it is
// asked again.
TEST(LedgerUnreferencedSince, AnObjectMaturesAGraceAfterItsFirstSighting) {
  dannan::test::TempDir dir;
  UnreferencedSince seen(dir.path() / "unreferenced.bin", quiet());
  EXPECT_FALSE(seen.matured(id(1), 1'000, 500ms));
  EXPECT_FALSE(seen.matured(id(1), 1'499, 500ms));
  EXPECT_TRUE(seen.matured(id(1), 1'500, 500ms));
  // A second object sighted later waits its own grace.
  EXPECT_FALSE(seen.matured(id(2), 1'500, 500ms));
  EXPECT_TRUE(seen.matured(id(2), 2'000, 500ms));
  // No grace: gone at first sight.
  EXPECT_TRUE(seen.matured(id(3), 2'000, 0ms));
  EXPECT_EQ(seen.size(), 3u);
}

// Macha: test_an_object_referenced_again_starts_its_wait_afresh.
TEST(LedgerUnreferencedSince, AnObjectReferencedAgainStartsItsWaitAfresh) {
  dannan::test::TempDir dir;
  UnreferencedSince seen(dir.path() / "unreferenced.bin", quiet());
  EXPECT_FALSE(seen.matured(id(1), 1'000, 500ms));
  seen.forget(id(1));
  EXPECT_EQ(seen.size(), 0u);
  EXPECT_FALSE(seen.matured(id(1), 1'600, 500ms));
  EXPECT_TRUE(seen.matured(id(1), 2'100, 500ms));
}

// Macha: test_a_clock_set_back_restarts_the_wait.
TEST(LedgerUnreferencedSince, AClockSetBackRestartsTheWait) {
  dannan::test::TempDir dir;
  UnreferencedSince seen(dir.path() / "unreferenced.bin", quiet());
  EXPECT_FALSE(seen.matured(id(1), 10'000, 500ms));
  EXPECT_FALSE(seen.matured(id(1), 4'000, 500ms));
  EXPECT_FALSE(seen.matured(id(1), 4'499, 500ms));
  EXPECT_TRUE(seen.matured(id(1), 4'500, 500ms));
}

// Macha: test_a_completed_sweep_drops_sightings_of_objects_no_longer_held.
TEST(LedgerUnreferencedSince, ACompletedSweepDropsSightingsOfObjectsNoLongerHeld) {
  dannan::test::TempDir dir;
  UnreferencedSince seen(dir.path() / "unreferenced.bin", quiet());
  (void)seen.matured(id(1), 1'000, 500ms);
  (void)seen.matured(id(2), 1'000, 500ms);
  seen.pass_complete();
  EXPECT_EQ(seen.size(), 2u);
  (void)seen.matured(id(2), 1'100, 500ms);
  seen.pass_complete();
  EXPECT_EQ(seen.size(), 1u);
  // The survivor kept its first sighting.
  EXPECT_TRUE(seen.matured(id(2), 1'500, 500ms));
}

// Macha: test_sightings_survive_a_restart. A restart keeps the sightings, so
// a node restarted more often than the grace still deletes; an unreadable
// table only starts the waits again.
TEST(LedgerUnreferencedSince, SightingsSurviveARestart) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "retention" / "unreferenced.bin";
  {
    UnreferencedSince seen(path, quiet());
    (void)seen.matured(id(1), 1'000, 500ms);
    (void)seen.matured(id(2), 1'200, 500ms);
    seen.save();
    // Nothing changed: nothing is written.
    std::filesystem::remove(path);
    seen.save();
    EXPECT_FALSE(std::filesystem::exists(path));
    seen.forget(id(2));
    seen.save();
    EXPECT_TRUE(std::filesystem::exists(path));
  }
  {
    UnreferencedSince seen(path, quiet());
    EXPECT_EQ(seen.size(), 1u);
    EXPECT_TRUE(seen.matured(id(1), 1'500, 500ms));
    // Loaded sightings count as renewed for the first sweep after it.
    seen.pass_complete();
    EXPECT_EQ(seen.size(), 1u);
  }
  std::ofstream(path, std::ios::binary | std::ios::trunc) << "damaged";
  auto sink = std::make_shared<test::RecordingSink>();
  UnreferencedSince seen(path, test::logger_into(sink));
  EXPECT_EQ(seen.size(), 0u);
  EXPECT_TRUE(sink->saw("unreferenced-since: ignoring"));
}

// Macha: test_the_sightings_codec_round_trips_and_refuses_damage.
TEST(LedgerUnreferencedSince, TheSightingsCodecRoundTripsAndRefusesDamage) {
  const std::map<ObjectId, uint64_t> sightings{{id(1), 7}, {id(9), 1'234'567}};
  const auto bytes = encode_unreferenced_since(sightings);
  EXPECT_EQ(decode_unreferenced_since(bytes), sightings);
  EXPECT_TRUE(decode_unreferenced_since(encode_unreferenced_since({})).empty());
  const auto refused = [](Bytes damaged) {
    try {
      (void)decode_unreferenced_since(damaged);
    } catch (const dannan::net::DecodeError&) {
      return true;
    }
    return false;
  };
  auto truncated = bytes;
  truncated.pop_back();
  EXPECT_TRUE(refused(truncated));
  auto trailing = bytes;
  trailing.push_back(0);
  EXPECT_TRUE(refused(trailing));
  auto schema = bytes;
  schema[0] ^= 0xff;
  EXPECT_TRUE(refused(schema));
  // The same object twice.
  dannan::net::Writer twice;
  twice.u32(1);
  twice.u64(2);
  for (int i = 0; i < 2; ++i) {
    twice.fixed(id(1).bytes);
    twice.u64(7);
  }
  EXPECT_TRUE(refused(twice.take()));
  // A count far past the bytes fails on the bytes, never on memory.
  dannan::net::Writer huge;
  huge.u32(1);
  huge.u64(~0ULL);
  EXPECT_TRUE(refused(huge.take()));
}

// Layout v18's sightings file, byte for byte (requirements 4.1): u32 schema
// 1, u64 count, then (id, u64 Unix ms) in id order, big-endian, written
// whole by save().
TEST(LedgerUnreferencedSince, TheFileIsLayoutV18s) {
  const std::map<ObjectId, uint64_t> sightings{{id(9), 1'234'567}, {id(1), 7}};
  const auto expected = dannan::identity::unhex(
                            "00000001"
                            "0000000000000002"
                            "0100000000000000000000000000000000000000000000000000000000000000"
                            "0000000000000007"
                            "0900000000000000000000000000000000000000000000000000000000000000"
                            "000000000012d687")
                            .value();
  EXPECT_EQ(encode_unreferenced_since(sightings), expected);

  dannan::test::TempDir dir;
  const auto path = dir.path() / "retention" / "unreferenced-data.bin";
  {
    UnreferencedSince seen(path, quiet());
    (void)seen.matured(id(9), 1'234'567, 1ms);
    (void)seen.matured(id(1), 7, 1ms);
    seen.save();
  }
  std::ifstream in(path, std::ios::binary);
  const Bytes written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(written, expected);
}

}  // namespace
}  // namespace dagda::ledger
