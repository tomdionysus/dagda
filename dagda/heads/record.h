//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "dagda/store/object_id.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/node_id.h"
#include "dannan/net/codec.h"

// A head's record and everything about it that is the same for any owner
// (requirements H1, H2, H3, H5, H12): its identity and hash, its history entry,
// its certificate, its dots, and the sections every owner's record frames alike.
// Byte for byte Macha's metadata/metadata at 9a08846. Pure; waits on nothing;
// thread-safe.
namespace dagda::heads {

using dannan::identity::Hash256;
using dannan::identity::NodeId;
using dannan::net::DecodeError;
using store::Bytes;

// Every codec's bound on a commit's extra parents: encode, decode, frame and
// import (H3). No live path of Macha's names more than one.
inline constexpr size_t merge_parents_limit = 64;

// Immutable bytes with value semantics and shared storage, so copying a record
// is cheap whatever its payload. Assigning takes new storage: an alias never
// sees a change.
class SharedBytes {
 public:
  SharedBytes() = default;
  SharedBytes(Bytes value) : _bytes(std::make_shared<const Bytes>(std::move(value))) {}

  SharedBytes& operator=(Bytes value) {
    _bytes = std::make_shared<const Bytes>(std::move(value));
    return *this;
  }

  template <class Iterator>
  void assign(Iterator first, Iterator last) {
    _bytes = std::make_shared<const Bytes>(first, last);
  }

  size_t size() const noexcept { return _bytes->size(); }
  bool empty() const noexcept { return _bytes->empty(); }
  const uint8_t* data() const noexcept { return _bytes->data(); }
  Bytes::const_iterator begin() const noexcept { return _bytes->begin(); }
  Bytes::const_iterator end() const noexcept { return _bytes->end(); }
  // A contiguous range: std::span takes it as one. No conversion operator besides,
  // or GCC sees two ways to make the span and warns at each.
  std::span<const uint8_t> span() const noexcept { return *_bytes; }

  friend bool operator==(const SharedBytes& a, const SharedBytes& b) { return a._bytes == b._bytes || *a._bytes == *b._bytes; }
  friend bool operator==(const SharedBytes& a, const Bytes& b) { return *a._bytes == b; }
  friend bool operator==(const Bytes& a, const SharedBytes& b) { return a == *b._bytes; }

 private:
  std::shared_ptr<const Bytes> _bytes{std::make_shared<const Bytes>()};
};

// A commit: immutable, and named by the hash of its generation, its primary
// parent and its payload.
struct Record {
  uint64_t generation = 0;
  Hash256 previous{};
  Hash256 hash{};
  SharedBytes payload;
};

struct HeadIdentity {
  uint64_t generation = 0;
  Hash256 hash{};
  auto operator<=>(const HeadIdentity&) const = default;
};

// Evidence a commit reached this node's standing: `replicas` hold it. `required`
// zero is genesis on a founder, which names nobody; otherwise it is how many held
// the commit at acceptance. No vote.
struct Acceptance {
  uint64_t generation = 0;
  Hash256 hash{};
  uint32_t required = 0;
  std::vector<NodeId> replicas;
  auto operator<=>(const Acceptance&) const = default;
};

// A record as history holds it: whole, or as a delta on its primary parent.
struct HistoryEntry {
  enum class Body : uint8_t { full = 1, delta = 2 };

  uint64_t generation = 0;
  Hash256 previous{};
  Hash256 hash{};
  bool previous_known = false;
  std::vector<Hash256> merge_parents;
  Body body = Body::full;
  Bytes payload;
  auto operator<=>(const HistoryEntry&) const = default;
};

struct HistoryLinks {
  uint64_t generation = 0;
  Hash256 previous{};
  bool previous_known = false;
  std::vector<Hash256> merge_parents;
  HistoryEntry::Body body = HistoryEntry::Body::full;
};

// One mutation of one author. A sequence of zero is no dot.
struct Dot {
  NodeId author{};
  uint64_t sequence = 0;
  explicit operator bool() const noexcept { return sequence != 0; }
  auto operator<=>(const Dot&) const = default;
};

// The highest sequence a head has incorporated, per author.
using HeadClock = std::map<NodeId, uint64_t>;

// Whether a head with this clock has incorporated the mutation.
bool clock_covers(const HeadClock& clock, const Dot& dot);

// SHA-256 of generation (u64be), previous, payload length (u32be), payload,
// streamed so no payload-sized buffer is made. Throws std::runtime_error for a
// payload past 2^32 - 1 bytes.
Hash256 record_hash(uint64_t generation, const Hash256& previous, std::span<const uint8_t> payload);
bool valid_record(const Record& record);
// generation 1, no parent: what a space begins at, given the owner's constant.
Record genesis_record(std::span<const uint8_t> payload);

Bytes encode_record(const Record& record);
// Throws DecodeError for anything else, a record whose hash does not hold included.
Record decode_record(std::span<const uint8_t> bytes);

// Throws std::runtime_error for a certificate that does not hold. The replicas
// are written sorted and unique.
Bytes encode_acceptance(const Acceptance& acceptance);
Acceptance decode_acceptance(std::span<const uint8_t> bytes);
Bytes encode_acceptance_set(const std::vector<Acceptance>& values);
std::vector<Acceptance> decode_acceptance_set(std::span<const uint8_t> bytes);

Bytes encode_history_entry(const HistoryEntry& entry);
HistoryEntry decode_history_entry(std::span<const uint8_t> bytes);

// The links a body needs to be materialised, a subset of all its links: a whole
// body needs none, however many parents it names, a delta its primary.
std::vector<Hash256> materialization_dependencies(const HistoryEntry& entry);

// The one rule for a delta over its primary parent, every reader's and writer's.
// A merge is numbered max(parents) + 1, so a delta may sit many generations above
// its primary; the hash binds the generation, so going up is enough.
constexpr bool delta_succession_valid(uint64_t parent_generation, uint64_t child_generation) noexcept { return parent_generation < child_generation; }

// The sections an owner's record frames as every record does, for the owner's
// codec to call. A clock: a u32 count, then each author and sequence. A zero
// sequence, a repeated author or more than 65536 entries is a DecodeError.
void encode_clock(dannan::net::Writer& out, const HeadClock& clock);
HeadClock decode_clock(dannan::net::Reader& in);
// Merge parents: a u32 count, then each hash. Past merge_parents_limit is a
// std::runtime_error writing and a DecodeError reading.
void encode_merge_parents(dannan::net::Writer& out, const std::vector<Hash256>& parents);
std::vector<Hash256> decode_merge_parents(dannan::net::Reader& in);

// A form's number from its 8-byte magic: `prefix` then a digit from 1, or 'A' for
// ten and 'B' for eleven. Zero for anything else. The owner names its prefix.
int form_version(std::span<const uint8_t> bytes, std::string_view prefix);

}  // namespace dagda::heads
