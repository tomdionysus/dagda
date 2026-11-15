//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// The neutral owner of a head space (the plan's section 2.0, Q1): a record with a
// floor, a clock, merge parents, a tree root and a few notes, nothing any real
// owner holds. Its forms are its own, so nothing here passes by matching another
// owner's bytes. Beside it, what the replica's tests share: a bed that opens
// replicas, and Macha's own test helpers (tests/test_storage_metadata.cpp) in
// neutral form.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "dagda/domain/domain_id.h"
#include "dagda/heads/codec.h"
#include "dagda/heads/record.h"
#include "dagda/heads/replica.h"
#include "dagda/heads/sealed_file.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/tree.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::test {

using heads::Acceptance;
using heads::Codec;
using heads::CostCounters;
using heads::DecodeError;
using heads::Dot;
using heads::Hash256;
using heads::HeadClock;
using heads::HeadFields;
using heads::HistoryEntry;
using heads::Magic;
using heads::Materialization;
using heads::NodeId;
using heads::Record;
using heads::Replica;
using heads::ReplicaMagics;
using heads::ReplicaOptions;
using heads::State;

using NeutralNotes = std::map<std::string, std::string>;

// Copied on write, so a state after a delta shares the notes it did not touch.
class SharedNotes {
 public:
  SharedNotes() : _notes(std::make_shared<const NeutralNotes>()) {}
  explicit SharedNotes(NeutralNotes notes) : _notes(std::make_shared<const NeutralNotes>(std::move(notes))) {}
  const NeutralNotes& get() const noexcept { return *_notes; }
  void set(const std::string& key, const std::string& value) {
    auto next = std::make_shared<NeutralNotes>(*_notes);
    (*next)[key] = value;
    _notes = std::move(next);
  }
  void erase(const std::string& key) {
    auto next = std::make_shared<NeutralNotes>(*_notes);
    next->erase(key);
    _notes = std::move(next);
  }
  bool operator==(const SharedNotes& other) const { return _notes == other._notes || *_notes == *other._notes; }

 private:
  std::shared_ptr<const NeutralNotes> _notes;
};

struct NeutralHead {
  // Zero only on genesis.
  uint32_t floor = 1;
  HeadClock clock;
  std::vector<Hash256> merge_parents;
  std::optional<ObjectId> root;
  SharedNotes notes;
  bool operator==(const NeutralHead&) const = default;
};

inline constexpr Magic neutral_full_magic{'N', 'T', 'R', 'L', 'H', 'E', 'D', '1'};
inline constexpr Magic neutral_delta_magic{'N', 'T', 'R', 'L', 'D', 'L', 'T', '1'};
inline constexpr Magic neutral_genesis_payload{'N', 'T', 'R', 'L', 'G', 'E', 'N', '1'};

inline const ReplicaMagics& neutral_magics() {
  static const ReplicaMagics magics{{'N', 'T', 'R', 'L', 'C', 'H', 'K', '1'}, {'N', 'T', 'R', 'L', 'J', 'N', 'L', '1'},
                                    {'N', 'T', 'R', 'L', 'H', 'S', 'T', '1'}, {'N', 'T', 'R', 'L', 'A', 'C', 'C', '1'},
                                    {'N', 'T', 'R', 'L', 'A', 'U', 'T', '1'}, {'N', 'T', 'R', 'L', 'S', 'A', 'S', '1'}};
  return magics;
}

inline Bytes neutral_payload(const NeutralHead& head) {
  dannan::net::Writer out;
  out.raw(neutral_full_magic);
  out.u32(head.floor);
  heads::encode_clock(out, head.clock);
  heads::encode_merge_parents(out, head.merge_parents);
  out.u8(head.root ? 1 : 0);
  if (head.root) out.fixed(head.root->bytes);
  out.u32(static_cast<uint32_t>(head.notes.get().size()));
  for (const auto& [key, value] : head.notes.get()) {
    out.string(key);
    out.string(value);
  }
  return out.take();
}

