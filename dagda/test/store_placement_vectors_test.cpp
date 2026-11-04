//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Placement across nodes against Macha's own answers: every key, every R, with
// and without failure domains, capacities unequal and zero. A difference here
// changes an object's owners, and repair would re-home a disk. The vectors were
// generated from Macha's code; data/placement-vectors.txt names the commit.

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "dagda/store/placement.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/node_id.h"
#include "dannan/placement/placement.h"

namespace dagda::store {
namespace {

using dannan::identity::NodeId;
using dannan::membership::NodeRecord;

TEST(StorePlacementVectors, DataOrderIsMachasOnEveryVector) {
  std::ifstream in(std::string(DAGDA_SOURCE_DIR) + "/dagda/test/data/placement-vectors.txt");
  ASSERT_TRUE(in) << "no vectors";
  std::vector<NodeRecord> nodes;
  std::string name;
  std::vector<uint8_t> key;
  size_t orders = 0;
  size_t rendezvous = 0;
  size_t logical = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string kind;
    fields >> kind;
    if (kind == "case") {
      fields >> name;
      nodes.clear();
    } else if (kind == "node") {
      std::string id;
      uint64_t capacity = 0;
      std::string domain;
      std::string host;
      fields >> id >> capacity >> domain >> host;
      auto record = placement_candidate(NodeId::parse(id).value(), capacity, domain == "-" ? std::string() : domain);
      record.host = host;
      nodes.push_back(std::move(record));
    } else if (kind == "logical") {
      size_t replicas = 0;
      uint64_t expected = 0;
      fields >> replicas >> expected;
      EXPECT_EQ(dannan::placement::logical_capacity(nodes, replicas, capacity_advert), expected) << name << " R=" << replicas;
      ++logical;
    } else if (kind == "key") {
      std::string hex;
      fields >> hex;
      key = dannan::identity::unhex(hex).value();
    } else if (kind == "order" || kind == "rendezvous") {
      size_t replicas = 0;
      fields >> replicas;
      std::vector<NodeId> expected;
      for (size_t index = 0; fields >> index;) expected.push_back(nodes.at(index).id);
      std::vector<NodeId> got;
      if (kind == "order") {
        got = data_order(key, nodes, replicas);
        ++orders;
      } else {
        for (const auto& record : dannan::placement::rendezvous(key, nodes, replicas)) got.push_back(record.id);
        ++rendezvous;
      }
      EXPECT_EQ(got, expected) << kind << " " << name << " R=" << replicas << " key " << dannan::identity::hex(key);
    } else {
      ADD_FAILURE() << "unknown line: " << line;
    }
  }
  // The whole file was read: a truncated one would pass every comparison.
  EXPECT_EQ(orders, 1352u);
  EXPECT_EQ(rendezvous, 1352u);
  EXPECT_EQ(logical, 26u);
}

}  // namespace
}  // namespace dagda::store
