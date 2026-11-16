//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Step 7b: the instant delete (the maintainer, 2026-11-04 night, "Claims and space").
// From the design, not from the code: a delete is checked like a write on
// every replica; each replica releases only the claims the caller's observed
// clock dominates, removes its copy at once when no claim remains and its
// current inventory does not refer to it, and keeps it otherwise; the
// evidence names, node by node, what was removed and what kept the rest. The
// partition case: a claim written where the caller has not seen survives
// and keeps that node's copy. What a delete leaves, the collector frees once
// its claims go.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "dagda/context.h"
#include "dagda/store/remove.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/reachability.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/contract/scope.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using contract::RetentionClass;
using contract::RetentionDot;
using dannan::contract::Principal;
using dannan::contract::ScopeDecision;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;

constexpr auto deadline = std::chrono::milliseconds(15000);

ClusterNodeOptions options(size_t replication) {
  ClusterNodeOptions out;
  out.store.replication = replication;
  out.store.write_copies = replication;
  return out;
}

// Written on `writer`, on disk on every member of its replication set.
ObjectId write_everywhere(ClusterNode& writer, uint64_t n, size_t copies) {
  WriteOptions write;
  write.ack_level = Durability::durable;
  const auto evidence = writer.api().objects().write(test::numbered(n), write, deadline);
  EXPECT_TRUE(evidence.met);
  EXPECT_EQ(evidence.acknowledged(), copies);
  return evidence.id;
}

// A claim by `dot`'s writer on every node holding the object (up to the
// store's write_copies).
void claim(ClusterNode& on, const ObjectId& id, RetentionDot dot) {
  const auto evidence = on.api().claims().retain(RetentionClass::data, {id}, dot, {}, deadline);
  EXPECT_TRUE(evidence.finished) << evidence.why;
  EXPECT_TRUE(evidence.unheld.empty());
}

// A directory that refuses one prefix and grants the rest, at generation 42.
class OnePrefixRefused final : public dannan::contract::ScopeSource {
 public:
  explicit OnePrefixRefused(std::string refused) : _refused(std::move(refused)) {}
  ScopeDecision may_write(const Principal& principal, std::string_view prefix) const override {
    if (prefix.starts_with(_refused)) return {false, 42, "no grant for " + principal.id + " under " + std::string(prefix)};
    return {true, 42, {}};
  }

 private:
  const std::string _refused;
};

// What an owner whose state refers to nothing, at a head whose release
// clock is `clock`, tells the collector.
void refer_to_nothing(test::TestProvider& provider, contract::RetentionClock clock) {
  provider.answer.complete = true;
  contract::Hash256 head{};
  head.bytes[0] = 1;
  provider.head = head;
  contract::Reachability release;
  release.clock = std::move(clock);
  release.complete = true;
  provider.heads[head] = release;
}

// The inventory a node publishes when its owner's state refers to nothing.
// A node with none published keeps every copy (the case below), so a case
// that expects a copy to go publishes this first.
void publish_nothing_referenced(ClusterNode& node) {
  test::TestProvider provider;
  provider.answer.complete = true;
  node.publish_inventory(provider);
}

