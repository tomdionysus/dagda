//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Session S6, the reconciler, on real multi-node clusters over the neutral owner and its merge
// (fixtures/heads_merge.h). Each case names Macha's case at 98b816d it comes from and follows it
// assertion for assertion; where one of Macha's assertions is about its filesystem, the comment
// says so and what stands in its place. The C cases are the plan's section 6.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dagda/contract/stopping.h"
#include "dagda/heads/control_node_store.h"
#include "dagda/heads/exchange.h"
#include "dagda/heads/messages.h"
#include "dagda/node.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/heads_merge.h"
#include "dagda/test/fixtures/heads_space.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/test/fixtures/gate.h"

namespace dagda {
namespace {

using namespace std::chrono_literals;
using contract::MaintenanceStopping;
using contract::RetentionClass;
using heads::Acceptance;
using heads::Exchange;
using heads::Hash256;
using heads::HeadsNotReady;
using heads::HistoryEntry;
using heads::NodeId;
using heads::ObjectId;
using heads::Record;
using heads::Space;
using heads::SpaceView;
using test::BaseReadingMerge;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::make_group;
using test::neutral_branch;
using test::neutral_conflicts;
using test::neutral_founding;
using test::neutral_tree;
using test::NeutralBranch;
using test::NeutralCodec;
using test::NeutralEditing;
using test::NeutralEntries;
using test::NeutralHead;
using test::NeutralItem;
using test::NeutralMerge;
using test::NeutralRef;
using test::NeutralTree;
using test::reconciling_spec;
using test::stamped_mutation;
using test::wait_until;

const std::string space_name = "test/neutral";

const NeutralHead& head_of(const SpaceView& view) { return *NeutralCodec::as_head(view.state); }
std::shared_ptr<const NeutralHead> decoded(const Record& record) { return NeutralCodec::as_head(NeutralCodec().decode_full(record.payload)); }

Space& space_on(ClusterNode& node, const std::string& name = space_name) {
  auto* found = node.dagda_node().spaces()->find(node.api().heads().id_of(name));
  EXPECT_NE(found, nullptr);
  return *found;
}

Exchange& exchange_on(ClusterNode& node, const std::string& name = space_name) {
  auto* found = node.dagda_node().spaces()->exchange(node.api().heads().id_of(name));
  EXPECT_NE(found, nullptr);
  return *found;
}

heads::Replica& replica_on(ClusterNode& node, const std::string& name = space_name) { return space_on(node, name).replica(); }

// A node on its own: Macha's solo config, every read a real read (metadata_cache = 0).
ClusterNodeOptions solo(std::shared_ptr<const heads::Merge> merge = std::make_shared<NeutralMerge>()) {
  ClusterNodeOptions out;
  out.spaces = {reconciling_spec(space_name, std::move(merge))};
  out.exchange.write_copies = 1;
  out.exchange.view_ttl = 0ms;
  return out;
}

// What a node of a bootstrapped cluster runs with, maintenance driven by the case.
ClusterNodeOptions joined(size_t write_copies = 2) {
  ClusterNodeOptions out;
  out.spaces = {reconciling_spec(space_name)};
  out.exchange.joiner = true;
  out.exchange.write_copies = write_copies;
  return out;
}

// The same with its maintenance thread running on short intervals, as Macha's Service has.
ClusterNodeOptions live(size_t write_copies = 2) {
  auto out = joined(write_copies);
  out.maintenance.automatic = true;
  out.maintenance.interval = 25ms;
  out.maintenance.gc_interval = 1h;
  return out;
}

void write_key(ClusterNode& node, const std::string& key, uint64_t seed = 1) {
  (void)space_on(node).mutate_delta(stamped_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys[key] = make_group(seed); }));
}

// Macha's retry_while_not_ready.
bool writes(ClusterNode& node, const std::string& key, uint64_t seed = 1, std::chrono::milliseconds within = 15s) {
  return wait_until(
      [&] {
        try {
          write_key(node, key, seed);
          return true;
        } catch (const HeadsNotReady&) {
          return false;
        }
      },
      within);
}

std::optional<NeutralItem> item_at(ClusterNode& node, const NeutralHead& head, const std::string& key) {
  if (!head.root) return std::nullopt;
  const auto reader = heads::ControlTreeNodeStore::for_reading(node.control(), node.store());
  return neutral_tree().lookup(*head.root, key, reader);
}

bool holds(ClusterNode& node, const std::string& key) {
  try {
    return item_at(node, head_of(space_on(node).converged()), key).has_value();
  } catch (const std::exception&) {
    return false;
  }
}

bool sees(ClusterNode& node, const std::string& key, std::chrono::milliseconds within = 20s) {
  return wait_until([&] { return holds(node, key); }, within);
}

bool one_shared_head(ClusterNode& a, ClusterNode& b, std::chrono::milliseconds within = 30s) {
  return wait_until(
      [&] {
        const auto mine = replica_on(a).accepted_heads();
        const auto theirs = replica_on(b).accepted_heads();
        return mine.size() == 1 && theirs.size() == 1 && mine.front().hash == theirs.front().hash;
      },
      within);
}

bool peers_up(ClusterNode& a, ClusterNode& b, size_t count = 2) {
  return wait_until([&] { return a.context().nodes().active().size() >= count && b.context().nodes().active().size() >= count; });
}

// Stored and accepted here, with a certificate of this node's: what a peer's accept does, and
// no more, so nothing is owed or offered.
void stand(ClusterNode& node, const Record& record, const std::string& name = space_name) {
  auto& space = space_on(node, name);
  ASSERT_TRUE(space.replica().store_commit(record));
  ASSERT_TRUE(space.accept(Acceptance{record.generation, record.hash, 1, {node.id()}}));
}

// Macha's make_sibling: another author's change to `base`, standing here as a head.
NeutralBranch sibling_on(ClusterNode& node, const Record& base, const heads::Dot& dot, const NeutralEntries& upsert, const std::string& name = space_name) {
  auto nodes = heads::ControlTreeNodeStore::for_commit(node.control(), node.store());
  auto branch = neutral_branch(base, *decoded(base), dot, upsert, {}, nodes);
  stand(node, branch.record, name);
  return branch;
}

heads::Dot new_dot(uint64_t sequence = 1) { return {NodeId::random(), sequence}; }

// ---- The merge, over the neutral owner ---------------------------------------

// Macha's test_reconciling_two_tree_backed_branches_keeps_the_namespace (NS:565), every assertion.
// Its `conflicts.empty()` is on the conflicts of this owner's blob.
TEST(HeadsReconcile, TwoTreeBackedBranchesKeepTheSpace) {
  heads::MemoryTreeNodeStore store;
  NeutralEntries base_entries{{"root", make_group(0)}, {"films", make_group(1)}, {"films/shared", test::make_item(10, 3)}};
  NeutralHead base;
  base.root = neutral_tree().build(base_entries, store);
  const auto make_branch = [&](NeutralEntries& expected, uint8_t who, const std::string& key, NeutralItem item) {
    NeutralHead branch = base;
    heads::Dot dot{test::author(who), 1};
    test::stamp_item(item, dot);
    branch.clock[dot.author] = dot.sequence;
    NeutralTree::Delta changes;
    changes.upsert[key] = item;
    branch.root = neutral_tree().apply(*branch.root, store, changes);
    expected[key] = item;
    return branch;
  };
  auto expected = base_entries;
  const auto left = make_branch(expected, 1, "films/left", test::make_item(20, 4));
  const auto right = make_branch(expected, 2, "films/right", test::make_item(30, 5));
  Hash256 left_head{}, right_head{};
  left_head.bytes[0] = 1;
  right_head.bytes[0] = 2;

  const auto merge = test::neutral_tree_merge(left, right, left_head, right_head, store);
  EXPECT_TRUE(neutral_conflicts(merge.merged.head).empty());
  const auto merged_root = neutral_tree().update(merge.onto, store, merge.changes);
  EXPECT_EQ(neutral_tree().read(merged_root, store), expected);
  heads::MemoryTreeNodeStore fresh;
  EXPECT_EQ(merged_root, neutral_tree().build(expected, fresh));
}

// One author's lineage, as Macha's Lineage (NT:1336): the keys it holds, and the mutations it
// authors on them, each stamped as a commit stamps what it writes.
struct Lineage {
  NodeId who{};
  uint64_t sequence = 0;
  NeutralHead head;
  NeutralEntries entries;

  explicit Lineage(uint8_t tag) { who.bytes[0] = tag; }
  heads::Dot next() {
    head.clock[who] = ++sequence;
    return {who, sequence};
  }
  void put(const std::string& key, NeutralItem item) {
    test::stamp_item(item, next());
    entries[key] = std::move(item);
  }
  void edit(const std::string& key, uint64_t grow) {
    auto item = entries.at(key);
    item.weight += grow;
    put(key, std::move(item));
  }
  void erase(const std::string& key) {
    (void)next();
    entries.erase(key);
  }
  void adopt(const NeutralHead& other, const NeutralEntries& at) {
    head = other;
    entries = at;
    sequence = std::max(sequence, [&] {
      const auto found = other.clock.find(who);
      return found == other.clock.end() ? uint64_t{0} : found->second;
    }());
  }
};

