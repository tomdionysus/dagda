//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Domain keys on a cluster (step 6): each node's wrap advert travels with
// its record, a peer verifies it under the cluster's CA, wraps a domain key
// to it, and the member opens it; each node's node-local domain is its own.

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

#include "dagda/domain/domain_id.h"
#include "dagda/domain/storage_key.h"
#include "dagda/domain/wrap.h"
#include "dagda/store/local_store.h"
#include "dagda/store/sealing.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/identity/crypto.h"

namespace dagda::domain {
namespace {

using namespace std::chrono_literals;
using dannan::identity::hex;
using test::Cluster;
using test::ClusterNode;
using test::pattern;
using test::wait_until;

std::optional<dannan::membership::NodeRecord> record_of(ClusterNode& observer, const dannan::identity::NodeId& node) {
  for (const auto& record : observer.context().nodes().active())
    if (record.id == node) return record;
  return std::nullopt;
}

TEST(DomainClusterKeys, AWrapAdvertIsVerifiedByAPeerAndADomainKeyWrappedToIt) {
  Cluster cluster;
  auto& a = cluster.add("a");
  auto& b = cluster.add("b");
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  // B's advert reaches A with B's record.
  ASSERT_TRUE(wait_until([&] {
    const auto record = record_of(a, b.id());
    return record && record->advert(wrap_advert).has_value();
  }));
  const auto now = a.context().time().unix_ms();
  const auto b_record = record_of(a, b.id()).value();
  const auto checked = verify_wrap_advert(b_record, cluster.ca().certificate(), now, a.context().identity().revocations());
  ASSERT_TRUE(checked.ok) << checked.why;
  EXPECT_TRUE(checked.issuer_checked);
  EXPECT_EQ(hex(checked.public_key), hex(b.wrap_key().public_key()));
  // Another cluster's CA does not vouch for it.
  const auto stranger = dannan::identity::CertificateAuthority::create("another cluster", now - 60000);
  EXPECT_FALSE(verify_wrap_advert(b_record, stranger.certificate(), now).ok);

  // A wraps a domain key to B; B opens it with A's advert.
  const StorageKey key(dannan::identity::Secret::random(32));
  const auto domain = DomainId::parse("00112233445566778899aabbccddeeff").value();
  const auto wrapped = wrap(key, domain, a.id(), a.wrap_key(), b.id(), checked.public_key);
  const auto bytes = encode_wrapped_key(wrapped);
  ASSERT_TRUE(wait_until([&] {
    const auto record = record_of(b, a.id());
    return record && record->advert(wrap_advert).has_value();
  }));
  const auto a_checked = verify_wrap_advert(record_of(b, a.id()).value(), cluster.ca().certificate(), now);
  ASSERT_TRUE(a_checked.ok) << a_checked.why;
  const auto opened = unwrap(decode_wrapped_key(bytes), b.wrap_key(), a_checked.public_key);
  EXPECT_EQ(hex(opened.bytes()), hex(key.bytes()));
  // Not for a third node's wrap key.
  auto& c = cluster.add("c");
  c.start();
  EXPECT_THROW((void)unwrap(wrapped, c.wrap_key(), a_checked.public_key), std::runtime_error);
  // The keys survive a restart: the same custody, the same advert.
  const auto before = hex(b.wrap_key().public_key());
  b.restart();
  EXPECT_EQ(hex(b.wrap_key().public_key()), before);
}

// A node's node-local domain: its key from its custody, the same each
// start and never another node's; a store under it is layout v19 with the
// node's own domain id, so its records open on that node alone.
TEST(DomainClusterKeys, ANodeLocalDomainIsTheNodesAlone) {
  Cluster cluster;
  auto& a = cluster.add("a");
  auto& b = cluster.add("b");
  cluster.start_all();
  const auto a_key = a.node_local_key();
  const auto b_key = b.node_local_key();
  EXPECT_NE(hex(a_key.bytes()), hex(b_key.bytes()));
  EXPECT_EQ(hex(a.node_local_key().bytes()), hex(a_key.bytes()));
  EXPECT_NE(node_local_domain(a.id()), node_local_domain(b.id()));

  test::StoreBed bed;
  auto options = store::LocalStoreOptions::loose_only(64ULL * 1024 * 1024);
  options.sealing = store::SealingScheme::v19(node_local_domain(a.id()));
  const auto data = pattern(2048, 7);
  const auto id = store::object_id(data);
  {
    store::LocalStore mine(bed.dir.path() / "a-local", options, std::make_shared<const StorageKey>(dannan::identity::Secret(a_key.bytes())), bed.services());
    ASSERT_TRUE(mine.put(id, data));
    EXPECT_EQ(mine.get(id), std::optional<store::Bytes>(data));
  }
  // B's key, or B's domain, opens nothing of A's.
  {
    store::LocalStore theirs(bed.dir.path() / "a-local", options, std::make_shared<const StorageKey>(dannan::identity::Secret(b_key.bytes())), bed.services());
    EXPECT_FALSE(theirs.valid(id));
  }
  auto as_b = options;
  as_b.sealing = store::SealingScheme::v19(node_local_domain(b.id()));
  store::LocalStore moved(bed.dir.path() / "a-local", as_b, std::make_shared<const StorageKey>(dannan::identity::Secret(a_key.bytes())), bed.services());
  EXPECT_FALSE(moved.valid(id));
  const auto restarted_key = a.node_local_key();
  a.restart();
  EXPECT_EQ(hex(a.node_local_key().bytes()), hex(restarted_key.bytes()));
}

}  // namespace
}  // namespace dagda::domain
