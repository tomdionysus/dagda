//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The lease record and the lease table (plan, "Store: leases and claimed
// collections"; requirements E2, E3, E4): the lease order, its join, its
// bytes (Macha's claim's, computed outside both codebases with Python's
// struct), and the table of named leases as a second instance of the
// claimed-collection template, with Dagda's own entry format.

#include "dagda/lease/lease.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "dagda/lease/lease_table.h"
#include "dagda/test/fixtures/lease.h"
#include "dannan/encoding/base.h"
#include "dannan/identity/crypto.h"

namespace dagda::lease {
namespace {

using dagda::test::node_of;
using dannan::encoding::hex;
using dannan::net::Bytes;
using dannan::net::DecodeError;
using dannan::net::Reader;
using dannan::net::Writer;

const NodeId a = node_of(0x11);
const NodeId b = node_of(0x22);

// Higher epoch; then the earlier claim; then the lower node id.
TEST(LeaseRecord, TheOrderIsEpochThenEarlierThenLowerNode) {
  EXPECT_TRUE(beats(Lease{b, 2, 900}, Lease{a, 1, 100}));
  EXPECT_TRUE(beats(Lease{b, 1, 290}, Lease{a, 1, 300}));
  EXPECT_TRUE(beats(Lease{a, 1, 300}, Lease{b, 1, 300}));
  EXPECT_FALSE(beats(Lease{a, 1, 300}, Lease{a, 1, 300}));
  EXPECT_EQ(merge(Lease{a, 1, 300}, Lease{b, 1, 290}), (Lease{b, 1, 290}));
  EXPECT_EQ(merge(std::nullopt, std::optional<Lease>(Lease{a, 1, 1})), (Lease{a, 1, 1}));
  EXPECT_EQ(merge(std::optional<Lease>{}, std::optional<Lease>{}), std::nullopt);
}

TEST(LeaseRecord, TheMergeIsAJoin) {
  std::vector<std::optional<Lease>> leases{std::nullopt};
  for (const auto& holder : {a, b, node_of(0x33)})
    for (uint64_t epoch : {1, 2, 3})
      for (uint64_t since : {100, 200}) leases.push_back(Lease{holder, epoch, since});
  for (const auto& x : leases) {
    EXPECT_EQ(merge(x, x), x);
    for (const auto& y : leases) {
      EXPECT_EQ(merge(x, y), merge(y, x));
      for (const auto& z : leases) EXPECT_EQ(merge(merge(x, y), z), merge(x, merge(y, z)));
    }
  }
}

TEST(LeaseRecord, TheBytesAreMachasClaims) {
  Writer writer;
  encode(writer, Lease{a, 2, 500});
  EXPECT_EQ(hex(writer.data()), "11111111111111111111111111111111000000000000000200000000000001f4");
  Writer optional;
  encode(optional, std::optional<Lease>{});
  encode(optional, std::optional<Lease>(Lease{b, 1, 1}));
  EXPECT_EQ(hex(optional.data()),
            "00"
            "01"
            "22222222222222222222222222222222"
            "0000000000000001"
            "0000000000000001");
  Reader reader(optional.data());
  EXPECT_EQ(decode_optional_lease(reader), std::nullopt);
  EXPECT_EQ(decode_optional_lease(reader), (Lease{b, 1, 1}));
  reader.finish();
  const Bytes bad{2};
  Reader refused(bad);
  EXPECT_THROW(decode_optional_lease(refused), DecodeError);
}

Bytes encoded(const LeaseTable& table) {
  Writer writer;
  table.encode(writer);
  return writer.take();
}

// Dagda's entry format, pinned: format 1, the name, the optional lease, the
// tombstone.
TEST(LeaseTable, AnEntryIsDagdasFormat) {
  Writer writer;
  LeaseEntryTraits::encode(writer, LeaseEntry{"athena/sweep", Lease{a, 3, 1793700000000}, 0});
  const auto expected = "010000000c617468656e612f737765657001111111111111111111111111111111110000000000000003000001a1a0d9f1000000000000000000";
  EXPECT_EQ(hex(writer.data()), expected);
  Reader reader(writer.data());
  EXPECT_EQ(LeaseEntryTraits::decode(reader), (LeaseEntry{"athena/sweep", Lease{a, 3, 1793700000000}, 0}));
  reader.finish();
  auto bad = writer.data();
  bad[0] = 2;
  Reader refused(bad);
  EXPECT_THROW(LeaseEntryTraits::decode(refused), DecodeError);
}

// The table is a claimed collection: joined per name by the lease order,
// three-way over the names, with its snapshot and delta forms.
TEST(LeaseTable, TablesJoinByTheLeaseOrder) {
  LeaseTable left, right;
  left.join(LeaseEntry{"lir/shard/1", Lease{a, 1, 100}, 0});
  left.join(LeaseEntry{"lir/shard/2", Lease{a, 1, 100}, 0});
  right.join(LeaseEntry{"lir/shard/1", Lease{b, 2, 900}, 0});
  right.join(LeaseEntry{"lir/shard/3", Lease{b, 1, 50}, 0});
  const auto merged = LeaseTable::merge(nullptr, left, right);
  ASSERT_EQ(merged.size(), 3u);
  EXPECT_EQ(merged.find("lir/shard/1")->lease, (Lease{b, 2, 900}));
  EXPECT_EQ(LeaseTable::merge(nullptr, right, left), merged);
  EXPECT_EQ(LeaseTable::merge(nullptr, merged, merged), merged);
  EXPECT_EQ(LeaseTable::merge(&left, merged, left), merged);

  // An entry one side dropped stands dropped against a base that held it
  // unchanged; one the other side changed since comes back.
  auto dropped = merged;
  dropped.erase(std::string("lir/shard/2"));
  EXPECT_FALSE(LeaseTable::merge(&merged, dropped, merged).find("lir/shard/2"));
  auto changed = merged;
  changed.join(LeaseEntry{"lir/shard/2", Lease{b, 2, 1000}, 0});
  EXPECT_TRUE(LeaseTable::merge(&merged, dropped, changed).find("lir/shard/2"));

  const auto bytes = encoded(merged);
  Reader reader(bytes);
  EXPECT_EQ(LeaseTable::decode(reader), merged);
  reader.finish();

  Writer delta;
  LeaseTable::encode(delta, LeaseTable::diff(left, merged));
  Reader delta_reader(delta.data());
  auto applied = left;
  applied.apply(LeaseTable::decode_delta(delta_reader));
  delta_reader.finish();
  EXPECT_EQ(applied, merged);
}

TEST(LeaseTable, AReleasedNameIsSweptAfterItsWindow) {
  LeaseTable table;
  table.join(LeaseEntry{"athena/sweep", Lease{a, 1, 100}, 1000});
  table.join(LeaseEntry{"athena/trunk", Lease{a, 1, 100}, 0});
  const uint64_t week = 7ull * 24 * 3600 * 1000;
  EXPECT_TRUE(table.expired(1000 + week).empty());
  EXPECT_EQ(table.expired(1001 + week), std::vector<std::string>{"athena/sweep"});
}

}  // namespace
}  // namespace dagda::lease
