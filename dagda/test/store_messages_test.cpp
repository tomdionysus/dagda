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

#include "dagda/ledger/trie_diff.h"
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

// Repair's messages (step 3b). have_valid_objects: the presence request's
// bytes, at most 16 ids; its reply a count then a byte each, 1 intact, 0
// absent, 2 cannot say (and any other byte read as cannot say, as Macha's
// reader does). delete_object: the id. trie_diff: ledger/trie_diff's
// question, here a root question (kind 0, trie 0, a zero hash, no
// prefixes).
TEST(StoreMessages, RepairsMessagesAreProtocol23s) {
  const std::vector<ObjectId> ids{id_a, id_b};
  const auto request =
      "00000002f110686f239efc75e4b3d14dbfd31600eaa4de19089d633bbea49bfec8740ac799f12af7fc1e445f724cbb357fb31cfd220ff639ad9380b4fd0d6a5d8435f8e6";
  EXPECT_EQ(hex(encode_ids(ids)), request);
  EXPECT_EQ(decode_ids(unhex(request), max_valid_batch), ids);
  std::vector<ObjectId> seventeen(17, id_a);
  EXPECT_THROW(decode_ids(encode_ids(seventeen), max_valid_batch), dannan::net::DecodeError);

  const std::vector<Validity> answers{Validity::intact, Validity::absent, Validity::cannot_say};
  EXPECT_EQ(hex(encode_validity(answers)), "00000003010002");
  EXPECT_EQ(decode_validity(unhex("00000003010002"), 3), answers);
  EXPECT_EQ(decode_validity(unhex("0000000107"), 1), std::vector<Validity>{Validity::cannot_say});
  EXPECT_THROW(decode_validity(unhex("00000003010002"), 2), dannan::net::DecodeError);
  EXPECT_THROW(decode_validity(unhex("000000030100"), 3), dannan::net::DecodeError);

  EXPECT_EQ(hex(encode_id(id_a)), "f110686f239efc75e4b3d14dbfd31600eaa4de19089d633bbea49bfec8740ac7");

  ledger::TrieQuestion root;
  EXPECT_EQ(hex(ledger::encode_trie_question(root)), "0000" + std::string(64, '0') + "00000000");
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
  EXPECT_EQ(static_cast<uint16_t>(delete_object), 512 + 13);
  EXPECT_EQ(static_cast<uint16_t>(have_valid_objects), 512 + 47);
  EXPECT_EQ(static_cast<uint16_t>(trie_diff), 512 + 49);
  EXPECT_EQ(static_cast<uint16_t>(have_valid_objects_reply), 512 + 123);
  EXPECT_EQ(static_cast<uint16_t>(trie_diff_reply), 512 + 125);

  // Registered on a table as Dannan's node registers them, with the lanes
  // protocol 23 gave each.
  auto table = dannan::net::RouteTable::with_dannan_types();
  struct Recorder final : dannan::Routes {
    explicit Recorder(dannan::net::RouteTable& t) : table(t) {}
    dannan::net::RouteTable& table;
    void add(dannan::net::MessageType type, dannan::net::Registration registration, dannan::net::RequestHandler) override { table.add(type, registration); }
    void retire(dannan::net::MessageType) override {}
    void bind(dannan::net::MessageType, dannan::net::RequestHandler) override {}
    void bind_deferred(dannan::net::MessageType, dannan::net::DeferringHandler) override {}
    void unbind(dannan::net::MessageType) override {}
    std::shared_ptr<dannan::net::CallHandle> call(const dannan::membership::NodeRecord&, dannan::net::MessageType, dannan::net::Bytes, dannan::sched::FrameType,
                                                  std::chrono::milliseconds, boost::asio::any_io_executor, Done, dannan::net::CallWatch) override {
      return {};
    }
    std::shared_ptr<dannan::net::CallHandle> call(const dannan::net::Endpoint&, dannan::net::MessageType, dannan::net::Bytes, dannan::sched::FrameType,
                                                  std::chrono::milliseconds, boost::asio::any_io_executor, Done, dannan::net::CallWatch) override {
      return {};
    }
    dannan::net::CallOutcome call_and_wait(const dannan::membership::NodeRecord&, dannan::net::MessageType, dannan::net::Bytes, dannan::sched::FrameType,
                                           std::chrono::milliseconds, dannan::net::CallWatch) override {
      return {};
    }
    dannan::net::CallOutcome call_and_wait(const dannan::net::Endpoint&, dannan::net::MessageType, dannan::net::Bytes, dannan::sched::FrameType,
                                           std::chrono::milliseconds, dannan::net::CallWatch) override {
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
  // Repair's: a validated presence and a delete take DATA credit on the
  // peer, so never in the control frame; a trie question in any class.
  for (const auto type : {delete_object, have_valid_objects}) {
    EXPECT_TRUE(table.allowed(type, Lane::control, FrameType::speculative)) << table.name(type);
    EXPECT_FALSE(table.allowed(type, Lane::control, FrameType::control)) << table.name(type);
    EXPECT_FALSE(table.allowed(type, Lane::data, FrameType::speculative)) << table.name(type);
  }
  EXPECT_TRUE(table.allowed(trie_diff, Lane::control, FrameType::speculative));
  EXPECT_TRUE(table.allowed(trie_diff, Lane::control, FrameType::control));
  EXPECT_FALSE(table.allowed(trie_diff, Lane::data, FrameType::speculative));
  EXPECT_TRUE(table.allowed(have_control_objects, Lane::control, FrameType::control));
  EXPECT_FALSE(table.allowed(have_control_objects, Lane::control, FrameType::speculative));
  EXPECT_TRUE(table.allowed(object_reply, Lane::data, FrameType::foreground));
  // Registering twice is a clash, refused.
  EXPECT_THROW(register_types(routes), std::invalid_argument);
}

// Step 6: the principal and the prefix a write or a claim is checked under
// ride after protocol 23's fields, which keep their bytes; without them a
// payload is the sending node's own write (every protocol 23 payload).
TEST(StoreMessages, APutOrAClaimCarriesItsPrincipalAfterMachasFields) {
  const WritePrincipal alice{{dannan::contract::Principal::Kind::directory, "alice"}, "lugh/secrets"};
  const auto plain = encode_object(id_a, dagda_bytes);
  const auto with = encode_object(id_a, dagda_bytes, alice);
  EXPECT_EQ(hex(plain), "f110686f239efc75e4b3d14dbfd31600eaa4de19089d633bbea49bfec8740ac7000000056461676461");
  EXPECT_EQ(hex(with), hex(plain) + "01" + "00000005616c696365" + "0000000c6c7567682f73656372657473");
  EXPECT_FALSE(decode_object(plain).principal.has_value());
  const auto decoded = decode_object(with);
  EXPECT_EQ(decoded.id, id_a);
  EXPECT_EQ(decoded.data, dagda_bytes);
  EXPECT_EQ(decoded.principal, std::optional<WritePrincipal>(alice));
  // A node principal is kind 0.
  const WritePrincipal node{{dannan::contract::Principal::Kind::node, "00112233445566778899aabbccddeeff"}, "macha"};
  EXPECT_EQ(decode_object(encode_object(id_b, macha_bytes, node)).principal, std::optional<WritePrincipal>(node));
  auto bad_kind = with;
  bad_kind[plain.size()] = 7;
  EXPECT_THROW(decode_object(bad_kind), dannan::net::DecodeError);
  auto truncated = with;
  truncated.pop_back();
  EXPECT_THROW(decode_object(truncated), dannan::net::DecodeError);

  contract::RetentionDot dot;
  dot.origin.bytes.fill(0x11);
  dot.sequence = 5;
  const auto claim = encode_retain({contract::RetentionClass::data, dot, {id_a}, std::nullopt});
  const auto claim_with = encode_retain({contract::RetentionClass::data, dot, {id_a}, alice});
  EXPECT_EQ(hex(claim_with), hex(claim) + "01" + "00000005616c696365" + "0000000c6c7567682f73656372657473");
  EXPECT_FALSE(decode_retain(claim).principal.has_value());
  const auto decoded_claim = decode_retain(claim_with);
  EXPECT_EQ(decoded_claim.ids, std::vector<ObjectId>{id_a});
  EXPECT_EQ(decoded_claim.principal, std::optional<WritePrincipal>(alice));
  auto short_claim = claim;
  short_claim.pop_back();
  EXPECT_THROW(decode_retain(short_claim), dannan::net::DecodeError);
}

// Step 7b, the instant delete, Dagda's own types from 768: the request is the
// observed clock as the claims' release frame carries it, the ids and the
// writer; the reply a replica's refusal (its generation and reason) or each
// object's state with the claims that remain.
TEST(StoreMessages, ADeleteCarriesItsClockIdsAndWriterAndItsAnswerEachObjectsState) {
  EXPECT_EQ(static_cast<uint16_t>(remove_objects), 769);
  EXPECT_EQ(static_cast<uint16_t>(remove_objects_reply), 770);
  const WritePrincipal alice{{dannan::contract::Principal::Kind::directory, "alice"}, "lugh/secrets"};
  contract::NodeId origin;
  origin.bytes.fill(0x11);
  const auto request = encode_remove({{{origin, 5}}, {id_a}, alice});
  const auto expected =
      "00000001" + std::string(32, '1') + "0000000000000005" + "00000001" + to_string(id_a) + "01" + "00000005616c696365" + "0000000c6c7567682f73656372657473";
  EXPECT_EQ(hex(request), expected);
  const auto back = decode_remove(request);
  EXPECT_EQ(back.observed, (contract::RetentionClock{{origin, 5}}));
  EXPECT_EQ(back.ids, std::vector<ObjectId>{id_a});
  EXPECT_EQ(back.writer, alice);
  EXPECT_THROW(decode_remove(encode_remove({{{origin, 0}}, {id_a}, alice})), dannan::net::DecodeError);
  EXPECT_THROW(decode_remove(encode_remove({{}, {}, alice})), dannan::net::DecodeError);
  auto unwritten = request;
  unwritten.resize(unwritten.size() - 31);
  EXPECT_THROW(decode_remove(unwritten), dannan::net::DecodeError);

  ReplicaRemoval answer;
  answer.objects = {{id_a, RemoveState::kept_by_claim, {{origin, 5}}}, {id_b, RemoveState::removed, {}}};
  const auto reply = encode_removal(answer);
  EXPECT_EQ(hex(reply),
            "00" + std::string("00000002") + to_string(id_a) + "02" + "0001" + std::string(32, '1') + "0000000000000005" + to_string(id_b) + "00" + "0000");
  const auto decoded = decode_removal(reply);
  EXPECT_FALSE(decoded.refused);
  ASSERT_EQ(decoded.objects.size(), 2u);
  EXPECT_EQ(decoded.objects[0].state, RemoveState::kept_by_claim);
  EXPECT_EQ(decoded.objects[0].claims, (std::vector<contract::RetentionDot>{{origin, 5}}));
  EXPECT_EQ(decoded.objects[1].id, id_b);
  EXPECT_EQ(decoded.objects[1].state, RemoveState::removed);
  ReplicaRemoval refused;
  refused.refused = true;
  refused.scope_generation = 42;
  refused.why = "nope";
  EXPECT_EQ(hex(encode_removal(refused)), "01000000000000002a000000046e6f7065");
  const auto said_no = decode_removal(encode_removal(refused));
  EXPECT_TRUE(said_no.refused);
  EXPECT_EQ(said_no.scope_generation, 42u);
  EXPECT_EQ(said_no.why, "nope");
  // A replica answers only the four states it can know; refused and
  // unreachable are the caller's words.
  auto bad_state = reply;
  bad_state[1 + 4 + 32] = static_cast<uint8_t>(RemoveState::refused);
  EXPECT_THROW(decode_removal(bad_state), dannan::net::DecodeError);
}

}  // namespace
}  // namespace dagda::store::message
