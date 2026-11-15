//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "dagda/lease/lease.h"
#include "dannan/net/codec.h"

// The claimed collection (plan, "Store: leases and claimed collections";
// requirements E1 to E6): a map of records any node may write, which every
// node joins the same way, with no operator and no conflict record.
//
// A record is merged field group by field group. Each group has its own
// clock (Tom, 2026-11-04: lease and claimed-collection records keep field
// groups with their own clocks), and the group whose clock is greater is
// taken whole; equal clocks take the greater record. The record's lease is
// the lease order's winner (lease.h), and its tombstone the greater. A
// dominating state (a cancel that beats any later pause) is a group whose
// clock leads with it (`dominating` below).
//
// The owner supplies, as a traits type:
//   Record      totally ordered (operator<=>): set-once fields, and any group
//               whose clocks tie, resolve to the greater record.
//   Key         the map's key, ordered; a default-constructed key is no key.
//   DuplicateKey
//   key_of(record)
//   groups      a tuple of field_group(clock, take): `clock(record)` returns
//               the group's clock, `take(out, from)` copies the group.
//   lease_of(record), set_lease(record, lease)
//   removed_unix_ms(record), set_removed_unix_ms(record, ms): the tombstone.
//   duplicate_key(record): the key two records must not share while live;
//               empty for a record the rule does not apply to.
//   keeps(x, y): of two records sharing a duplicate key, x keeps it before
//               y; a strict order over distinct keys.
//   lose(loser, keeper): what the rule does to every record but the keeper.
//               The merge is associative only if this does not depend on
//               which record kept it (Macha's does, so three concurrent
//               duplicates settle only at the next merge).
//   encode(writer, record), decode(reader), encode_key(writer, key),
//   decode_key(reader): the codec, with its own bounds.
//   max_records: the bound at decode (and at encode of a whole collection).
//   retention: how long a tombstone is kept before the sweep may erase it.
//
// Where the records live: in a head the owner owns (step 4). The collection
// is the value a head space carries: merge(base?, left, right) is its merge,
// encode/decode its snapshot form and diff/encode(Delta)/decode_delta/apply
// its delta form. An owner whose head is one blob (Macha) calls them from its
// own codec and merge; the head is not built here.
namespace dagda::lease {

template <class Clock, class Take>
struct FieldGroup {
  Clock clock;
  Take take;
};

template <class Clock, class Take>
constexpr FieldGroup<Clock, Take> field_group(Clock clock, Take take) {
  return {clock, take};
}

// A group's clock as the plan names it: last writer wins by (changed_ms,
// node).
inline std::tuple<uint64_t, NodeId> written(uint64_t changed_ms, const NodeId& node) { return {changed_ms, node}; }

// A clock a dominating state outranks whatever its time: (dominant,
// changed_ms, node).
inline std::tuple<bool, uint64_t, NodeId> dominating(bool dominant, uint64_t changed_ms, const NodeId& node) { return {dominant, changed_ms, node}; }

template <class T>
concept CollectionTraits =
    requires(const typename T::Record& record, typename T::Record& out, dannan::net::Writer& writer, dannan::net::Reader& reader, const typename T::Key& key) {
      requires std::totally_ordered<typename T::Record>;
      requires std::totally_ordered<typename T::Key>;
      requires std::totally_ordered<typename T::DuplicateKey>;
      { T::key_of(record) } -> std::convertible_to<typename T::Key>;
      T::groups;
      { T::lease_of(record) } -> std::convertible_to<std::optional<Lease>>;
      T::set_lease(out, std::optional<Lease>{});
      { T::removed_unix_ms(record) } -> std::convertible_to<uint64_t>;
      T::set_removed_unix_ms(out, uint64_t{});
      { T::duplicate_key(record) } -> std::convertible_to<std::optional<typename T::DuplicateKey>>;
      { T::keeps(record, record) } -> std::convertible_to<bool>;
      T::lose(out, record);
      T::encode(writer, record);
      { T::decode(reader) } -> std::convertible_to<typename T::Record>;
      T::encode_key(writer, key);
      { T::decode_key(reader) } -> std::convertible_to<typename T::Key>;
      { T::max_records } -> std::convertible_to<uint32_t>;
      { T::retention } -> std::convertible_to<std::chrono::milliseconds>;
    };

// A value: not thread-safe; copy it, as a head's snapshot is copied.
template <CollectionTraits Traits>
class ClaimedCollection {
 public:
  using Record = typename Traits::Record;
  using Key = typename Traits::Key;
  using Map = std::map<Key, Record, std::less<>>;

