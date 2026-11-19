//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/messages.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace dagda::store::message {
namespace {

using dannan::net::DecodeError;
using dannan::net::Frames;
using dannan::net::Lanes;
using dannan::net::Reader;
using dannan::net::Writer;

ObjectId read_id(Reader& reader) { return ObjectId{reader.fixed<32>()}; }

}  // namespace

void register_types(dannan::Routes& routes) {
  // Objects travel on DATA in any class but control; presence, CONTROL
  // objects and claims on CONTROL at whatever class the caller chose
  // (speculative by default, as protocol 23 sent them). have_control_objects
  // is control work and travels only in the control frame. Replies inherit
  // their request's class.
  routes.add(have_object, {"dagda.have_object", Lanes::control, Frames::any});
  routes.add(get_object, {"dagda.get_object", Lanes::data, Frames::not_control});
  routes.add(put_object, {"dagda.put_object", Lanes::data, Frames::not_control});
  routes.add(get_control_object, {"dagda.get_control_object", Lanes::control, Frames::any});
  routes.add(put_control_object, {"dagda.put_control_object", Lanes::control, Frames::any});
  routes.add(put_object_deferred, {"dagda.put_object_deferred", Lanes::data, Frames::not_control});
  routes.add(object_durability_barrier, {"dagda.object_durability_barrier", Lanes::data, Frames::not_control});
  routes.add(retain_objects, {"dagda.retain_objects", Lanes::control, Frames::any});
  routes.add(have_objects, {"dagda.have_objects", Lanes::control, Frames::any});
  routes.add(have_control_objects, {"dagda.have_control_objects", Lanes::control, Frames::control_only});
  routes.add(durability_covered, {"dagda.durability_covered", Lanes::data, Frames::not_control});
  // Repair's: a validated presence and a delete each take DATA credit on the
  // peer, so never the control frame; a trie question reads only the state
  // device.
  routes.add(delete_object, {"dagda.delete_object", Lanes::control, Frames::not_control});
  routes.add(have_valid_objects, {"dagda.have_valid_objects", Lanes::control, Frames::not_control});
  routes.add(trie_diff, {"dagda.trie_diff", Lanes::control, Frames::any});
  // A delete journals claims and unlinks files, never reads an object: the
  // control lane, any class but control.
  routes.add(remove_objects, {"dagda.remove_objects", Lanes::control, Frames::not_control});
  routes.add(remove_objects_reply, {"dagda.remove_objects_reply", Lanes::control, Frames::any});
  // An offer is answered from the index, as a presence is: the control lane.
  routes.add(offer_objects, {"dagda.offer_objects", Lanes::control, Frames::any});
  routes.add(offer_objects_reply, {"dagda.offer_objects_reply", Lanes::control, Frames::any});
  // A fetch of many reads DATA, as get_object does.
  routes.add(get_objects, {"dagda.get_objects", Lanes::data, Frames::not_control});
  routes.add(get_objects_reply, {"dagda.get_objects_reply", Lanes::data, Frames::any});
  routes.add(bool_reply, {"dagda.bool_reply", Lanes::both, Frames::any});
  routes.add(object_reply, {"dagda.object_reply", Lanes::data, Frames::any});
  routes.add(control_object_reply, {"dagda.control_object_reply", Lanes::control, Frames::any});
  routes.add(have_objects_reply, {"dagda.have_objects_reply", Lanes::control, Frames::any});
  routes.add(have_control_objects_reply, {"dagda.have_control_objects_reply", Lanes::control, Frames::any});
  routes.add(have_valid_objects_reply, {"dagda.have_valid_objects_reply", Lanes::control, Frames::any});
  routes.add(trie_diff_reply, {"dagda.trie_diff_reply", Lanes::control, Frames::any});
}

Bytes encode_id(const ObjectId& id) {
  Writer writer;
  writer.fixed(id.bytes);
  return writer.take();
}

ObjectId decode_id(std::span<const uint8_t> payload) {
  Reader reader(payload);
  const auto id = read_id(reader);
  reader.finish();
  return id;
}

void encode_principal(Writer& writer, const WritePrincipal& principal) {
  writer.u8(static_cast<uint8_t>(principal.principal.kind));
  writer.string(principal.principal.id);
  writer.string(principal.prefix);
}

