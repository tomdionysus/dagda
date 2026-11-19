//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Slice 1's node-local spaces (the three-systems design, D3, node_local, and "How slice
// 1 builds node_local"): this node only, never advertised, exchanged, offered or
// repaired; sealed under the node-local key; durable here before a commit returns; no
// rollback; bounded under churn; the same calls as any space.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dagda/heads/node_pack.h"
#include "dagda/heads/sealed_file.h"
#include "dagda/store/control_store.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/heads_space.h"
#include "dagda/test/fixtures/tree.h"

namespace dagda::heads {
namespace {

using namespace std::chrono_literals;
using test::make_group;
using test::neutral_mutation;
using test::neutral_tree;
using test::NeutralCodec;
using test::NeutralEditing;

SpaceSpec node_local_spec(const std::string& name) {
  auto spec = test::founded_spec(name);
  spec.policy = Policy::node_local();
  return spec;
}

Space& opened(test::ClusterNode& node, const std::string& name) {
  const auto result = node.api().heads().open(node_local_spec(name), 10s);
  EXPECT_TRUE(result.opened) << result.why;
  auto* space = node.dagda_node().spaces()->find(result.space);
  if (!space) throw std::runtime_error("not open");
  return *space;
}

MutationEvidence put(test::ClusterNode& node, const SpaceId& id, const std::string& key, int value) {
  return node.api().heads().mutate_delta(id, neutral_mutation([&](NeutralEditing& editing) { editing.delta.upsert_keys[key] = make_group(value); }), 10s);
}

bool holds(Space& space, const std::string& key) {
  const auto view = space.local();
  const auto root = NeutralCodec::as_head(view.state)->root;
  if (!root) return false;
  dannan::sched::Budget budget;
  const auto page = space.entries(neutral_tree(), view, {}, budget);
  for (const auto& [found, _] : page.items)
    if (found == key) return true;
  return false;
}

uint64_t bytes_under(const std::filesystem::path& directory) {
  uint64_t total = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
    if (entry.is_regular_file()) total += entry.file_size();
  return total;
}

bool any_file_under_holds(const std::filesystem::path& directory, const std::string& text) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.find(text) != std::string::npos) return true;
  }
  return false;
}

// Held on its node alone: a commit on a is not seen on b, which holds the same name as
// its own space, and neither has an exchange for it, so nothing is advertised, offered,
// answered or repaired.
TEST(HeadsNodeLocal, ACommitIsHeldOnItsNodeAloneAndNothingIsExchanged) {
  test::Cluster cluster;
  auto& a = cluster.add("a");
  auto& b = cluster.add("b");
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  auto& on_a = opened(a, "lugh/grants");
  auto& on_b = opened(b, "lugh/grants");
  const auto id = a.api().heads().id_of("lugh/grants");
  EXPECT_EQ(a.dagda_node().spaces()->exchange(id), nullptr);
  EXPECT_EQ(b.dagda_node().spaces()->exchange(id), nullptr);

  const auto result = put(a, id, "grant/a-only", 1);
  ASSERT_TRUE(result.finished) << result.why;
  EXPECT_TRUE(holds(on_a, "grant/a-only"));
  // Durable here before it returned: the replica's committed head is the commit.
  EXPECT_EQ(on_a.replica().committed().hash, result.outcome->record.hash);
  // b stands where it was founded, alone.
  cluster.converged();
  EXPECT_FALSE(holds(on_b, "grant/a-only"));
  EXPECT_EQ(on_b.replica().committed().generation, 2U);
  EXPECT_EQ(on_a.owed_commits(), 0U);
}

// Sealed: nothing of a node-local record is in clear anywhere in the node's state, and
// nothing of it is in the shared control store, which repair offers to every node.
TEST(HeadsNodeLocal, NothingOfItIsInClearOrInTheSharedControlStore) {
  test::Cluster cluster;
  auto& a = cluster.add("a");
  cluster.start_all();
  opened(a, "lugh/grants");
  const auto id = a.api().heads().id_of("lugh/grants");
  const auto control = store::control_store_path(a.state(), a.node_options().control);
  const auto control_before = bytes_under(control);
  ASSERT_TRUE(put(a, id, "grant/visible-only-sealed", 1).finished);
  EXPECT_FALSE(any_file_under_holds(a.state(), "grant/visible-only-sealed"));
  EXPECT_EQ(bytes_under(control), control_before);
  EXPECT_TRUE(std::filesystem::exists(NodePack::file(space_directory(a.state(), id) / "nodes")));
}

// Durable on this node across a restart.
TEST(HeadsNodeLocal, ACommitSurvivesARestart) {
  test::Cluster cluster;
  auto& a = cluster.add("a");
  cluster.start_all();
  opened(a, "lugh/grants");
  const auto id = a.api().heads().id_of("lugh/grants");
  ASSERT_TRUE(put(a, id, "grant/kept", 7).finished);
  a.restart();
  auto& again = opened(a, "lugh/grants");
  EXPECT_TRUE(holds(again, "grant/kept"));
}

