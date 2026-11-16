//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement L8: the survey by diff, as primitives over trie sources.
//
// Moved from Macha's tests/test_availability.cpp (develop 5a06d3c), every
// assertion kept: test_a_survey_a_peer_could_not_finish_keeps_what_was_unavailable,
// test_the_survey_by_diff_finds_what_no_node_holds,
// test_what_this_node_lacks_is_the_referenced_set_less_every_disk. The
// path-table cases there are Macha's (its namespace and files API).

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "dagda/contract/horizon.h"
#include "dagda/ledger/availability.h"
#include "dagda/test/fixtures/ledger.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

// Macha's extent_id: a deterministic id from a seed.
ObjectId extent_id(uint64_t seed) {
  ObjectId id{};
  for (auto& byte : id.bytes) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    byte = static_cast<uint8_t>(seed >> 33);
  }
  return id;
}

ObjectId numbered_id(uint32_t n) {
  ObjectId id;
  for (size_t i = 0; i < 4; ++i) id.bytes[i] = static_cast<uint8_t>(n >> (8 * (3 - i)));
  id.bytes[31] = 1;
  return id;
}

HeldFn holds(const std::set<ObjectId>& ids) {
  return [&ids](const ObjectId& id) { return ids.contains(id); };
}

// A held trie over `ids`, as a disk's held ledger keeps one: ids with empty
// values.
std::unique_ptr<ObjectTrie> held_trie(const std::filesystem::path& dir, const std::set<ObjectId>& ids) {
  std::mt19937_64 random(ids.size());
  std::map<ObjectId, Bytes> records;
  for (const auto& id : ids) records.emplace(id, Bytes{});
  return test::trie_of(dir, records, random);
}

// Macha: test_a_survey_a_peer_could_not_finish_keeps_what_was_unavailable.
TEST(LedgerAvailability, ASurveyAPeerCouldNotFinishKeepsWhatWasUnavailable) {
  AvailabilitySurvey last;
  last.unavailable = {extent_id(1), extent_id(2), extent_id(3)};
  std::sort(last.unavailable.begin(), last.unavailable.end());
  AvailabilitySurvey next;
  next.unavailable = {extent_id(4)};
  next.unknown = {extent_id(1), extent_id(5)};
  std::sort(next.unknown.begin(), next.unknown.end());
  // This node has since gained the second.
  const std::set<ObjectId> gained{extent_id(2)};
  keep_unavailable(next, last, holds(gained));
  std::vector<ObjectId> unavailable{extent_id(1), extent_id(3), extent_id(4)};
  std::sort(unavailable.begin(), unavailable.end());
  EXPECT_EQ(next.unavailable, unavailable);
  EXPECT_EQ(next.unknown, std::vector<ObjectId>{extent_id(5)});
}

// One peer's trie as a source that fails after its root.
class FailingSource final : public TrieSource {
 public:
  explicit FailingSource(ObjectTrie::Snapshot trie) : _inner(std::move(trie)) {}
  ObjectTrie::Summary root() override { return _inner.root(); }
  std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix>) override { throw std::runtime_error("peer went away"); }
  std::vector<std::vector<ObjectTrie::Record>> records(std::span<const ObjectTrie::Prefix>) override { throw std::runtime_error("peer went away"); }

 private:
  SnapshotSource _inner;
};

