//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dagda/contract/claim_store.h"
#include "dagda/store/durability_ticket.h"
#include "dagda/store/object_id.h"
#include "dagda/store/remove.h"
#include "dannan/context.h"
#include "dannan/contract/scope.h"
#include "dannan/net/codec.h"
#include "dannan/net/routes.h"

// The store's messages on Dannan (requirements 4.2, S30). Each type is Dagda's
// own registration from 512, numbered 512 plus the protocol-23 number Macha
// gave it, so a trace reads across; Dannan's framing, lanes and refusals
// carry them, and the payloads are protocol 23's byte for byte. Protocol 23's
// framing, handshake and numbers are gone (the maintainer, 2026-11-04): only the payload
// bytes are kept. Refusals are Dannan's error replies; a reply of nothing
// more than "done" is Dannan's ok.
namespace dagda::store::message {

using dannan::net::MessageType;

// Requests. DATA lane: get_object, put_object, put_object_deferred,
// object_durability_barrier and durability_covered; CONTROL lane: the rest.
inline constexpr MessageType have_object{515};
inline constexpr MessageType get_object{516};
inline constexpr MessageType put_object{517};
inline constexpr MessageType delete_object{525};
inline constexpr MessageType get_control_object{531};
inline constexpr MessageType put_control_object{532};
inline constexpr MessageType put_object_deferred{534};
inline constexpr MessageType object_durability_barrier{535};
inline constexpr MessageType retain_objects{544};
inline constexpr MessageType have_objects{552};
inline constexpr MessageType have_control_objects{557};
inline constexpr MessageType have_valid_objects{559};
// A question to the peer's held trie (ledger/trie_diff.h's encodings).
inline constexpr MessageType trie_diff{561};
// Dagda's own, with no protocol-23 number: whether a durability ticket's
// generation is durable now, answered without waiting (a ticket's status, as
// object_durability_barrier is its wait). The request is a ticket in its
// protocol-23 form; the reply a bool_reply.
inline constexpr MessageType durability_covered{768};
// The instant delete (remove.h): the caller's observed clock, the ids and
// the writer; the reply is remove_objects_reply.
inline constexpr MessageType remove_objects{769};
inline constexpr MessageType remove_objects_reply{770};

// Replies.
inline constexpr MessageType bool_reply{615};
inline constexpr MessageType object_reply{616};
inline constexpr MessageType control_object_reply{619};
inline constexpr MessageType have_objects_reply{630};
inline constexpr MessageType have_control_objects_reply{633};
inline constexpr MessageType have_valid_objects_reply{635};
inline constexpr MessageType trie_diff_reply{637};

// Bounds every handler checks, Macha's (storage_server.cpp, net.hpp).
inline constexpr size_t max_object_bytes = 128ULL * 1024 * 1024;
inline constexpr uint32_t max_presence_batch = 200000;
inline constexpr uint32_t max_barrier_probe = 4096;
inline constexpr uint32_t max_retain_batch = 1000000;
// Each id of have_valid_objects is a full read on the peer, so a request is
// bounded to one handler-sized job (Macha's have_valid_objects_max).
inline constexpr uint32_t max_valid_batch = 16;
// The ids one remove_objects carries; a larger delete is sent in parts.
inline constexpr uint32_t max_remove_batch = 4096;

// Registers every store type's wire rules (not their handlers) on `routes`.
// Before the node starts: Dannan refuses registration after. Throws as
// Routes::add does.
void register_types(dannan::Routes& routes);

// have_object, get_object, get_control_object, delete_object: the id.
Bytes encode_id(const ObjectId& id);
ObjectId decode_id(std::span<const uint8_t> payload);

// Who a write or a claim is for and under what prefix (dannan/contract/
// scope.h), as put_object, put_object_deferred and retain_objects carry it
// to the storing replicas (the maintainer, 2026-11-04 evening): appended after
// protocol 23's fields, whose bytes stay, as u8 kind, the id as a string,
// the prefix as a string. A payload without it is the sending node's own
// write under the domain's name, which is what every protocol 23 payload
// was.
struct WritePrincipal {
  dannan::contract::Principal principal;
  std::string prefix;
  bool operator==(const WritePrincipal& other) const {
    return principal.kind == other.principal.kind && principal.id == other.principal.id && prefix == other.prefix;
  }
};
void encode_principal(dannan::net::Writer& writer, const WritePrincipal& principal);
// Throws dannan::net::DecodeError for a kind out of range.
WritePrincipal decode_principal(dannan::net::Reader& reader);

// put_object, put_object_deferred, put_control_object, object_reply,
// control_object_reply: the id, then the bytes as a blob; for the two puts,
// the principal when given.
struct ObjectPayload {
  ObjectId id{};
  Bytes data;
  std::optional<WritePrincipal> principal;
};
Bytes encode_object(const ObjectId& id, std::span<const uint8_t> data, const std::optional<WritePrincipal>& principal = std::nullopt);
// Throws dannan::net::DecodeError past max_object_bytes.
ObjectPayload decode_object(std::span<const uint8_t> payload);

// bool_reply: one byte, 0 or 1.
Bytes encode_bool(bool value);
bool decode_bool(std::span<const uint8_t> payload);

// have_objects, have_control_objects: u32 count, then the ids. Throws
// dannan::net::DecodeError for none or more than `maximum`.
Bytes encode_ids(std::span<const ObjectId> ids);
std::vector<ObjectId> decode_ids(std::span<const uint8_t> payload, uint32_t maximum = max_presence_batch);

// have_objects_reply, have_control_objects_reply: u32 count, then a byte
// each. Throws dannan::net::DecodeError unless it answers `expected` ids.
Bytes encode_presence(const std::vector<bool>& present);
std::vector<bool> decode_presence(std::span<const uint8_t> payload, size_t expected);

// have_valid_objects_reply: u32 count, then a byte each: 1 held and read
// back intact, 0 not held, 2 cannot say now. Any other byte reads as
// cannot say, as Macha reads it. Throws dannan::net::DecodeError unless it
// answers `expected` ids. The request is encode_ids, at most max_valid_batch.
enum class Validity : uint8_t { absent = 0, intact = 1, cannot_say = 2 };
Bytes encode_validity(const std::vector<Validity>& answers);
std::vector<Validity> decode_validity(std::span<const uint8_t> payload, size_t expected);

// put_object_deferred's reply: the ticket. durability_covered's request is
// the same bytes.
Bytes encode_ticket(const DurabilityTicket& ticket);
DurabilityTicket decode_ticket(std::span<const uint8_t> payload);

// object_durability_barrier: the ticket, then, to ask again after the
// peer's process changed, u32 count and the ids (at most max_barrier_probe).
struct BarrierRequest {
  DurabilityTicket ticket;
  std::vector<ObjectId> probe;
};
Bytes encode_barrier(const BarrierRequest& request);
BarrierRequest decode_barrier(std::span<const uint8_t> payload);

// The reply to a probe: the peer's current epoch, u32 count, then each id it
// holds with its fresh domain, generation and backend instance.
struct BarrierProbeAnswer {
  dannan::identity::NodeId epoch;
  std::vector<std::pair<ObjectId, DurabilityTicket>> present;
};
Bytes encode_probe_answer(const BarrierProbeAnswer& answer);
// Each ticket carries the answer's epoch.
BarrierProbeAnswer decode_probe_answer(std::span<const uint8_t> payload);

// retain_objects: u8 class, the dot's origin (16) and sequence (u64), u32
// count, then the ids; then the principal when given.
struct RetainRequest {
  contract::RetentionClass object_class = contract::RetentionClass::data;
  contract::RetentionDot dot;
  std::vector<ObjectId> ids;
  std::optional<WritePrincipal> principal;
};
Bytes encode_retain(const RetainRequest& request);
// Throws dannan::net::DecodeError for a class out of range, or no ids or
// more than max_retain_batch.
RetainRequest decode_retain(std::span<const uint8_t> payload);

// remove_objects: u32 n, then (node 16, u64 sequence) x n, the caller's
// observed clock as the claims' release frame carries it; u32 count and the
// ids; the writer (always present).
struct RemoveRequest {
  contract::RetentionClock observed;
  std::vector<ObjectId> ids;
  WritePrincipal writer;
};
Bytes encode_remove(const RemoveRequest& request);
// Throws dannan::net::DecodeError for a clock entry with no node or
// sequence, a repeated node, no ids or more than max_remove_batch.
RemoveRequest decode_remove(std::span<const uint8_t> payload);

// remove_objects_reply: u8 refused; refused, u64 scope generation and the
// reason as a string; otherwise u32 count, then per object the id, u8 state
// (RemoveState), u16 n and the remaining claims (origin 16, u64 sequence) x n.
Bytes encode_removal(const ReplicaRemoval& removal);
// Throws dannan::net::DecodeError for a state out of range or past the
// bounds.
ReplicaRemoval decode_removal(std::span<const uint8_t> payload);

}  // namespace dagda::store::message