NeutralEntries library(size_t groups, size_t items) {
  NeutralEntries out;
  for (size_t g = 0; g < groups; ++g)
    for (size_t i = 0; i < items; ++i) out["group " + std::to_string(g) + "/item " + std::to_string(i)] = test::make_item(g * 100 + i + 1, 2);
  return out;
}

// Macha's check_tree_merge (NT:1400): what a tree merge must share with the merge of the
// materialised heads, and its delta replayed on the primary gives that record byte for byte.
std::pair<uint64_t, uint64_t> check_tree_merge(const test::NeutralMerged& expected, const test::NeutralTreeMerge& merge, const ObjectId& root,
                                               const NeutralHead& primary, const Hash256& secondary, heads::TreeNodeStore& store) {
  heads::MemoryTreeNodeStore fresh;
  const auto expected_root = neutral_tree().build(expected.entries, fresh);
  EXPECT_EQ(root, expected_root);
  EXPECT_EQ(merge.merged.conflicts_created, expected.conflicts_created);
  EXPECT_EQ(merge.merged.conflicts_superseded, expected.conflicts_superseded);
  EXPECT_EQ(merge.merged.head.notes.get(), expected.head.notes.get());
  EXPECT_TRUE(merge.merged.entries.empty());

  auto expected_tree = expected.head;
  expected_tree.root = expected_root;
  auto merged_tree = merge.merged.head;
  merged_tree.root = root;
  EXPECT_EQ(test::neutral_payload(merged_tree), test::neutral_payload(expected_tree));

  EXPECT_EQ(merge.onto, *primary.root);
  merged_tree.merge_parents = {secondary};
  const auto delta = test::neutral_tree_merge_delta(primary, merged_tree, merge.changes);
  if (!delta) {
    ADD_FAILURE() << "a tree merge has a delta";
    return {};
  }
  const auto encoded = test::encode_neutral_delta(*delta);
  NeutralCodec codec;
  const auto replayed = codec.apply_delta(std::make_shared<const NeutralHead>(primary), encoded, &store);
  EXPECT_EQ(codec.encode_full(replayed), test::neutral_payload(merged_tree));
  return {encoded.size(), test::neutral_payload(merged_tree).size()};
}

Hash256 head_named(uint8_t tag) {
  Hash256 out{};
  out.bytes[0] = tag;
  return out;
}

// Macha's test_a_tree_merge_is_the_merge_of_the_materialised_heads (NT:1505). The materialised
// merge is the oracle. Over the neutral owner: added on each side, removed on one, one key
// disputed, one changed on each side far apart, then forty random rounds of both outcomes. Owner's,
// and proved by Macha's own: the rename against an edit and the removed directory that comes back
// for its child, as only a filesystem has identity and parents.
TEST(HeadsReconcile, ATreeMergeIsTheMergeOfTheMaterialisedHeads) {
  Lineage origin{9};
  for (const auto& [key, item] : library(10, 8)) origin.put(key, item);
  Lineage a{1}, b{2};
  a.adopt(origin.head, origin.entries);
  b.adopt(origin.head, origin.entries);

  uint64_t delta_bytes = 0, full_bytes = 0;
  const auto merge_round = [&](int round) {
    const auto left_head = head_named(round % 2 ? 1 : 2);
    const auto right_head = head_named(round % 2 ? 2 : 1);
    auto expected = test::neutral_merge_over(a.head, b.head, a.entries, b.entries, left_head, right_head);

    heads::MemoryTreeNodeStore store;
    auto left = a.head;
    auto right = b.head;
    left.root = neutral_tree().build(a.entries, store);
    right.root = neutral_tree().build(b.entries, store);
    const auto merge = test::neutral_tree_merge(left, right, left_head, right_head, store);
    const auto root = neutral_tree().update(merge.onto, store, merge.changes);
    const bool left_primary = left_head < right_head;
    const auto [delta, full] = check_tree_merge(expected, merge, root, left_primary ? left : right, left_primary ? right_head : left_head, store);
    delta_bytes += delta;
    full_bytes += full;
    a.adopt(expected.head, expected.entries);
    b.adopt(expected.head, expected.entries);
    return expected;
  };

  const std::string added_a = "group 1/from a";
  const std::string added_b = "group 8/from b";
  const std::string removed = "group 2/item 1";
  const std::string disputed = "group 3/item 0";
  const std::string far_a = "group 0/item 0";
  const std::string far_b = "group 9/item 7";
  a.put(added_a, test::make_item(9001, 2));
  b.put(added_b, test::make_item(9002, 2));
  a.erase(removed);
  a.edit(disputed, 4096);
  b.edit(disputed, 8192);
  a.edit(far_a, 4096);
  b.edit(far_b, 4096);
  // The later by its sequence: a's third mutation against b's second.
  const auto later = a.entries.at(disputed);
  const auto far_a_weight = a.entries.at(far_a).weight;
  const auto far_b_weight = b.entries.at(far_b).weight;
  {
    const auto merged = merge_round(0);
    EXPECT_TRUE(merged.entries.contains(added_a));
    EXPECT_TRUE(merged.entries.contains(added_b));
    EXPECT_FALSE(merged.entries.contains(removed));
    EXPECT_EQ(merged.conflicts_created, 1u);
    const auto conflicts = neutral_conflicts(merged.head);
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts.front().key, disputed);
    EXPECT_EQ(merged.entries.at(disputed).weight, later.weight);
    EXPECT_EQ(merged.entries.at(far_a).weight, far_a_weight);
    EXPECT_EQ(merged.entries.at(far_b).weight, far_b_weight);
  }

  std::mt19937_64 random(2026);
  const auto mutate = [&](Lineage& lineage, size_t edits) {
    for (size_t i = 0; i < edits; ++i) {
      if (lineage.entries.empty()) return;
      // From a small pool as often as not, so two lineages change the same keys.
      const size_t pool = random() % 2 ? 12 : lineage.entries.size();
      auto target = lineage.entries.begin();
      std::advance(target, static_cast<ptrdiff_t>(random() % std::min(pool, lineage.entries.size())));
      const auto key = target->first;
      switch (random() % 3) {
        case 0:
          lineage.edit(key, 4096);
          break;
        case 1:
          lineage.erase(key);
          break;
        default:
          lineage.put("new " + std::to_string(random() % 1000), test::make_item(random() % 100000, 1));
          break;
      }
    }
  };
  size_t conflicted = 0, clean = 0;
  for (int round = 1; round <= 40; ++round) {
    mutate(a, 1 + random() % 6);
    mutate(b, 1 + random() % 6);
    // Every third round both heads create one key, differently.
    if (round % 3 == 0) {
      const auto clash = "clash " + std::to_string(round);
      a.put(clash, test::make_item(7000 + static_cast<uint64_t>(round), 2));
      b.put(clash, test::make_item(8000 + static_cast<uint64_t>(round), 3));
    }
    (merge_round(round).conflicts_created ? conflicted : clean) += 1;
  }
  EXPECT_GT(conflicted, 5u);
  EXPECT_GT(clean, 5u);
  EXPECT_GT(delta_bytes, 0u);
  EXPECT_GT(full_bytes, 0u);

  // One head twice is that head.
  heads::MemoryTreeNodeStore store;
  auto tree = a.head;
  tree.root = neutral_tree().build(a.entries, store);
  const auto same = test::neutral_tree_merge(tree, tree, head_named(1), head_named(1), store);
  EXPECT_TRUE(same.changes.empty());
  EXPECT_EQ(same.onto, *tree.root);

  // A head with no tree is refused. Macha's: "a map-backed head is not a tree".
  auto no_tree = a.head;
  no_tree.root.reset();
  EXPECT_THROW((void)test::neutral_tree_merge(tree, no_tree, head_named(1), head_named(2), store), std::logic_error);
}

// ---- The fold, on a node -----------------------------------------------------

// Macha's test_a_head_from_an_unknown_history_is_merged (RPC:3737), every assertion. Macha sets its
// foreign head's legacy_clock, an owner's field. Its generation and previous are nothing to do with
// this node's history.
TEST(HeadsReconcile, AHeadFromAnUnknownHistoryIsMerged) {
  Cluster cluster;
  auto& node = cluster.add("node", solo());
  node.start();
  write_key(node, "mine");
  const auto mine = replica_on(node).committed();
  ASSERT_GT(mine.generation, 1u);

  // Another author's space, with nothing of this node's in its clock: "mine" gone and "theirs" added.
  const auto theirs = new_dot(9);
  auto nodes = heads::ControlTreeNodeStore::for_commit(node.control(), node.store());
  auto foreign_head = *decoded(mine);
  foreign_head.clock.clear();
  auto foreign = neutral_branch(mine, foreign_head, theirs, {{"theirs", make_group(9)}}, {"mine"}, nodes);
  foreign.record = test::make_record(mine.generation + 7, heads::record_hash(0, Hash256{}, foreign.record.payload), test::neutral_payload(foreign.head));
  ASSERT_TRUE(replica_on(node).store_commit(foreign.record));
  ASSERT_TRUE(space_on(node).accept(Acceptance{foreign.record.generation, foreign.record.hash, 1, {theirs.author}}));
  ASSERT_EQ(replica_on(node).accepted_heads().size(), 2u);

  const auto merged = space_on(node).converged();
  EXPECT_TRUE(item_at(node, head_of(merged), "mine").has_value());
  EXPECT_TRUE(item_at(node, head_of(merged), "theirs").has_value());
  EXPECT_EQ(head_of(merged).clock.at(theirs.author), theirs.sequence);
  EXPECT_EQ(replica_on(node).accepted_heads().size(), 1u);
  EXPECT_EQ(space_on(node).set_aside_count(), 0u);

  write_key(node, "more");
  EXPECT_TRUE(holds(node, "theirs"));
  EXPECT_TRUE(holds(node, "more"));
}