// No rollback: the space's directory copied away, more committed, and the copy put back
// in its place: the open refuses it, naming the generations. (A whole state directory
// restored with it is not refused here; that defence is the peers', Dannan's, the
// design's question for the maintainer.)
TEST(HeadsNodeLocal, AnOlderCopyRestoredInPlaceIsRefused) {
  test::Cluster cluster;
  auto& a = cluster.add("a");
  cluster.start_all();
  opened(a, "lugh/grants");
  const auto id = a.api().heads().id_of("lugh/grants");
  ASSERT_TRUE(put(a, id, "grant/one", 1).finished);
  const auto directory = space_directory(a.state(), id);
  dannan::test::TempDir saved;
  std::filesystem::copy(directory, saved.path() / "copy", std::filesystem::copy_options::recursive);
  ASSERT_TRUE(put(a, id, "grant/two", 2).finished);
  a.stop();
  std::filesystem::remove_all(directory);
  std::filesystem::copy(saved.path() / "copy", directory, std::filesystem::copy_options::recursive);
  a.start();
  const auto result = a.api().heads().open(node_local_spec("lugh/grants"), 10s);
  EXPECT_FALSE(result.opened);
  EXPECT_NE(result.why.find("older copy"), std::string::npos) << result.why;
}

// Another node's node-local space does not open here: its key is that node's alone.
TEST(HeadsNodeLocal, AnotherNodesSpaceDoesNotOpenHere) {
  test::Cluster cluster;
  auto& a = cluster.add("a");
  auto& b = cluster.add("b");
  cluster.start_all();
  opened(a, "lugh/grants");
  const auto id = a.api().heads().id_of("lugh/grants");
  ASSERT_TRUE(put(a, id, "grant/a", 1).finished);
  b.stop();
  std::filesystem::create_directories(space_directory(b.state(), id).parent_path());
  std::filesystem::copy(space_directory(a.state(), id), space_directory(b.state(), id), std::filesystem::copy_options::recursive);
  b.start();
  const auto result = b.api().heads().open(node_local_spec("lugh/grants"), 10s);
  EXPECT_FALSE(result.opened);
  EXPECT_FALSE(result.why.empty());
}

// Bounded under churn (Lugh's G8): 10,000 records made and then expired by the owner's
// mutations, and a maintenance turn: the space's files and its node pack are back to
// what they were before, within a page.
TEST(HeadsNodeLocal, ChurnLeavesTheSpaceAsItWas) {
  test::Cluster cluster;
  auto& a = cluster.add("a");
  cluster.start_all();
  auto& space = opened(a, "lugh/grants");
  const auto id = a.api().heads().id_of("lugh/grants");
  const auto directory = space_directory(a.state(), id);
  a.run_maintenance_once();
  const auto files_before = bytes_under(directory);

  for (int batch = 0; batch < 10; ++batch) {
    const auto made = a.api().heads().mutate_delta(id, neutral_mutation([batch](NeutralEditing& editing) {
                                                     for (int i = 0; i < 1000; ++i)
                                                       editing.delta.upsert_keys["grant/" + std::to_string(batch * 1000 + i)] = make_group(i);
                                                   }),
                                                   60s);
    ASSERT_TRUE(made.finished) << made.why;
  }
  EXPECT_GT(bytes_under(directory), files_before + 100 * 1024);
  for (int batch = 0; batch < 10; ++batch) {
    const auto expired = a.api().heads().mutate_delta(id, neutral_mutation([batch](NeutralEditing& editing) {
                                                        for (int i = 0; i < 1000; ++i)
                                                          editing.delta.erase_keys.push_back("grant/" + std::to_string(batch * 1000 + i));
                                                      }),
                                                      60s);
    ASSERT_TRUE(expired.finished) << expired.why;
  }
  a.run_maintenance_once();
  EXPECT_LE(bytes_under(directory), files_before + 4096);
  EXPECT_FALSE(holds(space, "grant/1"));
}

// The same calls as any space: the change notice and changed() work on it.
TEST(HeadsNodeLocal, TheChangeNoticeAndChangedWorkOnIt) {
  test::Cluster cluster;
  auto& a = cluster.add("a");
  cluster.start_all();
  opened(a, "lugh/grants");
  const auto id = a.api().heads().id_of("lugh/grants");
  boost::asio::io_context io;
  std::vector<HeadMoved> moves;
  ASSERT_TRUE(a.api().heads().watch(id, io.get_executor(), [&](HeadMoved move) { moves.push_back(move); }).watching);
  ASSERT_TRUE(put(a, id, "grant/x", 1).finished);
  io.run();
  ASSERT_EQ(moves.size(), 1U);
  dannan::sched::Budget budget;
  const auto changes = a.api().heads().changed(neutral_tree(), id, moves[0].before, moves[0].after, {}, budget);
  ASSERT_TRUE(changes.finished) << changes.why;
  EXPECT_EQ(changes.page.items, std::vector<std::string>{"grant/x"});
}

}  // namespace
}  // namespace dagda::heads
