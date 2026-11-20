//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement L6, and L7's wire forms (the rounds that carry them are step
// 3's).
//
// Moved from Macha's tests/test_object_ledger.cpp (develop 5a06d3c):
// test_a_diff_finds_exactly_what_differs,
// test_a_diff_costs_what_differs_not_the_size,
// test_records_not_held_are_what_no_held_trie_holds; and from
// tests/test_availability.cpp: test_trie_questions_and_answers_survive_the_wire.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/trie_diff.h"
#include "dagda/test/fixtures/ledger.h"
#include "dannan/encoding/base.h"
#include "dannan/identity/crypto.h"
#include "dannan/net/codec.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

using test::clustered_id;
using test::id_of;
using test::trie_of;
using test::value_of;

std::vector<TrieDifference> diff_of(ObjectTrie& left, ObjectTrie& right, TrieDiffCost* cost = nullptr) {
  SnapshotSource a(left.snapshot());
  SnapshotSource b(right.snapshot());
  // In id order through a multimap rather than std::sort: a duplicate still shows, and
  // g++ 14 at -O3 misreads sort's swap of an optional<Bytes> as reading it uninitialised.
  std::multimap<ObjectId, TrieDifference> by_id;
  const auto spent = diff_tries(a, b, [&](const TrieDifference& d) { by_id.emplace(d.id, d); });
  if (cost) *cost = spent;
  std::vector<TrieDifference> out;
  out.reserve(by_id.size());
  for (auto& [id, d] : by_id) out.push_back(std::move(d));
  return out;
}

std::vector<TrieDifference> model_diff(const std::map<ObjectId, Bytes>& left, const std::map<ObjectId, Bytes>& right) {
  std::map<ObjectId, TrieDifference> out;
  for (const auto& [id, value] : left) out[id] = {id, value, std::nullopt};
  for (const auto& [id, value] : right) {
    auto& entry = out[id];
    entry.id = id;
    entry.right = value;
    if (entry.left == entry.right) out.erase(id);
  }
  std::vector<TrieDifference> flat;
  for (auto& [id, d] : out) flat.push_back(d);
  return flat;
}

// Macha: test_a_diff_finds_exactly_what_differs. The diff reports exactly
// what differs, both ways round, over pairs of every size around a leaf:
// empty, a few, one either side of a leaf's limit, thousands, one side
// crowded below a prefix the other barely holds.
TEST(LedgerTrieDiff, FindsExactlyWhatDiffers) {
  std::mt19937_64 random(22);
  const size_t sizes[] = {0, 1, 40, ObjectTrie::leaf_max, ObjectTrie::leaf_max + 1, 700, 2500};
  for (const auto base_size : sizes)
    for (int variant = 0; variant < 4; ++variant) {
      std::map<ObjectId, Bytes> base;
      while (base.size() < base_size) base.emplace(clustered_id(random), value_of(random));
      auto left = base;
      auto right = base;
      for (auto* side : {&left, &right}) {
        const auto changes = variant == 0 ? 0 : random() % static_cast<uint64_t>(variant * 40 + 1);
        for (size_t i = 0; i < changes; ++i) {
          const auto roll = random() % 3;
          if (roll == 0 || side->empty()) {
            side->emplace(clustered_id(random), value_of(random));
          } else {
            auto at = side->begin();
            std::advance(at, static_cast<long>(random() % side->size()));
            if (roll == 1)
              side->erase(at);
            else
              at->second = value_of(random);
          }
        }
      }
      if (variant == 3) {
        // One side holds hundreds below a prefix the other holds a few of.
        for (int i = 0; i < 400; ++i) {
          auto id = id_of(random);
          id.bytes[0] = 200;
          id.bytes[1] = static_cast<uint8_t>(i % 2);
          left.emplace(id, value_of(random));
        }
      }
      dannan::test::TempDir one;
      dannan::test::TempDir two;
      auto l = trie_of(one.path(), left, random);
      auto r = trie_of(two.path(), right, random);
      const auto expected = model_diff(left, right);
      EXPECT_TRUE(diff_of(*l, *r) == expected);
      auto swapped = expected;
      for (auto& d : swapped) std::swap(d.left, d.right);
      EXPECT_TRUE(diff_of(*r, *l) == swapped);
    }
}