// Macha's test_a_head_that_cannot_be_merged_is_set_aside (RPC:3806), every assertion. H10: a head
// whose tree nobody here holds is set aside, and it is the other head, not this node's own.
TEST(HeadsReconcile, AHeadThatCannotBeMergedIsSetAside) {
  Cluster cluster;
  auto& node = cluster.add("node", solo());
  node.start();
  write_key(node, "mine");
  const auto mine = replica_on(node).committed();

  auto foreign_head = *decoded(mine);
  foreign_head.root = test::fake_object(77);
  foreign_head.clock.clear();
  foreign_head.clock[NodeId::random()] = 9;
  const auto foreign = test::make_record(mine.generation + 7, heads::record_hash(0, Hash256{}, mine.payload), test::neutral_payload(foreign_head));
  ASSERT_TRUE(replica_on(node).store_commit(foreign));
  ASSERT_TRUE(space_on(node).accept(Acceptance{foreign.generation, foreign.hash, 1, {NodeId::random()}}));

  // Reads serve this node's own head.
  ASSERT_TRUE(wait_until([&] {
    (void)space_on(node).converged();
    return space_on(node).set_aside_count() == 1;
  }));
  EXPECT_TRUE(holds(node, "mine"));
  EXPECT_EQ(exchange_on(node).reconciliations().set_aside, 1u);

  // Writes extend it.
  write_key(node, "more");
  EXPECT_TRUE(holds(node, "more"));
  const auto after = space_on(node).converged();
  EXPECT_LT(after.generation, foreign.generation);

  // Release has a head to follow, and the other head is still held.
  auto read_nodes = [&] { return std::make_unique<heads::ControlTreeNodeStore>(heads::ControlTreeNodeStore::for_reading(node.control(), node.store())); };
  test::NeutralSpaceProvider provider(space_on(node), read_nodes);
  EXPECT_EQ(provider.release_head(), after.hash);
  const auto held = replica_on(node).accepted_heads();
  EXPECT_EQ(held.size(), 2u);
  EXPECT_TRUE(std::any_of(held.begin(), held.end(), [&](const Record& head) { return head.hash == foreign.hash; }));
}

// A merge of a head with no tree at all is the same: mgr:1196-1197, a head that holds none.
TEST(HeadsReconcile, AHeadHoldingNoTreeIsSetAside) {
  Cluster cluster;
  auto& node = cluster.add("node", solo());
  node.start();
  write_key(node, "mine");
  const auto mine = replica_on(node).committed();
  auto foreign_head = *decoded(mine);
  foreign_head.root.reset();
  foreign_head.clock.clear();
  foreign_head.clock[NodeId::random()] = 3;
  const auto foreign = test::make_record(mine.generation + 2, heads::record_hash(0, Hash256{}, mine.payload), test::neutral_payload(foreign_head));
  ASSERT_TRUE(replica_on(node).store_commit(foreign));
  ASSERT_TRUE(space_on(node).accept(Acceptance{foreign.generation, foreign.hash, 1, {NodeId::random()}}));
  EXPECT_EQ(space_on(node).converged().generation, mine.generation);
  EXPECT_EQ(space_on(node).set_aside_count(), 1u);
  EXPECT_EQ(exchange_on(node).reconciliations().merges, 0u);
}

// Macha's test_history_is_not_truncated_while_two_heads_are_held (RPC:6378), every assertion: S6's
// half, a read merging the two, is what lets the one head become the root.
TEST(HeadsReconcile, HistoryIsNotTruncatedWhileTwoHeadsAreHeld) {
  Cluster cluster;
  auto& node = cluster.add("node", solo());
  node.start();
  write_key(node, "mine");
  const auto mine = replica_on(node).committed();
  auto& replica = replica_on(node);

  const auto theirs = new_dot(9);
  auto nodes = heads::ControlTreeNodeStore::for_commit(node.control(), node.store());
  auto foreign_head = *decoded(mine);
  foreign_head.clock.clear();
  auto foreign = neutral_branch(mine, foreign_head, theirs, {{"theirs", make_group(9)}}, {"mine"}, nodes);
  foreign.record = test::make_record(mine.generation + 7, heads::record_hash(0, Hash256{}, foreign.record.payload), test::neutral_payload(foreign.head));
  ASSERT_TRUE(replica.store_commit(foreign.record));
  ASSERT_TRUE(space_on(node).accept(Acceptance{foreign.record.generation, foreign.record.hash, 1, {theirs.author}}));
  ASSERT_EQ(replica.accepted_heads().size(), 2u);

  const auto records = replica.diagnostics().history_records;
  ASSERT_GT(records, 1u);
  exchange_on(node).truncate_history(1, 1);
  EXPECT_EQ(replica.diagnostics().history_records, records);
  EXPECT_EQ(replica.accepted_heads().size(), 2u);

  // Reading merges the two; the one head then becomes the root.
  EXPECT_TRUE(holds(node, "theirs"));
  ASSERT_EQ(replica.accepted_heads().size(), 1u);
  exchange_on(node).truncate_history(1, 1);
  EXPECT_EQ(replica.diagnostics().history_records, 1u);
  EXPECT_TRUE(holds(node, "mine"));
  EXPECT_TRUE(holds(node, "theirs"));
}

// Macha's test_concurrent_reads_during_divergence_produce_one_reconciliation (RPC:3859), every
// assertion, on two real nodes with no maintenance running.
TEST(HeadsReconcile, ConcurrentReadsDuringDivergenceProduceOneReconciliation) {
  auto options_a = joined(1);
  options_a.exchange.view_ttl = 0ms;
  Cluster cluster;
  auto& a = cluster.add("a", options_a);
  auto& b = cluster.add("b", joined(1));
  cluster.start_all();
  ASSERT_TRUE(peers_up(a, b));
  ASSERT_TRUE(writes(a, "base-concurrent"));
  ASSERT_GT(replica_on(a).committed_generation(), 1u);
  ASSERT_TRUE(exchange_on(a).wait_replicated(10s));

  const auto base = replica_on(a).committed();
  const auto left = sibling_on(a, base, new_dot(), {{"left-concurrent", make_group(1)}});
  const auto right = sibling_on(a, base, new_dot(), {{"right-concurrent", make_group(2)}});
  ASSERT_EQ(right.record.generation, left.record.generation);
  ASSERT_NE(right.record.hash, left.record.hash);
  ASSERT_EQ(replica_on(a).accepted_heads().size(), 2u);
  const auto history_before = replica_on(a).diagnostics().history_records;

  constexpr int reader_count = 8;
  std::vector<std::thread> readers;
  std::vector<Hash256> results(reader_count);
  for (int i = 0; i < reader_count; ++i) readers.emplace_back([&, i] { results[i] = space_on(a).converged().hash; });
  for (auto& reader : readers) reader.join();

  EXPECT_EQ(replica_on(a).diagnostics().history_records - history_before, 1u);
  EXPECT_EQ(replica_on(a).accepted_heads().size(), 1u);
  for (const auto& hash : results) EXPECT_EQ(hash, results.front());
  EXPECT_EQ(exchange_on(a).reconciliations().merges, 1u);
}

