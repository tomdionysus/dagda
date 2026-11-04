//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/messages.h"

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
  routes.add(bool_reply, {"dagda.bool_reply", Lanes::both, Frames::any});
  routes.add(object_reply, {"dagda.object_reply", Lanes::data, Frames::any});
  routes.add(control_object_reply, {"dagda.control_object_reply", Lanes::control, Frames::any});
  routes.add(have_objects_reply, {"dagda.have_objects_reply", Lanes::control, Frames::any});
  routes.add(have_control_objects_reply, {"dagda.have_control_objects_reply", Lanes::control, Frames::any});
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

Bytes encode_object(const ObjectId& id, std::span<const uint8_t> data) {
  Writer writer;
  writer.fixed(id.bytes);
  writer.bytes(data);
  return writer.take();
}

ObjectPayload decode_object(std::span<const uint8_t> payload) {
  Reader reader(payload);
  ObjectPayload out;
  out.id = read_id(reader);
  out.data = reader.bytes(max_object_bytes);
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
  if (reader.remaining() != static_cast<size_t>(count) * 32) throw DecodeError("retention batch size does not match its count");
  out.ids.reserve(count);
  for (uint32_t i = 0; i < count; ++i) out.ids.push_back(read_id(reader));
  reader.finish();
  return out;
}

}  // namespace dagda::store::message
