//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement L15: an owner's references counted per object on the state
// device, kept at a root across a restart.
//
// Moved from Macha's tests/test_object_ledger.cpp (develop 5a06d3c), every
// assertion kept: test_reference_counts_follow_and_survive_a_restart. The
// state file's known answer was computed outside both codebases from Macha's
// reference_counts.cpp:17, :109-121 (hashlib for the root id).

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <thread>

#include "dagda/ledger/reference_counts.h"
#include "dagda/test/fixtures/ledger.h"
#include "dannan/identity/crypto.h"
#include "dannan/net/codec.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

using test::id_of;
using test::key_of;
using test::quiet_logger;

uint64_t count_of(const ObjectTrie::Snapshot& view, const ObjectId& id) {
  const auto value = view.get(id);
  if (!value) return 0;
  dannan::net::Reader reader(*value);
  return reader.u64();
}

// Macha: test_reference_counts_follow_and_survive_a_restart. Counted per
// reference, kept across a reopen at their root, a bad change refused whole,
// an interrupted one leaving no root (the next count walks).
TEST(LedgerReferenceCounts, FollowAndSurviveARestart) {
  dannan::test::TempDir dir;
  std::mt19937_64 random(23);
  const auto key = key_of(19);
  const auto a = id_of(random);
  const auto b = id_of(random);
  const auto node = id_of(random);
  const auto root1 = id_of(random);
  const auto root2 = id_of(random);
  {
    ReferenceCounts counts(dir.path() / "referenced", key, 1 << 20, quiet_logger());
    EXPECT_FALSE(counts.root());
    // `a` is shared by two references.
    counts.reset(root1, {a, b, a}, {node}, {3, 3});
    const auto views = counts.views();
    EXPECT_EQ(count_of(views.extents, a), 2u);
    EXPECT_EQ(count_of(views.extents, b), 1u);
    EXPECT_EQ(views.nodes.size(), 1u);
    // One reference to `a` goes, `b` goes: `a` stays, `b` is gone.
    EXPECT_TRUE(counts.change(root2, {}, {a, b}, {}, {}, {1, 1}));
    // The earlier view is frozen.
    EXPECT_EQ(count_of(views.extents, b), 1u);
    // Taking out what is not counted is refused, changing nothing.
    EXPECT_FALSE(counts.change(root1, {}, {b}, {}, {}, {0, 0}));
    EXPECT_EQ(counts.root(), std::optional(root2));
  }
  {
    ReferenceCounts counts(dir.path() / "referenced", key, 1 << 20, quiet_logger());
    EXPECT_EQ(counts.root(), std::optional(root2));
    EXPECT_EQ(counts.totals().entries, 1u);
    const auto views = counts.views();
    EXPECT_EQ(count_of(views.extents, a), 1u);
    EXPECT_FALSE(views.extents.get(b));
    EXPECT_TRUE(views.nodes.get(node).has_value());
  }
  // A change interrupted between its first state write and its last.
  {
    std::ofstream state(dir.path() / "referenced" / "state", std::ios::binary | std::ios::in | std::ios::out);
    state.seekp(8);
    state.put(0);
  }
  ReferenceCounts interrupted(dir.path() / "referenced", key, 1 << 20, quiet_logger());
  EXPECT_FALSE(interrupted.root());
}

// The state file is layout v18's byte for byte, a count is a u64 trie value,
// and a state Macha wrote opens at its root.
TEST(LedgerReferenceCounts, TheStateIsLayoutV18s) {
  const auto root = store::object_id(
      Bytes{'d', 'a', 'g', 'd', 'a', ' ', 'r', 'e', 'f', 'e', 'r', 'e', 'n', 'c', 'e', ' ', 'c', 'o', 'u', 'n', 't', 's', ' ', 'r', 'o', 'o', 't'});
  ASSERT_EQ(dannan::identity::hex(root), "940a17e39138cf491e51a75332cbd7c4cf46d1286f4e7247df8b62b8b9a3b737");
  constexpr char clean[] = "4d524546303030310101940a17e39138cf491e51a75332cbd7c4cf46d1286f4e7247df8b62b8b9a3b73700000000000000010000000000000001";
  dannan::test::TempDir dir;
  ObjectId a{};
  a.bytes[0] = 1;
  {
    ReferenceCounts counts(dir.path(), key_of(3), 1 << 20, quiet_logger());
    counts.reset(root, {a, a}, {}, {1, 1});
    EXPECT_EQ(dannan::identity::hex(counts.views().extents.get(a).value()), "0000000000000002");
  }
  std::ifstream in(dir.path() / "state", std::ios::binary);
  const Bytes written{std::istreambuf_iterator<char>(in), {}};
  EXPECT_EQ(dannan::identity::hex(written), clean);

  dannan::test::TempDir other;
  const auto bytes = dannan::identity::unhex(clean).value();
  std::ofstream(other.path() / "state", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  ReferenceCounts opened(other.path(), key_of(3), 1 << 20, quiet_logger());
  EXPECT_EQ(opened.root(), std::optional(root));
  EXPECT_EQ(opened.totals().entries, 1u);
  EXPECT_EQ(opened.totals().extent_count, 1u);
}

// The node's status timer reads the totals while the census sets them: each read
// is one reset's or one change's pair, never half of each. A race here is what
// ThreadSanitizer reported from the delete stress case; this case shows it there.
TEST(LedgerReferenceCounts, TotalsAreReadWholeWhileTheCensusSetsThem) {
  dannan::test::TempDir dir;
  std::mt19937_64 random(41);
  ReferenceCounts counts(dir.path(), key_of(5), 1 << 20, quiet_logger());
  const auto a = id_of(random);
  const auto root = id_of(random);
  std::atomic_bool done{false};
  uint64_t reads = 0;
  bool torn = false;
  bool fell = false;
  std::thread status([&] {
    uint64_t last = 0;
    while (!done.load()) {
      const auto totals = counts.totals();
      torn = torn || totals.entries != totals.extent_count;
      fell = fell || totals.entries < last;
      last = totals.entries;
      ++reads;
    }
  });
  counts.reset(root, {a}, {}, {1, 1});
  for (uint64_t n = 2; n <= 60; ++n) {
    if (n % 2)
      counts.reset(root, {a}, {}, {n, n});
    else
      ASSERT_TRUE(counts.change(root, {a}, {}, {}, {}, {n, n}));
  }
  done.store(true);
  status.join();
  EXPECT_FALSE(torn);
  EXPECT_FALSE(fell);
  EXPECT_GT(reads, 0u);
  EXPECT_EQ(counts.totals().entries, 60u);
}

}  // namespace
}  // namespace dagda::ledger
