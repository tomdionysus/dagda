//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement L3 through a store: the held ledger is kept current with
// every put and removal, is the answer once seeded, and is checked against
// the disk slice by slice.
//
// Moved from Macha's tests/test_object_ledger.cpp (develop 5a06d3c), the
// four cases 1b left for the store, every assertion kept:
// test_a_store_with_its_ledger_starts_knowing_what_it_holds,
// test_verification_corrects_the_ledger_from_the_disk,
// test_the_ledger_lists_what_the_packs_hold,
// test_verification_resumes_after_a_restart. Macha's stores there keep
// loose objects only (LocalStoreOptions' zero defaults); Dagda's defaults
// pack, so those cases ask for loose objects by name.

#include <gtest/gtest.h>

#include <vector>

#include "dagda/store/local_store.h"
#include "dagda/test/fixtures/store.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using test::pattern;
using test::StoreBed;
using test::wait_until;

LocalStoreOptions ledgered(const std::filesystem::path& ledger) {
  auto options = LocalStoreOptions::loose_only(64ULL * 1024 * 1024);
  options.ledger_dir = ledger;
  options.ledger_cache_bytes = 1024 * 1024;
  return options;
}

// The first start's walk has seeded the ledger. Macha waited for the
// `seeded` file alone, which the ledger writes just before it answers: a
// check begun in between is a no-op (seen about once in forty runs under
// load), so the store's own answer is awaited too.
bool seeded(const LocalStore& store, const std::filesystem::path& ledger) {
  return wait_until([&] { return std::filesystem::exists(ledger / "seeded") && store.held_view().identity.complete; });
}

// Macha: test_a_store_with_its_ledger_starts_knowing_what_it_holds.
TEST(StoreLocalStoreLedger, AStoreWithItsLedgerStartsKnowingWhatItHolds) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  const auto ledger = bed.dir.path() / "ledger";
  bed.key = test::key_of(14);
  std::vector<ObjectId> kept;
  std::vector<ObjectId> removed;
  {
    auto store = bed.open(root, ledgered(ledger));
    // The first start walks its objects once and seeds the ledger.
    ASSERT_TRUE(seeded(*store, ledger));
    for (int i = 0; i < 40; ++i) {
      const auto data = pattern(4096 + i, static_cast<uint8_t>(i));
      const auto id = object_id(data);
      ASSERT_TRUE(store->put(id, data));
      (i % 4 == 0 ? removed : kept).push_back(id);
    }
    for (const auto& id : removed) ASSERT_TRUE(store->remove(id));
  }
  // A file the ledger never heard of, as a crash between a file and its
  // record would leave: written by a store keeping no ledger.
  const auto unlisted_data = pattern(7777, 99);
  const auto unlisted = object_id(unlisted_data);
  {
    auto bare = bed.open(root, LocalStoreOptions::loose_only(64ULL * 1024 * 1024));
    ASSERT_TRUE(bare->put(unlisted, unlisted_data));
  }
  {
    auto store = bed.open(root, ledgered(ledger));
    // Known at once: no walk.
    EXPECT_TRUE(store->held_view().identity.complete);
    for (const auto& id : kept) EXPECT_TRUE(store->has(id));
    for (const auto& id : removed) EXPECT_FALSE(store->has(id));
    // The ledger is the answer, so the unlisted file is not seen, which is
    // also how this test knows nothing walked the store...
    EXPECT_FALSE(store->has(unlisted));
    // ...until a read finds it, which records it.
    EXPECT_EQ(store->get(unlisted), std::optional<Bytes>(unlisted_data));
    EXPECT_TRUE(store->has(unlisted));
  }
  auto again = bed.open(root, ledgered(ledger));
  EXPECT_TRUE(again->held_view().identity.complete);
  EXPECT_TRUE(again->has(unlisted));
  for (const auto& id : kept) EXPECT_TRUE(again->has(id));
}