WritePrincipal decode_principal(Reader& reader) {
  WritePrincipal out;
  const auto kind = reader.u8();
  if (kind > static_cast<uint8_t>(dannan::contract::Principal::Kind::directory)) throw DecodeError("invalid principal kind");
  out.principal.kind = static_cast<dannan::contract::Principal::Kind>(kind);
  out.principal.id = reader.string(4096);
  out.prefix = reader.string(4096);
  return out;
}

Bytes encode_object(const ObjectId& id, std::span<const uint8_t> data, const std::optional<WritePrincipal>& principal) {
  // The writer's bytes, laid out in one allocation of the exact size: an extent
  // written through a growing buffer is copied and its pages zeroed again.
  if (data.size() > 0xffffffffull) throw std::length_error("encoded blob too large");
  Writer tail;
  if (principal) encode_principal(tail, *principal);
  const auto& trailer = tail.data();
  Bytes out;
  out.reserve(id.bytes.size() + 4 + data.size() + trailer.size());
  out.insert(out.end(), id.bytes.begin(), id.bytes.end());
  const auto length = static_cast<uint32_t>(data.size());
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(length >> shift));
  out.insert(out.end(), data.begin(), data.end());
  out.insert(out.end(), trailer.begin(), trailer.end());
  return out;
}

ObjectPayload decode_object(std::span<const uint8_t> payload) {
  Reader reader(payload);
  ObjectPayload out;
  out.id = read_id(reader);
  out.data = reader.bytes(max_object_bytes);
  if (reader.remaining()) out.principal = decode_principal(reader);
  reader.finish();
  return out;
}

Bytes encode_bool(bool value) { return Bytes{static_cast<uint8_t>(value ? 1 : 0)}; }

bool decode_bool(std::span<const uint8_t> payload) {
  Reader reader(payload);
  const auto value = reader.u8();
  reader.finish();
  return value != 0;
}

Bytes encode_ids(std::span<const ObjectId> ids) {
  Writer writer;
  writer.u32(static_cast<uint32_t>(ids.size()));
  for (const auto& id : ids) writer.fixed(id.bytes);
  return writer.take();
}

std::vector<ObjectId> decode_ids(std::span<const uint8_t> payload, uint32_t maximum) {
  Reader reader(payload);
  const auto count = reader.u32();
  if (!count || count > maximum) throw DecodeError("invalid presence batch count");
  // The count is checked against what is there before anything is reserved.
  if (reader.remaining() != static_cast<size_t>(count) * 32) throw DecodeError("presence batch size does not match its count");
  std::vector<ObjectId> ids;
  ids.reserve(count);
  for (uint32_t i = 0; i < count; ++i) ids.push_back(read_id(reader));
  reader.finish();
  return ids;
}

Bytes encode_presence(const std::vector<bool>& present) {
  Writer writer;
  writer.u32(static_cast<uint32_t>(present.size()));
  for (const bool held : present) writer.u8(held ? 1 : 0);
  return writer.take();
}

std::vector<bool> decode_presence(std::span<const uint8_t> payload, size_t expected) {
  Reader reader(payload);
  if (reader.u32() != expected) throw DecodeError("presence reply count mismatch");
  std::vector<bool> out;
  out.reserve(expected);
  for (size_t i = 0; i < expected; ++i) out.push_back(reader.u8() != 0);
  reader.finish();
  return out;
}

Bytes encode_validity(const std::vector<Validity>& answers) {
  Writer writer;
  writer.u32(static_cast<uint32_t>(answers.size()));
  for (const auto answer : answers) writer.u8(static_cast<uint8_t>(answer));
  return writer.take();
}

std::vector<Validity> decode_validity(std::span<const uint8_t> payload, size_t expected) {
  Reader reader(payload);
  if (reader.u32() != expected) throw DecodeError("validated presence reply count mismatch");
  std::vector<Validity> out;
  out.reserve(expected);
  for (size_t i = 0; i < expected; ++i) {
    const auto held = reader.u8();
    out.push_back(held == 1 ? Validity::intact : held == 0 ? Validity::absent : Validity::cannot_say);
  }
  reader.finish();
  return out;
}

Bytes encode_ticket(const DurabilityTicket& ticket) {
  Writer writer;
  encode(writer, ticket);
  return writer.take();
}

DurabilityTicket decode_ticket(std::span<const uint8_t> payload) {
  Reader reader(payload);
  const auto ticket = decode_durability_ticket(reader);
  reader.finish();
  return ticket;
}

Bytes encode_barrier(const BarrierRequest& request) {
  Writer writer;
  encode(writer, request.ticket);
  if (!request.probe.empty()) {
    writer.u32(static_cast<uint32_t>(request.probe.size()));
    for (const auto& id : request.probe) writer.fixed(id.bytes);
  }
  return writer.take();
}