inline NeutralHead neutral_head_from(std::span<const uint8_t> payload, std::span<const uint8_t> genesis = neutral_genesis_payload) {
  if (std::equal(payload.begin(), payload.end(), genesis.begin(), genesis.end())) {
    NeutralHead head;
    head.floor = 0;
    return head;
  }
  dannan::net::Reader in(payload);
  const auto magic = in.view(neutral_full_magic.size());
  if (!std::equal(magic.begin(), magic.end(), neutral_full_magic.begin())) throw DecodeError("bad neutral record");
  NeutralHead head;
  head.floor = in.u32();
  if (head.floor == 0) throw DecodeError("bad neutral floor");
  head.clock = heads::decode_clock(in);
  head.merge_parents = heads::decode_merge_parents(in);
  if (in.u8() != 0) head.root = ObjectId{in.fixed<32>()};
  const auto notes = in.u32();
  NeutralNotes decoded;
  for (uint32_t i = 0; i < notes; ++i) {
    auto key = in.string();
    decoded[std::move(key)] = in.string(1 << 20);
  }
  head.notes = SharedNotes(std::move(decoded));
  in.finish();
  return head;
}

// What a neutral delta does: the clock's entries that moved, the notes changed, the
// parents replaced, and the tree's keys changed. A floor never changes in a delta.
struct NeutralDelta {
  HeadClock clock;
  std::optional<std::vector<Hash256>> merge_parents;
  NeutralNotes set_notes;
  std::vector<std::string> erase_notes;
  NeutralEntries upsert_keys;
  std::vector<std::string> erase_keys;
};

inline void encode_neutral_item(dannan::net::Writer& out, const NeutralItem& item) {
  out.u8(item.kind);
  out.u32(item.tag);
  out.i64(item.stamp);
  out.u64(item.weight);
  out.u32(static_cast<uint32_t>(item.refs.size()));
  for (const auto& ref : item.refs) {
    out.u64(ref.start);
    out.u32(ref.span);
    out.u8(ref.gap ? 1 : 0);
    out.fixed(ref.id.bytes);
  }
}

inline NeutralItem decode_neutral_item(dannan::net::Reader& in) {
  NeutralItem item;
  item.kind = in.u8();
  item.tag = in.u32();
  item.stamp = in.i64();
  item.weight = in.u64();
  const auto refs = in.u32();
  for (uint32_t i = 0; i < refs; ++i) {
    NeutralRef ref;
    ref.start = in.u64();
    ref.span = in.u32();
    ref.gap = in.u8() != 0;
    ref.id.bytes = in.fixed<32>();
    item.refs.push_back(ref);
  }
  return item;
}

inline Bytes encode_neutral_delta(const NeutralDelta& delta) {
  dannan::net::Writer out;
  out.raw(neutral_delta_magic);
  heads::encode_clock(out, delta.clock);
  out.u8(delta.merge_parents ? 1 : 0);
  if (delta.merge_parents) heads::encode_merge_parents(out, *delta.merge_parents);
  out.u32(static_cast<uint32_t>(delta.set_notes.size()));
  for (const auto& [key, value] : delta.set_notes) {
    out.string(key);
    out.string(value);
  }
  out.u32(static_cast<uint32_t>(delta.erase_notes.size()));
  for (const auto& key : delta.erase_notes) out.string(key);
  out.u32(static_cast<uint32_t>(delta.upsert_keys.size()));
  for (const auto& [key, item] : delta.upsert_keys) {
    out.string(key);
    encode_neutral_item(out, item);
  }
  out.u32(static_cast<uint32_t>(delta.erase_keys.size()));
  for (const auto& key : delta.erase_keys) out.string(key);
  return out.take();
}

inline NeutralDelta decode_neutral_delta(std::span<const uint8_t> bytes) {
  dannan::net::Reader in(bytes);
  const auto magic = in.view(neutral_delta_magic.size());
  if (!std::equal(magic.begin(), magic.end(), neutral_delta_magic.begin())) throw DecodeError("bad neutral delta");
  NeutralDelta delta;
  delta.clock = heads::decode_clock(in);
  if (in.u8() != 0) delta.merge_parents = heads::decode_merge_parents(in);
  for (uint32_t i = 0, n = in.u32(); i < n; ++i) {
    auto key = in.string();
    delta.set_notes[std::move(key)] = in.string(1 << 20);
  }
  for (uint32_t i = 0, n = in.u32(); i < n; ++i) delta.erase_notes.push_back(in.string());
  for (uint32_t i = 0, n = in.u32(); i < n; ++i) {
    auto key = in.string();
    delta.upsert_keys[std::move(key)] = decode_neutral_item(in);
  }
  for (uint32_t i = 0, n = in.u32(); i < n; ++i) delta.erase_keys.push_back(in.string());
  in.finish();
  return delta;
}