// Macha: test_a_diff_costs_what_differs_not_the_size. Equal tries cost
// nothing, and one record changed among thousands is found in a round per
// level, reading a leaf a side.
TEST(LedgerTrieDiff, CostsWhatDiffersNotTheSize) {
  std::mt19937_64 random(23);
  std::map<ObjectId, Bytes> records;
  while (records.size() < 20000) records.emplace(id_of(random), value_of(random));
  dannan::test::TempDir one;
  dannan::test::TempDir two;
  auto left = trie_of(one.path(), records, random);
  auto right = trie_of(two.path(), records, random);
  TrieDiffCost cost;
  EXPECT_TRUE(diff_of(*left, *right, &cost).empty());
  EXPECT_EQ(cost.rounds, 0u);

  const auto changed = std::next(records.begin(), 777)->first;
  right->apply(std::vector<ObjectTrie::Change>{{changed, Bytes{1, 2, 3}}});
  const auto found = diff_of(*left, *right, &cost);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found.front().id, changed);
  // 20,000 random ids: interior at the root and one level down.
  EXPECT_LE(cost.rounds, 3u);
  EXPECT_LE(cost.prefixes, 3u);
  EXPECT_LE(cost.records, 2 * ObjectTrie::leaf_max);
}

// Macha: test_records_not_held_are_what_no_held_trie_holds. What a wanted
// trie holds that no held trie does, in order, for any number of held
// tries, none included.
TEST(LedgerTrieDiff, RecordsNotHeldAreWhatNoHeldTrieHolds) {
  std::mt19937_64 random(24);
  for (const size_t wanted_size : {size_t{0}, size_t{5}, size_t{3000}})
    for (const size_t held_count : {size_t{0}, size_t{1}, size_t{3}}) {
      std::map<ObjectId, Bytes> wanted;
      while (wanted.size() < wanted_size) wanted.emplace(clustered_id(random), value_of(random));
      std::vector<std::map<ObjectId, Bytes>> held(held_count);
      for (auto& set : held) {
        for (const auto& [id, value] : wanted)
          if (random() % 3 == 0) set.emplace(id, Bytes{1});
        for (int i = 0; i < 500; ++i) set.emplace(clustered_id(random), Bytes{1});
      }
      std::vector<ObjectTrie::Record> expected;
      for (const auto& record : wanted)
        if (std::none_of(held.begin(), held.end(), [&](const auto& set) { return set.contains(record.first); })) expected.push_back(record);

      dannan::test::TempDir wanted_dir;
      auto wanted_trie = trie_of(wanted_dir.path(), wanted, random);
      std::vector<dannan::test::TempDir> dirs(held_count);
      std::vector<std::unique_ptr<ObjectTrie>> tries;
      std::vector<ObjectTrie::Snapshot> snapshots;
      for (size_t i = 0; i < held_count; ++i) {
        tries.push_back(trie_of(dirs[i].path(), held[i], random));
        snapshots.push_back(tries.back()->snapshot());
      }
      std::vector<ObjectTrie::Record> found;
      records_not_held(wanted_trie->snapshot(), snapshots, [&](const ObjectTrie::Record& record) { found.push_back(record); });
      EXPECT_TRUE(found == expected);
    }
}

ObjectId numbered_id(uint32_t n) {
  ObjectId id;
  for (size_t i = 0; i < 4; ++i) id.bytes[i] = static_cast<uint8_t>(n >> (8 * (3 - i)));
  id.bytes[31] = 1;
  return id;
}

