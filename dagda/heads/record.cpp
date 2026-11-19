//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/record.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace dagda::heads {
namespace {

using dannan::net::Reader;
using dannan::net::Writer;

constexpr uint32_t replicas_limit = 1000000;
constexpr uint32_t clock_limit = 65536;

void hash_u32(dannan::identity::Sha256Hasher& hash, uint32_t value) {
  std::array<uint8_t, 4> encoded{};
  for (int shift = 24, i = 0; shift >= 0; shift -= 8, ++i) encoded[static_cast<size_t>(i)] = static_cast<uint8_t>(value >> shift);
  hash.update(encoded);
}

void hash_u64(dannan::identity::Sha256Hasher& hash, uint64_t value) {
  std::array<uint8_t, 8> encoded{};
  for (int shift = 56, i = 0; shift >= 0; shift -= 8, ++i) encoded[static_cast<size_t>(i)] = static_cast<uint8_t>(value >> shift);
  hash.update(encoded);
}

}  // namespace

bool clock_covers(const HeadClock& clock, const Dot& dot) {
  const auto found = clock.find(dot.author);
  return found != clock.end() && found->second >= dot.sequence;
}

Hash256 record_hash(uint64_t generation, const Hash256& previous, std::span<const uint8_t> payload) {
  if (payload.size() > std::numeric_limits<uint32_t>::max()) throw std::runtime_error("encoded blob too large");
  dannan::identity::Sha256Hasher hash;
  hash_u64(hash, generation);
  hash.update(previous.bytes);
  hash_u32(hash, static_cast<uint32_t>(payload.size()));
  hash.update(payload);
  return hash.finish();
}

bool valid_record(const Record& record) { return record.generation != 0 && record_hash(record.generation, record.previous, record.payload) == record.hash; }

Record genesis_record(std::span<const uint8_t> payload) {
  Record record;
  record.generation = 1;
  record.payload.assign(payload.begin(), payload.end());
  record.hash = record_hash(1, record.previous, record.payload);
  return record;
}

Bytes encode_record(const Record& record) {
  Writer writer;
  writer.u64(record.generation);
  writer.fixed(record.previous.bytes);
  writer.fixed(record.hash.bytes);
  writer.bytes(record.payload);
  return writer.take();
}

Record decode_record(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  Record record;
  record.generation = reader.u64();
  record.previous.bytes = reader.fixed<32>();
  record.hash.bytes = reader.fixed<32>();
  record.payload = reader.bytes();
  reader.finish();
  if (!valid_record(record)) throw DecodeError("bad metadata hash");
  return record;
}

Bytes encode_acceptance(const Acceptance& input) {
  Acceptance value = input;
  std::sort(value.replicas.begin(), value.replicas.end());
  value.replicas.erase(std::unique(value.replicas.begin(), value.replicas.end()), value.replicas.end());
  if (value.generation == 0 || value.hash == Hash256{}) throw std::runtime_error("bad metadata acceptance identity");
  if (std::any_of(value.replicas.begin(), value.replicas.end(), [](const NodeId& replica) { return replica == NodeId{}; }))
    throw std::runtime_error("metadata acceptance contains empty replica identity");
  if (value.required != 0 && value.replicas.size() < value.required) throw std::runtime_error("metadata acceptance does not satisfy write floor");
  if (value.required == 0 && !value.replicas.empty()) throw std::runtime_error("metadata acceptance without a floor cannot name replicas");
  if (value.replicas.size() > replicas_limit) throw std::runtime_error("too many metadata acceptance replicas");

  Writer writer;
  writer.u64(value.generation);
  writer.fixed(value.hash.bytes);
  writer.u32(value.required);
  writer.u32(static_cast<uint32_t>(value.replicas.size()));
  for (const auto& replica : value.replicas) writer.fixed(replica.bytes);
  return writer.take();
}

Acceptance decode_acceptance(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  Acceptance value;
  value.generation = reader.u64();
  value.hash.bytes = reader.fixed<32>();
  value.required = reader.u32();
  const auto count = reader.u32();
  if (count > replicas_limit) throw DecodeError("too many metadata acceptance replicas");
  value.replicas.reserve(std::min<size_t>(count, reader.remaining() / 16));
  for (uint32_t i = 0; i < count; ++i) {
    NodeId replica;
    replica.bytes = reader.fixed<16>();
    value.replicas.push_back(replica);
  }
  reader.finish();
  if (value.generation == 0 || value.hash == Hash256{}) throw DecodeError("bad metadata acceptance identity");
  if (!std::is_sorted(value.replicas.begin(), value.replicas.end()) || std::adjacent_find(value.replicas.begin(), value.replicas.end()) != value.replicas.end())
    throw DecodeError("metadata acceptance replicas are not canonical");
  if (std::any_of(value.replicas.begin(), value.replicas.end(), [](const NodeId& replica) { return replica == NodeId{}; }))
    throw DecodeError("metadata acceptance contains empty replica identity");
  if (value.required != 0) {
    if (value.replicas.size() < value.required) throw DecodeError("metadata acceptance does not satisfy write floor");
  } else if (!value.replicas.empty()) {
    throw DecodeError("metadata acceptance without a floor cannot name replicas");
  }
  return value;
}

