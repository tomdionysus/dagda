//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement L5.
//
// Moved from Macha's tests/test_object_ledger.cpp (develop 5a06d3c):
// test_a_union_of_tries_reads_as_one_trie_of_their_records.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <random>
#include <vector>

#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/trie_diff.h"
#include "dagda/ledger/trie_union.h"
#include "dagda/test/fixtures/ledger.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

using test::clustered_id;
using test::id_of;
using test::prefix_of;
using test::trie_of;

// Macha: test_a_union_of_tries_reads_as_one_trie_of_their_records. Below
// every prefix their union reads as one trie holding their records does,
// however the records are split and shared among the parts, and diffs
// against it as equal. One made from the last after some puts and removals
// reads as one made afresh.
TEST(LedgerTrieUnion, ReadsAsOneTrieOfTheirRecords) {
  std::mt19937_64 random(25);
  const Bytes one_held{0, 0, 0, 0, 0, 0, 0, 1};
  for (const size_t size : {size_t{0}, size_t{90}, size_t{3000}})
    for (const size_t count : {size_t{1}, size_t{2}, size_t{3}}) {
      std::vector<std::map<ObjectId, Bytes>> held(count);
      for (size_t i = 0; i < size; ++i) {
        // On one part, and now and then on another as well.
        const auto id = clustered_id(random);
        const auto at = random() % count;
        held[at].emplace(id, one_held);
        if (random() % 4 == 0) held[(at + 1) % count].emplace(id, one_held);
      }
      std::vector<dannan::test::TempDir> dirs(count);
      std::vector<std::unique_ptr<ObjectTrie>> tries;
      for (size_t i = 0; i < count; ++i) tries.push_back(trie_of(dirs[i].path(), held[i], random));
      const auto snapshots = [&] {
        std::vector<ObjectTrie::Snapshot> out;
        for (const auto& trie : tries) out.push_back(trie->snapshot());
        return out;
      };
      const auto check = [&](const std::shared_ptr<const TrieUnion>& merged) {
        std::map<ObjectId, Bytes> all;
        for (const auto& part : held) all.insert(part.begin(), part.end());
        dannan::test::TempDir whole_dir;
        const auto whole = trie_of(whole_dir.path(), all, random);
        const auto one = whole->snapshot();
        EXPECT_TRUE(merged->root() == (ObjectTrie::Summary{one.size(), one.root_hash()}));
        std::vector<ObjectTrie::Prefix> prefixes{ObjectTrie::Prefix{}};
        for (uint8_t length = 1; length <= 4 && !all.empty(); ++length)
          for (int i = 0; i < 4; ++i) {
            auto at = all.begin();
            std::advance(at, static_cast<long>(random() % all.size()));
            prefixes.push_back(prefix_of(at->first, length));
          }
        prefixes.push_back(prefix_of(id_of(random), 2));
        EXPECT_TRUE(merged->children(prefixes) == one.children(prefixes));
        EXPECT_TRUE(merged->records(prefixes) == one.records(prefixes));
        if (!all.empty()) {
          EXPECT_EQ(merged->get(all.begin()->first), one_held);
          EXPECT_FALSE(merged->get(id_of(random)));
        }
        SnapshotSource a(one);
        UnionSource b(merged);
        size_t differences = 0;
        diff_tries(a, b, [&](const TrieDifference&) { ++differences; });
        EXPECT_EQ(differences, 0u);
      };
      const auto first = std::make_shared<const TrieUnion>(snapshots());
      check(first);

      for (int change = 0; change < 60; ++change) {
        const auto at = random() % count;
        if (random() % 2 == 0 || held[at].empty()) {
          const auto id = clustered_id(random);
          held[at].emplace(id, one_held);
          tries[at]->apply(std::vector<ObjectTrie::Change>{{id, one_held}});
        } else {
          auto victim = held[at].begin();
          std::advance(victim, static_cast<long>(random() % held[at].size()));
          tries[at]->apply(std::vector<ObjectTrie::Change>{{victim->first, std::nullopt}});
          held[at].erase(victim);
        }
      }
      check(std::make_shared<const TrieUnion>(snapshots(), first.get()));
      check(std::make_shared<const TrieUnion>(snapshots()));
    }
}

}  // namespace
}  // namespace dagda::ledger