// Macha: test_the_survey_by_diff_finds_what_no_node_holds. From diffs: what
// no peer holds of what this node lacks is unavailable, and each peer lacks
// what this node holds and it does not, within what is referenced, on
// however many disks either side holds them.
TEST(LedgerAvailability, TheSurveyByDiffFindsWhatNoNodeHolds) {
  std::set<ObjectId> referenced;
  for (uint32_t i = 0; i < 3000; ++i) referenced.insert(numbered_id(i * 7919));
  const auto pick = [&](auto keep) {
    std::set<ObjectId> out;
    size_t i = 0;
    for (const auto& id : referenced)
      if (keep(i++)) out.insert(id);
    return out;
  };
  auto mine = pick([](size_t i) { return i % 3 != 0; });
  const auto first = pick([](size_t i) { return i % 5 != 0; });
  const auto second = pick([](size_t i) { return i % 2 == 0; });
  // Garbage this node holds that nothing refers to.
  mine.insert(numbered_id(0xfffffff0));

  std::vector<ObjectId> missing_here;
  for (const auto& id : referenced)
    if (!mine.contains(id)) missing_here.push_back(id);
  std::vector<ObjectId> unavailable;
  for (const auto& id : missing_here)
    if (!first.contains(id) && !second.contains(id)) unavailable.push_back(id);
  const auto lacking = [&](const std::set<ObjectId>& peer) {
    std::vector<ObjectId> out;
    for (const auto& id : mine)
      if (referenced.contains(id) && !peer.contains(id)) out.push_back(id);
    return out;
  };

  // This node and the first peer each hold on two disks, some ids on both.
  const auto split = [](const std::set<ObjectId>& ids, size_t part) {
    std::set<ObjectId> out;
    size_t i = 0;
    for (const auto& id : ids)
      if (i++ % 3 != part) out.insert(id);
    return out;
  };
  dannan::test::TempDir dir;
  auto here_a = held_trie(dir.path() / "here-a", split(mine, 0));
  auto here_b = held_trie(dir.path() / "here-b", split(mine, 1));
  const auto here = std::make_shared<const TrieUnion>(std::vector{here_a->snapshot(), here_b->snapshot()});
  auto one_a = held_trie(dir.path() / "one-a", split(first, 0));
  auto one_b = held_trie(dir.path() / "one-b", split(first, 2));
  auto two = held_trie(dir.path() / "two", second);
  UnionSource one_source(std::make_shared<const TrieUnion>(std::vector{one_a->snapshot(), one_b->snapshot()}));
  SnapshotSource two_source(two->snapshot());
  std::vector<TrieSource*> peers{&one_source, &two_source};
  const std::vector<ObjectId> referenced_list(referenced.begin(), referenced.end());
  std::vector<PeerDiff> diffs;
  const auto survey = survey_by_diff(here, missing_here, contract::IdLookup(referenced_list), peers, diffs);
  EXPECT_EQ(survey.unavailable, unavailable);
  EXPECT_TRUE(survey.unknown.empty());
  EXPECT_EQ(survey.peers_asked, 2u);
  EXPECT_EQ(survey.peers_failed, 0u);
  ASSERT_EQ(diffs.size(), 2u);
  EXPECT_TRUE(diffs[0].answered);
  EXPECT_EQ(diffs[0].lacks, lacking(first));
  EXPECT_EQ(diffs[1].lacks, lacking(second));
  EXPECT_EQ(diffs[1].root, two->snapshot().root_hash());

  // Alone, nothing anyone else holds: everything missing is unavailable.
  std::vector<TrieSource*> none;
  EXPECT_EQ(survey_by_diff(here, missing_here, contract::IdLookup(referenced_list), none, diffs).unavailable, missing_here);

  // A peer that fails mid-diff: what the other does not hold might be its,
  // so it is unknown, and the other's lacks still stand.
  FailingSource failing(two->snapshot());
  std::vector<TrieSource*> broken{&one_source, &failing};
  const auto partial = survey_by_diff(here, missing_here, contract::IdLookup(referenced_list), broken, diffs);
  std::vector<ObjectId> not_first;
  for (const auto& id : missing_here)
    if (!first.contains(id)) not_first.push_back(id);
  EXPECT_EQ(partial.peers_failed, 1u);
  EXPECT_TRUE(partial.unavailable.empty());
  EXPECT_EQ(partial.unknown, not_first);
  EXPECT_TRUE(diffs[0].answered);
  EXPECT_EQ(diffs[0].lacks, lacking(first));
  EXPECT_FALSE(diffs[1].answered);
  EXPECT_EQ(diffs[1].why, "peer went away");
}

