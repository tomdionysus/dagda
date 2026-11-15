//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The scope check at the write floor (step 6; the maintainer, 2026-11-04 evening;
// stack document, "Scope"): every storing replica, the local write
// included, asks Identity::verify about the writer's principal and prefix
// before it stores, and a refusal is evidence per node, never a bare
// failure. Claims are checked the same way. Without a source only the
// cluster's nodes may write; a source the directory installs decides.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "dagda/store/distributed_store.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/contract/scope.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using dannan::contract::Principal;
using dannan::contract::ScopeDecision;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::pattern;

ClusterNodeOptions options(size_t replication, size_t write_copies) {
  ClusterNodeOptions out;
  out.store.replication = replication;
  out.store.write_copies = write_copies;
  return out;
}

// A directory that refuses one prefix and grants the rest, at a generation
// it names.
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

bool refused_for_scope(const ReplicaEvidence& replica) {
  return replica.state == ReplicaState::refused && replica.why.find("403 scope_refused") != std::string::npos;
}

// With no source installed, a node principal (this node's, or a peer's)
// writes and a directory principal is refused on every replica, each
// saying so.
TEST(StoreScope, WithNoSourceNodePrincipalsWriteAndADirectoryPrincipalIsRefused) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  const auto own = pattern(4096, 1);
  WriteOptions as_node;
  as_node.level = Durability::durable;
  const auto landed = a.store().write(object_id(own), own, as_node);
  EXPECT_TRUE(landed.met);
  EXPECT_EQ(landed.acknowledged(), 2u);
  EXPECT_TRUE(a.data().has(object_id(own)));
  EXPECT_TRUE(b.data().has(object_id(own)));

  // A peer's node principal, named by the caller, is a node of the cluster.
  const auto peers = pattern(4096, 2);
  WriteOptions as_peer = as_node;
  as_peer.principal = Principal{Principal::Kind::node, dannan::identity::to_string(b.id())};
  EXPECT_TRUE(a.store().write(object_id(peers), peers, as_peer).met);

  const auto alices = pattern(4096, 3);
  WriteOptions as_alice = as_node;
  as_alice.principal = Principal{Principal::Kind::directory, "alice"};
  const auto refused = a.store().write(object_id(alices), alices, as_alice);
  EXPECT_FALSE(refused.met);
  EXPECT_TRUE(refused.lost);
  EXPECT_TRUE(refused.settled);
  EXPECT_EQ(refused.holders(), 0u);
  ASSERT_EQ(refused.replicas.size(), 2u);
  for (const auto& replica : refused.replicas) {
    EXPECT_TRUE(refused_for_scope(replica)) << dannan::identity::to_string(replica.node) << " " << replica.why;
    EXPECT_NE(replica.why.find("only the cluster's nodes may write"), std::string::npos) << replica.why;
    EXPECT_NE(replica.why.find("scope generation 0"), std::string::npos) << replica.why;
  }
  EXPECT_TRUE(refused.on(a.id()) && refused.on(b.id()));
  EXPECT_FALSE(a.data().has(object_id(alices)));
  EXPECT_FALSE(b.data().has(object_id(alices)));
  // The refusal reached the peer as a request (the third put it was asked)
  // and cost it no storage.
  EXPECT_EQ(b.calls_of(message::put_object), 3u);
  // A node not of the cluster is refused too.
  WriteOptions as_stranger = as_node;
  as_stranger.principal = Principal{Principal::Kind::node, dannan::identity::to_string(dannan::identity::NodeId::random())};
  const auto stranger = a.store().write(object_id(pattern(4096, 4)), pattern(4096, 4), as_stranger);
  EXPECT_TRUE(stranger.lost);
  for (const auto& replica : stranger.replicas) EXPECT_TRUE(refused_for_scope(replica)) << replica.why;
  // Macha's forms: nothing landed is StorageUnavailable, or false.
  EXPECT_THROW((void)a.store().put(alices, as_alice), StorageUnavailable);
  EXPECT_FALSE(a.store().put(object_id(alices), alices, as_alice));
}

