//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/messages.h"

#include <algorithm>

#include "dannan/net/codec.h"

namespace dagda::heads::message {

using dannan::net::Frames;
using dannan::net::Lanes;
using dannan::net::Reader;
using dannan::net::Registration;
using dannan::net::Writer;

void register_types(dannan::Routes& routes, const Numbers& numbers) {
  const auto add = [&](MessageType type, const char* name, bool ordered = false, bool notification = false) {
    Registration registration;
    registration.name = name;
    registration.lanes = Lanes::control;
    registration.frames = Frames::any;
    registration.ordered = ordered;
    registration.notification = notification;
    routes.add(type, std::move(registration));
  };
  add(numbers.put_history_entry, "dagda.heads.put_history_entry", true);
  add(numbers.put_commit, "dagda.heads.put_commit", true);
  add(numbers.accept_commit, "dagda.heads.accept_commit", true);
  add(numbers.get_history_entry, "dagda.heads.get_history_entry");
  add(numbers.has_history_entry, "dagda.heads.has_history_entry");
  add(numbers.get_heads, "dagda.heads.get_heads");
  add(numbers.get_history_record, "dagda.heads.get_history_record");
  add(numbers.get_committed, "dagda.heads.get_committed");
  add(numbers.get_current, "dagda.heads.get_current");
  add(numbers.get_identity, "dagda.heads.get_identity");
  add(numbers.notice, "dagda.heads.notice", false, true);
  add(numbers.record_reply, "dagda.heads.record_reply");
  add(numbers.identity_reply, "dagda.heads.identity_reply");
  add(numbers.history_entry_reply, "dagda.heads.history_entry_reply");
  add(numbers.heads_reply, "dagda.heads.heads_reply");
}

Bytes encode_hash(const Hash256& hash) {
  Writer writer;
  writer.fixed(hash.bytes);
  return writer.take();
}

Hash256 decode_hash(std::span<const uint8_t> payload) {
  Reader reader(payload);
  Hash256 hash;
  hash.bytes = reader.fixed<32>();
  reader.finish();
  return hash;
}

Bytes encode_notice(uint64_t generation) {
  Writer writer;
  writer.u64(generation);
  return writer.take();
}

uint64_t decode_notice(std::span<const uint8_t> payload) {
  Reader reader(payload);
  const auto generation = reader.u64();
  reader.finish();
  return generation;
}

Bytes encode_identity(const HeadIdentity& identity) {
  Writer writer;
  writer.u64(identity.generation);
  writer.fixed(identity.hash.bytes);
  return writer.take();
}

HeadIdentity decode_identity(std::span<const uint8_t> payload) {
  Reader reader(payload);
  HeadIdentity identity;
  identity.generation = reader.u64();
  identity.hash.bytes = reader.fixed<32>();
  reader.finish();
  return identity;
}

Bytes with_space(Bytes payload, const std::optional<SpaceId>& space) {
  if (space) payload.insert(payload.end(), space->bytes.begin(), space->bytes.end());
  return payload;
}

std::optional<SpaceId> space_of_empty(std::span<const uint8_t> payload) {
  if (payload.empty()) return std::nullopt;
  if (payload.size() != sizeof(SpaceId{}.bytes)) throw DecodeError("a request with no body carries nothing but a space id");
  SpaceId space;
  std::copy(payload.begin(), payload.end(), space.bytes.begin());
  return space;
}

}  // namespace dagda::heads::message