// Macha: test_what_this_node_lacks_is_the_referenced_set_less_every_disk.
// What this node lacks: the referenced set, a trie and a list beside it, less
// what any of its disks holds, for none to three disks.
TEST(LedgerAvailability, WhatThisNodeLacksIsTheReferencedSetLessEveryDisk) {
  std::set<ObjectId> viewed;
  for (uint32_t i = 0; i < 2000; ++i) viewed.insert(numbered_id(i * 104729));
  // Beside the view: some ids of its own, one the view has as well.
  const std::vector<ObjectId> others{numbered_id(7), numbered_id(11), *viewed.begin()};
  std::set<ObjectId> referenced(viewed);
  referenced.insert(others.begin(), others.end());
  dannan::test::TempDir dir;
  auto view = held_trie(dir.path() / "view", viewed);
  const contract::ReferencedSets sets(contract::ReferencedSets::Views{view->snapshot(), std::nullopt}, others, {});
  ASSERT_EQ(sets.size(contract::RetentionClass::data), referenced.size());

  std::vector<std::unique_ptr<ObjectTrie>> disks;
  std::vector<std::set<ObjectId>> held;
  for (size_t count = 0; count <= 3; ++count) {
    std::vector<ObjectTrie::Snapshot> parts;
    for (const auto& disk : disks) parts.push_back(disk->snapshot());
    std::vector<ObjectId> expected;
    for (const auto& id : referenced)
      if (std::none_of(held.begin(), held.end(), [&](const std::set<ObjectId>& on) { return on.contains(id); })) expected.push_back(id);
    EXPECT_EQ(sets.not_held(contract::RetentionClass::data, parts), expected) << count << " disks";
    // The next disk holds every few referenced ids, one of the others, and
    // garbage nothing refers to.
    std::set<ObjectId> next;
    size_t i = 0;
    for (const auto& id : referenced)
      if (i++ % (count + 2) == count) next.insert(id);
    next.insert(others[count % 2]);
    next.insert(numbered_id(0xfffffff0 - static_cast<uint32_t>(count)));
    disks.push_back(held_trie(dir.path() / ("disk" + std::to_string(count)), next));
    held.push_back(next);
  }
}

// Macha's test_what_is_not_held_pauses_over_the_ids_beside_the_view
// (test_availability.cpp): the join pauses between pages over the ids beside the
// held view too, so a stop asked for there ends it at the next page, not after
// every id.
TEST(LedgerAvailability, WhatIsNotHeldPausesOverTheIdsBesideTheView) {
  const std::set<ObjectId> viewed{numbered_id(1), numbered_id(2)};
  std::vector<ObjectId> others;
  for (uint32_t i = 0; i < 10000; ++i) others.push_back(numbered_id(1000 + i));
  std::sort(others.begin(), others.end());
  dannan::test::TempDir dir;
  auto view = held_trie(dir.path() / "view", viewed);
  const contract::ReferencedSets sets(contract::ReferencedSets::Views{view->snapshot(), std::nullopt}, others, {});
  const std::vector<ObjectTrie::Snapshot> parts;

  size_t pauses = 0;
  const auto all = sets.not_held(contract::RetentionClass::data, parts, [&] { ++pauses; });
  EXPECT_EQ(all.size(), viewed.size() + others.size());
  EXPECT_GE(pauses, others.size() / 4096);

  size_t before_stop = 0;
  EXPECT_THROW((void)sets.not_held(contract::RetentionClass::data, parts,
                                   [&] {
                                     ++before_stop;
                                     throw MaintenanceStopping();
                                   }),
               MaintenanceStopping);
  EXPECT_EQ(before_stop, 1u);
}

}  // namespace
}  // namespace dagda::ledger