Bytes encode_acceptance_set(const std::vector<Acceptance>& values) {
  if (values.size() > replicas_limit) throw std::runtime_error("too many metadata accepted heads");
  Writer writer;
  writer.u32(static_cast<uint32_t>(values.size()));
  for (const auto& value : values) writer.bytes(encode_acceptance(value));
  return writer.take();
}

std::vector<Acceptance> decode_acceptance_set(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  const auto count = reader.u32();
  if (count > replicas_limit) throw DecodeError("too many metadata accepted heads");
  std::vector<Acceptance> values;
  // Bound the untrusted count by the input: each is at least a length, a
  // generation, a hash and two counts.
  values.reserve(std::min<size_t>(count, reader.remaining() / 52));
  std::set<Hash256> seen;
  for (uint32_t i = 0; i < count; ++i) {
    auto value = decode_acceptance(reader.bytes());
    if (!seen.insert(value.hash).second) throw DecodeError("duplicate metadata accepted head");
    values.push_back(std::move(value));
  }
  reader.finish();
  return values;
}

Bytes encode_history_entry(const HistoryEntry& entry) {
  Writer writer;
  writer.u8(static_cast<uint8_t>(entry.body));
  writer.u64(entry.generation);
  writer.fixed(entry.previous.bytes);
  writer.fixed(entry.hash.bytes);
  writer.u8(entry.previous_known ? 1 : 0);
  if (entry.merge_parents.size() > merge_parents_limit) throw std::runtime_error("too many metadata history merge parents");
  writer.u32(static_cast<uint32_t>(entry.merge_parents.size()));
  for (const auto& parent : entry.merge_parents) writer.fixed(parent.bytes);
  writer.bytes(entry.payload);
  return writer.take();
}

HistoryEntry decode_history_entry(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  HistoryEntry entry;
  const auto body = reader.u8();
  if (body < static_cast<uint8_t>(HistoryEntry::Body::full) || body > static_cast<uint8_t>(HistoryEntry::Body::delta))
    throw DecodeError("bad metadata history body kind");
  entry.body = static_cast<HistoryEntry::Body>(body);
  entry.generation = reader.u64();
  entry.previous.bytes = reader.fixed<32>();
  entry.hash.bytes = reader.fixed<32>();
  entry.previous_known = reader.u8() != 0;
  const auto parents = reader.u32();
  if (parents > merge_parents_limit) throw DecodeError("too many metadata history merge parents");
  entry.merge_parents.reserve(parents);
  for (uint32_t i = 0; i < parents; ++i) {
    Hash256 parent;
    parent.bytes = reader.fixed<32>();
    entry.merge_parents.push_back(parent);
  }
  entry.payload = reader.bytes();
  reader.finish();
  if (entry.generation == 0 || entry.hash == Hash256{}) throw DecodeError("bad metadata history identity");
  if (entry.generation <= 1 && entry.previous_known) throw DecodeError("genesis metadata history has a predecessor");
  if (entry.body == HistoryEntry::Body::delta && !entry.previous_known) throw DecodeError("metadata delta history is missing predecessor");
  for (const auto& parent : entry.merge_parents) {
    if (parent == Hash256{} || parent == entry.hash) throw DecodeError("bad metadata merge parent");
  }
  return entry;
}

std::vector<Hash256> materialization_dependencies(const HistoryEntry& entry) {
  if (entry.body == HistoryEntry::Body::delta) return {entry.previous};
  return {};
}

void encode_clock(Writer& out, const HeadClock& clock) {
  out.u32(static_cast<uint32_t>(clock.size()));
  for (const auto& [node, sequence] : clock) {
    out.fixed(node.bytes);
    out.u64(sequence);
  }
}

HeadClock decode_clock(Reader& in) {
  HeadClock clock;
  const auto count = in.u32();
  if (count > clock_limit) throw DecodeError("too many clock origins");
  for (uint32_t i = 0; i < count; ++i) {
    const NodeId node{in.fixed<16>()};
    const auto sequence = in.u64();
    if (sequence == 0 || !clock.emplace(node, sequence).second) throw DecodeError("bad clock entry");
  }
  return clock;
}

void encode_merge_parents(Writer& out, const std::vector<Hash256>& parents) {
  if (parents.size() > merge_parents_limit) throw std::runtime_error("too many metadata merge parents");
  out.u32(static_cast<uint32_t>(parents.size()));
  for (const auto& parent : parents) out.fixed(parent.bytes);
}

std::vector<Hash256> decode_merge_parents(Reader& in) {
  const auto count = in.u32();
  if (count > merge_parents_limit) throw DecodeError("too many metadata merge parents");
  std::vector<Hash256> parents;
  parents.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Hash256 parent;
    parent.bytes = in.fixed<32>();
    parents.push_back(parent);
  }
  return parents;
}

int form_version(std::span<const uint8_t> bytes, std::string_view prefix) {
  if (bytes.size() < prefix.size() + 1 ||
      !std::equal(prefix.begin(), prefix.end(), bytes.begin(), [](char a, uint8_t b) { return static_cast<uint8_t>(a) == b; }))
    return 0;
  const auto digit = bytes[prefix.size()];
  if (digit == 'A') return 10;
  if (digit == 'B') return 11;
  if (digit < '1' || digit > '9') return 0;
  return digit - '0';
}

}  // namespace dagda::heads