// On three nodes, a delete whose clock covers every claim on the object
// releases them all and removes every copy at once, in this call.
TEST(StoreRemove, ADeleteCoveringEveryClaimRemovesEveryCopyAtOnce) {
  Cluster cluster;
  auto& a = cluster.add("a", options(3));
  auto& b = cluster.add("b", options(3));
  auto& c = cluster.add("c", options(3));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  for (auto* node : {&a, &b, &c}) publish_nothing_referenced(*node);
  const auto id = write_everywhere(a, 1, 3);
  const RetentionDot by_a{a.id(), 1}, by_b{b.id(), 1};
  claim(a, id, by_a);
  claim(b, id, by_b);
  for (auto* node : {&a, &b, &c}) ASSERT_TRUE(node->claims().retained(RetentionClass::data, id)) << node->name();

  const auto evidence = a.api().objects().remove({id}, {{a.id(), 1}, {b.id(), 1}}, {}, deadline);
  EXPECT_TRUE(evidence.finished) << evidence.why;
  EXPECT_EQ(evidence.sought, std::vector<ObjectId>{id});
  ASSERT_EQ(evidence.nodes.size(), 3u);
  EXPECT_EQ(evidence.nodes.front().node, a.id()) << "the local replica answers first";
  for (auto* node : {&a, &b, &c}) {
    EXPECT_EQ(evidence.state(node->id(), id), RemoveState::removed) << node->name() << " " << evidence.on(node->id())->why;
    EXPECT_FALSE(node->data().has(id)) << node->name();
    EXPECT_FALSE(node->claims().retained(RetentionClass::data, id)) << node->name();
  }
  EXPECT_EQ(evidence.removed(), 3u);
  EXPECT_EQ(a.calls_of(message::remove_objects), 0u) << "the local replica is not called over the network";
  EXPECT_EQ(b.calls_of(message::remove_objects), 1u);
  EXPECT_EQ(c.calls_of(message::remove_objects), 1u);
}

// The partition case: b holds a claim from a writer whose write a's clock
// has not seen. a's delete releases what it observed everywhere, removes a's
// copy, and keeps b's, naming the claim that kept it; the released claim,
// arriving late on b, is refused (observed-remove).
TEST(StoreRemove, AClaimTheCallersClockHasNotSeenKeepsThatNodesCopyAndIsNamed) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2));
  auto& b = cluster.add("b", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  for (auto* node : {&a, &b}) publish_nothing_referenced(*node);
  const auto id = write_everywhere(a, 2, 2);
  claim(a, id, {a.id(), 1});
  // Written on b while a could not see it.
  const RetentionDot unseen{b.id(), 7};
  b.claims().retain(RetentionClass::data, id, unseen);

  const auto evidence = a.api().objects().remove({id}, {{a.id(), 1}}, {}, deadline);
  EXPECT_TRUE(evidence.finished) << evidence.why;
  EXPECT_EQ(evidence.state(a.id(), id), RemoveState::removed);
  ASSERT_EQ(evidence.state(b.id(), id), RemoveState::kept_by_claim);
  const auto* kept = evidence.on(b.id())->object(id);
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept->claims, std::vector<RetentionDot>{unseen});
  EXPECT_EQ(evidence.removed(), 1u);
  EXPECT_FALSE(a.data().has(id));
  EXPECT_TRUE(b.data().has(id));
  // On b only the unseen claim remains, and a's released claim cannot come
  // back late.
  EXPECT_EQ(b.claims().claims(RetentionClass::data, id).adds, (std::map<contract::NodeId, uint64_t>{{b.id(), 7}}));
  b.claims().retain(RetentionClass::data, id, {a.id(), 1});
  EXPECT_EQ(b.claims().claims(RetentionClass::data, id).adds, (std::map<contract::NodeId, uint64_t>{{b.id(), 7}}));
  // Asked again with the same clock, b still keeps it, for the same claim.
  const auto again = a.api().objects().remove({id}, {{a.id(), 1}}, {}, deadline);
  EXPECT_EQ(again.state(a.id(), id), RemoveState::absent);
  EXPECT_EQ(again.state(b.id(), id), RemoveState::kept_by_claim);
  EXPECT_TRUE(b.data().has(id));
}