// The delta from `before` to `after`, the tree aside; nothing if a clock goes back
// or a floor changes, which a delta cannot say.
inline std::optional<NeutralDelta> neutral_delta(const NeutralHead& before, const NeutralHead& after) {
  if (before.floor != after.floor) return std::nullopt;
  NeutralDelta delta;
  for (const auto& [author, sequence] : before.clock) {
    const auto found = after.clock.find(author);
    if (found == after.clock.end() || found->second < sequence) return std::nullopt;
  }
  for (const auto& [author, sequence] : after.clock) {
    const auto found = before.clock.find(author);
    if (found == before.clock.end() || found->second != sequence) delta.clock[author] = sequence;
  }
  if (before.merge_parents != after.merge_parents) delta.merge_parents = after.merge_parents;
  for (const auto& [key, value] : after.notes.get()) {
    const auto found = before.notes.get().find(key);
    if (found == before.notes.get().end() || found->second != value) delta.set_notes[key] = value;
  }
  for (const auto& [key, value] : before.notes.get())
    if (!after.notes.get().contains(key)) delta.erase_notes.push_back(key);
  return delta;
}

inline NeutralTree& replay_tree() {
  static NeutralTree tree = neutral_tree();
  return tree;
}

// The neutral owner's codec. A delta over a tree-backed head with keys to change is
// refused without a node store, as Macha's applier refuses it unset.
class NeutralCodec final : public Codec {
 public:
  explicit NeutralCodec(CostCounters* costs = nullptr, Bytes genesis = Bytes(neutral_genesis_payload.begin(), neutral_genesis_payload.end()))
      : _costs(costs), _genesis(std::move(genesis)) {}

  Bytes genesis() const override { return _genesis; }
  State decode_full(std::span<const uint8_t> payload) const override { return std::make_shared<const NeutralHead>(neutral_head_from(payload, _genesis)); }
  Bytes encode_full(const State& state) const override { return encode(*as_head(state)); }
  std::optional<Bytes> encode_delta(const State& before, const State& after) const override {
    const auto delta = neutral_delta(*as_head(before), *as_head(after));
    if (!delta) return std::nullopt;
    return encode_neutral_delta(*delta);
  }
  void check_delta(std::span<const uint8_t> delta) const override { (void)decode_neutral_delta(delta); }
  State apply_delta(const State& parent, std::span<const uint8_t> bytes, heads::TreeNodeStore* tree) const override {
    const auto delta = decode_neutral_delta(bytes);
    auto next = std::make_shared<NeutralHead>(*as_head(parent));
    for (const auto& [author, sequence] : delta.clock) next->clock[author] = sequence;
    if (delta.merge_parents) next->merge_parents = *delta.merge_parents;
    if (_costs && !(delta.set_notes.empty() && delta.erase_notes.empty())) _costs->parent_states_copied.fetch_add(1);
    for (const auto& [key, value] : delta.set_notes) next->notes.set(key, value);
    for (const auto& key : delta.erase_notes) next->notes.erase(key);
    if (!delta.upsert_keys.empty() || !delta.erase_keys.empty()) {
      if (!next->root) throw DecodeError("a neutral delta names keys of a head with no tree");
      if (tree == nullptr) throw DecodeError("a tree-backed delta needs a node store");
      NeutralTree::Delta changes;
      changes.upsert = delta.upsert_keys;
      changes.erase = delta.erase_keys;
      next->root = replay_tree().apply(*next->root, *tree, changes);
    }
    return next;
  }
  Bytes encode_after_delta(std::span<const uint8_t> delta, const State& state) const override {
    if (heads::form_version(delta, "NTRLDLT") != 1) throw DecodeError("bad neutral delta");
    return encode(*as_head(state));
  }
  HeadFields head_fields(const State& state) const override {
    const auto& head = *as_head(state);
    HeadFields fields{head.clock, head.merge_parents, head.floor, head.root, {}};
    // What only a policy commit may move: the notes under "policy/".
    for (auto at = head.notes.get().lower_bound("policy/"); at != head.notes.get().end() && at->first.starts_with("policy/"); ++at) {
      fields.policy.insert(fields.policy.end(), at->first.begin(), at->first.end());
      fields.policy.insert(fields.policy.end(), at->second.begin(), at->second.end());
    }
    return fields;
  }
  uint64_t resident_bytes(const State& state) const override {
    const auto& head = *as_head(state);
    uint64_t bytes = sizeof(NeutralHead) + head.clock.size() * 64 + head.merge_parents.size() * 32;
    for (const auto& [key, value] : head.notes.get()) bytes += key.size() + value.size() + 64;
    return bytes;
  }