// Macha's test_a_merge_claims_what_it_introduces (NS:852), every assertion: a merge's claims are one
// route by which an object regains its claim. Macha's filesystem writes and unlinks two files and
// makes sixty directories; here two objects are written, referenced and dropped, and groups stand in.
// What is in the head's ancestry is walked as Macha's walk does, though no maintenance commit lies
// over this merge: the neutral owner's maintenance does not erase a stale retirement here.
TEST(HeadsReconcile, AMergeClaimsWhatItIntroduces) {
  Cluster cluster;
  auto& node = cluster.add("node", solo());
  node.start();
  const auto put_object = [&](uint8_t seed) {
    const auto bytes = test::pattern(256 * 1024, seed);
    const auto id = store::object_id(bytes);
    store::WriteOptions write;
    write.level = store::Durability::durable;
    EXPECT_TRUE(node.store().write(id, bytes, write).met);
    return id;
  };
  const auto first = put_object(101);
  const auto second = put_object(102);
  const auto naming = [](const ObjectId& id) {
    NeutralItem item;
    item.kind = 2;
    NeutralRef ref;
    ref.span = 256 * 1024;
    ref.id = id;
    item.refs.push_back(ref);
    return item;
  };
  (void)space_on(node).mutate_delta(stamped_mutation([&](NeutralEditing& editing) {
    editing.delta.upsert_keys["first"] = naming(first);
    editing.delta.upsert_keys["second"] = naming(second);
    for (int i = 0; i < 60; ++i) editing.delta.upsert_keys["films/" + std::to_string(i)] = make_group(static_cast<uint64_t>(i));
  }));
  (void)space_on(node).mutate_delta(stamped_mutation([&](NeutralEditing& editing) {
    editing.delta.erase_keys.push_back("first");
    editing.delta.erase_keys.push_back("second");
  }));

  // Stopped, so whatever is due is done, then every claim is lost.
  node.restart();
  ASSERT_NO_THROW((void)space_on(node).converged());
  node.stop();
  std::filesystem::remove_all(node.state() / "retention");
  node.start();
  ASSERT_NO_THROW((void)space_on(node).converged());
  auto& claims = node.claims();
  for (const auto& id : {first, second}) ASSERT_FALSE(claims.retained(RetentionClass::data, id));

  // Two branches from one head, each adding a key of its own and a different value at the same key.
  // Each claims the tree nodes it wrote, as its commit would.
  auto& replica = replica_on(node);
  const auto base = replica.committed();
  const auto base_head = decoded(base);
  ASSERT_TRUE(base_head->root.has_value());
  auto nodes = heads::ControlTreeNodeStore::for_commit(node.control(), node.store());
  const auto make_branch = [&](const std::string& key, const ObjectId& disputed) {
    const auto dot = new_dot();
    auto branch = neutral_branch(base, *base_head, dot, {{key, make_group(7)}, {"disputed", naming(disputed)}}, {}, nodes);
    std::vector<ObjectId> written;
    neutral_tree().collect_changes(base_head->root, *branch.head.root, nodes, written);
    claims.retain_batch(RetentionClass::control, written, contract::RetentionDot{dot.author, dot.sequence});
    stand(node, branch.record);
    return branch;
  };
  const auto left = make_branch("films/left", first);
  const auto right = make_branch("films/right", second);
  const auto left_generation = left.record.generation;

  ASSERT_TRUE(wait_until([&] {
    (void)space_on(node).converged();
    const auto heads = replica.accepted_heads();
    return heads.size() == 1 && heads.front().generation > left_generation;
  }));
  // The merge that joined the two branches is found in the head's ancestry.
  std::shared_ptr<const heads::Materialization> merge;
  std::shared_ptr<const heads::Materialization> primary_parent;
  std::vector<ObjectId> introduced;
  const auto reader = heads::ControlTreeNodeStore::for_reading(node.control(), node.store());
  {
    std::vector<Hash256> pending{replica.accepted_heads().front().hash};
    std::set<Hash256> visited;
    while (!pending.empty() && !merge) {
      const auto hash = pending.back();
      pending.pop_back();
      if (!visited.insert(hash).second) continue;
      const auto at = replica.materialized(hash);
      if (!at || at->record.generation <= left_generation) continue;
      const auto& head = *NeutralCodec::as_head(at->state);
      pending.push_back(at->record.previous);
      for (const auto& parent : head.merge_parents) pending.push_back(parent);
      if (head.merge_parents.empty()) continue;
      const auto parent = replica.materialized(at->record.previous);
      if (!parent || !NeutralCodec::as_head(parent->state)->root) continue;
      std::vector<ObjectId> nodes_changed;
      neutral_tree().collect_changes(NeutralCodec::as_head(parent->state)->root, *head.root, reader, nodes_changed);
      if (nodes_changed.empty()) continue;
      merge = at;
      primary_parent = parent;
      introduced = std::move(nodes_changed);
    }
  }
  ASSERT_NE(merge, nullptr);
  const auto& merged = *NeutralCodec::as_head(merge->state);
  ASSERT_EQ(merged.merge_parents.size(), 1u);
  ASSERT_TRUE(merged.root.has_value());
  const auto other_parent = replica.materialized(merged.merge_parents.front());
  ASSERT_NE(other_parent, nullptr);
  // A pure join: the clock is the parents', so any reconciler mints it.
  EXPECT_EQ(merged.clock, test::joined_clock(NeutralCodec::as_head(primary_parent->state)->clock, NeutralCodec::as_head(other_parent->state)->clock));
  for (const auto* branch : {&left.head, &right.head})
    for (const auto& [who, sequence] : branch->clock) EXPECT_TRUE(heads::clock_covers(merged.clock, heads::Dot{who, sequence}));

  // The merge's claim on each: a dot of this node's, beyond the merged head's clock, so a release
  // at this head keeps it.
  const auto uncovered = [&](const ObjectId& id, const heads::HeadClock& clock) {
    const auto held = claims.claims(RetentionClass::control, id);
    return std::count_if(held.adds.begin(), held.adds.end(), [&](const auto& add) { return !heads::clock_covers(clock, heads::Dot{add.first, add.second}); });
  };
  for (const auto& id : introduced) EXPECT_EQ(uncovered(id, merged.clock), 1);
  ASSERT_FALSE(neutral_conflicts(merged).empty());
  for (const auto& id : {first, second}) EXPECT_TRUE(claims.retained(RetentionClass::data, id)) << "a merge is one route by which a claim comes back";

  // This node's next mutation is a head whose clock covers the merge's dot.
  write_key(node, "after-merge");
  const auto after = head_of(space_on(node).converged());
  for (const auto& id : introduced) EXPECT_EQ(uncovered(id, after.clock), 0);
  EXPECT_TRUE(holds(node, "films/left"));
  EXPECT_TRUE(holds(node, "films/right"));
}

// H22, mgr:1254-1270: the peers' share of a merge's claims is done before the merge is offered, and
// its failing defers it, so the merge is published all the same.
class PeersRefuse final : public heads::ClaimBarrier {
 public:
  std::vector<ObjectId> retain_data_here(const std::vector<ObjectId>&, const heads::Dot&) override { return {}; }
  std::vector<ObjectId> retain_data(const std::vector<ObjectId>&, const heads::Dot&) override { return {}; }
  bool retain_control_here(const std::vector<ObjectId>& ids, const heads::Dot&) override {
    control_here.fetch_add(ids.size());
    return true;
  }
  void retain_on_peers(const heads::Dot&, const heads::CommitClaims&) override {
    peers.fetch_add(1);
    if (throwing) throw std::runtime_error("no peer took them");
    if (stopping) throw MaintenanceStopping{};
  }
  std::atomic_uint64_t control_here{0};
  std::atomic_uint64_t peers{0};
  std::atomic_bool throwing{true};
  std::atomic_bool stopping{false};
};

TEST(HeadsReconcile, AMergesClaimsOnPeersAreDeferredIfTheyFail) {
  PeersRefuse barrier;
  auto options = solo();
  options.exchange.claims = &barrier;
  Cluster cluster;
  auto& node = cluster.add("node", options);
  node.start();
  write_key(node, "base");
  ASSERT_TRUE(exchange_on(node).wait_replicated(10s));
  const auto base = replica_on(node).committed();
  const auto peers_before = barrier.peers.load();
  (void)sibling_on(node, base, new_dot(), {{"left", make_group(1)}});
  (void)sibling_on(node, base, new_dot(), {{"right", make_group(2)}});
  EXPECT_TRUE(holds(node, "left"));
  EXPECT_TRUE(holds(node, "right"));
  EXPECT_EQ(replica_on(node).accepted_heads().size(), 1u);
  EXPECT_GT(barrier.peers.load(), peers_before);
  EXPECT_GT(barrier.control_here.load(), 0u);
}

// Nothing swallows a stop: from a merge's walk, from the claims on peers, or at the top of the fold.
// Nothing is set aside for it, and the two heads stand for the next run.
TEST(HeadsReconcile, AStopInAMergeGoesThroughAndSetsNothingAside) {
  auto merge = std::make_shared<NeutralMerge>();
  PeersRefuse barrier;
  barrier.throwing = false;
  auto options = solo(merge);
  options.exchange.claims = &barrier;
  Cluster cluster;
  auto& node = cluster.add("node", options);
  node.start();
  write_key(node, "base");
  auto& exchange = exchange_on(node);
  ASSERT_TRUE(exchange.wait_replicated(10s));
  const auto base = replica_on(node).committed();
  (void)sibling_on(node, base, new_dot(), {{"left", make_group(1)}});
  (void)sibling_on(node, base, new_dot(), {{"right", make_group(2)}});

  // The pause, in the merge's walk of the trees.
  const heads::TreePause stops = [&] {
    if (merge->calls.load() > 0) throw MaintenanceStopping{};
  };
  EXPECT_THROW(exchange.repair_once(stops), MaintenanceStopping);
  EXPECT_EQ(replica_on(node).accepted_heads().size(), 2u);
  EXPECT_EQ(space_on(node).set_aside_count(), 0u);

  // From the owner's merge itself.
  merge->before = [] { throw MaintenanceStopping{}; };
  EXPECT_THROW(exchange.repair_once(), MaintenanceStopping);
  EXPECT_EQ(space_on(node).set_aside_count(), 0u);
  EXPECT_EQ(replica_on(node).accepted_heads().size(), 2u);
  merge->before = nullptr;

  // From the claims on peers, whose other failures are deferred.
  barrier.stopping = true;
  EXPECT_THROW(exchange.repair_once(), MaintenanceStopping);
  EXPECT_EQ(replica_on(node).accepted_heads().size(), 2u);
  EXPECT_EQ(space_on(node).set_aside_count(), 0u);
  barrier.stopping = false;

  // A read does not turn it into its own head's answer: it was never a failure to read.
  merge->before = [] { throw MaintenanceStopping{}; };
  EXPECT_THROW((void)space_on(node).converged(), MaintenanceStopping);
  merge->before = nullptr;
  (void)space_on(node).converged();
  EXPECT_EQ(replica_on(node).accepted_heads().size(), 1u);
}