// What a delete left, the collector frees once its claims go: b's collector
// releases the unseen claim against an owner's head that has observed it,
// and, after b's grace on b's own clock, the copy goes.
TEST(StoreRemove, TheCollectorFreesWhatTheDeleteLeftOnceItsClaimsGo) {
  test::TestProvider provider;
  dannan::test::SteppedWall wall(dannan::contract::system_wall_clock().unix_ms());
  Cluster cluster;
  auto a_options = options(2);
  auto b_options = options(2);
  b_options.wall = &wall;
  b_options.garbage_grace = 2h;
  auto& a = cluster.add("a", a_options);
  auto& b = cluster.add("b", b_options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto id = write_everywhere(a, 3, 2);
  claim(a, id, {a.id(), 1});
  b.claims().retain(RetentionClass::data, id, {b.id(), 1});
  ASSERT_EQ(a.api().objects().remove({id}, {{a.id(), 1}}, {}, deadline).state(b.id(), id), RemoveState::kept_by_claim);

  // The owner's head has now seen b's claim write and refers to nothing.
  refer_to_nothing(provider, {{a.id(), 1}, {b.id(), 1}});
  b.dagda_node().register_reachability(domain::domain_v1, provider);
  const auto first = b.run_maintenance_once();
  ASSERT_TRUE(first.gc) << first.gc_skipped;
  EXPECT_TRUE(first.gc->rebuilt_inventory);
  const auto second = b.run_maintenance_once();
  ASSERT_TRUE(second.gc && second.gc->data);
  EXPECT_TRUE(second.gc->data->permitted) << second.gc->data->reason;
  EXPECT_EQ(second.gc->released_data, 1u);
  EXPECT_FALSE(b.claims().retained(RetentionClass::data, id));
  // First seen unreferenced and unclaimed now: kept for b's grace.
  EXPECT_TRUE(b.data().has(id));
  wall.advance(1h);
  (void)b.run_maintenance_once();
  EXPECT_TRUE(b.data().has(id));
  wall.advance(61min);
  const auto freed = b.run_maintenance_once();
  ASSERT_TRUE(freed.gc);
  EXPECT_GT(freed.gc->swept_data.bytes, 0u);
  EXPECT_FALSE(b.data().has(id));
  b.dagda_node().unregister_reachability(domain::domain_v1);
}

// An object the node's current inventory refers to is kept there, and said
// to be; a node whose inventory refers to nothing removes its copy.
TEST(StoreRemove, AnObjectTheInventoryRefersToIsKept) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2));
  auto& b = cluster.add("b", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  publish_nothing_referenced(a);
  const auto id = write_everywhere(a, 4, 2);
  test::TestProvider provider;
  provider.answer.data = {id};
  provider.answer.complete = true;
  b.publish_inventory(provider);

  const auto evidence = a.api().objects().remove({id}, {}, {}, deadline);
  EXPECT_TRUE(evidence.finished) << evidence.why;
  EXPECT_EQ(evidence.state(a.id(), id), RemoveState::removed);
  EXPECT_EQ(evidence.state(b.id(), id), RemoveState::kept_referenced);
  EXPECT_FALSE(a.data().has(id));
  EXPECT_TRUE(b.data().has(id));
}

// A node with no inventory published keeps its copy, as one whose inventory is
// incomplete does: it cannot say its owner's state does not refer to the object
// (the maintainer, 2026-11-16; gc-and-deletion gap 9). The claims the clock dominates are
// still released there. Once it publishes an inventory referring to nothing,
// the same delete removes the copy.
TEST(StoreRemove, ANodeWithNoInventoryPublishedKeepsItsCopy) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2));
  auto& b = cluster.add("b", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  publish_nothing_referenced(b);
  ASSERT_EQ(a.ledger().inventory(), nullptr);
  const auto id = write_everywhere(a, 8, 2);
  claim(a, id, {a.id(), 1});

  const auto evidence = a.api().objects().remove({id}, {{a.id(), 1}}, {}, deadline);
  EXPECT_TRUE(evidence.finished) << evidence.why;
  EXPECT_EQ(evidence.state(a.id(), id), RemoveState::kept_referenced);
  EXPECT_EQ(evidence.state(b.id(), id), RemoveState::removed);
  EXPECT_EQ(evidence.removed(), 1u);
  EXPECT_TRUE(a.data().has(id));
  EXPECT_FALSE(b.data().has(id));
  EXPECT_FALSE(a.claims().retained(RetentionClass::data, id));

  publish_nothing_referenced(a);
  const auto again = a.api().objects().remove({id}, {{a.id(), 1}}, {}, deadline);
  EXPECT_EQ(again.state(a.id(), id), RemoveState::removed);
  EXPECT_EQ(again.state(b.id(), id), RemoveState::absent);
  EXPECT_FALSE(a.data().has(id));
}