BarrierRequest decode_barrier(std::span<const uint8_t> payload) {
  Reader reader(payload);
  BarrierRequest out;
  out.ticket = decode_durability_ticket(reader);
  // An optional trailing id list turns a refusal into a probe.
  if (reader.remaining()) {
    const auto count = reader.u32();
    if (count > max_barrier_probe) throw DecodeError("too many durability probe ids");
    if (reader.remaining() != static_cast<size_t>(count) * 32) throw DecodeError("durability probe size does not match its count");
    out.probe.reserve(count);
    for (uint32_t i = 0; i < count; ++i) out.probe.push_back(read_id(reader));
  }
  reader.finish();
  return out;
}

Bytes encode_probe_answer(const BarrierProbeAnswer& answer) {
  Writer writer;
  writer.fixed(answer.epoch.bytes);
  writer.u32(static_cast<uint32_t>(answer.present.size()));
  for (const auto& [id, ticket] : answer.present) {
    writer.fixed(id.bytes);
    writer.u64(ticket.domain);
    writer.u64(ticket.generation);
    writer.u64(ticket.backend_instance);
  }
  return writer.take();
}

BarrierProbeAnswer decode_probe_answer(std::span<const uint8_t> payload) {
  Reader reader(payload);
  BarrierProbeAnswer out;
  out.epoch.bytes = reader.fixed<16>();
  const auto count = reader.u32();
  if (count > max_barrier_probe) throw DecodeError("too many reasserted ids");
  if (reader.remaining() != static_cast<size_t>(count) * (32 + 24)) throw DecodeError("reasserted ids do not match their count");
  out.present.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const auto id = read_id(reader);
    DurabilityTicket ticket;
    ticket.epoch = out.epoch;
    ticket.domain = reader.u64();
    ticket.generation = reader.u64();
    ticket.backend_instance = reader.u64();
    out.present.emplace_back(id, ticket);
  }
  reader.finish();
  return out;
}

Bytes encode_retain(const RetainRequest& request) {
  Writer writer;
  writer.u8(static_cast<uint8_t>(request.object_class));
  writer.fixed(request.dot.origin.bytes);
  writer.u64(request.dot.sequence);
  writer.u32(static_cast<uint32_t>(request.ids.size()));
  for (const auto& id : request.ids) writer.fixed(id.bytes);
  if (request.principal) encode_principal(writer, *request.principal);
  return writer.take();
}

RetainRequest decode_retain(std::span<const uint8_t> payload) {
  Reader reader(payload);
  RetainRequest out;
  const auto raw_class = reader.u8();
  if (raw_class < static_cast<uint8_t>(contract::RetentionClass::data) || raw_class > static_cast<uint8_t>(contract::RetentionClass::control))
    throw DecodeError("invalid retention object class");
  out.object_class = static_cast<contract::RetentionClass>(raw_class);
  out.dot.origin.bytes = reader.fixed<16>();
  out.dot.sequence = reader.u64();
  const auto count = reader.u32();
  if (!count || count > max_retain_batch) throw DecodeError("invalid retention object count");
  if (reader.remaining() < static_cast<size_t>(count) * 32) throw DecodeError("retention batch size does not match its count");
  out.ids.reserve(count);
  for (uint32_t i = 0; i < count; ++i) out.ids.push_back(read_id(reader));
  if (reader.remaining()) out.principal = decode_principal(reader);
  reader.finish();
  return out;
}

Bytes encode_remove(const RemoveRequest& request) {
  Writer writer;
  writer.u32(static_cast<uint32_t>(request.observed.size()));
  for (const auto& [node, sequence] : request.observed) {
    writer.fixed(node.bytes);
    writer.u64(sequence);
  }
  writer.u32(static_cast<uint32_t>(request.ids.size()));
  for (const auto& id : request.ids) writer.fixed(id.bytes);
  encode_principal(writer, request.writer);
  return writer.take();
}