// C2, a merge's half (mgr:1244). A delta strictly smaller than the record: a delta frame. Equal or
// larger: never offered, a whole frame. One byte smaller is offered: this delta is not a real one,
// so its offer shows as the replica's refusal and the whole frame after it.
class SizedDeltaMerge final : public NeutralMerge {
 public:
  Result merge(const std::optional<heads::State>& base, const heads::MergeHeads& heads) const override {
    auto result = NeutralMerge::merge(base, heads);
    const auto payload = test::neutral_payload(*NeutralCodec::as_head(result.state)).size();
    const auto given = offset.load();
    if (given != none) result.delta.resize(static_cast<size_t>(static_cast<int64_t>(payload) + given));
    return result;
  }
  static constexpr int64_t none = INT64_MIN;
  mutable std::atomic_int64_t offset{none};
};

TEST(HeadsReconcile, AMergeBodyIsADeltaOnlyWhenStrictlySmaller) {
  auto merge = std::make_shared<SizedDeltaMerge>();
  Cluster cluster;
  auto& node = cluster.add("node", solo(merge));
  node.start();
  write_key(node, "base");
  size_t round = 0;
  const auto fold = [&] {
    const auto base = replica_on(node).committed();
    const auto tag = std::to_string(++round);
    (void)sibling_on(node, base, new_dot(), {{"left" + tag, make_group(1)}});
    (void)sibling_on(node, base, new_dot(), {{"right" + tag, make_group(2)}});
    const auto view = space_on(node).converged();
    EXPECT_EQ(replica_on(node).accepted_heads().size(), 1u);
    return replica_on(node).history_links(view.hash)->body;
  };
  const auto refusals = [&] {
    size_t count = 0;
    for (const auto& line : node.log().lines())
      if (line.find("local metadata delta rejected") != std::string::npos) ++count;
    return count;
  };

  EXPECT_EQ(fold(), HistoryEntry::Body::delta);
  EXPECT_EQ(refusals(), 0u);
  merge->offset = 0;
  EXPECT_EQ(fold(), HistoryEntry::Body::full);
  EXPECT_EQ(refusals(), 0u) << "an equal delta is never offered";
  merge->offset = 5;
  EXPECT_EQ(fold(), HistoryEntry::Body::full);
  EXPECT_EQ(refusals(), 0u) << "a larger delta is never offered";
  merge->offset = -1;
  EXPECT_EQ(fold(), HistoryEntry::Body::full);
  EXPECT_EQ(refusals(), 1u) << "one byte smaller is";
}

// C8, H8. Three heads, every arrival order: the same final hash on the neutral owner, and the same
// on a second node, which folds them alone. One shared key among them, so the order could show.
TEST(HeadsReconcile, TheFoldIsTheSameInAnyArrivalOrder) {
  std::array<size_t, 3> order{0, 1, 2};
  std::vector<std::array<size_t, 3>> orders;
  do orders.push_back(order);
  while (std::next_permutation(order.begin(), order.end()));

  const auto space_at = [](size_t i) { return "test/order-" + std::to_string(i); };
  ClusterNodeOptions options;
  options.exchange.write_copies = 1;
  options.exchange.view_ttl = 0ms;
  for (size_t i = 0; i < orders.size(); ++i) options.spaces.push_back(reconciling_spec(space_at(i)));
  // The same three commits in every space and on both nodes: the same dots.
  const std::array<heads::Dot, 3> dots{heads::Dot{test::author(1), 1}, heads::Dot{test::author(2), 1}, heads::Dot{test::author(3), 1}};
  const std::array<NeutralEntries, 3> changes{NeutralEntries{{"one", make_group(1)}, {"shared", make_group(11)}},
                                              NeutralEntries{{"two", make_group(2)}, {"shared", make_group(12)}},
                                              NeutralEntries{{"three", make_group(3)}, {"shared", make_group(13)}}};

  Cluster cluster;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  const auto fold_all = [&](ClusterNode& node, bool reversed) {
    std::set<Hash256> finals;
    for (size_t i = 0; i < orders.size(); ++i) {
      const auto name = space_at(i);
      (void)space_on(node, name).converged();
      const auto base = replica_on(node, name).committed();
      EXPECT_EQ(base.generation, 2u);
      auto taken = orders[i];
      if (reversed) std::reverse(taken.begin(), taken.end());
      for (const auto at : taken) (void)sibling_on(node, base, dots[at], changes[at], name);
      EXPECT_EQ(replica_on(node, name).accepted_heads().size(), 3u);
      finals.insert(space_on(node, name).converged().hash);
      EXPECT_EQ(replica_on(node, name).accepted_heads().size(), 1u);
      EXPECT_EQ(exchange_on(node, name).reconciliations().merges, 2u) << name;
    }
    EXPECT_EQ(finals.size(), 1u);
    return *finals.begin();
  };

  a.start();
  const auto on_a = fold_all(a, false);
  a.stop();
  b.start();
  const auto on_b = fold_all(b, true);
  EXPECT_EQ(on_a, on_b);
  const auto merged = replica_on(b, space_at(0)).committed();
  const auto head = decoded(merged);
  // Two merges: the first over the lowest two heads, the second over it and the third.
  EXPECT_EQ(merged.generation, 5u);
  for (const auto* key : {"one", "two", "three", "shared"}) EXPECT_TRUE(item_at(b, *head, key).has_value()) << key;
  EXPECT_FALSE(neutral_conflicts(*head).empty());

  // Together nothing moves: they hold one head already.
  a.start();
  ASSERT_TRUE(peers_up(a, b));
  exchange_on(a, space_at(0)).repair_once();
  EXPECT_EQ(replica_on(a, space_at(0)).committed().hash, on_a);
  EXPECT_EQ(replica_on(b, space_at(0)).committed().hash, on_a);
  EXPECT_EQ(replica_on(a, space_at(0)).committed().generation, 5u);
}

// R2's other half, for C37: a policy commit's equal clock leaves two heads neither of which is
// dropped once no edge joins them, and a merge is what collapses them.
TEST(HeadsReconcile, AMergeCollapsesWhatAPolicyCommitLeft) {
  auto options = solo();
  Cluster cluster;
  auto& node = cluster.add("node", options);
  node.start();
  write_key(node, "x");
  const auto parent = replica_on(node).committed();
  auto changed = *decoded(parent);
  changed.notes.set("policy/copies", "2");
  const auto policy = test::record_after(parent, test::neutral_payload(changed));
  auto further_head = changed;
  further_head.notes.set("policy/copies", "3");
  const auto further = test::record_after(policy, test::neutral_payload(further_head));
  EXPECT_EQ(decoded(further)->clock, decoded(parent)->clock) << "no dot on either";

  // A truncated root, as a peer that has re-rooted hands it over: no edge to the parent.
  ASSERT_TRUE(replica_on(node).import_history(test::rootless_entry(further)));
  ASSERT_TRUE(space_on(node).accept(Acceptance{further.generation, further.hash, 1, {node.id()}}));
  ASSERT_EQ(replica_on(node).accepted_heads().size(), 2u);

  const auto merged = space_on(node).converged();
  EXPECT_EQ(replica_on(node).accepted_heads().size(), 1u);
  EXPECT_EQ(head_of(merged).notes.get().at("policy/copies"), "3") << "the greater, as a space's values take";
  EXPECT_EQ(merged.generation, further.generation + 1);
}

// ---- Two reconcilers ---------------------------------------------------------

// Two heads, on two nodes that are never up together, so each reconciles alone: `a` holds their
// common ancestor and `b` has truncated past it. What is common to C39 and C40. The tree nodes are
// written on each, the commits being the same bytes.
struct Fork {
  Record base;
  Record left;
  Record right;
};

Fork fork_on_each(ClusterNode& a, ClusterNode& b) {
  a.start();
  b.start();
  EXPECT_TRUE(peers_up(a, b));
  EXPECT_TRUE(writes(a, "base"));
  EXPECT_TRUE(exchange_on(a).wait_replicated(15s));
  EXPECT_TRUE(one_shared_head(a, b));
  (void)space_on(b).converged();
  const auto base = replica_on(a).committed();
  const heads::Dot left_dot{test::author(21), 1};
  const heads::Dot right_dot{test::author(22), 1};
  const NeutralEntries left_change{{"left", make_group(1)}};
  const NeutralEntries right_change{{"right", make_group(2)}};
  a.stop();
  EXPECT_TRUE(wait_until([&] { return b.context().nodes().active().size() == 1; }));
  auto on_b = heads::ControlTreeNodeStore::for_commit(b.control(), b.store());
  Fork out;
  out.base = base;
  out.left = neutral_branch(base, *decoded(base), left_dot, left_change, {}, on_b).record;
  out.right = neutral_branch(base, *decoded(base), right_dot, right_change, {}, on_b).record;
  // `b` takes one head and re-roots at it, so the base is gone from its history, and the other
  // arrives as a peer's root with no edge to anything here.
  stand(b, out.right);
  EXPECT_TRUE(replica_on(b).compact_history_if_safe(1, 1));
  EXPECT_FALSE(replica_on(b).history_contains(base.hash));
  EXPECT_TRUE(replica_on(b).import_history(test::rootless_entry(out.left)));
  EXPECT_TRUE(space_on(b).accept(Acceptance{out.left.generation, out.left.hash, 1, {b.id()}}));
  EXPECT_EQ(replica_on(b).accepted_heads().size(), 2u);
  // The walk still names the boundary parent both heads say they follow; what it names is not here
  // to read, so a merge is given no base.
  EXPECT_EQ(replica_on(b).history_common_ancestor(out.left.hash, out.right.hash), base.hash);
  EXPECT_EQ(replica_on(b).materialized(base.hash), nullptr);
  b.stop();
  a.start();
  EXPECT_EQ(sibling_on(a, base, left_dot, left_change).record.hash, out.left.hash);
  EXPECT_EQ(sibling_on(a, base, right_dot, right_change).record.hash, out.right.hash);
  return out;
}