// Macha (test_availability.cpp): test_trie_questions_and_answers_survive_the_wire.
// A question and every kind of answer survive the wire, and a malformed
// one is refused.
TEST(LedgerTrieDiff, QuestionsAndAnswersSurviveTheWire) {
  TrieQuestion question;
  question.kind = TrieQuestionKind::children;
  question.at.bytes.fill(7);
  for (uint8_t length = 0; length < 5; ++length) {
    ObjectTrie::Prefix prefix;
    prefix.length = length;
    for (uint8_t i = 0; i < length; ++i) prefix.bytes[i] = static_cast<uint8_t>(i * 31 + 1);
    question.prefixes.push_back(prefix);
  }
  EXPECT_TRUE(decode_trie_question(encode_trie_question(question)) == question);

  const ObjectTrie::Summary root{12345, question.at};
  EXPECT_TRUE(decode_trie_root(encode_trie_root(root)) == root);

  std::vector<ObjectTrie::Children> children(3);
  children[1][0] = {5, question.at};
  children[1][255] = {9, Hash256{}};
  children[2][17] = {1, question.at};
  EXPECT_TRUE(decode_trie_children(encode_trie_children(children)) == children);

  std::vector<std::vector<ObjectTrie::Record>> records(2);
  records[1] = {{numbered_id(1), Bytes{1}}, {numbered_id(2), Bytes{}}};
  EXPECT_TRUE(decode_trie_records(encode_trie_records(records)) == records);

  const auto refused = [](auto decode, Bytes bytes) {
    try {
      (void)decode(bytes);
    } catch (const dannan::net::DecodeError&) {
      return true;
    }
    return false;
  };
  auto too_long = encode_trie_question(question);
  // The first prefix's length.
  too_long[2 + 32 + 4] = 33;
  EXPECT_TRUE(refused(decode_trie_question, too_long));
  std::reverse(records[1].begin(), records[1].end());
  EXPECT_TRUE(refused(decode_trie_records, encode_trie_records(records)));
  auto whole = question;
  whole.prefixes = {ObjectTrie::Prefix{{}, 32}};
  EXPECT_TRUE(refused(decode_trie_question, encode_trie_question(whole)));

  // A records question carries more prefixes than a children question.
  TrieQuestion many;
  many.kind = TrieQuestionKind::records;
  many.prefixes.assign(trie_records_max, ObjectTrie::Prefix{{}, 2});
  EXPECT_TRUE(decode_trie_question(encode_trie_question(many)) == many);
  many.kind = TrieQuestionKind::children;
  EXPECT_THROW((void)encode_trie_question(many), std::invalid_argument);
}

// Protocol 23's bytes for each form, from trie_diff.cpp:176-283 at develop
// 5a06d3c: kind, trie, the pinned hash, a u32 count and each prefix as its
// length then its bytes; a root as u64 count then hash; children as a u32
// count of answers, each a u16 count of non-empty slots then (slot, u64
// count, hash) each; records as a u32 count of answers, each a u32 count
// then (id, u32 length, value) each.
TEST(LedgerTrieDiff, WireFormsAreMachas) {
  using dannan::encoding::hex;
  TrieQuestion question;
  question.kind = TrieQuestionKind::records;
  question.at.bytes.fill(0xab);
  ObjectTrie::Prefix prefix;
  prefix.length = 2;
  prefix.bytes[0] = 0x12;
  prefix.bytes[1] = 0x34;
  question.prefixes = {ObjectTrie::Prefix{}, prefix};
  EXPECT_EQ(hex(encode_trie_question(question)), std::string("02") + "00" + hex(question.at.bytes) + "00000002" + "00" + "021234");

  Hash256 hash;
  hash.bytes.fill(0x01);
  EXPECT_EQ(hex(encode_trie_root({0x0102, hash})), "0000000000000102" + hex(hash.bytes));

  std::vector<ObjectTrie::Children> children(1);
  children[0][3] = {7, hash};
  EXPECT_EQ(hex(encode_trie_children(children)), "00000001" + std::string("0001") + "03" + "0000000000000007" + hex(hash.bytes));

  std::vector<std::vector<ObjectTrie::Record>> records(1);
  records[0] = {{numbered_id(1), Bytes{0xee}}};
  EXPECT_EQ(hex(encode_trie_records(records)), "00000001" + std::string("00000001") + hex(numbered_id(1).bytes) + "00000001" + "ee");
}

}  // namespace
}  // namespace dagda::ledger