  // A head's change to the collection: records written whole, keys erased.
  struct Delta {
    Map upserts;
    std::vector<Key> erasures;
    bool empty() const noexcept { return upserts.empty() && erasures.empty(); }
    bool operator==(const Delta&) const = default;
  };

  ClaimedCollection() = default;
  // Throws std::invalid_argument for a record held under another key.
  explicit ClaimedCollection(Map records) : _records(std::move(records)) {
    for (const auto& [key, record] : _records)
      if (!(Traits::key_of(record) == key)) throw std::invalid_argument("claimed collection record keyed by another key");
  }

  const Map& records() const noexcept { return _records; }
  size_t size() const noexcept { return _records.size(); }
  bool empty() const noexcept { return _records.empty(); }
  template <class K>
  const Record* find(const K& key) const {
    const auto found = _records.find(key);
    return found == _records.end() ? nullptr : &found->second;
  }

  // An owner's write: joins `record` onto the one held under its key, or
  // adds it. Returns the record as it stands.
  const Record& join(const Record& record) {
    const auto key = Traits::key_of(record);
    const auto found = _records.find(key);
    if (found == _records.end()) return _records.emplace(key, record).first->second;
    found->second = merge(found->second, record);
    return found->second;
  }
  template <class K>
  bool erase(const K& key) {
    const auto found = _records.find(key);
    if (found == _records.end()) return false;
    _records.erase(found);
    return true;
  }

  // Tombstones past the retention window at `now_unix_ms`: what the sweep
  // erases, strictly past the window. A stamp from a clock ahead of this
  // one is fresh (Tom, 2026-11-04): Macha's unsigned `now - removed` wrapped
  // and swept it at once.
  std::vector<Key> expired(uint64_t now_unix_ms) const {
    const auto window = static_cast<uint64_t>(std::chrono::milliseconds(Traits::retention).count());
    std::vector<Key> out;
    for (const auto& [key, record] : _records) {
      const uint64_t removed = Traits::removed_unix_ms(record);
      if (removed && now_unix_ms > removed && now_unix_ms - removed > window) out.push_back(key);
    }
    return out;
  }

  // The join of two versions of one record. Commutative, associative and
  // idempotent where each group's clock names one write.
  static Record merge(const Record& a, const Record& b) {
    // From the greater record, so set-once fields that disagree resolve the
    // same from either side.
    Record out = a >= b ? a : b;
    std::apply([&](const auto&... group) { (take_group(group, a, b, out), ...); }, Traits::groups);
    Traits::set_lease(out, lease::merge(std::optional<Lease>(Traits::lease_of(a)), std::optional<Lease>(Traits::lease_of(b))));
    Traits::set_removed_unix_ms(out, std::max<uint64_t>(Traits::removed_unix_ms(a), Traits::removed_unix_ms(b)));
    return out;
  }

  // The three-way merge over the key set (Tom, 2026-11-04: merges get the
  // common ancestor when held; `base` is null when it is not, and an absent
  // base is an empty one). A key on both sides is the join of its records.
  // A key on one side is new there or was erased on the other: it is kept
  // unless it equals its base, so an erasure stands unless this side changed
  // the record since. Then the owner's duplicate rule: among live records
  // sharing a duplicate key, the one `keeps` puts first keeps it and every
  // other is handed to `lose`.
  static Map merge(const Map* base, const Map& left, const Map& right) {
    Map out;
    const auto keep_one_side = [&](const Key& key, const Record& present) {
      if (base) {
        const auto in_base = base->find(key);
        if (in_base != base->end() && in_base->second == present) return;
      }
      out.emplace(key, present);
    };
    for (const auto& [key, l] : left) {
      if (const auto r = right.find(key); r != right.end())
        out.emplace(key, merge(l, r->second));
      else
        keep_one_side(key, l);
    }
    for (const auto& [key, r] : right)
      if (!left.contains(key)) keep_one_side(key, r);

    std::map<typename Traits::DuplicateKey, std::vector<Record*>> by_key;
    for (auto& [_, record] : out)
      if (const auto duplicate = std::optional<typename Traits::DuplicateKey>(Traits::duplicate_key(record))) by_key[*duplicate].push_back(&record);
    for (auto& [_, group] : by_key) {
      if (group.size() < 2) continue;
      std::stable_sort(group.begin(), group.end(), [](const Record* x, const Record* y) { return Traits::keeps(*x, *y); });
      for (size_t i = 1; i < group.size(); ++i) Traits::lose(*group[i], *group[0]);
    }
    return out;
  }
  static ClaimedCollection merge(const ClaimedCollection* base, const ClaimedCollection& left, const ClaimedCollection& right) {
    ClaimedCollection out;
    out._records = merge(base ? &base->_records : nullptr, left._records, right._records);
    return out;
  }