ClusterNodeOptions alone_with(std::shared_ptr<const heads::Merge> merge) {
  auto out = joined(1);
  out.spaces = {reconciling_spec(space_name, std::move(merge))};
  // A node alone: there is no bootstrap peer to wait for.
  out.exchange.joiner = false;
  out.exchange.view_ttl = 0ms;
  return out;
}

// C39, R3. A merge reserves a dot its clock does not carry (mgr:1255-1256), so the author's sequence
// has a gap, and two reconcilers still mint one hash. This node's next mutation covers it.
TEST(HeadsReconcile, AMergeReservesADotItsClockDoesNotCarry) {
  Cluster cluster;
  auto& a = cluster.add("a", alone_with(std::make_shared<NeutralMerge>()));
  auto& b = cluster.add("b", alone_with(std::make_shared<NeutralMerge>()));
  const auto fork = fork_on_each(a, b);
  ASSERT_FALSE(HasFailure());

  const auto view_a = space_on(a).converged();
  ASSERT_EQ(replica_on(a).accepted_heads().size(), 1u);
  const auto minted_a = replica_on(a).committed();
  EXPECT_EQ(minted_a.generation, fork.left.generation + 1);
  EXPECT_EQ(minted_a.previous, std::min(fork.left.hash, fork.right.hash));
  ASSERT_EQ(head_of(view_a).merge_parents, std::vector<Hash256>{std::max(fork.left.hash, fork.right.hash)});
  // A pure join: nothing of this node's beyond what the parents carry.
  const auto joined_clock = test::joined_clock(decoded(fork.left)->clock, decoded(fork.right)->clock);
  EXPECT_EQ(head_of(view_a).clock, joined_clock);

  // The claim's dot, on the nodes the merge wrote: this node's, and past the clock.
  const auto own = replica_on(a).author_ids(a.id(), head_of(view_a).clock).front();
  std::vector<ObjectId> introduced;
  const auto reader = heads::ControlTreeNodeStore::for_reading(a.control(), a.store());
  neutral_tree().collect_changes(decoded(minted_a.previous == fork.left.hash ? fork.left : fork.right)->root, *head_of(view_a).root, reader, introduced);
  ASSERT_FALSE(introduced.empty());
  uint64_t merge_sequence = 0;
  for (const auto& id : introduced) {
    const auto held = a.claims().claims(RetentionClass::control, id);
    const auto found = held.adds.find(own);
    ASSERT_NE(found, held.adds.end());
    EXPECT_FALSE(heads::clock_covers(head_of(view_a).clock, heads::Dot{own, found->second}));
    merge_sequence = found->second;
  }
  EXPECT_GT(merge_sequence, head_of(view_a).clock.contains(own) ? head_of(view_a).clock.at(own) : 0u);

  a.stop();
  b.start();
  // The other reconciler, alone, with no ancestor and its own author's dot: the same commit.
  (void)space_on(b).converged();
  ASSERT_EQ(replica_on(b).accepted_heads().size(), 1u);
  EXPECT_EQ(replica_on(b).committed().hash, minted_a.hash);
  EXPECT_EQ(replica_on(b).committed().payload, minted_a.payload);
  b.stop();

  // The author's next mutation is one past the merge's dot, leaving the gap, and covers it.
  a.start();
  const auto outcome = space_on(a).mutate_delta(stamped_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["after"] = make_group(3); }));
  ASSERT_TRUE(outcome.dot.has_value());
  EXPECT_EQ(outcome.dot->author, own);
  EXPECT_EQ(outcome.dot->sequence, merge_sequence + 1);
  const auto after = head_of(space_on(a).converged());
  for (const auto& id : introduced)
    for (const auto& [who, sequence] : a.claims().claims(RetentionClass::control, id).adds)
      if (who == own) {
        EXPECT_TRUE(heads::clock_covers(after.clock, heads::Dot{who, sequence}));
      }
}

// R13, found reading mgr:1255-1256 beside md.cpp's reserve_mutation_dot; kept. The fold takes the two
// lowest hashes first, and those may both lack this node's latest mutation. The merge's dot is
// reserved against their clock, which "lacks a mutation this node had accepted", so this node takes a
// new author for a step in a fold whose result will carry that mutation all the same.
TEST(HeadsReconcile, AFoldStepWithoutThisNodesHeadTakesANewAuthor) {
  Cluster cluster;
  auto& node = cluster.add("node", solo());
  node.start();
  write_key(node, "base");
  const auto base = replica_on(node).committed();
  // This node's own head, past the base and high enough among hashes that two others fall below it.
  write_key(node, "own");
  size_t tries = 0;
  while (replica_on(node).committed().hash.bytes[0] < 0x40) {
    ASSERT_LT(++tries, 64u);
    write_key(node, "again" + std::to_string(tries));
  }
  const auto own_head = replica_on(node).committed();
  const auto clock = decoded(own_head)->clock;
  const auto before = replica_on(node).author_ids(node.id(), clock).front();

  auto nodes = heads::ControlTreeNodeStore::for_commit(node.control(), node.store());
  std::vector<NeutralBranch> below;
  // Only commits here that stand under this node's own head by hash: their clocks are base's.
  auto base_for_others = *decoded(base);
  base_for_others.clock.erase(before);
  for (uint64_t seed = 0; below.size() < 2 && seed < 4096; ++seed) {
    auto branch = neutral_branch(base, base_for_others, new_dot(), {{"other" + std::to_string(seed), make_group(seed)}}, {}, nodes);
    if (branch.record.hash < own_head.hash) below.push_back(std::move(branch));
  }
  ASSERT_EQ(below.size(), 2u);
  for (const auto& branch : below) stand(node, branch.record);
  ASSERT_EQ(replica_on(node).accepted_heads().size(), 3u);

  const auto merged = space_on(node).converged();
  EXPECT_EQ(replica_on(node).accepted_heads().size(), 1u);
  const auto ids = replica_on(node).author_ids(node.id(), head_of(merged).clock);
  EXPECT_NE(ids.front(), before) << "a new author, for a step of the fold";
  EXPECT_NE(std::find(ids.begin(), ids.end(), before), ids.end()) << "and the old one is still this node's";
  EXPECT_EQ(head_of(merged).clock.at(before), clock.at(before)) << "the head the fold reached carries the mutation all the same";
  const auto outcome = space_on(node).mutate_delta(stamped_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys["after"] = make_group(3); }));
  ASSERT_TRUE(outcome.dot.has_value());
  EXPECT_EQ(outcome.dot->author, ids.front());
}

// C40, Q5. The contract on Merge says what `base` may cost. A merge that lets it change its result
// mints sibling commits when one reconciler holds the ancestor and another does not; those are two
// heads, the next fold joins them, and it settles: no further commit however often repair runs. The
// neutral merge above, which ignores `base` as Macha's does, made one commit in C39's same fork.
TEST(HeadsReconcile, SiblingMergesConvergeRatherThanLoop) {
  auto options_a = alone_with(std::make_shared<BaseReadingMerge>());
  auto options_b = alone_with(std::make_shared<BaseReadingMerge>());
  Cluster cluster;
  auto& a = cluster.add("a", options_a);
  auto& b = cluster.add("b", options_b);
  (void)fork_on_each(a, b);
  ASSERT_FALSE(HasFailure());

  (void)space_on(a).converged();
  ASSERT_EQ(replica_on(a).accepted_heads().size(), 1u);
  const auto minted_a = replica_on(a).committed();
  a.stop();
  b.start();
  (void)space_on(b).converged();
  ASSERT_EQ(replica_on(b).accepted_heads().size(), 1u);
  const auto minted_b = replica_on(b).committed();
  // Siblings: the same parents and generation, and what each could reach differs.
  EXPECT_NE(minted_a.hash, minted_b.hash);
  EXPECT_EQ(minted_a.generation, minted_b.generation);
  EXPECT_EQ(minted_a.previous, minted_b.previous);
  EXPECT_EQ(decoded(minted_a)->notes.get().at(BaseReadingMerge::witness), "2-ancestor");
  EXPECT_EQ(decoded(minted_b)->notes.get().at(BaseReadingMerge::witness), "1-no-ancestor");

  a.start();
  ASSERT_TRUE(peers_up(a, b));
  ASSERT_TRUE(wait_until([&] {
    for (auto* node : {&a, &b}) {
      try {
        exchange_on(*node).repair_once();
      } catch (const HeadsNotReady&) {
      }
    }
    const auto mine = replica_on(a).accepted_heads();
    const auto theirs = replica_on(b).accepted_heads();
    return mine.size() == 1 && theirs.size() == 1 && mine.front().hash == theirs.front().hash;
  }));
  const auto settled = replica_on(a).committed();
  EXPECT_EQ(settled.generation, minted_a.generation + 1);
  EXPECT_EQ(decoded(settled)->notes.get().at(BaseReadingMerge::witness), "2-ancestor");
  EXPECT_TRUE(replica_on(a).history_is_ancestor(minted_a.hash, settled.hash));
  EXPECT_TRUE(replica_on(a).history_is_ancestor(minted_b.hash, settled.hash));
  for (int run = 0; run < 3; ++run) {
    exchange_on(a).repair_once();
    exchange_on(b).repair_once();
  }
  EXPECT_EQ(replica_on(a).committed().hash, settled.hash) << "no further commit";
  EXPECT_EQ(replica_on(b).committed().hash, settled.hash) << "no further commit";
}

