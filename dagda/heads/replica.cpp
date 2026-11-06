//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/replica.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "dagda/contract/stopping.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"

namespace dagda::heads {
namespace {

using contract::MaintenanceStopping;
using dannan::contract::Lock;
using dannan::identity::hex;
using dannan::net::Reader;
using dannan::net::Writer;

constexpr uint8_t journal_prepare_full = 1;
constexpr uint8_t journal_prepare_delta = 2;
constexpr uint8_t journal_seed_full = 3;
constexpr uint8_t journal_commit = 4;

std::string_view as_text(std::span<const uint8_t> bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

Bytes read_whole(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot open");
  return Bytes(std::istreambuf_iterator<char>(stream), {});
}

void sync_directory(const std::filesystem::path& path) {
  const auto directory = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot open metadata directory " + directory.string() + ": " + std::strerror(errno));
  if (::fsync(fd) != 0) {
    const auto error = errno;
    ::close(fd);
    throw std::runtime_error("cannot sync metadata directory " + directory.string() + ": " + std::strerror(error));
  }
  if (::close(fd) != 0) throw std::runtime_error("cannot close metadata directory " + directory.string() + ": " + std::strerror(errno));
}

void truncate_durable(const std::filesystem::path& path, uint64_t size) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot open metadata journal for truncation " + path.string() + ": " + std::strerror(errno));
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    const auto error = errno;
    ::close(fd);
    throw std::runtime_error("cannot truncate metadata journal " + path.string() + ": " + std::strerror(error));
  }
  if (::fsync(fd) != 0) {
    const auto error = errno;
    ::close(fd);
    throw std::runtime_error("cannot sync truncated metadata journal " + path.string() + ": " + std::strerror(error));
  }
  if (::close(fd) != 0) throw std::runtime_error("cannot close truncated metadata journal " + path.string() + ": " + std::strerror(errno));
}

// A name, not a decision: the real clock, nanoseconds as Macha stamps them, so two
// quarantines of a file do not collide.
std::string corrupt_stamp() {
  return ".corrupt." + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path quarantine_tail(const std::filesystem::path& path, std::span<const uint8_t> tail) {
  std::filesystem::path quarantine = path.string() + corrupt_stamp();
  dannan::identity::durable_replace_file(quarantine, as_text(tail));
  return quarantine;
}

std::optional<std::filesystem::path> quarantine_file(const std::filesystem::path& path, const std::string& suffix) {
  if (!std::filesystem::exists(path)) return std::nullopt;
  std::filesystem::path quarantine = path.string() + suffix;
  if (::rename(path.c_str(), quarantine.c_str()) != 0)
    throw std::runtime_error("cannot quarantine metadata file " + path.string() + ": " + std::strerror(errno));
  sync_directory(path);
  return quarantine;
}

// Whether merging `incoming` into the held certificate would leave it as it is
// (the rule accept_commit applies to a head it already holds).
bool certificate_adds_nothing(const Acceptance& held, const Acceptance& incoming) {
  if (held.required == 0) return incoming.required == 0;
  if (incoming.required == 0) return true;
  if (incoming.required < held.required) return false;
  return std::includes(held.replicas.begin(), held.replicas.end(), incoming.replicas.begin(), incoming.replicas.end());
}

const std::shared_ptr<const Codec>& required_codec(const std::shared_ptr<const Codec>& codec) {
  if (!codec) throw std::invalid_argument("a replica needs its owner's codec");
  return codec;
}

const std::shared_ptr<const domain::StorageKey>& required_key(const std::shared_ptr<const domain::StorageKey>& key) {
  if (!key) throw std::invalid_argument("a replica needs its keys");
  return key;
}

const AadRule& required_rule(const AadRule& rule) {
  if (!rule) throw std::invalid_argument("a replica needs its associated data");
  return rule;
}

std::function<void(const std::filesystem::path&, std::string_view)> file_replacer(std::function<void(const std::filesystem::path&, std::string_view)> given) {
  if (given) return given;
  return dannan::identity::durable_replace_file;
}

dannan::status::ScopedLogger logger_of(const std::optional<dannan::status::ScopedLogger>& given) {
  if (!given) throw std::invalid_argument("a replica needs its logger");
  return *given;
}

}  // namespace

Replica::Replica(ReplicaOptions options)
    : _checkpoint_path(options.directory / "checkpoint.meta"),
      _journal_path(options.directory / "journal.log"),
      _history_path(options.directory / "history.log"),
      _heads_path(options.directory / "heads.meta"),
      _author_path(options.directory / "author.meta"),
      _author_chain_broken_path(options.directory / "author-chain-broken"),
      _set_aside_path(options.directory / "set-aside.meta"),
      _recovery_path(options.directory / "recovery.required"),
      _content_key(required_key(options.content_key)),
      _local_key(required_key(options.local_key)),
      _magics(options.magics),
      _checkpoint_aad(required_rule(options.aad)(options.magics.checkpoint)),
      _journal_aad(options.aad(options.magics.journal)),
      _history_aad(options.aad(options.magics.history)),
      _heads_aad(options.aad(options.magics.heads)),
      _author_aad(options.aad(options.magics.author)),
      _set_aside_aad(options.aad(options.magics.set_aside)),
      _codec(required_codec(options.codec)),
      _genesis(genesis_record(options.codec->genesis())),
      _accept_pristine_genesis_authority(options.accept_pristine_genesis_authority),
      _materialized_limit_bytes(options.materialization_cache_limit_bytes),
      _tree_store(std::move(options.tree_store)),
      _time(options.time ? *options.time : dannan::contract::steady_time_source()),
      _logger(logger_of(options.logger)),
      _pause(std::move(options.pause)),
      _costs(options.costs),
      _replace_file(file_replacer(std::move(options.replace_file))) {
  std::filesystem::create_directories(options.directory);
  open(std::move(options.recovery_seed));
  _opening = false;
}

std::optional<Record> Replica::valid_seed(const std::optional<Record>& seed) const {
  if (!seed || !valid_record_counted(*seed)) return std::nullopt;
  try {
    (void)decode_counted(seed->payload);
    return seed;
  } catch (...) {
    return std::nullopt;
  }
}

void Replica::open(std::optional<Record> seed) {
  // While the recovery marker exists the fallback checkpoint stays stale, however
  // readable, until replica checkpointing clears the marker.
  if (std::filesystem::exists(_recovery_path)) {
    _recovery_required = true;
    try {
      if (auto checkpoint = load_checkpoint()) {
        _current = *checkpoint;
        set_committed_locked(*checkpoint);
        load_history();
        ensure_history_root(_committed);
        load_journal();
        ensure_history_root(_committed);
        load_heads();
        // This checkpoint may come from the cache: only heads.meta from a peer
        // recovery grants authority.
        refresh_materialized_head_locked();
        _logger.warn("metadata replica still requires replica recovery path=" + _checkpoint_path.parent_path().string() +
                     " generation=" + std::to_string(_committed.generation));
        return;
      }
    } catch (const MaintenanceStopping&) {
      throw;
    } catch (const std::exception& error) {
      _logger.error("metadata recovery checkpoint unreadable: " + std::string(error.what()));
    }
    const auto usable = valid_seed(seed);
    if (!usable) throw std::runtime_error("metadata recovery required but no valid persistent metadata cache seed is available at " + _recovery_path.string());
    recover_from_seed(*usable, "continuing previously marked metadata recovery");
    return;
  }

  try {
    if (auto checkpoint = load_checkpoint()) {
      _current = *checkpoint;
      set_committed_locked(*checkpoint);
      load_history();
      ensure_history_root(_committed);
      load_journal();
      ensure_history_root(_committed);
      load_heads();
      adopt_committed_head_locked();
      refresh_materialized_head_locked();
      return;
    }

    _current = _genesis;
    set_committed_locked(_current);
    reset_checkpoint(_committed);
    load_history();
    ensure_history_root(_committed);
    load_heads();
    adopt_committed_head_locked();
    refresh_materialized_head_locked();
  } catch (const MaintenanceStopping&) {
    // A stop is not a failed primary state: nothing is quarantined for it.
    throw;
  } catch (const std::exception& error) {
    const auto usable = valid_seed(seed);
    if (!usable) throw;
    recover_from_seed(*usable, error.what());
  }
}

void Replica::recover_from_seed(const Record& seed, const std::string& reason) {
  const auto stamp = corrupt_stamp();
  replace_file(_recovery_path, {reinterpret_cast<const uint8_t*>(reason.data()), reason.size()});
  // The seed may predate this node's own commits.
  replace_file(_author_chain_broken_path, {reinterpret_cast<const uint8_t*>(reason.data()), reason.size()});

  std::vector<std::filesystem::path> quarantined;
  for (const auto* path : {&_checkpoint_path, &_journal_path, &_history_path, &_heads_path}) {
    if (auto moved = quarantine_file(*path, stamp)) quarantined.push_back(*moved);
  }

  _current = seed;
  set_committed_locked(seed);
  reset_checkpoint(seed);
  load_history();
  ensure_history_root(seed);
  load_heads();
  // The seed is not accepted: it cannot stand in for the lost durable evidence.
  refresh_materialized_head_locked();
  _recovery_required = true;

  std::string preserved;
  for (const auto& path : quarantined) {
    if (!preserved.empty()) preserved += ",";
    preserved += path.string();
  }
  _logger.error("metadata primary state failed authentication/validation; using persistent cache seed pending replica recovery generation=" +
                std::to_string(seed.generation) + " state=" + _checkpoint_path.parent_path().string() + " preserved=" + preserved + " reason=" + reason);
}

void Replica::set_committed_locked(Record record) {
  _committed = std::move(record);
  _committed_generation.store(_committed.generation, std::memory_order_release);
}

void Replica::pause_walk() const {
  if (_pause) _pause();
}

void Replica::replace_file(const std::filesystem::path& path, std::span<const uint8_t> bytes) const {
  _replace_file(path, as_text(bytes));
  if (_costs) _costs->syncs.fetch_add(1, std::memory_order_relaxed);
}

bool Replica::valid_record_counted(const Record& record) const {
  if (_costs) _costs->record_bytes_hashed.fetch_add(record.payload.size(), std::memory_order_relaxed);
  return valid_record(record);
}

State Replica::decode_counted(std::span<const uint8_t> payload) const {
  if (_costs) _costs->full_decodes.fetch_add(1, std::memory_order_relaxed);
  return _codec->decode_full(payload);
}

Bytes Replica::encode_after_counted(std::span<const uint8_t> delta, const State& state) const {
  auto payload = _codec->encode_after_delta(delta, state);
  if (_costs) _costs->record_bytes_encoded.fetch_add(payload.size(), std::memory_order_relaxed);
  return payload;
}

State Replica::apply_delta(const State& parent, std::span<const uint8_t> delta) const {
  const auto store = _tree_store ? _tree_store() : nullptr;
  return _codec->apply_delta(parent, delta, store.get());
}

std::shared_ptr<const Materialization> Replica::make_materialization(Record record, State state) const {
  uint64_t bytes = sizeof(Materialization) + sizeof(Bytes) + 64;
  bytes += record.payload.size();
  bytes += _codec->resident_bytes(state);
  auto fields = _codec->head_fields(state);
  return std::make_shared<const Materialization>(Materialization{std::move(record), std::move(state), std::move(fields), bytes});
}

std::vector<Hash256> Replica::record_parents(const Record& record) const {
  if (!valid_record_counted(record)) throw DecodeError("invalid metadata record");
  std::vector<Hash256> parents;
  if (record.generation > 1 && record.previous != Hash256{}) parents.push_back(record.previous);
  const auto fields = _codec->head_fields(decode_counted(record.payload));
  for (const auto& parent : fields.merge_parents) {
    if (parent == Hash256{} || parent == record.hash) throw DecodeError("bad metadata merge parent");
    if (std::find(parents.begin(), parents.end(), parent) == parents.end()) parents.push_back(parent);
  }
  return parents;
}

Record Replica::current() const {
  Lock lock(_mutex);
  return _current;
}

Record Replica::committed() const {
  Lock lock(_mutex);
  return _committed;
}

HeadIdentity Replica::current_identity() const {
  Lock lock(_mutex);
  return {_current.generation, _current.hash};
}

HeadIdentity Replica::committed_identity() const {
  Lock lock(_mutex);
  return {_committed.generation, _committed.hash};
}

uint64_t Replica::generation() const {
  Lock lock(_mutex);
  return _current.generation;
}

bool Replica::recovery_required() const {
  Lock lock(_mutex);
  return _recovery_required;
}

void Replica::mark_recovered() {
  Lock lock(_mutex);
  if (!_recovery_required) return;
  std::error_code error;
  const bool removed = std::filesystem::remove(_recovery_path, error);
  if (error) throw std::runtime_error("cannot clear metadata recovery marker " + _recovery_path.string() + ": " + error.message());
  if (removed) sync_directory(_recovery_path);
  _recovery_required = false;
  _logger.info("metadata replica recovery complete generation=" + std::to_string(_committed.generation));
}

void Replica::load_author_locked(const NodeId& node, const HeadClock& head_clock) {
  if (_author_loaded) return;
  _author = {};
  if (std::filesystem::exists(_author_path)) {
    try {
      const auto plaintext = open_file(*_local_key, _magics.author, _author_aad, read_whole(_author_path));
      Reader plain(plaintext);
      _author.id.bytes = plain.fixed<16>();
      _author.reserved = plain.u64();
      _author.accepted = plain.u64();
      const auto past = plain.u32();
      if (past > 64) throw std::runtime_error("too many past author ids");
      for (uint32_t i = 0; i < past; ++i) _author.past.push_back(NodeId{plain.fixed<16>()});
      plain.finish();
    } catch (const std::exception& error) {
      throw std::runtime_error("metadata author file " + _author_path.string() + ": " + error.what());
    }
  } else {
    // No record of what this node has authored. If a head says it has authored
    // before, that record was lost: it may not continue the old author's sequence.
    _author.id = head_clock.contains(node) ? NodeId::random() : node;
    if (_author.id != node) {
      _author.past.push_back(node);
      _logger.warn("metadata author record missing; authoring as a new author");
    }
    _author_loaded = true;
    persist_author_locked();
  }
  _author_loaded = true;
}

void Replica::persist_author_locked() {
  Writer plain;
  plain.fixed(_author.id.bytes);
  plain.u64(_author.reserved);
  plain.u64(_author.accepted);
  plain.u32(static_cast<uint32_t>(_author.past.size()));
  for (const auto& id : _author.past) plain.fixed(id.bytes);
  replace_file(_author_path, seal_file(*_local_key, _magics.author, _author_aad, plain.data()));
}

void Replica::rotate_author_locked(std::string_view why) {
  _author.past.insert(_author.past.begin(), _author.id);
  if (_author.past.size() > 64) _author.past.resize(64);
  _author.id = NodeId::random();
  _author.reserved = 0;
  _author.accepted = 0;
  _logger.warn("metadata author chain cannot be continued (" + std::string(why) + "); authoring as a new author");
}

Dot Replica::reserve_mutation_dot(const NodeId& node, const HeadClock& head_clock) {
  Lock lock(_mutex);
  load_author_locked(node, head_clock);
  const auto observed = [&]() DANNAN_REQUIRES(_mutex) {
    const auto found = head_clock.find(_author.id);
    return found == head_clock.end() ? uint64_t{0} : found->second;
  };
  const bool marked = std::filesystem::exists(_author_chain_broken_path);
  if (marked) {
    rotate_author_locked("the head set was replaced");
  } else if (observed() < _author.accepted) {
    rotate_author_locked("the head lacks a mutation this node had accepted");
  }

  const auto floor = std::max(_author.reserved, observed());
  if (floor == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("metadata mutation sequence exhausted");
  _author.reserved = floor + 1;
  persist_author_locked();
  if (marked) {
    std::error_code ignored;
    std::filesystem::remove(_author_chain_broken_path, ignored);
  }
  return {_author.id, _author.reserved};
}

void Replica::note_author_accepted(const Dot& dot) {
  Lock lock(_mutex);
  if (_author_loaded && dot.author == _author.id) _author.accepted = std::max(_author.accepted, dot.sequence);
}

std::vector<NodeId> Replica::author_ids(const NodeId& node, const HeadClock& head_clock) {
  Lock lock(_mutex);
  load_author_locked(node, head_clock);
  std::vector<NodeId> ids{_author.id};
  ids.insert(ids.end(), _author.past.begin(), _author.past.end());
  return ids;
}

void Replica::load_journal() {
  _journal_records = 0;
  _journal_bytes = 0;
  if (!std::filesystem::exists(_journal_path)) return;

  std::ifstream stream(_journal_path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot open metadata journal " + _journal_path.string());
  const Bytes bytes(std::istreambuf_iterator<char>(stream), {});
  const auto checkpoint_generation = _committed.generation;
  const auto checkpoint_hash = _committed.hash;
  size_t offset = 0;
  size_t valid = 0;
  const auto input = std::span<const uint8_t>(bytes);
  std::string trailing_problem;

  while (offset + 4 <= bytes.size()) {
    pause_walk();
    Reader header(input.subspan(offset, 4));
    const auto length = header.u32();
    if (length > 256U * 1024U * 1024U)
      throw std::runtime_error("metadata journal " + _journal_path.string() + " offset=" + std::to_string(offset) + ": record too large");
    if (offset + 4ULL + length > bytes.size()) {
      trailing_problem = "incomplete trailing frame";
      break;
    }

    const bool final_frame = offset + 4ULL + length == bytes.size();
    FrameEnvelope envelope;
    try {
      envelope = parse_frame_envelope(input.subspan(offset + 4, length));
    } catch (const std::exception& error) {
      trailing_problem = std::string("malformed encrypted frame: ") + error.what();
      break;
    }

    Bytes plaintext;
    try {
      plaintext = open_frame_envelope(*_content_key, _journal_aad, envelope);
    } catch (const std::exception& error) {
      // The journal is a chain after the checkpoint: nothing after an
      // unauthenticated frame (or, below, one that does not fit the chain) can
      // apply. Replay the prefix, the state of a crash before that append; the tail
      // is quarantined below.
      trailing_problem = final_frame ? std::string("final frame failed AES-GCM authentication") : std::string("frame failed authentication: ") + error.what();
      break;
    }

    try {
      Reader reader(plaintext);
      const auto kind = reader.u8();
      Record record;
      record.generation = reader.u64();
      record.previous.bytes = reader.fixed<32>();
      record.hash.bytes = reader.fixed<32>();
      auto body = reader.bytes();
      reader.finish();

      // Compaction writes the checkpoint before truncating the journal, so frames
      // at or below the checkpoint are skipped.
      const bool checkpoint_rollback =
          kind == journal_seed_full && record.generation == checkpoint_generation && record.hash == checkpoint_hash && _current.hash != checkpoint_hash;
      if (record.generation < checkpoint_generation || (record.generation == checkpoint_generation && record.hash == checkpoint_hash && !checkpoint_rollback)) {
        offset += 4 + length;
        valid = offset;
        ++_journal_records;
        _journal_bytes += 4 + length;
        continue;
      }
      if (record.generation == checkpoint_generation && record.hash != checkpoint_hash) throw std::runtime_error("conflicts with checkpoint");

      switch (kind) {
        case journal_prepare_full: {
          if (record.generation != _current.generation + 1 || record.previous != _current.hash) throw std::runtime_error("full CAS chain broken");
          record.payload = std::move(body);
          if (!valid_record_counted(record)) throw std::runtime_error("full CAS hash invalid");
          (void)decode_counted(record.payload);
          _current = std::move(record);
          _pending_history = history_for_current();
          break;
        }
        case journal_prepare_delta: {
          if (record.generation != _current.generation + 1 || record.previous != _current.hash) throw std::runtime_error("delta CAS chain broken");
          const auto replayed = apply_delta(decode_counted(_current.payload), body);
          record.payload = encode_after_counted(body, replayed);
          if (!valid_record_counted(record)) throw std::runtime_error("delta CAS hash invalid");
          _current = std::move(record);
          _pending_history = history_for_current(body);
          break;
        }
        case journal_seed_full: {
          record.payload = std::move(body);
          if (!valid_record_counted(record)) throw std::runtime_error("seed hash invalid");
          const auto parents = record_parents(record);

          // A seed replaces the uncommitted proposal: it may roll back to the
          // committed head, advance from it, or install a descendant whose ancestry
          // is in history. Committed history is never rewritten.
          if (record.hash == _committed.hash) {
            _current = _committed;
            _pending_history.reset();
            break;
          }
          if (record.generation < _committed.generation) throw std::runtime_error("seed predates committed metadata");
          const bool fresh = _committed.generation <= 1;
          const bool direct_parent = std::find(parents.begin(), parents.end(), _committed.hash) != parents.end();
          const bool known_descendant = _history.contains(record.hash) && history_is_ancestor_locked(_committed.hash, record.hash, true);
          bool parent_descends_from_committed = false;
          for (const auto& parent : parents)
            parent_descends_from_committed |= _history.contains(parent) && history_is_ancestor_locked(_committed.hash, parent, true);
          if (!fresh && !direct_parent && !known_descendant && !parent_descends_from_committed)
            throw std::runtime_error("seed is not descended from committed metadata");
          _current = std::move(record);
          _pending_history = history_for_current();
          break;
        }
        case journal_commit:
          if (record.generation == _committed.generation && record.hash == _committed.hash) break;
          if (record.generation != _current.generation || record.hash != _current.hash) throw std::runtime_error("commit does not match current");
          set_committed_locked(_current);
          _pending_history.reset();
          break;
        default:
          throw std::runtime_error("unknown record type " + std::to_string(kind));
      }
    } catch (const MaintenanceStopping&) {
      throw;
    } catch (const std::exception& error) {
      // Ends the replayable prefix. _current and _committed are assigned only after
      // a record validates, so nothing partial remains.
      trailing_problem = std::string("record does not fit the chain: ") + error.what();
      break;
    }

    offset += 4 + length;
    valid = offset;
    ++_journal_records;
    _journal_bytes += 4 + length;
  }

  if (valid != bytes.size()) {
    if (trailing_problem.empty()) trailing_problem = "trailing bytes after last complete frame";
    const auto tail = input.subspan(valid);
    const auto quarantine = quarantine_tail(_journal_path, tail);
    truncate_durable(_journal_path, valid);
    _logger.warn("metadata journal recovered path=" + _journal_path.string() + " offset=" + std::to_string(valid) +
                 " discarded_bytes=" + std::to_string(tail.size()) + " quarantine=" + quarantine.string() + " reason=" + trailing_problem);
  }
}

Bytes Replica::encode_history_frame(const HistoryEntry& entry) const { return seal_frame(*_content_key, _history_aad, encode_history_entry(entry)); }

Replica::HistoryIndexEntry Replica::index_history_entry(const HistoryEntry& entry, uint64_t file_offset, uint64_t frame_bytes) {
  return HistoryIndexEntry{entry.generation, entry.previous, entry.hash, entry.previous_known, entry.merge_parents, entry.body, file_offset, frame_bytes};
}

HistoryEntry Replica::read_history_entry(const HistoryIndexEntry& index) const {
  if (index.frame_bytes < 4 || index.frame_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    throw std::runtime_error("invalid metadata history frame index");
  std::ifstream stream(_history_path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot open metadata history " + _history_path.string());
  stream.seekg(static_cast<std::streamoff>(index.file_offset), std::ios::beg);
  Bytes bytes(static_cast<size_t>(index.frame_bytes));
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("cannot read indexed metadata history frame");
  Reader frame(bytes);
  const auto length = frame.u32();
  const auto envelope_bytes = frame.view(length);
  frame.finish();
  auto value = decode_history_entry(open_frame_envelope(*_content_key, _history_aad, parse_frame_envelope(envelope_bytes)));
  if (value.hash != index.hash || value.generation != index.generation || value.previous != index.previous || value.previous_known != index.previous_known ||
      value.merge_parents != index.merge_parents || value.body != index.body)
    throw std::runtime_error("indexed metadata history identity mismatch");
  return value;
}

void Replica::append_history(const HistoryEntry& entry) {
  if (_history.contains(entry.hash)) return;
  const auto bytes = encode_history_frame(entry);
  const auto file_offset = _history_bytes;
  write_history_frame(bytes);
  _history.emplace(entry.hash, index_history_entry(entry, file_offset, bytes.size()));
  ++_history_records;
  _history_bytes += bytes.size();
}

void Replica::write_history_frame(std::span<const uint8_t> bytes) const {
  const int fd = ::open(_history_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("cannot open metadata history " + _history_path.string() + ": " + std::strerror(errno));
  size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      const auto error = errno;
      ::close(fd);
      throw std::runtime_error("cannot write metadata history " + _history_path.string() + ": " + std::strerror(error));
    }
    offset += static_cast<size_t>(count);
  }
  if (::fsync(fd) != 0) {
    const auto error = errno;
    ::close(fd);
    throw std::runtime_error("cannot sync metadata history " + _history_path.string() + ": " + std::strerror(error));
  }
  if (::close(fd) != 0) throw std::runtime_error("cannot close metadata history " + _history_path.string() + ": " + std::strerror(errno));
  if (_costs) {
    _costs->history_frames_written.fetch_add(1, std::memory_order_relaxed);
    _costs->syncs.fetch_add(1, std::memory_order_relaxed);
  }
}

void Replica::load_history() {
  constexpr uint64_t max_record = 2ULL * 1024ULL * 1024ULL * 1024ULL;
  constexpr uint64_t max_quarantine_tail = 16ULL * 1024ULL * 1024ULL;
  _history.clear();
  _materialized.clear();
  _materialized_clock = 0;
  _materialized_bytes = 0;
  _history_records = 0;
  _history_bytes = 0;
  if (!std::filesystem::exists(_history_path)) return;

  const uint64_t file_size = std::filesystem::file_size(_history_path);
  std::ifstream stream(_history_path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot open metadata history " + _history_path.string());
  uint64_t offset = 0;
  uint64_t valid = 0;
  std::string trailing_problem;
  size_t skipped = 0;
  std::string first_skipped;

  // Streaming: memory at open is bounded by one frame, not the file.
  while (offset + 4 <= file_size) {
    pause_walk();
    std::array<uint8_t, 4> header_bytes{};
    if (!stream.read(reinterpret_cast<char*>(header_bytes.data()), header_bytes.size())) {
      trailing_problem = "incomplete trailing frame header";
      break;
    }
    Reader header(header_bytes);
    const auto length = static_cast<uint64_t>(header.u32());
    if (length > max_record)
      throw std::runtime_error("metadata history " + _history_path.string() + " offset=" + std::to_string(offset) + ": record too large");
    if (offset + 4 + length > file_size) {
      trailing_problem = "incomplete trailing frame";
      break;
    }

    Bytes frame(static_cast<size_t>(length));
    if (length && !stream.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(length))) {
      trailing_problem = "incomplete trailing frame";
      break;
    }
    const bool final_frame = offset + 4 + length == file_size;
    bool torn = false;
    try {
      const auto envelope = parse_frame_envelope(frame);
      Bytes plaintext;
      try {
        plaintext = open_frame_envelope(*_content_key, _history_aad, envelope);
      } catch (const std::exception&) {
        if (!final_frame) throw;
        torn = true;
      }
      if (!torn) {
        auto entry = decode_history_entry(plaintext);

        // Linear in history bytes: each frame is validated alone; reconstruction
        // is left to the heads that can become authority.
        if (entry.body == HistoryEntry::Body::full) {
          Record record;
          record.generation = entry.generation;
          record.previous = entry.previous;
          record.hash = entry.hash;
          record.payload = entry.payload;
          if (!valid_record_counted(record)) throw DecodeError("invalid full metadata history record");
          // Not decoded: the frame and the hash bind the identity, and a head is
          // decoded and cross-checked when materialised.
        } else {
          if (!entry.previous_known || entry.generation <= 1) throw DecodeError("metadata delta history has no predecessor");
          const auto parent = _history.find(entry.previous);
          if (parent == _history.end() || !delta_succession_valid(parent->second.generation, entry.generation))
            throw DecodeError("metadata delta history predecessor missing or invalid");
          _codec->check_delta(entry.payload);
        }

        auto index = index_history_entry(entry, offset, 4 + length);
        if (const auto existing = _history.find(entry.hash); existing != _history.end()) {
          // reanchor_history appends a whole body for an indexed hash; the hash
          // binds the identity, so the whole body is preferred. A conflicting
          // identity is corruption.
          if (existing->second.generation != entry.generation || existing->second.previous != entry.previous)
            throw DecodeError("duplicate metadata history record with conflicting identity");
          if (entry.body == HistoryEntry::Body::full) existing->second = std::move(index);
        } else {
          _history.emplace(entry.hash, std::move(index));
        }
      }
    } catch (const MaintenanceStopping&) {
      throw;
    } catch (const std::exception& error) {
      // Entries are independent and hash-indexed: skip a bad one (its dependants
      // fail their predecessor check too); heads needing it are repaired from peers.
      ++skipped;
      if (skipped == 1) first_skipped = "offset=" + std::to_string(offset) + ": " + error.what();
      offset += 4 + length;
      valid = offset;
      continue;
    }
    if (torn) {
      trailing_problem = "final frame failed AES-GCM authentication";
      break;
    }

    offset += 4 + length;
    valid = offset;
    ++_history_records;
  }
  if (skipped != 0)
    _logger.warn("metadata history skipped frames that could not be replayed path=" + _history_path.string() + " count=" + std::to_string(skipped) +
                 " first=" + first_skipped + "; dependent heads are repaired live from peers");

  if (valid != file_size) {
    if (trailing_problem.empty()) trailing_problem = "trailing bytes after last complete frame";
    const uint64_t tail_size = file_size - valid;
    std::string quarantine = "<discarded: over quarantine limit>";
    if (tail_size <= max_quarantine_tail) {
      stream.clear();
      stream.seekg(static_cast<std::streamoff>(valid), std::ios::beg);
      Bytes tail(static_cast<size_t>(tail_size));
      if (tail_size && !stream.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail_size)))
        throw std::runtime_error("cannot read corrupt metadata history tail");
      quarantine = quarantine_tail(_history_path, tail).string();
    }
    stream.close();
    truncate_durable(_history_path, valid);
    _logger.warn("metadata history recovered path=" + _history_path.string() + " offset=" + std::to_string(valid) +
                 " discarded_bytes=" + std::to_string(tail_size) + " quarantine=" + quarantine + " reason=" + trailing_problem);
  }
  _history_bytes = valid;
#if defined(__GLIBC__)
  // Only the index is retained; return the scan's transient arenas to the OS.
  (void)malloc_trim(0);
#endif
}

void Replica::load_heads() {
  const RevisionBump bump(_heads_revision);
  _accepted_heads.clear();
  if (!std::filesystem::exists(_heads_path)) return;
  try {
    auto values = decode_acceptance_set(open_file(*_local_key, _magics.heads, _heads_aad, read_whole(_heads_path)));
    for (auto& value : values) {
      const auto reconstructed = materialized_locked(value.hash);
      if (!reconstructed) {
        // The certificate is durable evidence of acceptance and any peer can supply
        // the immutable record: keep it, flag the head, repair it live.
        const auto reason = diagnose_unreconstructable_locked(value.hash);
        _logger.error("metadata accepted head is not reconstructible locally; keeping it for live repair from peers hash=" + hex(value.hash) +
                      " generation=" + std::to_string(value.generation) + " reason=" + reason);
        _unreconstructable_retry_at[value.hash] = _time.now() + unreconstructable_retry_cooldown;
        const auto hash = value.hash;
        _accepted_heads.emplace(hash, std::move(value));
        continue;
      }
      // The hash binds the generation: a mismatch is a corrupt certificate.
      if (reconstructed->record.generation != value.generation)
        throw std::runtime_error("accepted metadata head certificate generation does not match its record");
      if (!acceptance_matches_record_policy_locked(value, *reconstructed)) throw std::runtime_error("accepted metadata head policy does not match commit");
      const auto hash = value.hash;
      _accepted_heads.emplace(hash, std::move(value));
    }
    if (prune_accepted_heads_locked()) persist_heads_locked();
  } catch (const MaintenanceStopping&) {
    throw;
  } catch (const std::exception& error) {
    throw std::runtime_error("metadata accepted-head file " + _heads_path.string() + ": " + error.what());
  }
}

void Replica::persist_heads_locked() {
  std::vector<Acceptance> values;
  values.reserve(_accepted_heads.size());
  for (const auto& [_, value] : _accepted_heads) values.push_back(value);
  const auto sealed = seal_file(*_local_key, _magics.heads, _heads_aad, encode_acceptance_set(values));
  try {
    replace_file(_heads_path, sealed);
    _persistence_writes.fetch_add(1, std::memory_order_relaxed);
    _persistence_bytes.fetch_add(sealed.size(), std::memory_order_relaxed);
  } catch (...) {
    _persistence_failures.fetch_add(1, std::memory_order_relaxed);
    throw;
  }
}

bool Replica::prune_accepted_heads_locked() {
  const RevisionBump bump(_heads_revision);
  bool changed = false;
  for (auto it = _accepted_heads.begin(); it != _accepted_heads.end();) {
    bool ancestor = false;
    for (const auto& [other, _] : _accepted_heads) {
      if (other == it->first) continue;
      if (accepted_head_is_ancestor_locked(it->first, other)) {
        ancestor = true;
        break;
      }
    }
    if (ancestor) {
      it = _accepted_heads.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  return changed;
}

bool Replica::accepted_head_is_ancestor_locked(const Hash256& ancestor, const Hash256& descendant) const {
  if (history_is_ancestor_locked(ancestor, descendant, false)) return true;

  // Genesis is subsumed by any later record even when compaction removed the edge,
  // so a pristine replica can adopt a cluster head and a joiner's late genesis
  // certificate is ignored.
  if (ancestor == _genesis.hash) {
    const auto found = materialized_locked(descendant);
    return found && found->record.generation > _genesis.generation;
  }

  // History is truncated by each node on its own, so the edge between two heads may
  // be gone. The clocks still say it: a head whose clock covers another's and goes
  // beyond it has incorporated every mutation the other holds.
  const auto older = materialized_locked(ancestor);
  const auto newer = materialized_locked(descendant);
  if (!older || !newer) return false;
  const auto& behind = older->fields.clock;
  const auto& ahead = newer->fields.clock;
  // A head with no clock says nothing about what it holds.
  return !behind.empty() && behind != ahead && std::all_of(behind.begin(), behind.end(), [&](const auto& item) {
    const auto found = ahead.find(item.first);
    return found != ahead.end() && found->second >= item.second;
  });
}

void Replica::adopt_committed_head_locked() {
  const RevisionBump bump(_heads_revision);
  ensure_history_root(_committed);
  if (_committed.hash == _genesis.hash) {
    // A joiner never holds genesis as an accepted head; it stays local material. A
    // founder holds it until its first commit, or any accepted head subsumes it.
    if (!_accept_pristine_genesis_authority) {
      if (_accepted_heads.erase(_genesis.hash) != 0) persist_heads_locked();
    } else if (_accepted_heads.empty()) {
      _accepted_heads.emplace(_genesis.hash, Acceptance{_genesis.generation, _genesis.hash, 0, {}});
      persist_heads_locked();
    }
    return;
  }
  if (_accepted_heads.contains(_committed.hash)) return;

  // Without a certificate a checkpoint is not authority: keep it and force peer
  // recovery.
  _recovery_required = true;
  const std::string_view reason = "metadata checkpoint has no durable acceptance certificate";
  replace_file(_recovery_path, {reinterpret_cast<const uint8_t*>(reason.data()), reason.size()});
}

bool Replica::acceptance_matches_record_policy_locked(const Acceptance& acceptance, const Materialization& materialization) const {
  // Only genesis has no write floor, and its certificate names nobody.
  if (materialization.fields.floor == 0) return materialization.record.hash == _genesis.hash && acceptance.required == 0 && acceptance.replicas.empty();

  // `required` records how many nodes held the commit at acceptance; one, the
  // author, suffices.
  if (acceptance.required == 0) return false;
  for (const auto& witness : acceptance.replicas)
    if (witness == NodeId{}) return false;
  return true;
}

bool Replica::refresh_materialized_head_in_memory_locked() {
  const RevisionBump bump(_heads_revision);
  if (_accepted_heads.empty()) return false;
  // The cooldown bounds how often a broken head re-throws; this runs on every read.
  const auto now = _time.now();
  for (auto it = _unreconstructable_retry_at.begin(); it != _unreconstructable_retry_at.end();) {
    it = _accepted_heads.contains(it->first) ? std::next(it) : _unreconstructable_retry_at.erase(it);
  }
  for (auto it = _set_aside.begin(); it != _set_aside.end();) {
    it = _accepted_heads.contains(it->first) ? std::next(it) : _set_aside.erase(it);
  }
  if (_set_aside.size() >= _accepted_heads.size()) _set_aside.clear();
  note_set_aside_locked();
  std::optional<Record> selected;
  for (const auto& [hash, _] : _accepted_heads) {
    if (_set_aside.contains(hash)) continue;
    if (const auto found = _unreconstructable_retry_at.find(hash); found != _unreconstructable_retry_at.end() && now < found->second) continue;
    auto record = historical_locked(hash);
    if (!record) {
      const auto reason = flag_unreconstructable_locked(hash, now, "materialized head refresh");
      throw std::runtime_error("accepted metadata head cannot be reconstructed hash=" + hex(hash) + " reason=" + reason);
    }
    _unreconstructable_retry_at.erase(hash);
    if (!selected || record->generation > selected->generation || (record->generation == selected->generation && record->hash > selected->hash))
      selected = std::move(record);
  }
  if (!selected || selected->hash == _committed.hash) return false;

  // _committed is only the preferred materialised head; authority is the accepted
  // set, and moving it deletes no branch from history.
  set_committed_locked(*selected);
  _current = _committed;
  _pending_history.reset();
  return true;
}

void Replica::note_set_aside_locked() {
  uint64_t highest = 0;
  for (const auto& [_, generation] : _set_aside) highest = std::max(highest, generation);
  _set_aside_generation.store(highest, std::memory_order_release);
}

void Replica::load_set_aside_since_locked() {
  if (_set_aside_since_loaded) return;
  _set_aside_since_loaded = true;
  if (!std::filesystem::exists(_set_aside_path)) return;
  try {
    const auto plaintext = open_file(*_local_key, _magics.set_aside, _set_aside_aad, read_whole(_set_aside_path));
    Reader reader(plaintext);
    const auto count = reader.u32();
    if (count > 4096) throw DecodeError("too many set-aside heads");
    for (uint32_t i = 0; i < count; ++i) {
      const Hash256 hash{reader.fixed<32>()};
      const auto since = reader.u64();
      _set_aside_since.emplace(hash, since);
    }
    reader.finish();
  } catch (const std::exception& error) {
    // Lost times only restart the wait.
    _logger.warn("metadata set-aside record unreadable: " + std::string(error.what()));
    _set_aside_since.clear();
  }
}

void Replica::persist_set_aside_since_locked() {
  Writer writer;
  writer.u32(static_cast<uint32_t>(_set_aside_since.size()));
  for (const auto& [hash, since] : _set_aside_since) {
    writer.fixed(hash.bytes);
    writer.u64(since);
  }
  replace_file(_set_aside_path, seal_file(*_local_key, _magics.set_aside, _set_aside_aad, writer.data()));
}

bool Replica::set_aside(const Hash256& hash, uint64_t now_unix_ms) {
  const RevisionBump bump(_heads_revision);
  Lock durable(_durable_mutex);
  Lock lock(_mutex);
  const auto found = _accepted_heads.find(hash);
  if (found == _accepted_heads.end() || _set_aside.size() + 1 >= _accepted_heads.size()) return false;
  _set_aside.emplace(hash, found->second.generation);
  load_set_aside_since_locked();
  if (_set_aside_since.try_emplace(hash, now_unix_ms).second) persist_set_aside_since_locked();
  const auto pending_checkpoint = refresh_materialized_head_deferred_locked();
  lock.unlock();
  if (pending_checkpoint) persist_checkpoint(*pending_checkpoint);
  return true;
}

size_t Replica::expire_set_aside(uint64_t now_unix_ms, std::chrono::milliseconds horizon) {
  const RevisionBump bump(_heads_revision);
  Lock durable(_durable_mutex);
  Lock lock(_mutex);
  load_set_aside_since_locked();
  if (_set_aside_since.empty()) return 0;
  size_t dropped = 0;
  bool changed = false;
  for (auto it = _set_aside_since.begin(); it != _set_aside_since.end();) {
    const bool held = _accepted_heads.contains(it->first);
    const bool expired = held && _set_aside.contains(it->first) && _accepted_heads.size() > 1 && now_unix_ms >= it->second &&
                         now_unix_ms - it->second >= static_cast<uint64_t>(horizon.count());
    if (expired) {
      _accepted_heads.erase(it->first);
      _set_aside.erase(it->first);
      ++dropped;
    }
    if (expired || !held) {
      it = _set_aside_since.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) persist_set_aside_since_locked();
  std::optional<Record> pending_checkpoint;
  if (dropped != 0) {
    persist_heads_locked();
    pending_checkpoint = refresh_materialized_head_deferred_locked();
  }
  lock.unlock();
  if (pending_checkpoint) persist_checkpoint(*pending_checkpoint);
  return dropped;
}

void Replica::clear_set_aside() {
  const RevisionBump bump(_heads_revision);
  Lock durable(_durable_mutex);
  Lock lock(_mutex);
  if (_set_aside.empty()) return;
  _set_aside.clear();
  const auto pending_checkpoint = refresh_materialized_head_deferred_locked();
  lock.unlock();
  if (pending_checkpoint) persist_checkpoint(*pending_checkpoint);
}

std::vector<Record> Replica::usable_heads() const {
  auto heads = accepted_heads();
  Lock lock(_mutex);
  std::vector<Record> out;
  out.reserve(heads.size());
  for (auto& head : heads) {
    if (!_set_aside.contains(head.hash)) out.push_back(std::move(head));
  }
  return out;
}

std::vector<HeadIdentity> Replica::usable_head_identities() const {
  Lock lock(_mutex);
  std::vector<HeadIdentity> out;
  out.reserve(_accepted_heads.size());
  for (const auto& [hash, head] : _accepted_heads)
    if (!_set_aside.contains(hash)) out.push_back({head.generation, hash});
  return out;
}

void Replica::refresh_materialized_head_locked() {
  if (refresh_materialized_head_in_memory_locked()) reset_checkpoint(_committed);
}

std::optional<Record> Replica::refresh_materialized_head_deferred_locked() {
  if (!refresh_materialized_head_in_memory_locked()) return std::nullopt;
  reset_checkpoint_journal_locked();
  return _committed;
}

void Replica::ensure_history_root(const Record& record) {
  if (_history.contains(record.hash)) {
    (void)cache_materialization_locked(record);
    return;
  }
  HistoryEntry root;
  root.generation = record.generation;
  root.previous = record.previous;
  root.hash = record.hash;
  // The predecessor material may be absent; the record is a local history root.
  root.previous_known = false;
  root.merge_parents = _codec->head_fields(decode_counted(record.payload)).merge_parents;
  root.body = HistoryEntry::Body::full;
  root.payload.assign(record.payload.begin(), record.payload.end());
  append_history(root);
  (void)cache_materialization_locked(record);
}

HistoryEntry Replica::history_for_current(std::span<const uint8_t> delta) {
  HistoryEntry entry;
  entry.generation = _current.generation;
  entry.previous = _current.previous;
  entry.hash = _current.hash;
  entry.previous_known = _current.generation > 1 && _current.previous != Hash256{};
  entry.merge_parents = _codec->head_fields(decode_counted(_current.payload)).merge_parents;
  constexpr uint64_t full_anchor_interval = 256;
  if (!delta.empty() && _current.generation % full_anchor_interval != 0) {
    entry.body = HistoryEntry::Body::delta;
    entry.payload.assign(delta.begin(), delta.end());
  } else {
    entry.body = HistoryEntry::Body::full;
    entry.payload.assign(_current.payload.begin(), _current.payload.end());
  }
  return entry;
}

std::shared_ptr<const Materialization> Replica::cache_materialization_locked(const Record& record) const {
  if (const auto found = _materialized.find(record.hash); found != _materialized.end()) {
    found->second.last_used = ++_materialized_clock;
    return found->second.value;
  }
  return cache_materialization_locked(make_materialization(record, decode_counted(record.payload)));
}

std::shared_ptr<const Materialization> Replica::cache_materialization_locked(std::shared_ptr<const Materialization> value) const {
  constexpr size_t cache_limit = 64;
  const auto& hash = value->record.hash;
  if (const auto found = _materialized.find(hash); found != _materialized.end()) {
    found->second.last_used = ++_materialized_clock;
    return found->second.value;
  }
  const uint64_t bytes = value->resident_bytes;
  const bool incoming_pinned = hash == _current.hash || hash == _committed.hash;

  while (_materialized.size() >= cache_limit || bytes > _materialized_limit_bytes || _materialized_bytes > _materialized_limit_bytes - bytes) {
    auto victim = _materialized.end();
    for (auto it = _materialized.begin(); it != _materialized.end(); ++it) {
      if (it->first == _current.hash || it->first == _committed.hash) continue;
      if (victim == _materialized.end() || it->second.last_used < victim->second.last_used) victim = it;
    }
    // Only current and committed are pinned; other heads are reconstructible.
    if (victim == _materialized.end()) break;
    _materialized_bytes -= victim->second.bytes;
    _materialized.erase(victim);
    _cache_evictions.fetch_add(1, std::memory_order_relaxed);
  }

  // Over budget and unpinned: return it uncached.
  if (bytes > _materialized_limit_bytes && !incoming_pinned) return value;
  _materialized.emplace(hash, MaterializedEntry{value, ++_materialized_clock, bytes});
  _materialized_bytes += bytes;
  return value;
}

std::shared_ptr<const Materialization> Replica::materialized_locked(const Hash256& target) const {
  _historical_requests.fetch_add(1, std::memory_order_relaxed);
  const auto found = _history.find(target);
  if (found == _history.end()) return {};

  if (const auto cached = _materialized.find(target); cached != _materialized.end()) {
    cached->second.last_used = ++_materialized_clock;
    _cache_hits.fetch_add(1, std::memory_order_relaxed);
    return cached->second.value;
  }
  _cache_misses.fetch_add(1, std::memory_order_relaxed);
  _historical_reconstructions.fetch_add(1, std::memory_order_relaxed);

  // _history holds only frame indexes; this holds one state and one delta body at a
  // time. Intermediates never enter the cache.
  std::vector<HistoryIndexEntry> deltas;
  std::set<Hash256> seen;
  HistoryIndexEntry cursor = found->second;
  std::shared_ptr<const Materialization> materialized_parent;
  while (cursor.body == HistoryEntry::Body::delta) {
    if (!seen.insert(cursor.hash).second || !cursor.previous_known) return {};
    deltas.push_back(cursor);
    const auto parent = _history.find(cursor.previous);
    if (parent == _history.end()) return {};
    cursor = parent->second;
    if (const auto cached = _materialized.find(cursor.hash); cached != _materialized.end()) {
      cached->second.last_used = ++_materialized_clock;
      materialized_parent = cached->second.value;
      break;
    }
  }

  Record working_record;
  State working_state;
  try {
    if (materialized_parent) {
      working_record = materialized_parent->record;
      working_state = materialized_parent->state;
      materialized_parent.reset();
    } else {
      auto anchor = read_history_entry(cursor);
      working_record.generation = anchor.generation;
      working_record.previous = anchor.previous;
      working_record.hash = anchor.hash;
      working_record.payload = std::move(anchor.payload);
      if (!valid_record_counted(working_record)) return {};
      working_state = decode_counted(working_record.payload);
      if (_codec->head_fields(working_state).merge_parents != anchor.merge_parents) return {};
    }

    for (auto it = deltas.rbegin(); it != deltas.rend(); ++it) {
      if (_opening) pause_walk();
      const auto child = read_history_entry(*it);
      if (child.previous != working_record.hash || !delta_succession_valid(working_record.generation, child.generation)) return {};
      working_state = apply_delta(working_state, child.payload);
      _historical_deltas_applied.fetch_add(1, std::memory_order_relaxed);
      Record next;
      next.generation = child.generation;
      next.previous = child.previous;
      next.hash = child.hash;
      next.payload = encode_after_counted(child.payload, working_state);
      if (!valid_record_counted(next) || _codec->head_fields(working_state).merge_parents != child.merge_parents) return {};
      working_record = std::move(next);
    }
    if (working_record.hash != target) return {};
    return cache_materialization_locked(make_materialization(std::move(working_record), std::move(working_state)));
  } catch (const MaintenanceStopping&) {
    throw;
  } catch (...) {
    return {};
  }
}

std::optional<Record> Replica::historical_locked(const Hash256& target) const {
  const auto value = materialized_locked(target);
  if (!value) return std::nullopt;
  return value->record;
}

ReplicaDiagnostics Replica::diagnostics() const {
  Lock lock(_mutex);
  return {
      _historical_requests.load(std::memory_order_relaxed),
      _historical_reconstructions.load(std::memory_order_relaxed),
      _historical_deltas_applied.load(std::memory_order_relaxed),
      _cache_hits.load(std::memory_order_relaxed),
      _cache_misses.load(std::memory_order_relaxed),
      _cache_evictions.load(std::memory_order_relaxed),
      _materialized.size(),
      _materialized_bytes,
      _materialized_limit_bytes,
      _history_records,
      _history_bytes,
      0,
      _persistence_writes.load(std::memory_order_relaxed),
      _persistence_bytes.load(std::memory_order_relaxed),
      _persistence_failures.load(std::memory_order_relaxed),
  };
}

bool Replica::history_is_ancestor_locked(const Hash256& ancestor, const Hash256& descendant, bool may_pause) const {
  if (ancestor == descendant) return _history.contains(ancestor);
  std::vector<Hash256> pending{descendant};
  std::set<Hash256> seen;
  while (!pending.empty()) {
    if (may_pause) pause_walk();
    const auto current = pending.back();
    pending.pop_back();
    if (!seen.insert(current).second) continue;
    const auto found = _history.find(current);
    if (found == _history.end()) continue;
    const auto& entry = found->second;
    // A compacted root's direct parent hash is valid ancestry, though not
    // traversable when previous_known is false.
    if (entry.previous != Hash256{} && entry.previous == ancestor) return true;
    if (entry.previous_known || (entry.previous != Hash256{} && _history.contains(entry.previous))) pending.push_back(entry.previous);
    for (const auto& parent : entry.merge_parents) {
      if (parent == ancestor) return true;
      pending.push_back(parent);
    }
  }
  return false;
}

std::optional<Hash256> Replica::history_common_ancestor_locked(const Hash256& left, const Hash256& right) const {
  std::map<Hash256, uint64_t> left_ancestors;
  std::vector<Hash256> pending{left};
  while (!pending.empty()) {
    pause_walk();
    const auto current = pending.back();
    pending.pop_back();
    if (left_ancestors.contains(current)) continue;
    const auto found = _history.find(current);
    const uint64_t generation = found == _history.end() ? 0 : found->second.generation;
    left_ancestors[current] = generation;
    if (found == _history.end()) continue;
    if (found->second.previous_known || (found->second.previous != Hash256{} && _history.contains(found->second.previous))) {
      pending.push_back(found->second.previous);
    } else if (found->second.previous != Hash256{}) {
      // The boundary parent may be the common ancestor; do not walk into compacted
      // history.
      const auto parent = _history.find(found->second.previous);
      left_ancestors.emplace(found->second.previous, parent == _history.end() ? 0 : parent->second.generation);
    }
    for (const auto& parent : found->second.merge_parents) pending.push_back(parent);
  }

  std::optional<Hash256> best;
  uint64_t best_generation = 0;
  std::set<Hash256> seen;
  const auto consider = [&](const Hash256& candidate, uint64_t generation) {
    const auto intersection = left_ancestors.find(candidate);
    if (intersection == left_ancestors.end()) return;
    generation = std::max(generation, intersection->second);
    if (!best || generation > best_generation || (generation == best_generation && *best < candidate)) {
      best = candidate;
      best_generation = generation;
    }
  };
  pending = {right};
  while (!pending.empty()) {
    pause_walk();
    const auto current = pending.back();
    pending.pop_back();
    if (!seen.insert(current).second) continue;
    const auto found = _history.find(current);
    consider(current, found == _history.end() ? 0 : found->second.generation);
    if (found == _history.end()) continue;
    if (found->second.previous_known || (found->second.previous != Hash256{} && _history.contains(found->second.previous))) {
      pending.push_back(found->second.previous);
    } else if (found->second.previous != Hash256{}) {
      // As above: matched, not traversed.
      const auto parent = _history.find(found->second.previous);
      consider(found->second.previous, parent == _history.end() ? 0 : parent->second.generation);
    }
    for (const auto& parent : found->second.merge_parents) pending.push_back(parent);
  }
  return best;
}

std::optional<HistoryEntry> Replica::history_entry(const Hash256& hash) const {
  HistoryIndexEntry index;
  {
    Lock lock(_mutex);
    const auto found = _history.find(hash);
    if (found == _history.end()) return std::nullopt;
    index = found->second;
  }
  try {
    return read_history_entry(index);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<HistoryLinks> Replica::history_links(const Hash256& hash) const {
  Lock lock(_mutex);
  const auto found = _history.find(hash);
  if (found == _history.end()) return std::nullopt;
  return HistoryLinks{found->second.generation, found->second.previous, found->second.previous_known, found->second.merge_parents, found->second.body};
}

std::optional<HistoryEntry> Replica::full_history_record(const Hash256& hash) const {
  const auto value = materialized(hash);
  if (!value) return std::nullopt;
  HistoryEntry entry;
  entry.generation = value->record.generation;
  entry.previous = value->record.previous;
  entry.hash = value->record.hash;
  entry.previous_known = entry.generation > 1 && entry.previous != Hash256{} && history_contains(entry.previous);
  entry.merge_parents = value->fields.merge_parents;
  entry.body = HistoryEntry::Body::full;
  entry.payload.assign(value->record.payload.begin(), value->record.payload.end());
  return entry;
}

std::vector<Hash256> Replica::unreconstructable_heads() const {
  Lock lock(_mutex);
  std::vector<Hash256> out;
  for (const auto& [hash, _] : _unreconstructable_retry_at)
    if (_accepted_heads.contains(hash)) out.push_back(hash);
  return out;
}

std::string Replica::flag_unreconstructable_locked(const Hash256& hash, dannan::contract::TimePoint now, std::string_view context) const {
  const RevisionBump bump(_heads_revision);
  const bool first_failure = !_unreconstructable_retry_at.contains(hash);
  _unreconstructable_retry_at[hash] = now + unreconstructable_retry_cooldown;
  const auto reason = diagnose_unreconstructable_locked(hash);
  if (first_failure) {
    uint64_t generation = 0;
    if (const auto head = _accepted_heads.find(hash); head != _accepted_heads.end()) generation = head->second.generation;
    _logger.warn("metadata accepted head cannot be reconstructed locally; excluded from reads pending live repair hash=" + hex(hash) +
                 " generation=" + std::to_string(generation) + " during=" + std::string(context) + " reason=" + reason);
  }
  return reason;
}

std::string Replica::diagnose_unreconstructable_locked(const Hash256& target) const {
  const auto describe = [](const HistoryIndexEntry& entry) {
    return "gen=" + std::to_string(entry.generation) + " hash=" + hex(entry.hash) +
           " body=" + std::string(entry.body == HistoryEntry::Body::delta ? "delta" : "full");
  };
  const auto found = _history.find(target);
  if (found == _history.end()) return "not in local history index";
  std::vector<HistoryIndexEntry> chain;
  std::set<Hash256> seen;
  HistoryIndexEntry cursor = found->second;
  while (cursor.body == HistoryEntry::Body::delta) {
    if (!seen.insert(cursor.hash).second) return "delta chain cycle at " + describe(cursor);
    if (!cursor.previous_known) return "delta " + describe(cursor) + " has no known predecessor";
    chain.push_back(cursor);
    const auto parent = _history.find(cursor.previous);
    if (parent == _history.end()) return "delta " + describe(cursor) + " primary parent hash=" + hex(cursor.previous) + " absent from local history";
    if (!delta_succession_valid(parent->second.generation, cursor.generation))
      return "delta " + describe(cursor) + " violates succession over parent gen=" + std::to_string(parent->second.generation);
    cursor = parent->second;
  }
  try {
    (void)read_history_entry(cursor);
  } catch (const std::exception& error) {
    return "anchor frame " + describe(cursor) + " unreadable: " + error.what();
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    try {
      (void)read_history_entry(*it);
    } catch (const std::exception& error) {
      return "delta frame " + describe(*it) + " unreadable: " + error.what();
    }
  }
  // The chain was not replayed: report only what was checked.
  return "delta chain from anchor " + describe(cursor) + " over " + std::to_string(chain.size()) +
         " frame(s) is structurally sound and every frame is readable; the failure is in materialisation itself and was not diagnosed here";
}

bool Replica::reanchor_history(const HistoryEntry& input) {
  const RevisionBump bump(_heads_revision);
  if (input.body != HistoryEntry::Body::full || input.generation == 0 || input.hash == Hash256{} || input.merge_parents.size() > merge_parents_limit)
    return false;
  Record record;
  record.generation = input.generation;
  record.previous = input.previous;
  record.hash = input.hash;
  record.payload = input.payload;
  if (!valid_record_counted(record)) return false;
  State state;
  try {
    state = decode_counted(record.payload);
  } catch (...) {
    return false;
  }
  if (_codec->head_fields(state).merge_parents != input.merge_parents) return false;

  auto entry = input;
  Lock durable(_durable_mutex);
  uint64_t file_offset = 0;
  {
    Lock lock(_mutex);
    if (const auto existing = _history.find(entry.hash); existing != _history.end()) {
      if (existing->second.generation != entry.generation || existing->second.previous != entry.previous) return false;  // same hash, different identity
      if (materialized_locked(entry.hash)) {
        // Already reconstructible: just lift the exclusion.
        _unreconstructable_retry_at.erase(entry.hash);
        return true;
      }
    }
    entry.previous_known = entry.generation > 1 && entry.previous != Hash256{} && _history.contains(entry.previous);
    file_offset = _history_bytes;
  }
  const auto frame = encode_history_frame(entry);
  write_history_frame(frame);

  Lock lock(_mutex);
  const bool superseded = _history.contains(entry.hash);
  _history[entry.hash] = index_history_entry(entry, file_offset, frame.size());
  ++_history_records;
  _history_bytes += frame.size();
  // Its bytes stay charged to the cache, as Macha's erase leaves them.
  _materialized.erase(entry.hash);
  const auto value = cache_materialization_locked(make_materialization(record, state));
  _unreconstructable_retry_at.erase(entry.hash);

  if (const auto head = _accepted_heads.find(entry.hash); head != _accepted_heads.end()) {
    // Verify against the record just cached, not a fresh reconstruction.
    if (!acceptance_matches_record_policy_locked(head->second, *value)) {
      // Drop only this certificate.
      _logger.error("metadata accepted head certificate does not match its repaired record; dropping the certificate hash=" + hex(entry.hash) +
                    " generation=" + std::to_string(head->second.generation));
      _accepted_heads.erase(head);
      persist_heads_locked();
    }
  }
  _logger.info(std::string("metadata history re-anchored ") + (superseded ? "superseding unreplayable frame" : "with new record") + " hash=" + hex(entry.hash) +
               " generation=" + std::to_string(entry.generation) + " bytes=" + std::to_string(frame.size()));
  if (prune_accepted_heads_locked()) persist_heads_locked();
  // Another flagged head may make the refresh throw; this repair still succeeded.
  std::optional<Record> pending_checkpoint;
  try {
    pending_checkpoint = refresh_materialized_head_deferred_locked();
  } catch (const MaintenanceStopping&) {
    throw;
  } catch (const std::exception& error) {
    _logger.debug("metadata materialized head refresh deferred after re-anchor: " + std::string(error.what()));
  }
  lock.unlock();
  if (pending_checkpoint) persist_checkpoint(*pending_checkpoint);
  return true;
}

bool Replica::import_history(const HistoryEntry& input) {
  if (input.generation == 0 || input.hash == Hash256{} || input.merge_parents.size() > merge_parents_limit) return false;
  auto entry = input;
  std::shared_ptr<const Materialization> parent;
  if (entry.body == HistoryEntry::Body::delta) {
    if (!entry.previous_known) return false;
    parent = materialized(entry.previous);
    if (!parent || !delta_succession_valid(parent->record.generation, entry.generation)) return false;
  } else if (entry.previous_known && entry.previous != Hash256{} && !history_contains(entry.previous)) {
    // A whole record stands alone; do not claim an absent predecessor.
    entry.previous_known = false;
  }

  std::shared_ptr<const Materialization> candidate;
  try {
    Record record;
    record.generation = entry.generation;
    record.previous = entry.previous;
    record.hash = entry.hash;
    State state;
    if (entry.body == HistoryEntry::Body::full) {
      record.payload = entry.payload;
      state = decode_counted(record.payload);
    } else {
      state = apply_delta(parent->state, entry.payload);
      record.payload = encode_after_counted(entry.payload, state);
    }
    if (!valid_record_counted(record) || _codec->head_fields(state).merge_parents != entry.merge_parents) return false;
    candidate = make_materialization(std::move(record), std::move(state));
  } catch (const MaintenanceStopping&) {
    throw;
  } catch (...) {
    return false;
  }
  const auto frame = encode_history_frame(entry);

  Lock durable(_durable_mutex);
  uint64_t file_offset = 0;
  {
    Lock lock(_mutex);
    if (_history.contains(entry.hash)) return true;
    if (entry.body == HistoryEntry::Body::delta && !_history.contains(entry.previous)) return false;
    file_offset = _history_bytes;
  }
  write_history_frame(frame);
  Lock lock(_mutex);
  _history.emplace(entry.hash, index_history_entry(entry, file_offset, frame.size()));
  ++_history_records;
  _history_bytes += frame.size();
  (void)cache_materialization_locked(candidate);
  std::optional<Record> pending_checkpoint;
  if (prune_accepted_heads_locked()) {
    persist_heads_locked();
    pending_checkpoint = refresh_materialized_head_deferred_locked();
  }
  lock.unlock();
  if (pending_checkpoint) persist_checkpoint(*pending_checkpoint);
  return true;
}

bool Replica::history_contains(const Hash256& hash) const {
  Lock lock(_mutex);
  return _history.contains(hash);
}

bool Replica::store_commit(const Record& record, std::span<const uint8_t> encoded_delta) {
  if (!valid_record_counted(record)) return false;
  HistoryEntry entry;
  entry.generation = record.generation;
  entry.previous = record.previous;
  entry.hash = record.hash;
  entry.previous_known = record.generation > 1 && record.previous != Hash256{} && history_contains(record.previous);
  try {
    entry.merge_parents = _codec->head_fields(decode_counted(record.payload)).merge_parents;
  } catch (...) {
    return false;
  }

  if (!encoded_delta.empty() && entry.previous_known && materialized(entry.previous)) {
    entry.body = HistoryEntry::Body::delta;
    entry.payload.assign(encoded_delta.begin(), encoded_delta.end());
  } else {
    entry.body = HistoryEntry::Body::full;
    entry.payload.assign(record.payload.begin(), record.payload.end());
  }

  std::shared_ptr<const Materialization> reconstructed;
  try {
    if (entry.body == HistoryEntry::Body::full) {
      reconstructed = make_materialization(record, decode_counted(record.payload));
    } else {
      const auto parent = materialized(entry.previous);
      if (!parent || !delta_succession_valid(parent->record.generation, record.generation)) {
        _logger.debug("metadata delta body rejected generation=" + std::to_string(record.generation) +
                      (parent ? " reason=succession" : " reason=parent-not-materialized"));
        return false;
      }
      auto state = apply_delta(parent->state, entry.payload);
      Record value;
      value.generation = entry.generation;
      value.previous = entry.previous;
      value.hash = entry.hash;
      value.payload = encode_after_counted(entry.payload, state);
      reconstructed = make_materialization(std::move(value), std::move(state));
    }
  } catch (const MaintenanceStopping&) {
    throw;
  } catch (...) {
    return false;
  }
  if (!reconstructed || reconstructed->record.generation != record.generation || reconstructed->record.previous != record.previous ||
      reconstructed->record.hash != record.hash || reconstructed->record.payload != record.payload) {
    // The caller falls back to a whole body; this log is the delta's only trace.
    if (entry.body == HistoryEntry::Body::delta)
      _logger.debug("metadata delta body rejected generation=" + std::to_string(record.generation) + " reason=" +
                    (!reconstructed                                    ? "no-reconstruction"
                     : reconstructed->record.payload != record.payload ? "payload-mismatch"
                                                                       : "identity-mismatch"));
    return false;
  }
  const auto frame = encode_history_frame(entry);

  Lock durable(_durable_mutex);
  uint64_t file_offset = 0;
  {
    Lock lock(_mutex);
    if (_history.contains(record.hash)) return true;
    if (entry.body == HistoryEntry::Body::delta && !_history.contains(entry.previous)) return false;
    file_offset = _history_bytes;
  }
  write_history_frame(frame);
  Lock lock(_mutex);
  _history.emplace(entry.hash, index_history_entry(entry, file_offset, frame.size()));
  ++_history_records;
  _history_bytes += frame.size();
  (void)cache_materialization_locked(reconstructed);
  return true;
}

bool Replica::accept_commit(const Acceptance& input, bool* heads_changed) {
  const RevisionBump bump(_heads_revision);
  if (heads_changed) *heads_changed = false;
  Acceptance value = input;
  std::sort(value.replicas.begin(), value.replicas.end());
  value.replicas.erase(std::unique(value.replicas.begin(), value.replicas.end()), value.replicas.end());
  if (value.generation == 0 || value.hash == Hash256{}) return false;
  if (std::any_of(value.replicas.begin(), value.replicas.end(), [](const NodeId& replica) { return replica == NodeId{}; })) return false;
  if (value.required != 0) {
    if (value.replicas.size() < value.required) return false;
  } else if (!value.replicas.empty()) {
    return false;
  }

  // A head already accepted under a certificate this one adds nothing to: accepting
  // it again changes nothing, so it costs a lookup, not a materialisation.
  {
    Lock lock(_mutex);
    if (const auto found = _accepted_heads.find(value.hash);
        found != _accepted_heads.end() && found->second.generation == value.generation && certificate_adds_nothing(found->second, value))
      return true;
  }

  // Warm the cache off the lock so the locked validation below hits it.
  const auto prepared = materialized(value.hash);
  if (!prepared || prepared->record.generation != value.generation) return false;

  Lock durable(_durable_mutex);
  // A head change rewrites the checkpoint: it is written after releasing _mutex,
  // under the durable mutex only, so readers do not stall behind the sync.
  std::optional<Record> pending_checkpoint;
  {
    Lock lock(_mutex);
    const auto held = materialized_locked(value.hash);
    if (!held || held->record.generation != value.generation) return false;
    if (!acceptance_matches_record_policy_locked(value, *held)) return false;

    bool changed = false;
    // An accepted ancestor is not a head: remove it if already present.
    bool incoming_is_ancestor = false;
    for (const auto& [head, _] : _accepted_heads) {
      if (head != value.hash && accepted_head_is_ancestor_locked(value.hash, head)) {
        incoming_is_ancestor = true;
        break;
      }
    }
    if (incoming_is_ancestor) {
      if (_accepted_heads.erase(value.hash) != 0) {
        if (heads_changed) *heads_changed = true;
        persist_heads_locked();
        if (refresh_materialized_head_in_memory_locked()) {
          reset_checkpoint_journal_locked();
          pending_checkpoint = _committed;
        }
      }
    } else {
      for (auto it = _accepted_heads.begin(); it != _accepted_heads.end();) {
        if (it->first != value.hash && accepted_head_is_ancestor_locked(it->first, value.hash)) {
          it = _accepted_heads.erase(it);
          changed = true;
        } else {
          ++it;
        }
      }

      const auto found = _accepted_heads.find(value.hash);
      if (found == _accepted_heads.end()) {
        _accepted_heads.emplace(value.hash, value);
        changed = true;
      } else {
        auto merged = found->second;
        if (merged.required == 0 && value.required != 0) {
          merged = value;
        } else if (merged.required != 0 && value.required != 0) {
          merged.required = std::min(merged.required, value.required);
          merged.replicas.insert(merged.replicas.end(), value.replicas.begin(), value.replicas.end());
          std::sort(merged.replicas.begin(), merged.replicas.end());
          merged.replicas.erase(std::unique(merged.replicas.begin(), merged.replicas.end()), merged.replicas.end());
        }
        if (merged != found->second) {
          found->second = std::move(merged);
          changed = true;
        }
      }

      changed = prune_accepted_heads_locked() || changed;
      if (heads_changed) *heads_changed = changed;
      if (changed) {
        persist_heads_locked();
        if (refresh_materialized_head_in_memory_locked()) {
          reset_checkpoint_journal_locked();
          pending_checkpoint = _committed;
        }
      }
    }
  }
  if (pending_checkpoint) persist_checkpoint(*pending_checkpoint);
  return true;
}

std::vector<Acceptance> Replica::accepted_head_certificates() const {
  Lock lock(_mutex);
  std::vector<Acceptance> out;
  out.reserve(_accepted_heads.size());
  for (const auto& [_, value] : _accepted_heads) out.push_back(value);
  return out;
}

std::vector<Record> Replica::accepted_heads() const {
  std::vector<Hash256> hashes;
  {
    Lock lock(_mutex);
    hashes.reserve(_accepted_heads.size());
    for (const auto& [hash, _] : _accepted_heads) hashes.push_back(hash);
  }
  // Hot path: a broken head is excluded for its cooldown rather than thrown.
  // Callers read the set's size as not-ready, converged or needs-reconciliation.
  const auto now = _time.now();
  std::vector<Record> out;
  out.reserve(hashes.size());
  for (const auto& hash : hashes) {
    {
      Lock lock(_mutex);
      const auto found = _unreconstructable_retry_at.find(hash);
      if (found != _unreconstructable_retry_at.end() && now < found->second) continue;
    }
    const auto value = materialized(hash);
    Lock lock(_mutex);
    if (!value) {
      (void)flag_unreconstructable_locked(hash, now, "accepted head enumeration");
      continue;
    }
    _unreconstructable_retry_at.erase(hash);
    out.push_back(value->record);
  }
  return out;
}

std::optional<Acceptance> Replica::acceptance(const Hash256& hash) const {
  Lock lock(_mutex);
  const auto found = _accepted_heads.find(hash);
  if (found == _accepted_heads.end()) return std::nullopt;
  return found->second;
}

bool Replica::history_is_ancestor(const Hash256& ancestor, const Hash256& descendant) const {
  Lock lock(_mutex);
  return history_is_ancestor_locked(ancestor, descendant, true);
}

std::optional<Hash256> Replica::history_common_ancestor(const Hash256& left, const Hash256& right) const {
  Lock lock(_mutex);
  return history_common_ancestor_locked(left, right);
}

std::optional<Record> Replica::historical(const Hash256& hash) const {
  const auto value = materialized(hash);
  if (!value) return std::nullopt;
  return value->record;
}

std::shared_ptr<const Materialization> Replica::materialized(const Hash256& hash) const {
  _historical_requests.fetch_add(1, std::memory_order_relaxed);
  Lock computation(_compute_mutex);

  std::vector<HistoryIndexEntry> delta_indexes;
  HistoryIndexEntry anchor_index;
  std::shared_ptr<const Materialization> base;
  {
    Lock lock(_mutex);
    const auto found = _history.find(hash);
    if (found == _history.end()) return {};
    if (const auto cached = _materialized.find(hash); cached != _materialized.end()) {
      cached->second.last_used = ++_materialized_clock;
      _cache_hits.fetch_add(1, std::memory_order_relaxed);
      return cached->second.value;
    }
    _cache_misses.fetch_add(1, std::memory_order_relaxed);
    _historical_reconstructions.fetch_add(1, std::memory_order_relaxed);

    std::set<Hash256> seen;
    auto cursor = found;
    while (cursor->second.body == HistoryEntry::Body::delta) {
      if (!seen.insert(cursor->first).second || !cursor->second.previous_known) return {};
      delta_indexes.push_back(cursor->second);
      cursor = _history.find(cursor->second.previous);
      if (cursor == _history.end()) return {};
      if (const auto cached = _materialized.find(cursor->first); cached != _materialized.end()) {
        cached->second.last_used = ++_materialized_clock;
        base = cached->second.value;
        break;
      }
    }
    if (!base) anchor_index = cursor->second;
  }

  // Decode, apply, encode and hash run without _mutex, holding one state at a time.
  Record working_record;
  State working_state;
  try {
    if (base) {
      working_record = base->record;
      working_state = base->state;
      base.reset();
    } else {
      auto anchor = read_history_entry(anchor_index);
      working_record.generation = anchor.generation;
      working_record.previous = anchor.previous;
      working_record.hash = anchor.hash;
      working_record.payload = std::move(anchor.payload);
      if (!valid_record_counted(working_record)) return {};
      working_state = decode_counted(working_record.payload);
      if (_codec->head_fields(working_state).merge_parents != anchor.merge_parents) return {};
    }

    for (auto it = delta_indexes.rbegin(); it != delta_indexes.rend(); ++it) {
      pause_walk();
      const auto child = read_history_entry(*it);
      if (child.previous != working_record.hash || !delta_succession_valid(working_record.generation, child.generation)) return {};
      working_state = apply_delta(working_state, child.payload);
      _historical_deltas_applied.fetch_add(1, std::memory_order_relaxed);
      Record record;
      record.generation = child.generation;
      record.previous = child.previous;
      record.hash = child.hash;
      record.payload = encode_after_counted(child.payload, working_state);
      if (!valid_record_counted(record) || _codec->head_fields(working_state).merge_parents != child.merge_parents) return {};
      working_record = std::move(record);
    }
  } catch (const MaintenanceStopping&) {
    throw;
  } catch (...) {
    return {};
  }

  auto value = make_materialization(std::move(working_record), std::move(working_state));

  Lock lock(_mutex);
  // Cache only the requested result, and only if compaction has not removed it.
  if (_history.contains(value->record.hash)) value = cache_materialization_locked(value);
  if (const auto cached = _materialized.find(hash); cached != _materialized.end()) return cached->second.value;
  return value && value->record.hash == hash ? value : std::shared_ptr<const Materialization>{};
}

void Replica::reset_checkpoint_journal_locked() {
  replace_file(_journal_path, {});
  _journal_records = 0;
  _journal_bytes = 0;
}

void Replica::reset_checkpoint(const Record& record) {
  persist_checkpoint(record);
  reset_checkpoint_journal_locked();
}

void Replica::compact() {
  constexpr size_t record_threshold = 128;
  constexpr uint64_t byte_threshold = 8ULL * 1024 * 1024;
  // With the durable mutex held the journal and the committed head stay as read
  // here while the files are written without _mutex: no reader waits on the sync.
  Lock durable(_durable_mutex);
  Record checkpoint;
  size_t records = 0;
  uint64_t bytes = 0;
  {
    Lock lock(_mutex);
    if (_current.generation != _committed.generation || _current.hash != _committed.hash) return;
    if (_journal_records < record_threshold && _journal_bytes < byte_threshold) return;
    checkpoint = _committed;
    records = _journal_records;
    bytes = _journal_bytes;
  }
  persist_checkpoint(checkpoint);
  replace_file(_journal_path, {});
  {
    Lock lock(_mutex);
    _journal_records = 0;
    _journal_bytes = 0;
  }
  _logger.debug("metadata journal compacted generation=" + std::to_string(checkpoint.generation) + " records=" + std::to_string(records) +
                " journal_bytes=" + std::to_string(bytes) + " snapshot_bytes=" + std::to_string(checkpoint.payload.size()));
}

bool Replica::compact_history_if_safe(size_t record_threshold, uint64_t byte_threshold) {
  Lock durable(_durable_mutex);
  Lock lock(_mutex);
  if ((_history_records < record_threshold && _history_bytes < byte_threshold) || _current.generation != _committed.generation ||
      _current.hash != _committed.hash || _pending_history || _accepted_heads.size() != 1 || !_accepted_heads.contains(_committed.hash))
    return false;

  // The sole accepted head becomes a whole entry with no known previous: the root
  // of this node's history.
  HistoryEntry root;
  root.generation = _committed.generation;
  root.previous = _committed.previous;
  root.hash = _committed.hash;
  root.previous_known = false;
  root.merge_parents = _codec->head_fields(decode_counted(_committed.payload)).merge_parents;
  root.body = HistoryEntry::Body::full;
  root.payload.assign(_committed.payload.begin(), _committed.payload.end());

  const auto frame = encode_history_frame(root);
  replace_file(_history_path, frame);

  _history.clear();
  _history.emplace(root.hash, index_history_entry(root, 0, frame.size()));
  _materialized.clear();
  _materialized_clock = 0;
  _materialized_bytes = 0;
  (void)cache_materialization_locked(_committed);
  _history_records = 1;
  _history_bytes = frame.size();
  return true;
}

void Replica::persist_checkpoint(const Record& record) {
  replace_file(_checkpoint_path, seal_file(*_content_key, _magics.checkpoint, _checkpoint_aad, encode_record(record)));
}

std::optional<Record> Replica::load_checkpoint() const {
  if (!std::filesystem::exists(_checkpoint_path)) return std::nullopt;
  try {
    return decode_record(open_file(*_content_key, _magics.checkpoint, _checkpoint_aad, read_whole(_checkpoint_path)));
  } catch (const std::exception& error) {
    throw std::runtime_error("metadata file " + _checkpoint_path.string() + ": " + error.what());
  }
}

}  // namespace dagda::heads
