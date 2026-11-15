//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Built against the installed package, as Lugh's and Lir's multi-node tests are:
// the in-process cluster and the neutral owner from dagda::test_fixtures alone. Two
// nodes found one space; a commit on one is held by the other.
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/heads_space.h"
#include "dagda/test/fixtures/tree.h"

namespace {

using namespace std::chrono_literals;

TEST(InstalledFixture, ACommitOnOneNodeOfTheClusterReachesTheOther) {
  dagda::test::Cluster cluster;
  dagda::test::ClusterNodeOptions options;
  options.spaces.push_back(dagda::test::founded_spec("consumer/space"));
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  const auto id = a.api().heads().id_of("consumer/space");
  const auto result = a.api().heads().mutate_delta(
      id, dagda::test::neutral_mutation([](dagda::test::NeutralEditing& editing) { editing.delta.upsert_keys["k/1"] = dagda::test::make_group(1); }), 10s);
  ASSERT_TRUE(result.finished) << result.why;
  ASSERT_TRUE(result.outcome);

  auto* held = b.dagda_node().spaces()->find(id);
  ASSERT_NE(held, nullptr);
  const auto until = std::chrono::steady_clock::now() + 10s;
  while (held->replica().committed_generation() < result.outcome->record.generation && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(10ms);
  EXPECT_GE(held->replica().committed_generation(), result.outcome->record.generation);
}

}  // namespace
