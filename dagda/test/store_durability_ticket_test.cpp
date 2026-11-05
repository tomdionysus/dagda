//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement S15: a ticket is {process epoch, domain, generation, backend
// incarnation}, and one from another process or incarnation names nothing.

#include <gtest/gtest.h>

#include "dagda/store/durability_ticket.h"
#include "dannan/identity/crypto.h"
#include "dannan/net/codec.h"

namespace dagda::store {
namespace {

DurabilityTicket sample() {
  DurabilityTicket ticket;
  for (uint8_t i = 0; i < 16; ++i) ticket.epoch.bytes[i] = i;
  ticket.domain = 1;
  ticket.generation = 0x0102030405060708ull;
  ticket.backend_instance = 3;
  return ticket;
}

// Macha's StoragePool::DurabilityToken::valid(): a domain and an incarnation.
TEST(StoreDurabilityTicket, IsValidOnlyNamingADomainAndAnIncarnation) {
  EXPECT_FALSE(DurabilityTicket{}.valid());
  auto ticket = sample();
  EXPECT_TRUE(ticket.valid());
  ticket.backend_instance = 0;
  EXPECT_FALSE(ticket.valid());
  ticket = sample();
  ticket.domain = 0;
  EXPECT_FALSE(ticket.valid());
}

TEST(StoreDurabilityTicket, NamesOnlyTheProcessDomainAndIncarnationThatIssuedIt) {
  const auto ticket = sample();
  EXPECT_TRUE(ticket.names(ticket.epoch, 1, 3));
  auto restarted = ticket.epoch;
  restarted.bytes[0] ^= 1;
  EXPECT_FALSE(ticket.names(restarted, 1, 3));
  EXPECT_FALSE(ticket.names(ticket.epoch, 2, 3));
  EXPECT_FALSE(ticket.names(ticket.epoch, 1, 4));
  EXPECT_FALSE(DurabilityTicket{}.names({}, 0, 0));
}

// Protocol 23's bytes: epoch 16, then domain, generation and instance as
// big-endian u64 (storage_server.cpp:247-252).
TEST(StoreDurabilityTicket, EncodesAsProtocol23) {
  dannan::net::Writer writer;
  encode(writer, sample());
  EXPECT_EQ(dannan::identity::hex(writer.data()),
            "000102030405060708090a0b0c0d0e0f"
            "0000000000000001"
            "0102030405060708"
            "0000000000000003");
  dannan::net::Reader reader(writer.data());
  EXPECT_EQ(decode_durability_ticket(reader), sample());
  reader.finish();
}

TEST(StoreDurabilityTicket, RefusesATruncatedTicket) {
  dannan::net::Writer writer;
  encode(writer, sample());
  const auto bytes = writer.take();
  dannan::net::Reader reader{std::span<const uint8_t>(bytes).first(bytes.size() - 1)};
  EXPECT_THROW((void)decode_durability_ticket(reader), dannan::net::DecodeError);
}

}  // namespace
}  // namespace dagda::store