  static std::shared_ptr<const NeutralHead> as_head(const State& state) { return std::static_pointer_cast<const NeutralHead>(state); }

 private:
  Bytes encode(const NeutralHead& head) const {
    if (head.floor == 0) return _genesis;
    return neutral_payload(head);
  }

  CostCounters* const _costs;
  const Bytes _genesis;
};

// A node store several applies share, counting what they read and write: what a
// control store would be, in memory. Thread-safe.
class SharedTreeStore {
 public:
  explicit SharedTreeStore(CostCounters* costs = nullptr) : _costs(costs) {}

  class Facade final : public heads::TreeNodeStore {
   public:
    explicit Facade(SharedTreeStore& owner) : _owner(owner) {}
    ObjectId put(std::span<const uint8_t> node) override {
      std::lock_guard lock(_owner._mutex);
      if (_owner._costs) _owner._costs->tree_nodes_written.fetch_add(1);
      return _owner._store.put(node);
    }
    std::optional<Bytes> get(const ObjectId& id) const override {
      std::lock_guard lock(_owner._mutex);
      if (_owner._costs) _owner._costs->tree_nodes_read.fetch_add(1);
      return _owner._store.get(id);
    }

   private:
    SharedTreeStore& _owner;
  };

  std::function<std::unique_ptr<heads::TreeNodeStore>()> factory() {
    return [this]() -> std::unique_ptr<heads::TreeNodeStore> { return std::make_unique<Facade>(*this); };
  }

 private:
  CostCounters* const _costs;
  mutable std::mutex _mutex;
  mutable heads::MemoryTreeNodeStore _store;
};