// C16's second half, H27. Founders configured differently build two generation 2 commits and merge
// them like any two heads. Macha's own in the same-config form is RPC:3114, below.
TEST(HeadsFounding, FoundersConfiguredDifferentlyMerge) {
  auto configured = [](uint64_t seed, const std::string& note) {
    ClusterNodeOptions out;
    auto spec = reconciling_spec(space_name);
    spec.founding = neutral_founding({{"a", make_group(seed)}}, note);
    out.spaces = {spec};
    out.exchange.write_copies = 2;
    out.exchange.view_ttl = 0ms;
    return out;
  };
  Cluster cluster;
  auto& a = cluster.add("a", configured(1, "from-a"));
  auto& b = cluster.add("b", configured(2, "from-b"));
  a.start();
  (void)space_on(a).converged();
  const auto founded_a = replica_on(a).committed();
  a.stop();
  b.start();
  (void)space_on(b).converged();
  const auto founded_b = replica_on(b).committed();
  ASSERT_EQ(founded_a.generation, 2u);
  ASSERT_EQ(founded_b.generation, 2u);
  ASSERT_NE(founded_a.hash, founded_b.hash);

  a.start();
  ASSERT_TRUE(peers_up(a, b));
  ASSERT_TRUE(wait_until([&] {
    try {
      exchange_on(a).repair_once();
    } catch (const HeadsNotReady&) {
    }
    return replica_on(a).accepted_heads().size() == 1 && replica_on(a).committed_generation() == 3;
  }));
  ASSERT_TRUE(one_shared_head(a, b));
  const auto merged = replica_on(a).committed();
  EXPECT_EQ(merged.previous, std::min(founded_a.hash, founded_b.hash));
  const auto head = decoded(merged);
  EXPECT_EQ(head->merge_parents, std::vector<Hash256>{std::max(founded_a.hash, founded_b.hash)});
  EXPECT_EQ(head->notes.get().at("founding"), "from-b") << "the greater";
  // One key founded two ways: no clock says which, so the later stands and the other is kept.
  EXPECT_EQ(item_at(a, *head, "a")->stamp, 2);
  EXPECT_EQ(neutral_conflicts(*head).size(), 1u);
  EXPECT_EQ(exchange_on(a).reconciliations().conflicts_created, 1u);
}

// Macha's test_two_node_mutual_bootstrap_metadata_write_floor (RPC:3114). Both may form at once, and
// competing proposals converge on one history: each answered from its own commit, and the two merge.
// Macha's `metadata_voters.empty()` and `data_replication == 2` are fields of its own record, and
// nothing here writes a voter either. A file through its filesystem is an object through the store,
// the same two holders.
TEST(HeadsReconcile, TwoNodesFormingAtOnceConverge) {
  auto with_store = [] {
    auto out = live(2);
    out.store.replication = 2;
    out.store.write_copies = 2;
    return out;
  };
  Cluster cluster;
  auto& a = cluster.add("a", with_store());
  auto& b = cluster.add("b", with_store());
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  std::exception_ptr first_error, second_error;
  std::atomic<unsigned> ready{0};
  std::atomic_bool go{false};
  const auto racing = [&](ClusterNode& node, const std::string& key, std::exception_ptr& error) {
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    try {
      write_key(node, key);
    } catch (...) {
      error = std::current_exception();
    }
  };
  std::thread first(racing, std::ref(a), "from-node-1", std::ref(first_error));
  std::thread second(racing, std::ref(b), "from-node-2", std::ref(second_error));
  while (ready.load() != 2) std::this_thread::yield();
  go = true;
  first.join();
  second.join();
  if (first_error) std::rethrow_exception(first_error);
  if (second_error) std::rethrow_exception(second_error);

  EXPECT_TRUE(sees(a, "from-node-2"));
  EXPECT_TRUE(sees(b, "from-node-1"));

  ASSERT_TRUE(writes(a, "media"));
  const auto input = test::pattern(128 * 1024);
  const auto extent = store::object_id(input);
  store::WriteOptions write;
  write.level = store::Durability::durable;
  ASSERT_TRUE(a.store().write(extent, input, write).met);
  (void)space_on(a).mutate_delta(stamped_mutation([&](NeutralEditing& editing) {
    NeutralItem item;
    item.kind = 2;
    NeutralRef ref;
    ref.span = static_cast<uint32_t>(input.size());
    ref.id = extent;
    item.refs.push_back(ref);
    editing.delta.upsert_keys["media/two-replicas"] = item;
  }));
  EXPECT_TRUE(sees(b, "media/two-replicas"));
  const auto item = item_at(b, head_of(space_on(b).converged()), "media/two-replicas");
  ASSERT_TRUE(item.has_value());
  ASSERT_EQ(item->refs.size(), 1u);
  EXPECT_TRUE(wait_until([&] { return a.data().has(extent) && b.data().has(extent); }));
}

// RPC:3060's fork, which HeadsExchange.NodesSeekingDifferentCopyCountsShareOneSpace waits to avoid:
// its two writes race, and a merge joins them on nodes seeking different copy counts.
TEST(HeadsReconcile, NodesSeekingDifferentCopyCountsForkAndMerge) {
  Cluster cluster;
  auto& a = cluster.add("a", live(1));
  auto& b = cluster.add("b", live(2));
  cluster.start_all();
  ASSERT_TRUE(peers_up(a, b));
  ASSERT_TRUE(writes(a, "base"));
  ASSERT_TRUE(sees(b, "base"));
  std::thread first([&] { write_key(a, "from-one"); });
  std::thread second([&] { write_key(b, "from-two"); });
  first.join();
  second.join();
  EXPECT_TRUE(sees(a, "from-two"));
  EXPECT_TRUE(sees(b, "from-one"));
  EXPECT_TRUE(one_shared_head(a, b));
}

// Macha's test_service_same_generation_sibling_notice_triggers_reconciliation (RPC:3556), every
// assertion. A real pass on each node, each held at its stage hook. Macha sleeps 100 ms twice around
// its duplicate; what it checks is that nothing is sent, and this asserts that at the sender: with
// no announcement made, nothing is in flight to count at the other node.
TEST(HeadsReconcile, ASameGenerationSiblingNoticeTriggersReconciliation) {
  dannan::test::Gate gate_a, gate_b;
  dannan::test::GateOpener open_a(gate_a), open_b(gate_b);
  std::atomic_bool gate_repairs{false};
  std::atomic_bool once_a{false}, once_b{false};
  const auto gated = [&](dannan::test::Gate& gate, std::atomic_bool& once) {
    return [&gate_repairs, &gate, &once](std::string_view stage) {
      if (stage == "metadata-repair-begin" && gate_repairs.load() && !once.exchange(true)) gate.enter_and_wait();
    };
  };
  auto options_a = live(1);
  options_a.maintenance.stage = gated(gate_a, once_a);
  auto options_b = live(1);
  options_b.maintenance.stage = gated(gate_b, once_b);
  Cluster cluster;
  auto& a = cluster.add("a", options_a);
  auto& b = cluster.add("b", options_b);
  cluster.start_all();
  ASSERT_TRUE(writes(a, "seed"));
  ASSERT_TRUE(wait_until(
      [&] {
        const auto d1 = exchange_on(a).demand();
        const auto d2 = exchange_on(b).demand();
        return peers_up(a, b) && replica_on(a).committed_generation() > 1 && replica_on(a).committed().hash == replica_on(b).committed().hash &&
               !d1.scheduled && d1.runs_scheduled == d1.runs_completed && !d2.scheduled && d2.runs_scheduled == d2.runs_completed;
      },
      15s));

  const auto base = replica_on(a).committed();
  gate_repairs = true;
  const auto left = sibling_on(a, base, new_dot(), {{"left-sibling", make_group(1)}});
  ASSERT_TRUE(gate_a.wait_for_entries(1, 5s));
  ASSERT_TRUE(gate_b.wait_for_entries(1, 5s));

  // Node a already knows that generation, so the next notice's only news is a changed head set at
  // the same generation.
  exchange_on(a).note_notice(left.record.generation);
  ASSERT_TRUE(wait_until([&] { return exchange_on(a).remote_generation() == left.record.generation; }, 5s));
  const auto convergence_events = [](ClusterNode& node) {
    return node.context().events().count(node.context().events().kind("metadata")) + node.context().events().count(dannan::status::NodeEvents::membership);
  };
  const auto before_sibling_notice = convergence_events(a);

  const auto right = sibling_on(b, base, new_dot(), {{"right-sibling", make_group(2)}});
  ASSERT_EQ(right.record.generation, left.record.generation);
  ASSERT_NE(right.record.hash, left.record.hash);
  ASSERT_TRUE(wait_until([&] { return convergence_events(a) > before_sibling_notice; }, 2s));

  // Identical acceptance evidence produces no local event and no rebroadcast.
  const auto before_duplicate = convergence_events(b);
  const auto announcements_before = exchange_on(b).announcements();
  ASSERT_TRUE(space_on(b).accept(Acceptance{right.record.generation, right.record.hash, 1, {b.id()}}));
  EXPECT_EQ(exchange_on(b).announcements(), announcements_before);
  EXPECT_EQ(convergence_events(b), before_duplicate);

  gate_a.open();
  ASSERT_TRUE(wait_until(
      [&] {
        const auto heads = replica_on(a).accepted_heads();
        return heads.size() == 1 && heads.front().generation > left.record.generation && holds(a, "left-sibling") && holds(a, "right-sibling");
      },
      10s));

  gate_b.open();
  ASSERT_TRUE(wait_until([&] { return replica_on(b).accepted_heads().size() == 1 && holds(b, "left-sibling") && holds(b, "right-sibling"); }, 10s));

  // The first write after a reconciliation extends the merge commit and does not carry its
  // topology. Either body is compact; the store keeps the smaller.
  const auto merge_heads = replica_on(a).accepted_heads();
  ASSERT_EQ(merge_heads.size(), 1u);
  const auto merge_head = merge_heads.front();
  const auto merge_entry = replica_on(a).history_entry(merge_head.hash);
  ASSERT_TRUE(merge_entry.has_value());
  ASSERT_EQ(merge_entry->merge_parents.size(), 1u);

  write_key(a, "after-merge");
  const auto after_heads = replica_on(a).accepted_heads();
  ASSERT_EQ(after_heads.size(), 1u);
  const auto after_entry = replica_on(a).history_entry(after_heads.front().hash);
  ASSERT_TRUE(after_entry.has_value());
  EXPECT_EQ(after_entry->previous, merge_head.hash);
  EXPECT_TRUE(after_entry->merge_parents.empty());
}

