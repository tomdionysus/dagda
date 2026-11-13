//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/ledger/retention.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include "dannan/identity/durable_file.h"
#include "dannan/net/codec.h"

namespace dagda::ledger {
// The checkpoint forms Macha read once, before the tries held the claims:
// reserved names, never read here.
bool retention_holds_unmoved_checkpoint(const std::filesystem::path& root) {
  return std::filesystem::exists(root / "claims.current") || std::filesystem::exists(root / "claims.meta");
}

namespace {

using dannan::contract::Lock;
using dannan::net::DecodeError;
using dannan::net::Reader;
using dannan::net::Writer;
using retention_format::ObjectState;

constexpr uint64_t journal_compact_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t retain_ids_per_frame = 65536;
// A checkpoint is due once the tries hold this many changed objects unsaved,
// or the journal this many bytes: what a start replays stays small.
constexpr uint64_t checkpoint_changed = 8192;
constexpr uint64_t checkpoint_journal_bytes = 4ULL * 1024 * 1024;
constexpr size_t scan_page = 256;

bool valid_class(uint8_t value) { return value == static_cast<uint8_t>(RetentionClass::data) || value == static_cast<uint8_t>(RetentionClass::control); }

void remove_reserved_checkpoint_files(const std::filesystem::path& root, const dannan::status::ScopedLogger& logger) {
  std::error_code error;
  std::filesystem::remove(root / "claims.current", error);
  std::filesystem::remove(root / "claims.meta", error);
  std::filesystem::remove_all(root / "checkpoints", error);
  if (error) logger.debug("cannot remove retention checkpoint files: " + error.message());
}

std::vector<ObjectId> sorted_unique(std::vector<ObjectId> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

// A release clock as it is journalled: entries with no node or no sequence
// say nothing and are left out.
RetentionClock journalled_clock(const RetentionClock& observed) {
  RetentionClock out;
  for (const auto& [node, sequence] : observed)
    if (node != NodeId{} && sequence) out[node] = sequence;
  return out;
}

}  // namespace

namespace retention_format {

Bytes encode_state(const ObjectState& state) {
  Writer writer;
  for (const auto* side : {&state.adds, &state.removed}) {
    if (side->size() > UINT16_MAX) throw std::runtime_error("too many retention clocks for one object");
    writer.u16(static_cast<uint16_t>(side->size()));
    for (const auto& [origin, sequence] : *side) {
      writer.fixed(origin.bytes);
      writer.u64(sequence);
    }
  }
  return writer.take();
}

ObjectState decode_state(std::span<const uint8_t> bytes) {
  ObjectState state;
  Reader reader(bytes);
  for (auto* side : {&state.adds, &state.removed}) {
    const auto count = reader.u16();
    for (uint16_t i = 0; i < count; ++i) {
      NodeId origin{reader.fixed<16>()};
      const auto sequence = reader.u64();
      if (origin == NodeId{} || !sequence || !side->emplace(origin, sequence).second) throw DecodeError("bad retention record clock");
    }
  }
  reader.finish();
  return state;
}

bool claimed(std::span<const uint8_t> record) noexcept { return record.size() >= 2 && (record[0] != 0 || record[1] != 0); }

Bytes encode_add(RetentionClass type, const RetentionDot& dot, std::span<const ObjectId> ids) {
  Writer writer;
  writer.u8(op_add);
  writer.u8(static_cast<uint8_t>(type));
  writer.fixed(dot.origin.bytes);
  writer.u64(dot.sequence);
  writer.u32(static_cast<uint32_t>(ids.size()));
  for (const auto& id : ids) writer.fixed(id.bytes);
  return writer.take();
}

Bytes encode_release(RetentionClass type, const RetentionClock& clock, std::span<const ObjectId> ids) {
  Writer writer;
  writer.u8(op_release);
  writer.u8(static_cast<uint8_t>(type));
  writer.u32(static_cast<uint32_t>(clock.size()));
  for (const auto& [node, sequence] : clock) {
    writer.fixed(node.bytes);
    writer.u64(sequence);
  }
  writer.u32(static_cast<uint32_t>(ids.size()));
  for (const auto& id : ids) writer.fixed(id.bytes);
  return writer.take();
}

Bytes encode_prune(RetentionClass type, std::span<const ObjectId> ids) {
  Writer writer;
  writer.u8(op_prune);
  writer.u8(static_cast<uint8_t>(type));
  writer.u32(static_cast<uint32_t>(ids.size()));
  for (const auto& id : ids) writer.fixed(id.bytes);
  return writer.take();
}

}  // namespace retention_format

RetentionStore::RetentionStore(const std::filesystem::path& state, std::shared_ptr<const domain::StorageKey> key, dannan::status::ScopedLogger logger,
                               size_t cache_bytes)
    : _root(state / "retention"),
      _key(std::move(key)),
      _logger(std::move(logger)),
      _journal(_root / "claims.log", _key, retention_format::journal_aad, retention_format::max_journal_frame, _logger) {
  if (!_key) throw std::invalid_argument("a retention store needs a key");
  std::filesystem::create_directories(_root);
  load(cache_bytes);
}

RetentionStore::ClassLedger& RetentionStore::class_for(RetentionClass type) { return type == RetentionClass::data ? _data : _control; }

const RetentionStore::ClassLedger& RetentionStore::class_for(RetentionClass type) const { return type == RetentionClass::data ? _data : _control; }

void RetentionStore::apply_add_locked(RetentionClass type, const RetentionDot& dot, std::vector<ObjectId> ids) {
  if (dot.origin == NodeId{} || !dot.sequence) throw DecodeError("invalid retention mutation dot");
  auto& trie = *class_for(type).trie;
  std::vector<ObjectTrie::Change> changes;
  for (const auto& id : sorted_unique(std::move(ids))) {
    const auto record = trie.get(id);
    auto object = record ? retention_format::decode_state(*record) : ObjectState{};
    // A remove context at or past the dot: a delayed or replayed old add.
    const auto removed = object.removed.find(dot.origin);
    if (removed != object.removed.end() && removed->second >= dot.sequence) continue;
    auto& current = object.adds[dot.origin];
    if (current >= dot.sequence) continue;
    current = dot.sequence;
    changes.push_back({id, retention_format::encode_state(object)});
  }
  _changed += changes.size();
  trie.install(changes);
}

size_t RetentionStore::apply_release_locked(RetentionClass type, const RetentionClock& observed, std::vector<ObjectId> ids) {
  auto& trie = *class_for(type).trie;
  std::vector<ObjectTrie::Change> changes;
  for (const auto& id : sorted_unique(std::move(ids))) {
    const auto record = trie.get(id);
    if (!record) continue;
    auto object = retention_format::decode_state(*record);
    bool object_changed = false;
    for (const auto& [origin, sequence] : observed) {
      if (!sequence) continue;
      auto& removed = object.removed[origin];
      if (removed < sequence) {
        removed = sequence;
        object_changed = true;
      }
      // Only an add the clock dominates goes: a later one, or one from an
      // origin the clock has not seen, stays.
      auto add = object.adds.find(origin);
      if (add != object.adds.end() && add->second <= sequence) {
        object.adds.erase(add);
        object_changed = true;
      }
    }
    if (object_changed) changes.push_back({id, retention_format::encode_state(object)});
  }
  _changed += changes.size();
  trie.install(changes);
  return changes.size();
}

size_t RetentionStore::apply_prune_locked(RetentionClass type, std::vector<ObjectId> ids) {
  auto& trie = *class_for(type).trie;
  std::vector<ObjectTrie::Change> changes;
  for (const auto& id : sorted_unique(std::move(ids))) {
    const auto record = trie.get(id);
    if (record && !retention_format::claimed(*record)) changes.push_back({id, std::nullopt});
  }
  _changed += changes.size();
  trie.install(changes);
  return changes.size();
}

void RetentionStore::checkpoint_locked() {
  _data.trie->checkpoint();
  _control.trie->checkpoint();
  // The tries are durable; replaying the old journal over them would be
  // idempotent had this not happened.
  _journal.reset();
  _changed = 0;
  remove_reserved_checkpoint_files(_root, _logger);
}

void RetentionStore::checkpoint_if_due_locked() {
  if (_changed >= checkpoint_changed || _journal.bytes() >= checkpoint_journal_bytes) checkpoint_locked();
}

void RetentionStore::scan_locked(const ClassLedger& ledger, std::optional<ObjectId>& cursor, size_t limit,
                                 const std::function<bool(const ObjectId&, std::span<const uint8_t>)>& visit) const {
  std::optional<ObjectId> start;
  std::optional<ObjectId> after = cursor;
  bool wrapped = false;
  size_t visited = 0;
  while (visited < limit) {
    const auto page = ledger.trie->next(after, std::min(scan_page, limit - visited));
    if (page.empty()) {
      if (wrapped || !after) return;
      wrapped = true;
      after.reset();
      continue;
    }
    for (const auto& [id, record] : page) {
      if (wrapped && start && !(id < *start)) return;
      if (!start) start = id;
      ++visited;
      cursor = id;
      after = id;
      if (!visit(id, record)) return;
    }
  }
}

void RetentionStore::create_locked() {
  // As Macha's first start of the tries, with nothing to move into them: the
  // tries begin empty and the journal written since is replayed over them.
  _data.trie->replace_all({});
  _control.trie->replace_all({});
  dannan::identity::durable_replace_file(_root / "ledger", "1\n");
  // Macha's line (retention.cpp:398 at 98b816d), whose checkpoint here is always empty.
  _logger.info("retention claims moved into the ledger data=0 control=0");
}

void RetentionStore::load_journal_locked() {
  (void)_journal.replay(
      [&](std::span<const uint8_t> plaintext) DANNAN_REQUIRES(_mutex) {
        Reader reader(plaintext);
        const auto op = reader.u8();
        const auto raw_class = reader.u8();
        if (!valid_class(raw_class)) throw DecodeError("bad retention object class");
        const auto type = static_cast<RetentionClass>(raw_class);
        const auto read_ids = [&](const char* what) {
          const auto count = reader.u32();
          if (count > 1'000'000) throw DecodeError(std::string("too many retention ") + what + " objects");
          std::vector<ObjectId> ids;
          ids.reserve(count);
          for (uint32_t i = 0; i < count; ++i) ids.push_back(ObjectId{reader.fixed<32>()});
          reader.finish();
          return ids;
        };
        if (op == retention_format::op_add) {
          RetentionDot dot;
          dot.origin.bytes = reader.fixed<16>();
          dot.sequence = reader.u64();
          apply_add_locked(type, dot, read_ids("add"));
        } else if (op == retention_format::op_release) {
          const auto clocks = reader.u32();
          if (clocks > 1'000'000) throw DecodeError("too many retention release clocks");
          RetentionClock observed;
          for (uint32_t i = 0; i < clocks; ++i) {
            NodeId node{reader.fixed<16>()};
            const auto sequence = reader.u64();
            if (node == NodeId{} || !sequence || !observed.emplace(node, sequence).second) throw DecodeError("bad retention release clock");
          }
          (void)apply_release_locked(type, observed, read_ids("release"));
        } else if (op == retention_format::op_prune) {
          (void)apply_prune_locked(type, read_ids("prune"));
        } else {
          throw DecodeError("unknown retention journal operation");
        }
      },
      retention_format::max_read_frame, "retention journal");
}

void RetentionStore::load(size_t cache_bytes) {
  Lock lock(_mutex);
  const bool marked = std::filesystem::exists(_root / "ledger");
  if (!marked && retention_holds_unmoved_checkpoint(_root))
    throw std::runtime_error("retention claims at " + _root.string() +
                             " are in a checkpoint from before the claims ledger (claims.meta or claims.current), which Dagda does not read");
  ObjectTrie::Options data_options;
  data_options.cache_bytes = cache_bytes - cache_bytes / 4;
  ObjectTrie::Options control_options;
  control_options.cache_bytes = cache_bytes / 4;
  _data.trie = std::make_unique<ObjectTrie>(_root / "ledger-data", _key, data_options, _logger);
  _control.trie = std::make_unique<ObjectTrie>(_root / "ledger-control", _key, control_options, _logger);
  if (!marked) {
    // Tries holding claims under no marker: Macha rebuilt both empty and
    // replayed the journal, losing every checkpointed claim
    // (retention.cpp:383-400 at develop), and GC could then remove what
    // someone still needs. Refused instead (the maintainer, 2026-11-04; discipline 3):
    // the operator restores the marker or says the claims may go.
    if (!_data.trie->next(std::nullopt, 1).empty() || !_control.trie->next(std::nullopt, 1).empty())
      throw std::runtime_error("retention claims at " + _root.string() + " hold claims but no ledger marker; Dagda will not rebuild them empty");
    create_locked();
  }
  load_journal_locked();
  checkpoint_if_due_locked();
}

void RetentionStore::retain(RetentionClass type, const ObjectId& id, const RetentionDot& dot) { retain_batch(type, std::vector<ObjectId>{id}, dot); }

void RetentionStore::retain_batch(RetentionClass type, const std::vector<ObjectId>& input, const RetentionDot& dot) {
  if (input.empty()) return;
  if (dot.origin == NodeId{} || !dot.sequence) throw std::invalid_argument("invalid retention mutation dot");
  const auto ids = sorted_unique(input);

  Lock lock(_mutex);
  for (size_t begin = 0; begin < ids.size(); begin += retain_ids_per_frame) {
    const auto end = std::min(ids.size(), begin + retain_ids_per_frame);
    const std::span<const ObjectId> part(ids.data() + begin, end - begin);
    _journal.append(retention_format::encode_add(type, dot, part));
    apply_add_locked(type, dot, std::vector<ObjectId>(part.begin(), part.end()));
    checkpoint_if_due_locked();
  }
}

bool RetentionStore::retained(RetentionClass type, const ObjectId& id) const {
  Lock lock(_mutex);
  const auto record = class_for(type).trie->get(id);
  return record && retention_format::claimed(*record);
}

RetentionStore::Claims RetentionStore::claims(RetentionClass type, const ObjectId& id) const {
  Lock lock(_mutex);
  const auto record = class_for(type).trie->get(id);
  if (!record) return {};
  auto object = retention_format::decode_state(*record);
  return {std::move(object.adds), std::move(object.removed)};
}

std::vector<ObjectId> RetentionStore::retained_ids(RetentionClass type) const {
  Lock lock(_mutex);
  std::vector<ObjectId> ids;
  std::optional<ObjectId> after;
  while (true) {
    const auto page = class_for(type).trie->next(after, scan_page);
    for (const auto& [id, record] : page)
      if (retention_format::claimed(record)) ids.push_back(id);
    if (page.size() < scan_page) return ids;
    after = page.back().first;
  }
}

std::optional<ObjectId> RetentionStore::next_retained(RetentionClass type, std::optional<ObjectId>& cursor, bool& complete) const {
  Lock lock(_mutex);
  while (true) {
    const auto page = class_for(type).trie->next(cursor, scan_page);
    for (const auto& [id, record] : page) {
      cursor = id;
      if (retention_format::claimed(record)) {
        complete = false;
        return id;
      }
    }
    if (page.size() < scan_page) {
      complete = true;
      cursor.reset();
      return {};
    }
  }
}

size_t RetentionStore::release_unreferenced(RetentionClass type, const contract::IdLookup& live, const RetentionClock& observed, size_t operation_budget) {
  if (!operation_budget || observed.empty()) return 0;
  const auto encoded_clock = journalled_clock(observed);
  if (encoded_clock.empty()) return 0;

  Lock lock(_mutex);
  auto& ledger = class_for(type);
  if (ledger.trie->size() == 0) {
    ledger.release_after.reset();
    return 0;
  }
  // A bounded stretch of claims from where the last call stopped: commits
  // take this lock for their own claims, and nearly every claim is live, so
  // a call that looked at them all would hold it for the whole store.
  std::vector<ObjectId> candidates;
  candidates.reserve(operation_budget);
  scan_locked(ledger, ledger.release_after, std::max<size_t>(operation_budget * 128, 8192), [&](const ObjectId& id, std::span<const uint8_t> record) {
    if (retention_format::claimed(record) && !live.contains(id)) candidates.push_back(id);
    return candidates.size() < operation_budget;
  });
  if (candidates.empty()) return 0;

  _journal.append(retention_format::encode_release(type, encoded_clock, candidates));
  const auto released = apply_release_locked(type, encoded_clock, std::move(candidates));
  checkpoint_if_due_locked();
  return released;
}

size_t RetentionStore::release_observed(RetentionClass type, const std::vector<ObjectId>& input, const RetentionClock& observed) {
  const auto encoded_clock = journalled_clock(observed);
  if (encoded_clock.empty() || input.empty()) return 0;
  const auto ids = sorted_unique(input);
  // The collector's release, frame and rule, on the objects named rather
  // than on a walk's stretch.
  Lock lock(_mutex);
  size_t released = 0;
  for (size_t begin = 0; begin < ids.size(); begin += retain_ids_per_frame) {
    const auto end = std::min(ids.size(), begin + retain_ids_per_frame);
    std::vector<ObjectId> part(ids.begin() + static_cast<std::ptrdiff_t>(begin), ids.begin() + static_cast<std::ptrdiff_t>(end));
    _journal.append(retention_format::encode_release(type, encoded_clock, part));
    released += apply_release_locked(type, encoded_clock, std::move(part));
    checkpoint_if_due_locked();
  }
  return released;
}

size_t RetentionStore::claim_objects(RetentionClass type) const { return retained_ids(type).size(); }

bool RetentionStore::compact_if_needed(size_t record_threshold) {
  Lock lock(_mutex);
  if (_journal.frames() < record_threshold && _journal.bytes() < journal_compact_bytes) return false;
  checkpoint_locked();
  return true;
}

size_t RetentionStore::prune_unclaimed(RetentionClass type, const std::function<bool(const ObjectId&)>& exists, size_t operation_budget) {
  if (!operation_budget || !exists) return 0;
  // Three steps, so `exists` (which may read a device) runs without the
  // lock: pick the unclaimed records under it, ask the store without it, then
  // journal and erase under it only what is still unclaimed.
  std::vector<ObjectId> unclaimed;
  {
    Lock lock(_mutex);
    auto& ledger = class_for(type);
    if (ledger.trie->size() == 0) {
      ledger.prune_after.reset();
      return 0;
    }
    scan_locked(ledger, ledger.prune_after, operation_budget, [&](const ObjectId& id, std::span<const uint8_t> record) {
      if (!retention_format::claimed(record)) unclaimed.push_back(id);
      return true;
    });
  }

  std::erase_if(unclaimed, [&](const ObjectId& id) { return exists(id); });
  if (unclaimed.empty()) return 0;

  Lock lock(_mutex);
  _journal.append(retention_format::encode_prune(type, unclaimed));
  const auto removed = apply_prune_locked(type, std::move(unclaimed));
  if (class_for(type).trie->size() == 0) class_for(type).prune_after.reset();
  checkpoint_if_due_locked();
  return removed;
}

}  // namespace dagda::ledger