  // The snapshot form: a u32 count, then each record in key order. Throws
  // std::invalid_argument past max_records.
  void encode(dannan::net::Writer& writer) const {
    if (_records.size() > Traits::max_records) throw std::invalid_argument("too many claimed collection records");
    writer.u32(static_cast<uint32_t>(_records.size()));
    for (const auto& [_, record] : _records) Traits::encode(writer, record);
  }
  // Throws dannan::net::DecodeError past max_records, for a record with no
  // key, or for two under one key.
  static ClaimedCollection decode(dannan::net::Reader& reader) {
    const auto count = reader.u32();
    if (count > Traits::max_records) throw dannan::net::DecodeError("too many claimed collection records");
    ClaimedCollection out;
    for (uint32_t i = 0; i < count; ++i) {
      auto record = Traits::decode(reader);
      auto key = Traits::key_of(record);
      if (key == Key{} || !out._records.emplace(std::move(key), std::move(record)).second) throw dannan::net::DecodeError("bad claimed collection key");
    }
    return out;
  }

  // The delta form: what changed from `before` to `after`, records whole.
  static Delta diff(const ClaimedCollection& before, const ClaimedCollection& after) {
    Delta delta;
    for (const auto& [key, record] : after._records) {
      const auto found = before._records.find(key);
      if (found == before._records.end() || found->second != record) delta.upserts.emplace(key, record);
    }
    for (const auto& [key, _] : before._records)
      if (!after._records.contains(key)) delta.erasures.push_back(key);
    return delta;
  }
  // A u32 count and the records written, then a u32 count and the keys
  // erased. Throws std::invalid_argument for a record under another key.
  static void encode(dannan::net::Writer& writer, const Delta& delta) {
    writer.u32(static_cast<uint32_t>(delta.upserts.size()));
    for (const auto& [key, record] : delta.upserts) {
      if (!(Traits::key_of(record) == key)) throw std::invalid_argument("claimed collection record keyed by another key");
      Traits::encode(writer, record);
    }
    writer.u32(static_cast<uint32_t>(delta.erasures.size()));
    for (const auto& key : delta.erasures) Traits::encode_key(writer, key);
  }
  // Throws dannan::net::DecodeError past max_records on either side, for a
  // record with no key (Tom, 2026-11-04: what the snapshot refuses, the
  // delta refuses too, or a head holds what it cannot read back), for a
  // record written twice, or for a key both written and erased.
  static Delta decode_delta(dannan::net::Reader& reader) {
    Delta delta;
    const auto upserts = reader.u32();
    if (upserts > Traits::max_records) throw dannan::net::DecodeError("too many claimed collection records in delta");
    for (uint32_t i = 0; i < upserts; ++i) {
      auto record = Traits::decode(reader);
      auto key = Traits::key_of(record);
      if (key == Key{} || !delta.upserts.emplace(std::move(key), std::move(record)).second)
        throw dannan::net::DecodeError("bad claimed collection key in delta");
    }
    const auto erasures = reader.u32();
    if (erasures > Traits::max_records) throw dannan::net::DecodeError("too many claimed collection erasures in delta");
    for (uint32_t i = 0; i < erasures; ++i) {
      auto key = Traits::decode_key(reader);
      if (delta.upserts.contains(key)) throw dannan::net::DecodeError("claimed collection record both written and erased");
      delta.erasures.push_back(std::move(key));
    }
    return delta;
  }
  void apply(const Delta& delta) {
    for (const auto& [key, record] : delta.upserts) _records[key] = record;
    for (const auto& key : delta.erasures) _records.erase(key);
  }

  bool operator==(const ClaimedCollection&) const = default;

 private:
  template <class Group>
  static void take_group(const Group& group, const Record& a, const Record& b, Record& out) {
    const auto clock_a = group.clock(a);
    const auto clock_b = group.clock(b);
    // A pointer, as GCC's dangling-reference check misfires on a reference.
    const Record* from = clock_a != clock_b ? (clock_a > clock_b ? &a : &b) : (a >= b ? &a : &b);
    group.take(out, *from);
  }

  Map _records;
};

}  // namespace dagda::lease
