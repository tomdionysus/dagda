//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The store's messages: protocol 23's payloads byte for byte (requirements
// 4.2), pinned by known answers computed outside both codebases (Python's
// hashlib and struct, big-endian, from the layout in storage_server.cpp and
// distributed_store.cpp at Macha develop 5a06d3c), and Dagda's numbers from
// 512 on Dannan's routes.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "dagda/store/messages.h"
#include "dannan/identity/crypto.h"
#include "dannan/net/routes.h"

namespace dagda::store::message {
namespace {

using dannan::identity::hex;

Bytes unhex(const std::string& text) { return dannan::identity::unhex(text).value(); }

const Bytes dagda_bytes{'d', 'a', 'g', 'd', 'a'};
const Bytes macha_bytes{'m', 'a', 'c', 'h', 'a'};
const auto id_a = object_id(dagda_bytes);
const auto id_b = object_id(macha_bytes);

DurabilityTicket ticket_of(uint8_t first, uint64_t domain, uint64_t generation, uint64_t instance) {
  DurabilityTicket ticket;
  for (size_t i = 0; i < 16; ++i) ticket.epoch.bytes[i] = static_cast<uint8_t>(first + i);
  ticket.domain = domain;
  ticket.generation = generation;
  ticket.backend_instance = instance;
  return ticket;
}

TEST(StoreMessages, IdsAreTheHashesTheAnswersWereComputedOver) {
  EXPECT_EQ(to_string(id_a), "f110686f239efc75e4b3d14dbfd31600eaa4de19089d633bbea49bfec8740ac7");
  EXPECT_EQ(to_string(id_b), "99f12af7fc1e445f724cbb357fb31cfd220ff639ad9380b4fd0d6a5d8435f8e6");
}

// have_object, get_object, get_control_object: the id alone.
TEST(StoreMessages, AnIdRequestIsTheIdsBytes) {
  EXPECT_EQ(hex(encode_id(id_a)), to_string(id_a));
  EXPECT_EQ(decode_id(unhex(to_string(id_b))), id_b);
  EXPECT_THROW(decode_id(Bytes(31, 0)), dannan::net::DecodeError);
  EXPECT_THROW(decode_id(Bytes(33, 0)), dannan::net::DecodeError);
}

// put_object, put_object_deferred, put_control_object, object_reply,
// control_object_reply: id, u32 length, bytes.
TEST(StoreMessages, AnObjectIsItsIdThenItsBytesAsABlob) {
  const auto expected = "f110686f239efc75e4b3d14dbfd31600eaa4de19089d633bbea49bfec8740ac7000000056461676461";
  EXPECT_EQ(hex(encode_object(id_a, dagda_bytes)), expected);
  const auto decoded = decode_object(unhex(expected));
  EXPECT_EQ(decoded.id, id_a);
  EXPECT_EQ(decoded.data, dagda_bytes);
  auto truncated = unhex(expected);
  truncated.pop_back();
  EXPECT_THROW(decode_object(truncated), dannan::net::DecodeError);
}

TEST(StoreMessages, ABoolReplyIsOneByte) {
  EXPECT_EQ(encode_bool(true), Bytes{1});
  EXPECT_EQ(encode_bool(false), Bytes{0});
  EXPECT_TRUE(decode_bool(Bytes{1}));
  EXPECT_FALSE(decode_bool(Bytes{0}));
  EXPECT_THROW(decode_bool(Bytes{}), dannan::net::DecodeError);
}

// have_objects and have_control_objects, and their replies.
TEST(StoreMessages, PresenceIsACountThenIdsAndACountThenBytes) {
  const std::vector<ObjectId> ids{id_a, id_b};
  const auto request =
      "00000002f110686f239efc75e4b3d14dbfd31600eaa4de19089d633bbea49bfec8740ac799f12af7fc1e445f724cbb357fb31cfd220ff639ad9380b4fd0d6a5d8435f8e6";
  EXPECT_EQ(hex(encode_ids(ids)), request);
  EXPECT_EQ(decode_ids(unhex(request)), ids);
  EXPECT_EQ(hex(encode_presence({true, false})), "000000020100");
  EXPECT_EQ(decode_presence(unhex("000000020100"), 2), (std::vector<bool>{true, false}));
  // Macha's bounds: none, or past 200,000, is refused; a reply must answer
  // every id asked.
  EXPECT_THROW(decode_ids(unhex("00000000")), dannan::net::DecodeError);
  EXPECT_THROW(decode_ids(unhex("00030d41")), dannan::net::DecodeError);
  EXPECT_THROW(decode_presence(unhex("000000020100"), 3), dannan::net::DecodeError);
  // A count claiming more ids than the payload holds is refused before
  // anything is reserved for them.
  EXPECT_THROW(decode_ids(unhex("00000003" + to_string(id_a))), dannan::net::DecodeError);
}

// put_object_deferred's reply, and the head of object_durability_barrier:
// epoch 16, domain, generation, backend instance.
TEST(StoreMessages, ATicketIsTheEpochThenThreeU64s) {
  const auto ticket = ticket_of(0, 3, 7, 2);
  const auto expected = "000102030405060708090a0b0c0d0e0f000000000000000300000000000000070000000000000002";
  EXPECT_EQ(hex(encode_ticket(ticket)), expected);
  EXPECT_EQ(decode_ticket(unhex(expected)), ticket);
  EXPECT_EQ(hex(encode_barrier({ticket, {}})), expected);
  EXPECT_EQ(decode_barrier(unhex(expected)).ticket, ticket);
  EXPECT_TRUE(decode_barrier(unhex(expected)).probe.empty());
}

// A barrier carrying ids is a probe; its answer names the peer's new epoch
// and a fresh ticket for each id it holds.
TEST(StoreMessages, ABarrierProbeAndItsAnswer) {
  const auto ticket = ticket_of(0, 3, 7, 2);
  const auto probe = "000102030405060708090a0b0c0d0e0f00000000000000030000000000000007000000000000000200000001" + to_string(id_a);
  EXPECT_EQ(hex(encode_barrier({ticket, {id_a}})), probe);
  const auto decoded = decode_barrier(unhex(probe));
  EXPECT_EQ(decoded.ticket, ticket);
  EXPECT_EQ(decoded.probe, std::vector<ObjectId>{id_a});

  BarrierProbeAnswer answer;
  for (size_t i = 0; i < 16; ++i) answer.epoch.bytes[i] = static_cast<uint8_t>(15 - i);
  auto fresh = ticket_of(0, 4, 9, 5);
  fresh.epoch = answer.epoch;
  answer.present.emplace_back(id_a, fresh);
  const auto expected = "0f0e0d0c0b0a0908070605040302010000000001" + to_string(id_a) + "000000000000000400000000000000090000000000000005";
  EXPECT_EQ(hex(encode_probe_answer(answer)), expected);
  const auto back = decode_probe_answer(unhex(expected));
  EXPECT_EQ(back.epoch, answer.epoch);
  ASSERT_EQ(back.present.size(), 1u);
  EXPECT_EQ(back.present[0].first, id_a);
  EXPECT_EQ(back.present[0].second, fresh);
  // Past Macha's 4096 a probe is refused.
  EXPECT_THROW(decode_barrier(unhex("000102030405060708090a0b0c0d0e0f000000000000000300000000000000070000000000000002"
                                    "00001001")),
               dannan::net::DecodeError);
}

// retain_objects: class, origin, sequence, count, ids.
TEST(StoreMessages, AClaimIsClassDotThenIds) {
  RetainRequest request;
  request.object_class = contract::RetentionClass::control;
  request.dot.origin.bytes.fill(0xaa);
  request.dot.sequence = 42;
  request.ids = {id_a, id_b};
  const auto expected = "02aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa000000000000002a00000002" + to_string(id_a) + to_string(id_b);
  EXPECT_EQ(hex(encode_retain(request)), expected);
  const auto back = decode_retain(unhex(expected));
  EXPECT_EQ(back.object_class, contract::RetentionClass::control);
  EXPECT_EQ(back.dot, request.dot);
  EXPECT_EQ(back.ids, request.ids);
  auto bad_class = unhex(expected);
  bad_class[0] = 3;
  EXPECT_THROW(decode_retain(bad_class), dannan::net::DecodeError);
}

// Dagda's numbers: 512 plus protocol 23's, all of them user types, none of
// them two things.
TEST(StoreMessages, TheNumbersAre512PlusProtocol23s) {
  EXPECT_EQ(static_cast<uint16_t>(have_object), 512 + 3);
  EXPECT_EQ(static_cast<uint16_t>(get_object), 512 + 4);
  EXPECT_EQ(static_cast<uint16_t>(put_object), 512 + 5);
  EXPECT_EQ(static_cast<uint16_t>(get_control_object), 512 + 19);
  EXPECT_EQ(static_cast<uint16_t>(put_control_object), 512 + 20);
  EXPECT_EQ(static_cast<uint16_t>(put_object_deferred), 512 + 22);
  EXPECT_EQ(static_cast<uint16_t>(object_durability_barrier), 512 + 23);
  EXPECT_EQ(static_cast<uint16_t>(retain_objects), 512 + 32);
  EXPECT_EQ(static_cast<uint16_t>(have_objects), 512 + 40);
  EXPECT_EQ(static_cast<uint16_t>(have_control_objects), 512 + 45);
  EXPECT_EQ(static_cast<uint16_t>(bool_reply), 512 + 103);
  EXPECT_EQ(static_cast<uint16_t>(object_reply), 512 + 104);
  EXPECT_EQ(static_cast<uint16_t>(control_object_reply), 512 + 107);
  EXPECT_EQ(static_cast<uint16_t>(have_objects_reply), 512 + 118);
  EXPECT_EQ(static_cast<uint16_t>(have_control_objects_reply), 512 + 121);

  // Registered on a table as Dannan's node registers them, with the lanes
  // protocol 23 gave each.
  auto table = dannan::net::RouteTable::with_dannan_types();
  struct Recorder final : dannan::Routes {
    explicit Recorder(dannan::net::RouteTable& t) : table(t) {}
    dannan::net::RouteTable& table;
    void add(dannan::net::MessageType type, dannan::net::Registration registration, dannan::net::RequestHandler) override { table.add(type, registration); }
    void bind(dannan::net::MessageType, dannan::net::RequestHandler) override {}
    void unbind(dannan::net::MessageType) override {}
    std::shared_ptr<dannan::net::CallHandle> call(const dannan::membership::NodeRecord&, dannan::net::MessageType, dannan::net::Bytes, dannan::sched::FrameType,
                                                  std::chrono::milliseconds, boost::asio::any_io_executor, Done) override {
      return {};
    }
    std::shared_ptr<dannan::net::CallHandle> call(const dannan::net::Endpoint&, dannan::net::MessageType, dannan::net::Bytes, dannan::sched::FrameType,
                                                  std::chrono::milliseconds, boost::asio::any_io_executor, Done) override {
      return {};
    }
    dannan::net::CallOutcome call_and_wait(const dannan::membership::NodeRecord&, dannan::net::MessageType, dannan::net::Bytes, dannan::sched::FrameType,
                                           std::chrono::milliseconds) override {
      return {};
    }
    dannan::net::CallOutcome call_and_wait(const dannan::net::Endpoint&, dannan::net::MessageType, dannan::net::Bytes, dannan::sched::FrameType,
                                           std::chrono::milliseconds) override {
      return {};
    }
  } routes(table);
  register_types(routes);
  using dannan::net::Lane;
  using dannan::sched::FrameType;
  for (const auto type : {get_object, put_object, put_object_deferred, object_durability_barrier}) {
    EXPECT_TRUE(table.allowed(type, Lane::data, FrameType::loader)) << table.name(type);
    EXPECT_FALSE(table.allowed(type, Lane::control, FrameType::loader)) << table.name(type);
    EXPECT_FALSE(table.allowed(type, Lane::data, FrameType::control)) << table.name(type);
  }
  for (const auto type : {have_object, get_control_object, put_control_object, retain_objects, have_objects}) {
    EXPECT_TRUE(table.allowed(type, Lane::control, FrameType::speculative)) << table.name(type);
    EXPECT_FALSE(table.allowed(type, Lane::data, FrameType::speculative)) << table.name(type);
  }
  EXPECT_TRUE(table.allowed(have_control_objects, Lane::control, FrameType::control));
  EXPECT_FALSE(table.allowed(have_control_objects, Lane::control, FrameType::speculative));
  EXPECT_TRUE(table.allowed(object_reply, Lane::data, FrameType::foreground));
  // Registering twice is a clash, refused.
  EXPECT_THROW(register_types(routes), std::invalid_argument);
}

}  // namespace
}  // namespace dagda::store::message