// Macha: test_verification_corrects_the_ledger_from_the_disk. The check
// corrects the ledger both ways: a file it does not list is recorded held,
// and a listed object whose file is gone is recorded lost.
TEST(StoreLocalStoreLedger, VerificationCorrectsTheLedgerFromTheDisk) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  const auto ledger = bed.dir.path() / "ledger";
  bed.key = test::key_of(15);
  std::vector<ObjectId> held;
  {
    auto store = bed.open(root, ledgered(ledger));
    ASSERT_TRUE(seeded(*store, ledger));
    for (int i = 0; i < 30; ++i) {
      const auto data = pattern(4096 + i, static_cast<uint8_t>(i));
      held.push_back(object_id(data));
      ASSERT_TRUE(store->put(held.back(), data));
    }
  }
  const auto unlisted_data = pattern(5555, 77);
  const auto unlisted = object_id(unlisted_data);
  {
    auto bare = bed.open(root, LocalStoreOptions::loose_only(64ULL * 1024 * 1024));
    ASSERT_TRUE(bare->put(unlisted, unlisted_data));
  }
  auto store = bed.open(root, ledgered(ledger));
  const auto gone = held.front();
  ASSERT_TRUE(std::filesystem::remove(store->object_path(gone)));
  EXPECT_TRUE(store->has(gone));
  EXPECT_FALSE(store->has(unlisted));
  const auto losses = store->losses();

  // Slice by slice until a pass completes.
  LocalStore::VerifyResult total;
  size_t steps = 0;
  while (!total.complete) {
    const auto step = store->verify_step(4096);
    total.recorded += step.recorded;
    total.lost += step.lost;
    total.complete = step.complete;
    ASSERT_LE(++steps, 16u);
  }
  EXPECT_EQ(steps, 16u);
  EXPECT_EQ(total.recorded, 1u);
  EXPECT_EQ(total.lost, 1u);
  EXPECT_TRUE(store->has(unlisted));
  EXPECT_FALSE(store->has(gone));
  EXPECT_EQ(store->losses(), losses + 1);
  for (size_t i = 1; i < held.size(); ++i) EXPECT_TRUE(store->has(held[i]));
  // Nothing left to correct.
  const auto again = store->verify_step(65536);
  EXPECT_TRUE(again.complete);
  EXPECT_EQ(again.recorded, 0u);
  EXPECT_EQ(again.lost, 0u);
}

// Macha: test_the_ledger_lists_what_the_packs_hold. Packed objects are held
// like any other: listed by every put and removal, by the next open when a
// store keeping no ledger packed them, and never recorded lost by the check
// for having no file of their own.
TEST(StoreLocalStoreLedger, TheLedgerListsWhatThePacksHold) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  const auto ledger = bed.dir.path() / "ledger";
  bed.key = test::key_of(16);
  auto packing = ledgered(ledger);
  packing.pack_threshold = 64 * 1024;
  packing.pack_target_size = 1024 * 1024;
  const auto small = [](int i) { return pattern(1000 + i, static_cast<uint8_t>(i)); };
  const auto kept = object_id(small(1));
  const auto dropped = object_id(small(2));
  {
    auto store = bed.open(root, packing);
    ASSERT_TRUE(wait_until([&] { return store->held_view().identity.complete; }));
    ASSERT_TRUE(store->put(kept, small(1)));
    ASSERT_TRUE(store->put(dropped, small(2)));
    ASSERT_EQ(store->object_path(kept).parent_path(), root / "packs");
    const auto view = store->held_view();
    EXPECT_TRUE(view.held(kept));
    EXPECT_TRUE(view.held(dropped));
    ASSERT_TRUE(store->remove(dropped));
    EXPECT_FALSE(store->held_view().held(dropped));
  }
  const auto unlisted = object_id(small(3));
  {
    auto bare = packing;
    bare.ledger_dir.clear();
    auto store = bed.open(root, bare);
    ASSERT_TRUE(store->put(unlisted, small(3)));
  }
  auto store = bed.open(root, packing);
  const auto view = store->held_view();
  EXPECT_TRUE(view.identity.complete);
  EXPECT_TRUE(view.held(kept));
  EXPECT_TRUE(view.held(unlisted));
  EXPECT_FALSE(view.held(dropped));

  LocalStore::VerifyResult total;
  while (!total.complete) {
    const auto step = store->verify_step(65536);
    total.lost += step.lost;
    total.complete = step.complete;
  }
  EXPECT_EQ(total.lost, 0u);
  EXPECT_EQ(store->held_view().identity, view.identity);
}

// Macha: test_verification_resumes_after_a_restart.
TEST(StoreLocalStoreLedger, VerificationResumesAfterARestart) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  const auto ledger = bed.dir.path() / "ledger";
  bed.key = test::key_of(17);
  {
    auto store = bed.open(root, ledgered(ledger));
    ASSERT_TRUE(seeded(*store, ledger));
    EXPECT_EQ(store->verify_step(100).directories, 100u);
  }
  auto store = bed.open(root, ledgered(ledger));
  const auto rest = store->verify_step(65536);
  EXPECT_TRUE(rest.complete);
  EXPECT_EQ(rest.directories, 65536u - 100);
}

}  // namespace
}  // namespace dagda::store
