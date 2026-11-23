//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Domain ids (step 6): domain v1's fixed id and a node's node-local domain,
// pinned by known answers computed outside Dagda (Python's hashlib).

#include <gtest/gtest.h>

#include "dagda/domain/domain.h"
#include "dagda/domain/domain_id.h"
#include "dannan/encoding/base.h"
#include "dannan/identity/crypto.h"

namespace dagda::domain {
namespace {

dannan::identity::NodeId node_of(uint8_t first) {
  dannan::identity::NodeId out;
  for (size_t i = 0; i < 16; ++i) out.bytes[i] = static_cast<uint8_t>(first + i);
  return out;
}

// The first sixteen bytes of SHA-256("dagda/domain/v1").
TEST(DomainId, DomainV1IsFixedAndWellKnown) {
  EXPECT_EQ(to_string(domain_v1), "827b07101d79d9d84a433d8e9cb262ca");
  const auto hash = dannan::identity::sha256({reinterpret_cast<const uint8_t*>("dagda/domain/v1"), 15});
  EXPECT_EQ(dannan::encoding::hex(hash.bytes).substr(0, 32), to_string(domain_v1));
}

// The first sixteen bytes of SHA-256("dagda/node-local/1" || node id): one
// per node, never v1's.
TEST(DomainId, ANodeLocalDomainIsTheNodesOwn) {
  const auto node = node_of(0x10);
  EXPECT_EQ(dannan::identity::to_string(node), "101112131415161718191a1b1c1d1e1f");
  EXPECT_EQ(to_string(node_local_domain(node)), "4010a2f63c8e8897d052b3550bdd647f");
  EXPECT_EQ(node_local_domain(node), node_local_domain(node));
  EXPECT_NE(node_local_domain(node), node_local_domain(node_of(0x11)));
  EXPECT_NE(node_local_domain(node), domain_v1);
}

TEST(DomainId, ParsesItsOwnText) {
  const auto id = node_local_domain(node_of(0x10));
  EXPECT_EQ(DomainId::parse(to_string(id)), id);
  EXPECT_EQ(DomainId::parse("4010A2F63C8E8897D052B3550BDD647F"), id);
  EXPECT_FALSE(DomainId::parse("4010a2f63c8e8897d052b3550bdd647").has_value());
  EXPECT_FALSE(DomainId::parse("not a domain id at all, thirty-two").has_value());
  EXPECT_FALSE(DomainId::parse("").has_value());
}

// The descriptors a store is handed until the catalogue exists.
TEST(DomainId, TheDescriptorsNameTheirPrefixAndRole) {
  const auto v1 = domain_v1_descriptor();
  EXPECT_EQ(v1.id, domain_v1);
  EXPECT_EQ(v1.name, "macha");
  EXPECT_EQ(v1.role, Role::member);
  const auto local = node_local_descriptor(node_of(0x10));
  EXPECT_EQ(local.id, node_local_domain(node_of(0x10)));
  EXPECT_EQ(local.name, "node-local");
  EXPECT_EQ(local.role, Role::member);
  EXPECT_STREQ(role_name(Role::member), "member");
  EXPECT_STREQ(role_name(Role::blind), "blind");
}

}  // namespace
}  // namespace dagda::domain