// With a source installed on every node that refuses one prefix, a write
// under it is refused on every replica, the evidence naming each with the
// source's reason and generation, and nothing is stored anywhere; a write
// under another prefix lands. The prefix travels with the put.
TEST(StoreScope, AnInstalledSourceRefusesOnePrefixOnEveryReplica) {
  Cluster cluster;
  auto& a = cluster.add("a", options(3, 2));
  auto& b = cluster.add("b", options(3, 2));
  auto& c = cluster.add("c", options(3, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  for (auto* node : {&a, &b, &c}) node->context().identity().install_scope_source(std::make_shared<OnePrefixRefused>("lugh/secrets"));

  const auto secret = pattern(4096, 11);
  WriteOptions under_secrets;
  under_secrets.level = Durability::durable;
  under_secrets.principal = Principal{Principal::Kind::directory, "alice"};
  under_secrets.prefix = "lugh/secrets/alice";
  const auto refused = a.store().write(object_id(secret), secret, under_secrets);
  EXPECT_FALSE(refused.met);
  EXPECT_TRUE(refused.lost);
  EXPECT_EQ(refused.holders(), 0u);
  ASSERT_EQ(refused.replicas.size(), 3u);
  for (auto* node : {&a, &b, &c}) {
    const auto* replica = refused.on(node->id());
    ASSERT_NE(replica, nullptr) << node->name();
    EXPECT_TRUE(refused_for_scope(*replica)) << node->name() << " " << replica->why;
    EXPECT_NE(replica->why.find("no grant for alice under lugh/secrets/alice"), std::string::npos) << replica->why;
    EXPECT_NE(replica->why.find("scope generation 42"), std::string::npos) << replica->why;
    EXPECT_FALSE(node->data().has(object_id(secret))) << node->name();
  }

  const auto photo = pattern(4096, 12);
  WriteOptions under_photos = under_secrets;
  under_photos.prefix = "macha/alice/photos";
  const auto landed = a.store().write(object_id(photo), photo, under_photos);
  EXPECT_TRUE(landed.met) << landed.replicas.size();
  EXPECT_EQ(landed.acknowledged(), 3u);
  for (auto* node : {&a, &b, &c}) EXPECT_TRUE(node->data().has(object_id(photo))) << node->name();

  // A node principal under the refused prefix is refused the same: the
  // source decides, whoever writes.
  WriteOptions node_under_secrets;
  node_under_secrets.level = Durability::durable;
  node_under_secrets.prefix = "lugh/secrets";
  const auto node_refused = a.store().write(object_id(pattern(4096, 13)), pattern(4096, 13), node_under_secrets);
  EXPECT_TRUE(node_refused.lost);
  EXPECT_EQ(node_refused.replicas.size(), 3u);
  for (const auto& replica : node_refused.replicas) EXPECT_TRUE(refused_for_scope(replica)) << replica.why;

  // The default prefix is the domain's name ("macha" for domain v1), which
  // the source grants.
  WriteOptions by_default;
  by_default.level = Durability::durable;
  EXPECT_TRUE(a.store().write(object_id(pattern(4096, 14)), pattern(4096, 14), by_default).met);
}

// Claims are checked as writes are: a node refuses to claim for a writer
// the source refuses, locally and for a peer, and claims for one it grants.
TEST(StoreScope, ClaimsAreCheckedAsWritesAre) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto data = pattern(4096, 21);
  const auto id = object_id(data);
  WriteOptions durable;
  durable.level = Durability::durable;
  ASSERT_TRUE(a.store().write(id, data, durable).met);
  for (auto* node : {&a, &b}) node->context().identity().install_scope_source(std::make_shared<OnePrefixRefused>("lugh/secrets"));

  WriteScope under_secrets{Principal{Principal::Kind::directory, "alice"}, "lugh/secrets"};
  const auto unheld = a.store().retain_data({id}, contract::RetentionDot{a.id(), 1}, under_secrets);
  EXPECT_EQ(unheld, std::vector<ObjectId>{id});
  EXPECT_FALSE(a.claims().retained(contract::RetentionClass::data, id));
  EXPECT_FALSE(b.claims().retained(contract::RetentionClass::data, id));
  EXPECT_GE(b.calls_of(message::retain_objects), 1u);

  WriteScope under_photos{Principal{Principal::Kind::directory, "alice"}, "macha/alice/photos"};
  EXPECT_TRUE(a.store().retain_data({id}, contract::RetentionDot{a.id(), 2}, under_photos).empty());
  EXPECT_TRUE(a.claims().retained(contract::RetentionClass::data, id));
  EXPECT_TRUE(b.claims().retained(contract::RetentionClass::data, id));

  // A CONTROL claim too: the local claim must hold, and it does not for a
  // refused writer.
  const auto control = pattern(512, 22);
  const auto control_id = object_id(control);
  ASSERT_TRUE(a.control().put(control_id, control));
  EXPECT_FALSE(a.store().retain_control({control_id}, contract::RetentionDot{a.id(), 3}, DistributedStore::ClaimScope::everywhere, under_secrets));
  EXPECT_FALSE(a.claims().retained(contract::RetentionClass::control, control_id));
  EXPECT_TRUE(a.store().retain_control({control_id}, contract::RetentionDot{a.id(), 4}, DistributedStore::ClaimScope::everywhere, under_photos));
  EXPECT_TRUE(a.claims().retained(contract::RetentionClass::control, control_id));
}

}  // namespace
}  // namespace dagda::store