// Macha's test_disjoint_metadata_pairs_branch_and_reconcile (RPC:4590), every assertion: two pairs
// that each stay writable, one member of each back, and the merge descends from both, as a delta on
// both holders.
TEST(HeadsReconcile, DisjointPairsBranchAndReconcile) {
  Cluster cluster;
  auto& n1 = cluster.add("n1", live(2));
  auto& n2 = cluster.add("n2", live(2));
  auto& n3 = cluster.add("n3", live(2));
  auto& n4 = cluster.add("n4", live(2));
  cluster.start_all();
  const auto all_see = [&](size_t count) {
    return wait_until([&] {
      for (auto* node : {&n1, &n2, &n3, &n4})
        if (node->context().nodes().active().size() < count) return false;
      return true;
    });
  };
  ASSERT_TRUE(all_see(4));

  // One accepted base everywhere before partitioning into two writable pairs.
  ASSERT_TRUE(writes(n1, "base"));
  ASSERT_TRUE(wait_until([&] {
    try {
      exchange_on(n1).repair_once();
      return true;
    } catch (const HeadsNotReady&) {
      return false;
    }
  }));
  for (auto* node : {&n2, &n3, &n4}) ASSERT_TRUE(sees(*node, "base"));

  n3.stop();
  n4.stop();
  ASSERT_TRUE(wait_until([&] { return n1.context().nodes().active().size() == 2 && n2.context().nodes().active().size() == 2; }));
  write_key(n1, "left");
  ASSERT_TRUE(sees(n2, "left"));
  const auto left_head = replica_on(n2).committed().hash;
  EXPECT_TRUE(replica_on(n2).acceptance(left_head).has_value());
  n2.stop();
  n1.stop();

  // Nodes 3 and 4 never saw "left", and stay writable.
  n3.start();
  n4.start();
  ASSERT_TRUE(wait_until([&] { return n3.context().nodes().active().size() == 2 && n4.context().nodes().active().size() == 2; }));
  EXPECT_FALSE(holds(n3, "left"));
  ASSERT_TRUE(writes(n3, "right"));
  ASSERT_TRUE(sees(n4, "right"));
  const auto right_head = replica_on(n3).committed().hash;
  ASSERT_NE(right_head, left_head);
  EXPECT_TRUE(replica_on(n3).acceptance(right_head).has_value());

  // With one member of the other pair back, two accepted sibling histories meet.
  n4.stop();
  const auto id2 = n2.id();
  n2.start();
  ASSERT_EQ(n2.id(), id2);
  ASSERT_TRUE(wait_until([&] { return n2.context().nodes().active().size() == 2 && n3.context().nodes().active().size() == 2; }));
  ASSERT_TRUE(wait_until(
      [&] {
        try {
          exchange_on(n2).repair_once();
          return holds(n2, "left") && holds(n2, "right") && holds(n3, "left") && holds(n3, "right");
        } catch (const std::exception&) {
          return false;
        }
      },
      20s));
  ASSERT_TRUE(one_shared_head(n2, n3));

  const auto merged = replica_on(n2).accepted_heads().front();
  EXPECT_TRUE(replica_on(n2).history_is_ancestor(left_head, merged.hash));
  EXPECT_TRUE(replica_on(n2).history_is_ancestor(right_head, merged.hash));
  const auto local_history = replica_on(n2).history_entry(merged.hash);
  const auto remote_history = replica_on(n3).history_entry(merged.hash);
  ASSERT_TRUE(local_history.has_value());
  ASSERT_TRUE(remote_history.has_value());
  EXPECT_EQ(local_history->body, HistoryEntry::Body::delta);
  EXPECT_EQ(remote_history->body, HistoryEntry::Body::delta);
  EXPECT_LT(local_history->payload.size(), merged.payload.size());
  EXPECT_EQ(remote_history->payload, local_history->payload);
}

// H11, the seam. A conflict is the owner's blob, and its GC roots are the owner's to answer through
// the provider: both values stay claimed by the merge and rooted while the conflict stands, and a
// decision drops the alternative. Macha's own (SM:2145, MRG:162, MRG:421) keep its filesystem's.
TEST(HeadsReconcile, AConflictsValuesAreClaimedAndRootedWhileItStands) {
  Cluster cluster;
  auto& node = cluster.add("node", solo());
  node.start();
  write_key(node, "base");
  const auto base = replica_on(node).committed();
  std::array<ObjectId, 3> ids{};
  std::array<NeutralItem, 3> naming{};
  for (size_t i = 0; i < ids.size(); ++i) {
    const auto bytes = test::pattern(4096, static_cast<uint8_t>(i + 1));
    ids[i] = store::object_id(bytes);
    store::WriteOptions write;
    write.level = store::Durability::durable;
    ASSERT_TRUE(node.store().write(ids[i], bytes, write).met);
    naming[i].kind = 2;
    NeutralRef ref;
    ref.span = 4096;
    ref.id = ids[i];
    naming[i].refs.push_back(ref);
  }
  (void)sibling_on(node, base, new_dot(), {{"disputed", naming[0]}});
  (void)sibling_on(node, base, new_dot(), {{"disputed", naming[1]}});
  const auto merged = space_on(node).converged();
  const auto conflicts = neutral_conflicts(head_of(merged));
  ASSERT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts.front().refs.size(), 2u);
  EXPECT_EQ(exchange_on(node).reconciliations().conflicts_created, 1u);
  EXPECT_TRUE(node.claims().retained(RetentionClass::data, ids[0]));
  EXPECT_TRUE(node.claims().retained(RetentionClass::data, ids[1]));

  auto read_nodes = [&] { return std::make_unique<heads::ControlTreeNodeStore>(heads::ControlTreeNodeStore::for_reading(node.control(), node.store())); };
  test::NeutralSpaceProvider provider(space_on(node), read_nodes);
  provider.extra_roots = [](const NeutralHead& head, contract::Reachability& out) {
    for (const auto& conflict : neutral_conflicts(head)) out.data.insert(out.data.end(), conflict.refs.begin(), conflict.refs.end());
  };
  const auto contains = [](const contract::Reachability& roots, const ObjectId& id) {
    return std::find(roots.data.begin(), roots.data.end(), id) != roots.data.end();
  };
  const auto standing = provider.inventory({});
  EXPECT_TRUE(contains(standing, ids[0]));
  EXPECT_TRUE(contains(standing, ids[1]));

  // A decision: a later value, and the conflict gone. A head that has not seen it does not bring
  // it back: its subject no longer holds the value it stood on.
  const auto before_decision = replica_on(node).committed();
  (void)space_on(node).mutate_delta(stamped_mutation([&](NeutralEditing& editing) {
    editing.delta.upsert_keys["disputed"] = naming[2];
    editing.erase_note(test::conflict_note(conflicts.front()));
  }));
  const auto decided = provider.inventory({});
  EXPECT_FALSE(contains(decided, ids[0]));
  EXPECT_FALSE(contains(decided, ids[1]));
  EXPECT_TRUE(contains(decided, ids[2]));
  (void)sibling_on(node, before_decision, new_dot(), {{"unrelated", make_group(9)}});
  const auto after = head_of(space_on(node).converged());
  EXPECT_TRUE(neutral_conflicts(after).empty());
  EXPECT_EQ(exchange_on(node).reconciliations().conflicts_superseded, 1u);
}

}  // namespace
}  // namespace dagda
