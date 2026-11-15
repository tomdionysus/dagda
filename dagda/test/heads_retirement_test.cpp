//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Retirement batches (H23). The object's bytes against Macha's layout, its magic a
// parameter, and what reading and rewriting a head's batches do.

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/heads/retirement.h"

namespace dagda::heads {
namespace {

constexpr Magic magic{'T', 'E', 'S', 'T', 'B', 'T', 'C', '1'};

ObjectId object(uint8_t fill) {
  ObjectId id{};
  id.bytes.fill(fill);
  return id;
}

std::vector<ObjectId> sorted(std::initializer_list<uint8_t> fills) {
  std::vector<ObjectId> out;
  for (const auto fill : fills) out.push_back(object(fill));
  std::sort(out.begin(), out.end());
  return out;
}

// md.cpp's encode_tombstone_batch: magic, i64be, u32be, the ids.
TEST(HeadsRetirement, ABatchIsMachasLayout) {
  const auto bytes = encode_retirement_batch(magic, 0x0102030405060708, sorted({1, 2}));
  Bytes expected(magic.begin(), magic.end());
  for (const auto byte : Bytes{1, 2, 3, 4, 5, 6, 7, 8}) expected.push_back(byte);
  for (const auto byte : Bytes{0, 0, 0, 2}) expected.push_back(byte);
  for (const auto fill : Bytes{1, 2}) expected.insert(expected.end(), 32, fill);
  EXPECT_EQ(bytes, expected);

  const auto opened = decode_retirement_batch(magic, bytes);
  EXPECT_EQ(opened.retired_at_ns, 0x0102030405060708);
  EXPECT_EQ(opened.ids, sorted({1, 2}));
}

TEST(HeadsRetirement, WhatIsNotABatchIsRefused) {
  EXPECT_THROW((void)encode_retirement_batch(magic, 1, {}), std::invalid_argument);
  EXPECT_THROW((void)encode_retirement_batch(magic, -1, sorted({1})), std::invalid_argument);
  EXPECT_THROW((void)encode_retirement_batch(magic, 1, std::vector<ObjectId>{object(2), object(1)}), std::invalid_argument);
  EXPECT_THROW((void)encode_retirement_batch(magic, 1, std::vector<ObjectId>{object(1), object(1)}), std::invalid_argument);

  const auto good = encode_retirement_batch(magic, 1, sorted({1, 2}));
  auto other = good;
  other[0] ^= 0xff;
  EXPECT_THROW((void)decode_retirement_batch(magic, other), DecodeError);
  auto trailing = good;
  trailing.push_back(0);
  EXPECT_THROW((void)decode_retirement_batch(magic, trailing), DecodeError);
  EXPECT_THROW((void)decode_retirement_batch(magic, std::span<const uint8_t>(good).first(good.size() - 1)), DecodeError);
  // Out of order on the wire.
  auto swapped = good;
  std::swap_ranges(swapped.end() - 64, swapped.end() - 32, swapped.end() - 32);
  EXPECT_THROW((void)decode_retirement_batch(magic, swapped), DecodeError);
}

// mgr:1776-1794: sorted and unique, none left is no batch, a list stays sorted by id
// and names a batch once.
TEST(HeadsRetirement, ABatchIsWrittenSortedAndNamedOnce) {
  MemoryTreeNodeStore store;
  EXPECT_FALSE(write_retirement_batch(store, magic, 5, {}));
  EXPECT_EQ(store.nodes(), 0U);
  const auto batch = write_retirement_batch(store, magic, 5, {object(2), object(1), object(2)});
  ASSERT_TRUE(batch);
  EXPECT_EQ(batch->retired_at_ns, 5);
  EXPECT_EQ(batch->count, 2U);
  EXPECT_EQ(decode_retirement_batch(magic, *store.get(batch->id)).ids, sorted({1, 2}));
  const auto other = write_retirement_batch(store, magic, 6, {object(3)});
  std::vector<GarbageBatch> list;
  insert_retirement_batch(list, *other);
  insert_retirement_batch(list, *batch);
  insert_retirement_batch(list, *batch);
  ASSERT_EQ(list.size(), 2U);
  EXPECT_LT(list[0].id, list[1].id);
}

// md.cpp's tombstones_of. The latest retirement per object, each with its batch as its
// token, and one that cannot be read or does not match its name is counted and left out.
TEST(HeadsRetirement, ReadingBatchesTakesTheLatestAndCountsWhatItCannot) {
  MemoryTreeNodeStore store;
  const auto early = *write_retirement_batch(store, magic, 5, {object(1), object(2)});
  const auto late = *write_retirement_batch(store, magic, 9, {object(2), object(3)});
  auto wrong_count = *write_retirement_batch(store, magic, 3, {object(4)});
  wrong_count.count = 7;
  GarbageBatch missing;
  missing.id = object(9);
  missing.retired_at_ns = 1;
  missing.count = 1;
  const std::vector<GarbageBatch> batches{early, late, wrong_count, missing};
  size_t unreadable = 0;
  const auto all = retirements_of(batches, store, magic, &unreadable);
  EXPECT_EQ(unreadable, 2U);
  ASSERT_EQ(all.size(), 3U);
  EXPECT_EQ(all[0].id, object(1));
  EXPECT_EQ(all[0].retired_at_ns, 5);
  EXPECT_EQ(all[1].id, object(2));
  EXPECT_EQ(all[1].retired_at_ns, 9);
  EXPECT_TRUE(std::equal(all[1].token.bytes.begin(), all[1].token.bytes.end(), late.id.bytes.begin()));
  EXPECT_EQ(all[2].id, object(3));

  const TreePause stop = [] { throw contract::MaintenanceStopping(); };
  EXPECT_THROW((void)retirements_of(batches, store, magic, nullptr, stop), contract::MaintenanceStopping);
}

// maintenance.cpp:174-218. A batch holding one to erase is dropped and the rest written
// again at its own time. A later retirement of the same object is another batch and stays,
// and so does a batch that cannot be read.
TEST(HeadsRetirement, RewritingDropsABatchAndKeepsTheRestOfIt) {
  MemoryTreeNodeStore store;
  const auto holding = *write_retirement_batch(store, magic, 5, {object(1), object(2)});
  const auto whole = *write_retirement_batch(store, magic, 5, {object(3)});
  const auto later = *write_retirement_batch(store, magic, 9, {object(1)});
  const auto alone = *write_retirement_batch(store, magic, 4, {object(7)});
  GarbageBatch missing;
  missing.id = object(9);
  missing.count = 1;
  std::vector<GarbageBatch> batches{holding, whole, later, alone, missing};
  const std::vector<Garbage> erase{{object(1), 5, {}}, {object(7), 4, {}}};
  const auto rewrite = rewrite_retirements(batches, store, magic, erase);
  ASSERT_EQ(rewrite.dropped.size(), 2U);
  EXPECT_TRUE(std::is_sorted(rewrite.dropped.begin(), rewrite.dropped.end()));
  EXPECT_NE(std::find(rewrite.dropped.begin(), rewrite.dropped.end(), holding.id), rewrite.dropped.end());
  EXPECT_NE(std::find(rewrite.dropped.begin(), rewrite.dropped.end(), alone.id), rewrite.dropped.end());
  ASSERT_EQ(rewrite.rest.size(), 1U);
  EXPECT_EQ(rewrite.rest[0].retired_at_ns, 5);
  EXPECT_EQ(rewrite.rest[0].ids, sorted({2}));
  EXPECT_EQ(rewrite.kept.size(), 3U);
}

}  // namespace
}  // namespace dagda::heads