inline const domain::DomainId& neutral_domain() {
  static const domain::DomainId id{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
  return id;
}

inline const heads::SpaceId& neutral_space() {
  static const heads::SpaceId id = heads::space_id(neutral_domain(), "test/neutral");
  return id;
}

// What a replica runs on in a test, and how to open one at a path.
struct ReplicaBed {
  dannan::test::TempDir dir;
  dannan::test::SteppedTime time;
  std::shared_ptr<const domain::StorageKey> content_key = key_of(41);
  std::shared_ptr<const domain::StorageKey> local_key = key_of(42);
  std::shared_ptr<const Codec> codec = std::make_shared<NeutralCodec>();

  ReplicaOptions options(const std::string& name = "space") {
    ReplicaOptions out;
    out.directory = dir.path() / name;
    out.content_key = content_key;
    out.local_key = local_key;
    out.magics = neutral_magics();
    out.aad = heads::replica_aad(neutral_domain(), neutral_space());
    out.codec = codec;
    out.time = &time;
    out.logger = quiet_logger();
    return out;
  }

  std::unique_ptr<Replica> open(const std::string& name = "space", std::optional<Record> seed = std::nullopt, bool pristine = true,
                                uint64_t cache_limit = 128ULL * 1024 * 1024) {
    auto out = options(name);
    out.recovery_seed = std::move(seed);
    out.accept_pristine_genesis_authority = pristine;
    out.materialization_cache_limit_bytes = cache_limit;
    return std::make_unique<Replica>(std::move(out));
  }

  Record genesis() const { return heads::genesis_record(codec->genesis()); }
  std::filesystem::path path(const std::string& name = "space") const { return dir.path() / name; }
};

inline NodeId author(uint8_t id) {
  NodeId node{};
  node.bytes[15] = id;
  return node;
}

inline Record make_record(uint64_t generation, const Hash256& previous, Bytes payload) {
  Record record;
  record.generation = generation;
  record.previous = previous;
  record.hash = heads::record_hash(generation, previous, payload);
  record.payload = std::move(payload);
  return record;
}

inline Record record_after(const Record& previous, Bytes payload) { return make_record(previous.generation + 1, previous.hash, std::move(payload)); }

inline NeutralHead head_with(std::initializer_list<const char*> notes, HeadClock clock = {}, uint32_t floor = 1) {
  NeutralHead head;
  head.floor = floor;
  head.clock = std::move(clock);
  for (const auto* key : notes) head.notes.set(key, "");
  return head;
}

inline Record record_of(uint64_t generation, const Hash256& previous, const NeutralHead& head) {
  return make_record(generation, previous, neutral_payload(head));
}

// Macha's record_with_clock.
inline Record record_with_clock(uint64_t generation, const Hash256& previous, HeadClock clock, std::initializer_list<const char*> notes) {
  return record_of(generation, previous, head_with(notes, std::move(clock), 2));
}

inline Bytes delta_between(const NeutralHead& before, const NeutralHead& after) {
  const auto delta = neutral_delta(before, after);
  if (!delta) throw std::logic_error("no delta between these heads");
  return encode_neutral_delta(*delta);
}

// Macha's rootless_entry: the record as a whole entry with no known previous.
inline HistoryEntry rootless_entry(const Record& record, std::vector<Hash256> merge_parents = {}) {
  HistoryEntry entry;
  entry.generation = record.generation;
  entry.previous = record.previous;
  entry.hash = record.hash;
  entry.previous_known = false;
  entry.merge_parents = std::move(merge_parents);
  entry.body = HistoryEntry::Body::full;
  entry.payload.assign(record.payload.begin(), record.payload.end());
  return entry;
}

inline Acceptance accepted_by_two(const Record& record) { return {record.generation, record.hash, 2, {author(1), author(2)}}; }

inline std::vector<Hash256> hashes_of(const std::vector<Record>& heads) {
  std::vector<Hash256> out;
  for (const auto& head : heads) out.push_back(head.hash);
  std::sort(out.begin(), out.end());
  return out;
}

inline std::vector<Hash256> sorted_hashes(std::initializer_list<const Record*> records) {
  std::vector<Hash256> out;
  for (const auto* record : records) out.push_back(record->hash);
  std::sort(out.begin(), out.end());
  return out;
}

// Macha's commit_record: stored and accepted, as a commit is.
inline void commit_record(Replica& replica, const Record& record, std::span<const uint8_t> delta = {}) {
  ASSERT_TRUE(replica.store_commit(record, delta));
  Acceptance acceptance;
  acceptance.generation = record.generation;
  acceptance.hash = record.hash;
  acceptance.required = 1;
  acceptance.replicas = {author(1)};
  ASSERT_TRUE(replica.accept_commit(acceptance));
}

// Flips the last byte of the frame ending at `frame_end`: still indexed, no longer
// authenticating, until it is flipped back.
inline void flip_history_frame(const std::filesystem::path& history, uint64_t frame_end) {
  std::fstream file(history, std::ios::binary | std::ios::in | std::ios::out);
  char byte{};
  file.seekg(static_cast<std::streamoff>(frame_end - 1));
  file.read(&byte, 1);
  byte = static_cast<char>(byte ^ 0x5a);
  file.seekp(static_cast<std::streamoff>(frame_end - 1));
  file.write(&byte, 1);
  file.flush();
  ASSERT_TRUE(file.good());
}

inline void flip_byte_at(const std::filesystem::path& path, uint64_t at) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.good());
  char byte{};
  file.seekg(static_cast<std::streamoff>(at));
  file.read(&byte, 1);
  ASSERT_TRUE(file.good());
  byte = static_cast<char>(byte ^ 0x5a);
  file.seekp(static_cast<std::streamoff>(at));
  file.write(&byte, 1);
  file.flush();
  ASSERT_TRUE(file.good());
}

// One frame of a replica's journal, at the bytes Macha's reader accepts: no live
// code at 0.93.4 writes one, so this helper (Macha's append_journal_frame) is the
// only writer there is.
inline constexpr uint8_t journal_prepare_full = 1;
inline constexpr uint8_t journal_prepare_delta = 2;
inline constexpr uint8_t journal_commit = 4;
inline void append_journal_frame(const ReplicaBed& bed, const std::filesystem::path& directory, uint8_t kind, const Record& record,
                                 std::span<const uint8_t> body = {}) {
  dannan::net::Writer plain;
  plain.u8(kind);
  plain.u64(record.generation);
  plain.fixed(record.previous.bytes);
  plain.fixed(record.hash.bytes);
  plain.bytes(body);
  const auto aad = heads::replica_aad(neutral_domain(), neutral_space())(neutral_magics().journal);
  const auto frame = heads::seal_frame(*bed.content_key, aad, plain.data());
  std::ofstream out(directory / "journal.log", std::ios::binary | std::ios::app);
  out.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
  ASSERT_TRUE(out.good());
}

}  // namespace dagda::test
