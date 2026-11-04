//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement S11: CONTROL is a store of its own beside the DATA pool, with
// its own quota, so DATA exhaustion cannot block a CONTROL write.
//
// Moved from Macha's tests/test_storage_v18.cpp (develop 5a06d3c), every
// assertion kept: test_metadata_control_store_is_independent_of_data_quota,
// which Macha ran on a whole test node; the separate budget is the store's
// wiring of the control store beside the pool, made here as a node's local
// state makes it.

#include "dagda/test/fixtures/store.h"

#include <gtest/gtest.h>

#include <limits>
#include <optional>

#include "dagda/store/control_store.h"
#include "dagda/store/object_id.h"
#include "dagda/store/storage_pool.h"

namespace dagda::store {
namespace {

using test::pattern;
using test::StoreBed;

// Macha: test_metadata_control_store_is_independent_of_data_quota.
TEST(StoreControlStore, AControlWriteIsNotBlockedByDataExhaustion) {
  StoreBed bed;
  const auto state = bed.dir.path() / "state";
  const auto data = bed.dir.path() / "data";
  std::filesystem::create_directories(data);
  auto pool = bed.pool(state, {{data, 512ULL * 1024, 0}});
  ControlStoreOptions control_options;
  control_options.path = bed.dir.path() / "control";
  control_options.limit = 8ULL * 1024 * 1024;
  auto control = open_control_store(state, control_options, bed.key, bed.node_local, bed.services());

  // Exhaust ordinary DATA admission.
  size_t stored = 0;
  for (size_t i = 0; i < 16; ++i) {
    auto bytes = pattern(128 * 1024 + i);
    bytes[0] ^= static_cast<uint8_t>(i);
    if (!pool->put(object_id(bytes), bytes)) break;
    ++stored;
  }
  EXPECT_GT(stored, 0u);
  const auto extra = pattern(256 * 1024);
  EXPECT_FALSE(pool->put(object_id(extra), extra));

  // Control-plane durability still has its own admission budget.
  const auto bytes = pattern(128 * 1024 + 17);
  const auto id = object_id(bytes);
  ASSERT_TRUE(control->put(id, bytes));
  EXPECT_EQ(control->get(id), std::optional<Bytes>{bytes});
  EXPECT_FALSE(pool->has(id));
}

// Macha's local state's placement and defaults: <state>/metadata-objects
// unless a path is given, a held ledger at <state>/ledger/control, 4 GiB,
// and objects at or below 1 MiB packed; and, over Macha, a 256 MiB
// free-space reserve (the maintainer, 2026-11-04).
TEST(StoreControlStore, TheControlStoreTakesMachasPlaceAndDefaults) {
  StoreBed bed;
  const auto state = bed.dir.path() / "state";
  const ControlStoreOptions defaults;
  EXPECT_EQ(defaults.limit, 4ULL * 1024 * 1024 * 1024);
  EXPECT_EQ(defaults.reserve_free, 256ULL * 1024 * 1024);
  EXPECT_EQ(control_store_path(state, defaults), state / "metadata-objects");
  EXPECT_EQ(ledger_cache_share(default_ledger_cache_bytes, LedgerShare::control_held), 4ULL * 1024 * 1024);
  EXPECT_EQ(ledger_cache_share(default_ledger_cache_bytes, LedgerShare::claims), 24ULL * 1024 * 1024);
  EXPECT_EQ(ledger_cache_share(default_ledger_cache_bytes, LedgerShare::references), 16ULL * 1024 * 1024);
  EXPECT_EQ(ledger_cache_share(default_ledger_cache_bytes, LedgerShare::data_held), 20ULL * 1024 * 1024);

  auto control = open_control_store(state, defaults, bed.key, bed.node_local, bed.services());
  EXPECT_EQ(control->limit(), defaults.limit);
  const auto small = pattern(64 * 1024, 3);
  ASSERT_TRUE(control->put(object_id(small), small));
  EXPECT_TRUE(control->is_packed(object_id(small)));
  EXPECT_TRUE(std::filesystem::exists(state / "metadata-objects" / ".macha.accounting"));
  // The held ledger lists it from the moment it says it is seeded.
  ASSERT_TRUE(test::wait_until([&] { return control->held_view().identity.complete; }));
  EXPECT_TRUE(control->held_view().held(object_id(small)));
  EXPECT_TRUE(std::filesystem::is_directory(state / "ledger" / "control"));
}

// What Macha lacks (requirements section 7): a free-space reserve for the
// control store, which stops a CONTROL put before the device is full.
TEST(StoreControlStore, AControlReserveStopsAWriteBeforeTheDeviceIsFull) {
  StoreBed bed;
  ControlStoreOptions options;
  options.reserve_free = std::numeric_limits<uint64_t>::max();
  auto control = open_control_store(bed.dir.path() / "state", options, bed.key, bed.node_local, bed.services());
  const auto bytes = pattern(64 * 1024, 4);
  EXPECT_FALSE(control->put(object_id(bytes), bytes));
  EXPECT_FALSE(control->has(object_id(bytes)));
}

}  // namespace
}  // namespace dagda::store