// A scope refused on every replica removes nothing anywhere and releases no
// claim; each node says why, at the generation of the scope data it read.
TEST(StoreRemove, ARefusedScopeRemovesNothing) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2));
  auto& b = cluster.add("b", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  for (auto* node : {&a, &b}) publish_nothing_referenced(*node);
  const auto id = write_everywhere(a, 5, 2);
  claim(a, id, {a.id(), 1});
  for (auto* node : {&a, &b}) node->context().identity().install_scope_source(std::make_shared<OnePrefixRefused>("private/"));

  WriteScope scope;
  scope.prefix = "private/alice";
  const auto evidence = a.api().objects().remove({id}, {{a.id(), 1}}, scope, deadline);
  EXPECT_TRUE(evidence.finished) << evidence.why;
  ASSERT_EQ(evidence.nodes.size(), 2u);
  for (auto* node : {&a, &b}) {
    const auto* answer = evidence.on(node->id());
    ASSERT_NE(answer, nullptr);
    EXPECT_EQ(evidence.state(node->id(), id), RemoveState::refused) << node->name();
    EXPECT_EQ(answer->scope_generation, 42u);
    EXPECT_NE(answer->why.find("no grant for"), std::string::npos) << answer->why;
    EXPECT_TRUE(node->data().has(id)) << node->name();
    EXPECT_TRUE(node->claims().retained(RetentionClass::data, id)) << node->name();
  }
  EXPECT_EQ(evidence.removed(), 0u);
  // Under a granted prefix the same delete lands.
  scope.prefix = "public/alice";
  const auto granted = a.api().objects().remove({id}, {{a.id(), 1}}, scope, deadline);
  EXPECT_EQ(granted.removed(), 2u);
}

// Asked again, a delete changes nothing more: every node answers that it
// holds nothing, and nothing fails.
TEST(StoreRemove, TheDeleteIsIdempotent) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2));
  auto& b = cluster.add("b", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  for (auto* node : {&a, &b}) publish_nothing_referenced(*node);
  const auto id = write_everywhere(a, 6, 2);
  const auto never = object_id(test::numbered(600));
  claim(a, id, {a.id(), 3});
  const auto first = b.api().objects().remove({id, never, id}, {{a.id(), 3}}, {}, deadline);
  EXPECT_TRUE(first.finished) << first.why;
  EXPECT_EQ(first.sought.size(), 2u);
  EXPECT_EQ(first.removed(), 2u);
  for (auto* node : {&a, &b}) EXPECT_EQ(first.state(node->id(), never), RemoveState::absent);
  const auto second = b.api().objects().remove({id, never}, {{a.id(), 3}}, {}, deadline);
  EXPECT_TRUE(second.finished) << second.why;
  EXPECT_EQ(second.removed(), 0u);
  for (auto* node : {&a, &b}) {
    EXPECT_EQ(second.state(node->id(), id), RemoveState::absent);
    EXPECT_EQ(second.state(node->id(), never), RemoveState::absent);
    EXPECT_FALSE(node->data().has(id));
  }
}

// A member of the replication set that is not present is named unreachable;
// the members present remove their copies.
TEST(StoreRemove, AMemberAwayIsNamedUnreachable) {
  Cluster cluster;
  auto& a = cluster.add("a", options(3));
  auto& b = cluster.add("b", options(3));
  auto& c = cluster.add("c", options(3));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  for (auto* node : {&a, &b, &c}) publish_nothing_referenced(*node);
  const auto id = write_everywhere(a, 7, 3);
  const auto away = c.id();
  c.stop();
  ASSERT_TRUE(cluster.dead_to(a, away));

  const auto evidence = a.api().objects().remove({id}, {}, {}, deadline);
  EXPECT_TRUE(evidence.finished) << evidence.why;
  EXPECT_EQ(evidence.state(a.id(), id), RemoveState::removed);
  EXPECT_EQ(evidence.state(b.id(), id), RemoveState::removed);
  ASSERT_NE(evidence.on(away), nullptr);
  EXPECT_EQ(evidence.state(away, id), RemoveState::unreachable);
  EXPECT_EQ(evidence.on(away)->why, "not present");
}

}  // namespace
}  // namespace dagda::store