RemoveRequest decode_remove(std::span<const uint8_t> payload) {
  Reader reader(payload);
  RemoveRequest out;
  const auto clocks = reader.u32();
  if (clocks > max_remove_batch) throw DecodeError("too many observed clock entries");
  for (uint32_t i = 0; i < clocks; ++i) {
    contract::NodeId node{reader.fixed<16>()};
    const auto sequence = reader.u64();
    if (node == contract::NodeId{} || !sequence || !out.observed.emplace(node, sequence).second) throw DecodeError("bad observed clock entry");
  }
  const auto count = reader.u32();
  if (!count || count > max_remove_batch) throw DecodeError("invalid remove object count");
  if (reader.remaining() < static_cast<size_t>(count) * 32) throw DecodeError("remove batch size does not match its count");
  out.ids.reserve(count);
  for (uint32_t i = 0; i < count; ++i) out.ids.push_back(read_id(reader));
  out.writer = decode_principal(reader);
  reader.finish();
  return out;
}

Bytes encode_removal(const ReplicaRemoval& removal) {
  Writer writer;
  writer.u8(removal.refused ? 1 : 0);
  if (removal.refused) {
    writer.u64(removal.scope_generation);
    writer.string(removal.why);
    return writer.take();
  }
  writer.u32(static_cast<uint32_t>(removal.objects.size()));
  for (const auto& object : removal.objects) {
    writer.fixed(object.id.bytes);
    writer.u8(static_cast<uint8_t>(object.state));
    writer.u16(static_cast<uint16_t>(std::min<size_t>(object.claims.size(), UINT16_MAX)));
    for (size_t i = 0; i < object.claims.size() && i < UINT16_MAX; ++i) {
      writer.fixed(object.claims[i].origin.bytes);
      writer.u64(object.claims[i].sequence);
    }
  }
  return writer.take();
}

ReplicaRemoval decode_removal(std::span<const uint8_t> payload) {
  Reader reader(payload);
  ReplicaRemoval out;
  const auto refused = reader.u8();
  if (refused > 1) throw DecodeError("invalid removal flag");
  if (refused) {
    out.refused = true;
    out.scope_generation = reader.u64();
    out.why = reader.string(4096);
    reader.finish();
    return out;
  }
  const auto count = reader.u32();
  if (count > max_remove_batch) throw DecodeError("too many removal answers");
  out.objects.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    RemovedObject object;
    object.id = read_id(reader);
    const auto state = reader.u8();
    if (state > static_cast<uint8_t>(RemoveState::kept_referenced)) throw DecodeError("invalid removal state");
    object.state = static_cast<RemoveState>(state);
    const auto claims = reader.u16();
    for (uint16_t c = 0; c < claims; ++c) {
      contract::RetentionDot dot;
      dot.origin.bytes = reader.fixed<16>();
      dot.sequence = reader.u64();
      object.claims.push_back(dot);
    }
    out.objects.push_back(std::move(object));
  }
  reader.finish();
  return out;
}

Bytes encode_offer_answers(const std::vector<OfferAnswer>& answers) {
  Writer writer;
  writer.u32(static_cast<uint32_t>(answers.size()));
  for (const auto answer : answers) writer.u8(static_cast<uint8_t>(answer));
  return writer.take();
}

std::vector<OfferAnswer> decode_offer_answers(std::span<const uint8_t> payload, size_t expected) {
  Reader reader(payload);
  if (reader.u32() != expected) throw DecodeError("offer reply count mismatch");
  std::vector<OfferAnswer> out;
  out.reserve(expected);
  for (size_t i = 0; i < expected; ++i) {
    const auto answer = reader.u8();
    if (answer > static_cast<uint8_t>(OfferAnswer::no_room)) throw DecodeError("invalid offer answer");
    out.push_back(static_cast<OfferAnswer>(answer));
  }
  reader.finish();
  return out;
}

Bytes encode_fetched(const std::vector<FetchedObject>& objects) {
  Writer writer;
  writer.u32(static_cast<uint32_t>(objects.size()));
  for (const auto& object : objects) {
    writer.u8(static_cast<uint8_t>(object.as));
    if (object.as == Fetched::sent) writer.bytes(object.data);
  }
  return writer.take();
}

std::vector<FetchedObject> decode_fetched(std::span<const uint8_t> payload, size_t expected) {
  Reader reader(payload);
  if (reader.u32() != expected) throw DecodeError("fetch reply count mismatch");
  std::vector<FetchedObject> out;
  out.reserve(expected);
  for (size_t i = 0; i < expected; ++i) {
    const auto as = reader.u8();
    if (as > static_cast<uint8_t>(Fetched::left)) throw DecodeError("invalid fetch answer");
    FetchedObject object;
    object.as = static_cast<Fetched>(as);
    if (object.as == Fetched::sent) object.data = reader.bytes(max_object_bytes);
    out.push_back(std::move(object));
  }
  reader.finish();
  return out;
}

}  // namespace dagda::store::message
