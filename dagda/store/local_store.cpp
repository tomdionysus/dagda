//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/local_store.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

#include "dagda/store/local_format.h"
#include "dagda/store/sealing.h"

namespace dagda::store {
namespace {

using dannan::contract::Lock;
using dannan::contract::Mutex;
using dannan::contract::Unlocked;
using namespace local_format;

void write_all_at(int fd, std::span<const uint8_t> data, uint64_t offset) {
  size_t done = 0;
  while (done < data.size()) {
    const auto n = ::pwrite(fd, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("cannot write storage state: " + std::string(std::strerror(errno)));
    }
    done += static_cast<size_t>(n);
  }
}

std::optional<Bytes> read_all_at(int fd, size_t size, uint64_t offset) {
  Bytes out(size);
  size_t done = 0;
  while (done < out.size()) {
    const auto n = ::pread(fd, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (n == 0) return std::nullopt;
    done += static_cast<size_t>(n);
  }
  return out;
}

void write_all(int fd, std::span<const uint8_t> bytes) {
  size_t done = 0;
  while (done < bytes.size()) {
    const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::strerror(errno));
    }
    done += static_cast<size_t>(n);
  }
}

std::filesystem::file_time_type file_time_from_unix_ms(uint64_t value) {
  using namespace std::chrono;
  const auto target = system_clock::time_point(milliseconds(value));
  return std::filesystem::file_time_type::clock::now() + duration_cast<std::filesystem::file_time_type::duration>(target - system_clock::now());
}

int64_t unix_ms_of(std::filesystem::file_time_type time) {
  using namespace std::chrono;
  const auto since = duration_cast<system_clock::duration>(time - std::filesystem::file_time_type::clock::now());
  return duration_cast<milliseconds>((system_clock::now() + since).time_since_epoch()).count();
}

}  // namespace

LocalStore::LocalStore(std::filesystem::path root, LocalStoreOptions options, std::shared_ptr<const domain::StorageKey> key, Services services,
                       LocalStoreMode mode, std::shared_ptr<DurabilityDomain> durability_domain, LocalStoreFiles& files)
    : _root(std::move(root)),
      _objects(_root / "objects"),
      _packs(_root / "packs"),
      _accounting_path(_root / ".macha.accounting"),
      _limit(options.limit),
      _reserve_free(options.reserve_free),
      _pack_threshold(options.pack_threshold),
      _pack_target_size(options.pack_target_size),
      _key(std::move(key)),
      _sealing(options.sealing),
      _role(options.role),
      _mode(mode),
      _threads(services.threads),
      _progress(services.progress),
      _logger(std::move(services.logger)),
      _wall(services.wall),
      _files(files),
      _durability_domain(std::move(durability_domain)) {
  if (_role == domain::Role::member && !_key) throw std::invalid_argument("local store needs a storage key");
  if (!options.ledger_dir.empty() && !options.ledger_key)
    throw std::invalid_argument(
        "a held ledger is sealed under the node-local key (LocalStoreOptions::ledger_key): a node without custody has none and cannot keep one");
  if (!_limit) throw std::invalid_argument("local store limit must be non-zero");
  if ((_pack_threshold == 0) != (_pack_target_size == 0)) throw std::invalid_argument("pack threshold and target size must both be zero or non-zero");
  if (_pack_threshold && _pack_target_size < _pack_threshold) throw std::invalid_argument("pack target size must be >= pack threshold");

  std::filesystem::create_directories(_objects);
  std::filesystem::create_directories(_packs);
  rebuild_pack_index(true);

  if (_mode == LocalStoreMode::ephemeral) {
    _durability_domain.reset();
    start_scan();
    return;
  }

  if (!_durability_domain)
    _durability_domain = std::make_shared<DurabilityDomain>(1, _root, DurabilityDomain::Services{services.threads, services.observations, services.time});
  _accounting_fd = ::open(_accounting_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (_accounting_fd < 0) throw std::runtime_error("cannot open accounting state: " + std::string(std::strerror(errno)));

  // The ledger exists before any walk starts, so the walk records into it.
  if (!options.ledger_dir.empty()) {
    ledger::ObjectTrie::Options ledger_options;
    ledger_options.cache_bytes = options.ledger_cache_bytes;
    _ledger = std::make_unique<ledger::HeldLedger>(options.ledger_dir, options.ledger_key, ledger_options, _logger.child("ledger"));
  }

  // Pack indexes are rebuilt from the append-only records at every start. A
  // clean checkpoint is trusted with packs too: ensure_accounting_dirty()
  // persists "dirty" before a session's first mutation, so a torn pack tail
  // only exists behind a dirty checkpoint. A dirty checkpoint is carried as
  // an estimate while the walk reconciles; puts are admitted against it
  // (reserve_free covers the bounded error) and the walk's total replaces it.
  bool clean = false;
  const bool restored = restore_accounting(&clean);
  if (restored && clean) {
    _accounting_trusted.store(true, std::memory_order_release);
    _scan_complete.store(true, std::memory_order_release);
    _logger.debug("storage accounting restored path=" + _root.string() + " used=" + std::to_string(_used.load(std::memory_order_relaxed)));
  } else {
    if (restored) {
      _accounting_estimate.store(true, std::memory_order_release);
      _logger.info("storage accounting estimate path=" + _root.string() + " used=" + std::to_string(_used.load(std::memory_order_relaxed)) +
                   " (dirty checkpoint; admitting writes while the scan reconciles)");
    }
    start_scan();
  }
  if (_ledger && _ledger->seeded()) {
    // The ledger knows what is held: nothing to walk. The packs were replayed
    // above, so it is brought to list every packed object, including one a
    // crash left between its record and its journal.
    {
      Lock lock(_mutex);
      _presence.warmed();
      for (const auto& [id, entry] : _packed)
        if (!_ledger->held(id).value_or(true)) _ledger->record(id, true);
    }
    flush_ledger();
    _presence_index_entries.store(_ledger->size(), std::memory_order_relaxed);
    return;
  }
  _presence_thread = std::jthread([this](std::stop_token stop) { _threads.once("local-store-presence", [this, stop] { warm_presence_index(stop); }); });
}

void LocalStore::mark_scan_failed() {
  {
    // As the scan's completion: under the mutex the waiters check it under.
    Lock lock(_mutex);
    _scan_failed.store(true, std::memory_order_release);
  }
  _accounting_cv.notify_all();
}

void LocalStore::start_scan() {
  _scan_thread = std::jthread([this](std::stop_token stop) {
    _threads.once("local-store-scan", [this, stop] {
      try {
        scan(stop);
      } catch (const std::exception& error) {
        mark_scan_failed();
        _logger.warn("storage accounting scan failed path=" + _root.string() + " error=" + error.what());
        throw;
      } catch (...) {
        mark_scan_failed();
        _logger.warn("storage accounting scan failed path=" + _root.string() + " error=<unknown>");
        throw;
      }
    });
  });
}

LocalStore::~LocalStore() {
  if (_presence_thread.joinable()) {
    _presence_thread.request_stop();
    _presence_thread.join();
  }
  if (_scan_thread.joinable()) {
    _scan_thread.request_stop();
    _scan_thread.join();
  }
  if (_accounting_fd >= 0) {
    bool can_checkpoint = _accounting_trusted.load(std::memory_order_acquire);
    if (can_checkpoint && _durability_domain && _last_mutation_generation) {
      try {
        _durability_domain->await_durable(_last_mutation_generation, DurabilityUrgency::immediate);
      } catch (...) {
        can_checkpoint = false;
      }
    }
    // Every other thread has stopped: no lock is needed.
    if (can_checkpoint && _mode != LocalStoreMode::ephemeral && _accounting_dirty) {
      try {
        persist_accounting(_used.load(std::memory_order_relaxed), accounting_clean, ObjectId{}, 0, true);
      } catch (...) {
      }
    }
    ::close(_accounting_fd);
    _accounting_fd = -1;
  }
}

contract::HeldView LocalStore::held_view() const {
  if (!_ledger || !_ledger->seeded()) return {{{}, held_removals(), false}, [](const ObjectId&) { return false; }, {}};
  auto view = _ledger->view();
  contract::HeldView out;
  out.identity = {view.records.root_hash(), view.removals, true};
  out.trie = std::make_shared<const ledger::TrieUnion>(std::vector{view.records});
  out.held = [records = std::move(view.records)](const ObjectId& id) { return records.get(id).has_value(); };
  return out;
}

void LocalStore::flush_ledger() const {
  if (_ledger) _ledger->flush();
}

void LocalStore::seed_ledger() {
  const auto started = dannan::contract::Clock::now();
  _ledger->seed([this] {
    Lock lock(_mutex);
    auto present = _presence.present();
    for (const auto& [id, entry] : _packed) present.push_back(id);
    std::sort(present.begin(), present.end());
    present.erase(std::unique(present.begin(), present.end()), present.end());
    // Every change queued so far is in the snapshot.
    _ledger->drop_queued();
    return present;
  });
  {
    // The ledger answers from now on; the walked index is not kept.
    Lock lock(_mutex);
    _presence = PresenceIndex{};
    _presence.warmed();
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(dannan::contract::Clock::now() - started).count();
  _logger.info("storage held ledger seeded path=" + _root.string() + " objects=" + std::to_string(_ledger->size()) + " elapsed_ms=" + std::to_string(elapsed));
}

std::filesystem::path LocalStore::path(const ObjectId& id) const { return loose_path(_objects, id); }

void LocalStore::persist_accounting(uint64_t used, uint8_t operation, const ObjectId& id, uint64_t size, bool durable) {
  const AccountingRecord record{++_accounting_sequence, used, operation, id, size};
  _accounting_slot ^= 1U;
  const auto encoded = encode_accounting(record);
  write_all_at(_accounting_fd, encoded, _accounting_slot * accounting_slot_size);
  if (durable) {
    if (::fsync(_accounting_fd) != 0) throw std::runtime_error("cannot sync accounting state: " + std::string(std::strerror(errno)));
    _own_syncs.fetch_add(1, std::memory_order_relaxed);
  }
}

void LocalStore::sync_directory(const std::filesystem::path& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return;
  int rc;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  ::close(fd);
  _own_syncs.fetch_add(1, std::memory_order_relaxed);
}

bool LocalStore::restore_accounting(bool* clean) {
  std::optional<AccountingRecord> best;
  unsigned best_slot = 0;
  for (unsigned slot = 0; slot < 2; ++slot) {
    const auto bytes = read_all_at(_accounting_fd, accounting_slot_size, slot * accounting_slot_size);
    if (!bytes) continue;
    const auto decoded = decode_accounting(*bytes);
    if (!decoded) continue;
    if (!best || decoded->sequence > best->sequence) {
      best = *decoded;
      best_slot = slot;
    }
  }
  if (!best) return false;
  _accounting_sequence = best->sequence;
  _accounting_slot = best_slot;
  _used.store(best->used, std::memory_order_relaxed);
  if (clean) *clean = best->operation != accounting_dirty;
  return true;
}

void LocalStore::ensure_accounting_dirty(Lock& lock) {
  if (_mode == LocalStoreMode::ephemeral || _accounting_dirty) return;
  _accounting_state_cv.wait(lock.native(), [this]() DANNAN_REQUIRES(_mutex) { return !_accounting_dirty_in_progress; });
  if (_accounting_dirty) return;
  _accounting_dirty_in_progress = true;
  const auto used = _used.load(std::memory_order_relaxed);
  try {
    Unlocked unlocked(lock, _mutex);
    persist_accounting(used, accounting_dirty, ObjectId{}, 0, true);
  } catch (...) {
    _accounting_dirty_in_progress = false;
    _accounting_state_cv.notify_all();
    throw;
  }
  _accounting_dirty = true;
  _accounting_dirty_in_progress = false;
  _accounting_state_cv.notify_all();
}

void LocalStore::reap_durable_generations_locked() {
  if (!_durability_domain) {
    _provisional_generations.clear();
    _provisional_order.clear();
    return;
  }
  const auto durable = _durability_domain->durable_generation();
  while (!_provisional_order.empty() && _provisional_order.front().first <= durable) {
    const auto [generation, id] = _provisional_order.front();
    _provisional_order.pop_front();
    const auto found = _provisional_generations.find(id);
    if (found != _provisional_generations.end() && found->second == generation) _provisional_generations.erase(found);
  }
}

bool LocalStore::physical_space_available_locked(uint64_t need) const {
  const auto before = _used.load(std::memory_order_relaxed);
  if (need > _limit || _reserved_write_bytes > _limit - need || before > _limit - need - _reserved_write_bytes) return false;
  return true;
}

bool LocalStore::filesystem_space_available_for_reservations() const {
  uint64_t reserved = 0;
  {
    Lock lock(_mutex);
    reserved = _reserved_write_bytes;
  }
  std::error_code error;
  const auto space = std::filesystem::space(_root, error);
  if (error) return false;
  if (reserved > space.available) return false;
  return _reserve_free <= space.available - reserved;
}

std::optional<LocalStore::LooseStamp> LocalStore::loose_stamp(const std::filesystem::path& path) {
  struct stat value{};
  if (::stat(path.c_str(), &value) != 0 || value.st_size < 0) return std::nullopt;
#if defined(__APPLE__)
  const auto modified = static_cast<int64_t>(value.st_mtimespec.tv_sec) * 1000000000LL + value.st_mtimespec.tv_nsec;
  const auto changed = static_cast<int64_t>(value.st_ctimespec.tv_sec) * 1000000000LL + value.st_ctimespec.tv_nsec;
#else
  const auto modified = static_cast<int64_t>(value.st_mtim.tv_sec) * 1000000000LL + value.st_mtim.tv_nsec;
  const auto changed = static_cast<int64_t>(value.st_ctim.tv_sec) * 1000000000LL + value.st_ctim.tv_nsec;
#endif
  return LooseStamp{static_cast<uint64_t>(value.st_dev), static_cast<uint64_t>(value.st_ino), static_cast<uint64_t>(value.st_size), modified, changed};
}

void LocalStore::remember_verified_loose_locked(const ObjectId& id, const LooseStamp& stamp) const {
  const auto sequence = ++_verified_loose_sequence;
  _verified_loose[id] = {stamp, sequence};
  _verified_loose_order.push_back({sequence, id});
  while (_verified_loose.size() > verified_loose_limit && !_verified_loose_order.empty()) {
    const auto [candidate_sequence, candidate] = _verified_loose_order.front();
    _verified_loose_order.pop_front();
    const auto found = _verified_loose.find(candidate);
    if (found != _verified_loose.end() && found->second.sequence == candidate_sequence) _verified_loose.erase(found);
  }
  if (_verified_loose_order.size() > verified_loose_limit * 4) {
    _verified_loose_order.clear();
    for (const auto& [candidate, verified] : _verified_loose) _verified_loose_order.push_back({verified.sequence, candidate});
    std::sort(_verified_loose_order.begin(), _verified_loose_order.end());
  }
}

void LocalStore::forget_verified_loose_locked(const ObjectId& id) const {
  _verified_loose.erase(id);
  _presence.forgotten(id);
}

bool LocalStore::prune_empty_loose(const ObjectId& id, const std::filesystem::path& file) const {
  std::error_code error;
  if (std::filesystem::file_size(file, error) != 0 || error) return false;
  // Zero bytes hold nothing to account for: usage is unchanged.
  if (!std::filesystem::remove(file, error) || error) return false;
  {
    Lock lock(_mutex);
    _verified_loose.erase(id);
    _presence.pruned(id);
    if (_ledger) _ledger->record(id, false);
  }
  _losses.fetch_add(1, std::memory_order_release);
  flush_ledger();
  _logger.warn("storage pruned an empty object file id=" + to_string(id) + " path=" + file.string());
  return true;
}

void LocalStore::select_active_pack_locked(uint64_t next_record_size) {
  if (!_pack_threshold) throw std::logic_error("packing is disabled");
  if (_active_pack.empty() || (_active_pack_size && _active_pack_size + next_record_size > _pack_target_size)) {
    _active_pack = _packs / pack_name(_next_pack_sequence++);
    _active_pack_size = 0;
  }
}

bool LocalStore::append_pack_record_locked(uint8_t type, const ObjectId& id, const SealedObject* sealed, uint64_t touched_ms, PackEntry* entry,
                                           uint64_t* record_size, Lock& lock) {
  PackHeader header;
  header.type = type;
  header.touched_ms = touched_ms;
  header.id = id;
  Bytes encoded;
  uint64_t total = 0;
  std::filesystem::path target;
  uint64_t start = 0;
  {
    Unlocked unlocked(lock, _mutex);
    std::span<const uint8_t> payload;
    try {
      if (type == pack_put) {
        if (!sealed) throw std::logic_error("a pack put record needs its sealed object");
        payload = sealed->ciphertext;
        header.plain_size = sealed->plain_size;
        header.payload_size = payload.size();
        header.nonce = sealed->nonce;
        header.tag = sealed->tag;
      }
      encoded = encode_pack_header(header);
      total = encoded.size() + payload.size();
    } catch (...) {
      // A failure before the append abandons the active pack, as a failed
      // rollback does.
      Lock pack_io_lock(_pack_io_mutex);
      _active_pack.clear();
      _active_pack_size = 0;
      throw;
    }

    // The pack stream has its own single-writer lock; _mutex stays free
    // during encryption and I/O so unrelated reads are not trapped behind an
    // append.
    Lock pack_io_lock(_pack_io_mutex);
    try {
      select_active_pack_locked(total);
      target = _active_pack;
      start = _active_pack_size;
      _files.append(target, encoded, payload);
      _active_pack_size += total;
    } catch (...) {
      // A failed append (ENOSPC, EIO, a short write) must not leave the live
      // process appending behind a torn record. Restore the previous boundary
      // when possible; if the rollback itself fails, abandon this pack so later
      // writes never append behind the torn bytes. Restart recovery truncates
      // it.
      if (!target.empty() && _files.truncate(target, start)) {
        _active_pack_size = start;
      } else {
        _active_pack.clear();
        _active_pack_size = 0;
      }
      throw;
    }
  }
  if (entry) {
    entry->file = target;
    entry->payload_offset = start + encoded.size();
    entry->payload_size = header.payload_size;
    entry->plain_size = header.plain_size;
    entry->record_size = total;
    entry->touched_unix_ms = touched_ms;
    entry->nonce = header.nonce;
    entry->tag = header.tag;
  }
  if (record_size) *record_size = total;
  return true;
}

std::optional<Bytes> LocalStore::get_packed_locked(const ObjectId& id, Lock& lock) const {
  const auto found = _packed.find(id);
  if (found == _packed.end()) return std::nullopt;
  const auto entry = found->second;
  ++_active_pack_readers[entry.file];

  // A reader lease pins the indexed pack against compaction before any
  // filesystem call, keeping even open(2) outside _mutex; compaction switches
  // the index and waits for old readers before unlinking the victim.
  auto release_reader = [&]() DANNAN_REQUIRES(_mutex) {
    const auto active = _active_pack_readers.find(entry.file);
    if (active == _active_pack_readers.end() || active->second == 0) throw std::logic_error("local store pack reader underflow");
    if (--active->second == 0) _active_pack_readers.erase(active);
    _pack_readers_cv.notify_all();
  };
  Bytes plain;
  try {
    Unlocked unlocked(lock, _mutex);
    const auto payload = _files.read_at(entry.file, entry.payload_offset, static_cast<size_t>(entry.payload_size));
    if (!payload) throw std::runtime_error("short packed object read");
    plain = open_object(*_key, _sealing, id, entry.nonce, entry.tag, *payload, entry.plain_size);
  } catch (...) {
    release_reader();
    throw;
  }
  release_reader();
  return plain;
}

std::optional<LocalStore::SealedObject> LocalStore::get_sealed_locked(const ObjectId& id, Lock& lock) const {
  if (const auto found = _packed.find(id); found != _packed.end()) {
    const auto entry = found->second;
    ++_active_pack_readers[entry.file];
    auto release_reader = [&]() DANNAN_REQUIRES(_mutex) {
      const auto active = _active_pack_readers.find(entry.file);
      if (active == _active_pack_readers.end() || active->second == 0) throw std::logic_error("local store pack reader underflow");
      if (--active->second == 0) _active_pack_readers.erase(active);
      _pack_readers_cv.notify_all();
    };
    SealedObject out;
    try {
      Unlocked unlocked(lock, _mutex);
      auto payload = _files.read_at(entry.file, entry.payload_offset, static_cast<size_t>(entry.payload_size));
      if (!payload) throw std::runtime_error("short packed object read");
      out.plain_size = entry.plain_size;
      out.nonce = entry.nonce;
      out.tag = entry.tag;
      out.ciphertext = std::move(*payload);
    } catch (...) {
      release_reader();
      throw;
    }
    release_reader();
    return out;
  }
  const auto file = path(id);
  Unlocked unlocked(lock, _mutex);
  if (!std::filesystem::exists(file)) return std::nullopt;
  const auto encoded = _files.read(file);
  if (encoded.empty()) return std::nullopt;
  const auto loose = decode_loose(encoded);
  SealedObject out;
  out.plain_size = loose.plain_size;
  out.nonce = loose.nonce;
  out.tag = loose.tag;
  out.ciphertext.assign(loose.ciphertext.begin(), loose.ciphertext.end());
  return out;
}

Bytes LocalStore::open_loose(const ObjectId& id, std::span<const uint8_t> file) const {
  const auto loose = decode_loose(file);
  return open_object(*_key, _sealing, id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size);
}

LocalStore::SealedObject LocalStore::seal(const ObjectId& id, std::span<const uint8_t> data) const {
  if (_role != domain::Role::member) throw std::logic_error("a blind store holds ciphertext only and cannot seal");
  auto sealed = seal_object(*_key, _sealing, id, data);
  SealedObject out;
  out.plain_size = data.size();
  out.nonce = sealed.nonce;
  out.tag = sealed.tag;
  out.ciphertext = std::move(sealed.ciphertext);
  return out;
}

Bytes LocalStore::open(const ObjectId& id, const SealedObject& sealed) const {
  if (_role != domain::Role::member) throw std::logic_error("a blind store holds ciphertext only and cannot open");
  return open_object(*_key, _sealing, id, sealed.nonce, sealed.tag, sealed.ciphertext, sealed.plain_size);
}

void LocalStore::rebuild_pack_index(bool truncate_incomplete_tail) {
  _packed.clear();
  _pack_dead_bytes = 0;
  _next_pack_sequence = 1;
  _active_pack.clear();
  _active_pack_size = 0;

  std::vector<std::pair<uint64_t, std::filesystem::path>> files;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(_packs, error)) {
    if (error) break;
    if (!entry.is_regular_file()) continue;
    const auto name = entry.path().filename().string();
    if (name.find(".tmp") != std::string::npos || name.starts_with(".compact-")) {
      std::error_code remove_error;
      std::filesystem::remove(entry.path(), remove_error);
      continue;
    }
    if (const auto sequence = pack_sequence(name)) files.push_back({*sequence, entry.path()});
  }
  if (error) throw std::runtime_error("cannot enumerate packs: " + error.message());
  std::sort(files.begin(), files.end());

  for (const auto& [sequence, file] : files) {
    _next_pack_sequence = std::max(_next_pack_sequence, sequence + 1);
    const int fd = ::open(file.c_str(), (truncate_incomplete_tail ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open pack during recovery: " + std::string(std::strerror(errno)));
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
      const auto saved = errno;
      ::close(fd);
      throw std::runtime_error("cannot stat pack during recovery: " + std::string(std::strerror(saved)));
    }
    const uint64_t file_size = static_cast<uint64_t>(st.st_size);
    uint64_t offset = 0;
    while (offset < file_size) {
      if (file_size - offset < pack_header_size) {
        if (truncate_incomplete_tail && ::ftruncate(fd, static_cast<off_t>(offset)) == 0) _logger.warn("truncated incomplete pack tail path=" + file.string());
        break;
      }
      const auto bytes = read_all_at(fd, pack_header_size, offset);
      if (!bytes) {
        ::close(fd);
        throw std::runtime_error("cannot read pack header during recovery");
      }
      const auto header = decode_pack_header(*bytes);
      if (!header) {
        // An undecodable header with nothing decodable after it is a torn
        // tail (power lost before the barrier; nothing acknowledged) and is
        // discarded. If a decodable record follows, the damage is inside the
        // pack: the span is skipped as dead bytes and its objects repaired
        // from replicas. Neither takes the store offline.
        const bool zero_header = std::all_of(bytes->begin(), bytes->end(), [](uint8_t b) { return b == 0; });
        const auto next = find_next_pack_header(fd, offset + 1, file_size);
        if (!next) {
          const auto discarded = file_size - offset;
          if (truncate_incomplete_tail) {
            if (::ftruncate(fd, static_cast<off_t>(offset)) != 0) {
              const auto saved = errno;
              ::close(fd);
              throw std::runtime_error("cannot truncate torn pack tail at " + file.string() + " offset=" + std::to_string(offset) + ": " +
                                       std::strerror(saved));
            }
            _logger.warn("truncated undecodable pack tail path=" + file.string() + " offset=" + std::to_string(offset) + " bytes=" + std::to_string(discarded) +
                         " zero_header=" + std::to_string(zero_header ? 1 : 0));
          }
          _pack_recovery_truncated_tails.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        const auto skipped = *next - offset;
        _logger.error("skipped unreadable pack region path=" + file.string() + " offset=" + std::to_string(offset) + " bytes=" + std::to_string(skipped) +
                      " zero_header=" + std::to_string(zero_header ? 1 : 0) + " (objects recorded there are absent from this backend)");
        _pack_dead_bytes += skipped;
        _pack_recovery_skipped_regions.fetch_add(1, std::memory_order_relaxed);
        _pack_recovery_skipped_bytes.fetch_add(skipped, std::memory_order_relaxed);
        offset = *next;
        continue;
      }
      const uint64_t total = pack_header_size + header->payload_size;
      if (total > file_size - offset) {
        if (truncate_incomplete_tail && ::ftruncate(fd, static_cast<off_t>(offset)) == 0)
          _logger.warn("truncated incomplete packed object path=" + file.string());
        break;
      }
      if (header->type == pack_put) {
        if (const auto old = _packed.find(header->id); old != _packed.end()) _pack_dead_bytes += old->second.record_size;
        PackEntry entry;
        entry.file = file;
        entry.payload_offset = offset + pack_header_size;
        entry.payload_size = header->payload_size;
        entry.plain_size = header->plain_size;
        entry.record_size = total;
        entry.touched_unix_ms = header->touched_ms;
        entry.nonce = header->nonce;
        entry.tag = header->tag;
        _packed[header->id] = entry;
      } else if (header->type == pack_remove) {
        if (const auto old = _packed.find(header->id); old != _packed.end()) {
          _pack_dead_bytes += old->second.record_size;
          _packed.erase(old);
        }
        _pack_dead_bytes += total;
      } else {
        if (const auto old = _packed.find(header->id); old != _packed.end()) old->second.touched_unix_ms = header->touched_ms;
        _pack_dead_bytes += total;
      }
      offset += total;
    }
    ::close(fd);
    std::error_code size_error;
    const auto actual = std::filesystem::file_size(file, size_error);
    if (!size_error) {
      _active_pack = file;
      _active_pack_size = actual;
    }
  }
  if (_active_pack_size >= _pack_target_size) {
    _active_pack.clear();
    _active_pack_size = 0;
  }
}

bool LocalStore::put_loose_locked(const ObjectId& id, const SealedObject& sealed, uint64_t& generation, Lock& lock) {
  // Reserve capacity and mark accounting dirty under _mutex, then install
  // under the object's lock only.
  const uint64_t need = loose_header_size + sealed.ciphertext.size();
  if (!physical_space_available_locked(need)) return false;
  const bool ephemeral = _mode == LocalStoreMode::ephemeral;
  _reserved_write_bytes += need;
  bool space = false;
  {
    Unlocked unlocked(lock, _mutex);
    space = filesystem_space_available_for_reservations();
  }
  if (!space) {
    _reserved_write_bytes -= need;
    return false;
  }
  if (!ephemeral) {
    try {
      ensure_accounting_dirty(lock);
    } catch (...) {
      _reserved_write_bytes -= need;
      throw;
    }
  }
  bool reserved = true;
  try {
    const auto file = path(id);
    std::optional<LooseStamp> installed_stamp;
    {
      Unlocked unlocked(lock, _mutex);
      dannan::identity::SealedData record;
      record.nonce = sealed.nonce;
      record.tag = sealed.tag;
      const auto header = encode_loose_header(sealed.plain_size, record);
      if (header.size() + sealed.ciphertext.size() != need) throw std::logic_error("local store loose object size mismatch");
      _files.install(file, header, sealed.ciphertext);
      installed_stamp = loose_stamp(file);
      if (!installed_stamp) throw std::runtime_error("cannot stat installed local object");
    }
    if (_reserved_write_bytes < need) throw std::logic_error("local store write reservation underflow");
    _reserved_write_bytes -= need;
    reserved = false;
    _used.fetch_add(need, std::memory_order_relaxed);
    remember_verified_loose_locked(id, *installed_stamp);
    _presence.installed(id);
    if (_ledger) _ledger->record(id, true);
    generation = 0;
    if (!ephemeral && _durability_domain) {
      generation = _durability_domain->complete_mutation(file, file.parent_path());
      _last_mutation_generation = std::max(_last_mutation_generation, generation);
      _provisional_generations[id] = generation;
      _provisional_order.push_back({generation, id});
      reap_durable_generations_locked();
    }
    return true;
  } catch (...) {
    if (reserved) {
      if (_reserved_write_bytes < need) throw std::logic_error("local store write reservation underflow during failure");
      _reserved_write_bytes -= need;
    }
    throw;
  }
}

bool LocalStore::put_packed_locked(const ObjectId& id, const SealedObject& sealed, uint64_t& generation, Lock& lock) {
  const uint64_t need = pack_header_size + sealed.ciphertext.size();
  if (!physical_space_available_locked(need)) return false;
  const bool ephemeral = _mode == LocalStoreMode::ephemeral;
  _reserved_write_bytes += need;
  bool space = false;
  {
    Unlocked unlocked(lock, _mutex);
    space = filesystem_space_available_for_reservations();
  }
  if (!space) {
    _reserved_write_bytes -= need;
    return false;
  }
  if (!ephemeral) {
    try {
      ensure_accounting_dirty(lock);
    } catch (...) {
      _reserved_write_bytes -= need;
      throw;
    }
  }
  PackEntry entry;
  uint64_t record_size = 0;
  try {
    append_pack_record_locked(pack_put, id, &sealed, _wall.unix_ms(), &entry, &record_size, lock);
  } catch (...) {
    if (_reserved_write_bytes < need) throw std::logic_error("local store packed write reservation underflow");
    _reserved_write_bytes -= need;
    throw;
  }
  if (_reserved_write_bytes < need) throw std::logic_error("local store packed write reservation underflow");
  _reserved_write_bytes -= need;
  if (const auto old = _packed.find(id); old != _packed.end()) _pack_dead_bytes += old->second.record_size;
  _packed[id] = entry;
  if (_ledger) _ledger->record(id, true);
  _used.fetch_add(record_size, std::memory_order_relaxed);

  generation = 0;
  if (!ephemeral && _durability_domain) {
    generation = _durability_domain->complete_mutation(entry.file, _packs);
    _last_mutation_generation = std::max(_last_mutation_generation, generation);
    _provisional_generations[id] = generation;
    _provisional_order.push_back({generation, id});
    reap_durable_generations_locked();
  }
  return true;
}

bool LocalStore::store_sealed_locked(const ObjectId& id, const SealedObject& sealed, uint64_t& generation, Lock& lock) {
  const bool pack = _pack_threshold && sealed.plain_size <= _pack_threshold;
  return pack ? put_packed_locked(id, sealed, generation, lock) : put_loose_locked(id, sealed, generation, lock);
}

bool LocalStore::put_sealed(const ObjectId& id, const SealedObject& sealed) {
  if (sealed.ciphertext.size() != sealed.plain_size) throw std::invalid_argument("sealed object size mismatch");
  await_accounting();
  ObjectLock object_guard(_object_locks.mutex(id));
  Lock lock(_mutex);
  wait_for_accounting(lock);
  // Held already: the record stands. A member re-putting plaintext verifies
  // and re-affirms (put); a sealed copy is taken only where none is.
  if (_packed.contains(id)) return true;
  const auto file = path(id);
  bool loose_exists = false;
  {
    Unlocked unlocked(lock, _mutex);
    loose_exists = std::filesystem::exists(file);
  }
  if (loose_exists) return true;
  uint64_t generation = 0;
  if (!store_sealed_locked(id, sealed, generation, lock)) return false;
  lock.unlock();
  if (generation) _durability_domain->await_durable(generation, DurabilityUrgency::immediate);
  // As put: once the held ledger is seeded it answers has(), so the record
  // is in it before the put returns.
  flush_ledger();
  return true;
}

std::optional<LocalStore::SealedObject> LocalStore::get_sealed(const ObjectId& id) const {
  ObjectLock object_guard(_object_locks.mutex(id));
  Lock lock(_mutex);
  return get_sealed_locked(id, lock);
}

std::optional<dannan::identity::Hash256> LocalStore::held_digest(const ObjectId& id) const {
  const auto sealed = get_sealed(id);
  if (!sealed) return std::nullopt;
  return blind_digest(sealed->nonce, sealed->tag, sealed->ciphertext);
}

bool LocalStore::put_impl(const ObjectId& id, std::span<const uint8_t> data, WriteDurability durability, uint64_t* deferred_generation) {
  if (_role != domain::Role::member) throw std::logic_error("a blind store holds ciphertext only: put_sealed");
  if (object_id(data) != id) throw std::runtime_error("object hash mismatch");
  await_accounting();
  ObjectLock object_guard(_object_locks.mutex(id));
  Lock lock(_mutex);
  wait_for_accounting(lock);

  bool pack = false;
  if (_packed.contains(id)) {
    std::optional<Bytes> existing;
    try {
      existing = get_packed_locked(id, lock);
    } catch (...) {
      existing.reset();
    }
    if (existing && existing->size() == data.size() && std::equal(existing->begin(), existing->end(), data.begin())) {
      // A compact touch record makes the re-affirmation's age survive a crash.
      uint64_t record_size = 0;
      PackEntry record;
      if (_mode != LocalStoreMode::ephemeral) ensure_accounting_dirty(lock);
      const auto touched = _wall.unix_ms();
      append_pack_record_locked(pack_touch, id, nullptr, touched, &record, &record_size, lock);
      _packed.at(id).touched_unix_ms = touched;
      _pack_dead_bytes += record_size;
      _used.fetch_add(record_size, std::memory_order_relaxed);
      uint64_t generation = 0;
      if (_mode != LocalStoreMode::ephemeral && _durability_domain) {
        generation = _durability_domain->complete_mutation(record.file, _packs);
        _last_mutation_generation = std::max(_last_mutation_generation, generation);
        _provisional_generations[id] = generation;
        _provisional_order.push_back({generation, id});
      }
      if (deferred_generation) *deferred_generation = generation;
      if (durability == WriteDurability::immediate && generation) {
        lock.unlock();
        _durability_domain->await_durable(generation, DurabilityUrgency::immediate);
      }
      return true;
    }
    // A corrupt packed record is superseded by the new record. The old bytes
    // stay dead until compaction; no in-place rewrite can damage neighbours.
    pack = true;
  } else {
    const auto file = path(id);
    lock.unlock();
    const bool loose_exists = std::filesystem::exists(file);
    lock.lock();
    if (loose_exists) {
      std::optional<LooseStamp> remembered;
      if (const auto verified = _verified_loose.find(id); verified != _verified_loose.end()) remembered = verified->second.stamp;
      std::optional<Bytes> existing;
      lock.unlock();
      const auto current_stamp = loose_stamp(file);
      const bool verified_fast = remembered && current_stamp && *remembered == *current_stamp;
      if (verified_fast) {
        _loose_reaffirmation_fast_paths.fetch_add(1, std::memory_order_relaxed);
      } else {
        _loose_reaffirmation_full_validations.fetch_add(1, std::memory_order_relaxed);
        try {
          existing = open_loose(id, _files.read(file));
        } catch (...) {
          existing.reset();
        }
      }
      if (verified_fast || (existing && existing->size() == data.size() && std::equal(existing->begin(), existing->end(), data.begin()))) {
        std::error_code touch_error;
        std::filesystem::last_write_time(file, wall_file_time(), touch_error);
        const auto touched_stamp = loose_stamp(file);
        lock.lock();
        if (touched_stamp)
          remember_verified_loose_locked(id, *touched_stamp);
        else
          forget_verified_loose_locked(id);
        reap_durable_generations_locked();
        uint64_t generation = 0;
        if (const auto found = _provisional_generations.find(id); found != _provisional_generations.end()) generation = found->second;
        if (deferred_generation) *deferred_generation = generation;
        if (durability == WriteDurability::immediate && _durability_domain && generation > _durability_domain->durable_generation()) {
          lock.unlock();
          _durability_domain->await_durable(generation, DurabilityUrgency::immediate);
        }
        return true;
      }

      // Keep the strong put acknowledgement: an object corrupted from outside
      // is replaced by the bytes supplied rather than trusted merely because
      // its content-addressed name exists.
      lock.lock();
      if (!remove_locked(id, lock)) {
        lock.unlock();
        const bool still_exists = std::filesystem::exists(file);
        lock.lock();
        if (still_exists) throw std::runtime_error("cannot replace corrupt local object");
      }
    }
    pack = _pack_threshold && data.size() <= _pack_threshold;
  }

  // Sealed under the object's lock with the index free, as the install is.
  SealedObject sealed;
  {
    Unlocked unlocked(lock, _mutex);
    sealed = seal(id, data);
  }
  uint64_t generation = 0;
  const bool stored = pack ? put_packed_locked(id, sealed, generation, lock) : put_loose_locked(id, sealed, generation, lock);
  if (!stored) return false;
  if (deferred_generation) *deferred_generation = generation;
  if (durability == WriteDurability::immediate && generation) {
    lock.unlock();
    _durability_domain->await_durable(generation, DurabilityUrgency::immediate);
  }
  return true;
}

std::optional<Bytes> LocalStore::get(const ObjectId& id) const {
  if (_role != domain::Role::member) throw std::logic_error("a blind store holds ciphertext only: get_sealed");
  ObjectLock object_guard(_object_locks.mutex(id));
  Lock lock(_mutex);
  if (_packed.contains(id)) return get_packed_locked(id, lock);
  const auto file = path(id);
  lock.unlock();
  if (!std::filesystem::exists(file)) return std::nullopt;
  const auto encoded = _files.read(file);
  if (encoded.empty() && prune_empty_loose(id, file)) return std::nullopt;
  auto plain = open_loose(id, encoded);
  if (const auto stamp = loose_stamp(file)) {
    lock.lock();
    remember_verified_loose_locked(id, *stamp);
    // A file the ledger does not list (a crash between the file and its
    // record) is recorded by the read that finds it.
    const bool unlisted = _ledger && _ledger->seeded() && !_ledger->held(id).value_or(true);
    if (unlisted) _ledger->record(id, true);
    lock.unlock();
    if (unlisted) flush_ledger();
  }
  return plain;
}

std::optional<bool> LocalStore::presence_from_index(const ObjectId& id) const {
  dannan::contract::NoIoRegion no_io_region;
  Lock lock(_mutex);
  if (_packed.contains(id)) return true;
  if (const auto known = _presence.answer(id); known != PresenceIndex::Answer::unknown) return known == PresenceIndex::Answer::present;
  return std::nullopt;
}

bool LocalStore::presence_authoritative() const {
  Lock lock(_mutex);
  return _presence.authoritative();
}

bool LocalStore::has(const ObjectId& id) const noexcept {
  try {
    // Once the ledger is seeded it is the answer, packs aside.
    if (_ledger && _ledger->seeded()) {
      {
        Lock lock(_mutex);
        if (_packed.contains(id)) return true;
      }
      return _ledger->held(id).value_or(false);
    }
    // After warm-up the index is the answer. A put publishes presence only
    // once its file is installed, so a put in progress reads as absent,
    // never as half-written; nothing else writes objects.
    if (const auto known = presence_from_index(id)) return *known;
    // Before warm-up has listed the store, a miss is asked of the device,
    // under the object's lock so a put of it is not seen midway.
    ObjectLock object_guard(_object_locks.mutex(id));
    {
      Lock lock(_mutex);
      if (_packed.contains(id) || _presence.answer(id) == PresenceIndex::Answer::present) return true;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path(id), error);
    const bool present = !error && size > 0;
    if (present) {
      Lock lock(_mutex);
      _presence.observed(id);
      if (_ledger) _ledger->record(id, true);
    }
    return present;
  } catch (...) {
    return false;
  }
}

bool LocalStore::valid(const ObjectId& id) const noexcept {
  try {
    if (_role != domain::Role::member) {
      const auto sealed = get_sealed(id);
      return sealed && sealed->ciphertext.size() == sealed->plain_size;
    }
    return get(id).has_value();
  } catch (...) {
    return false;
  }
}

bool LocalStore::remove_locked(const ObjectId& id, Lock& lock) {
  forget_verified_loose_locked(id);
  if (const auto found = _packed.find(id); found != _packed.end()) {
    if (_mode != LocalStoreMode::ephemeral) ensure_accounting_dirty(lock);
    uint64_t tomb_size = 0;
    PackEntry tomb;
    append_pack_record_locked(pack_remove, id, {}, _wall.unix_ms(), &tomb, &tomb_size, lock);
    // The append released _mutex: find the entry again.
    const auto current = _packed.find(id);
    if (current != _packed.end()) {
      _pack_dead_bytes += current->second.record_size;
      _packed.erase(current);
    }
    _pack_dead_bytes += tomb_size;
    if (_ledger) _ledger->record(id, false);
    _losses.fetch_add(1, std::memory_order_release);
    _used.fetch_add(tomb_size, std::memory_order_relaxed);
    _provisional_generations.erase(id);
    if (_mode != LocalStoreMode::ephemeral && _durability_domain) {
      const auto generation = _durability_domain->complete_mutation(tomb.file, _packs);
      _last_mutation_generation = std::max(_last_mutation_generation, generation);
    }
    return true;
  }

  const auto file = path(id);
  std::error_code error;
  uintmax_t size = 0;
  {
    Unlocked unlocked(lock, _mutex);
    size = std::filesystem::file_size(file, error);
  }
  if (error) return false;
  if (_mode != LocalStoreMode::ephemeral) ensure_accounting_dirty(lock);
  bool removed = false;
  uint64_t generation = 0;
  {
    Unlocked unlocked(lock, _mutex);
    removed = std::filesystem::remove(file, error);
    if (removed && !error && _mode != LocalStoreMode::ephemeral && _durability_domain)
      generation = _durability_domain->complete_mutation({}, file.parent_path());
  }
  if (error || !removed) return false;
  if (_ledger) _ledger->record(id, false);
  _losses.fetch_add(1, std::memory_order_release);
  const auto before = _used.fetch_sub(size, std::memory_order_relaxed);
  if (size > before) {
    _used.fetch_add(size, std::memory_order_relaxed);
    throw std::runtime_error("local accounting underflow");
  }
  _provisional_generations.erase(id);
  if (generation) _last_mutation_generation = std::max(_last_mutation_generation, generation);
  return true;
}

bool LocalStore::put(const ObjectId& id, std::span<const uint8_t> data) {
  const auto stored = put_impl(id, data, WriteDurability::immediate, nullptr);
  flush_ledger();
  return stored;
}

std::optional<uint64_t> LocalStore::put_deferred(const ObjectId& id, std::span<const uint8_t> data) {
  uint64_t generation = 0;
  const auto stored = put_impl(id, data, WriteDurability::deferred, &generation);
  flush_ledger();
  if (!stored) return std::nullopt;
  return generation;
}

void LocalStore::durability_barrier(uint64_t required_generation, DurabilityUrgency urgency) {
  if (_mode == LocalStoreMode::ephemeral || !_durability_domain || !required_generation) return;
  _durability_domain->await_durable(required_generation, urgency);
}

void LocalStore::durability_barrier() {
  if (_mode == LocalStoreMode::ephemeral || !_durability_domain) return;
  uint64_t generation = 0;
  {
    Lock lock(_mutex);
    generation = _last_mutation_generation;
  }
  if (generation) _durability_domain->await_durable(generation, DurabilityUrgency::immediate);
}

uint64_t LocalStore::durable_generation() const { return _durability_domain ? _durability_domain->durable_generation() : 0; }

uint64_t LocalStore::durability_domain_id() const noexcept { return _durability_domain ? _durability_domain->id() : 0; }

bool LocalStore::remove(const ObjectId& id) {
  bool removed = false;
  await_accounting();
  {
    ObjectLock object_guard(_object_locks.mutex(id));
    Lock lock(_mutex);
    wait_for_accounting(lock);
    removed = remove_locked(id, lock);
  }
  flush_ledger();
  return removed;
}

bool LocalStore::remove_if_older_than(const ObjectId& id, std::chrono::milliseconds age) {
  await_accounting();
  const bool removed = [&] {
    ObjectLock object_guard(_object_locks.mutex(id));
    Lock lock(_mutex);
    wait_for_accounting(lock);
    if (const auto found = _packed.find(id); found != _packed.end()) {
      const auto now = _wall.unix_ms();
      if (now < found->second.touched_unix_ms || now - found->second.touched_unix_ms < static_cast<uint64_t>(age.count())) return false;
      return remove_locked(id, lock);
    }
    lock.unlock();
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path(id), error);
    if (error || age_ms(modified) < age.count()) return false;
    lock.lock();
    return remove_locked(id, lock);
  }();
  flush_ledger();
  return removed;
}

std::vector<ObjectId> LocalStore::list() const {
  std::vector<ObjectId> out;
  Cursor cursor;
  bool exhausted = false;
  while (!exhausted)
    if (const auto id = next_object(cursor, exhausted)) out.push_back(*id);
  return out;
}

std::optional<ObjectId> LocalStore::next_object(Cursor& cursor, bool& exhausted) const {
  exhausted = false;
  if (!cursor.packed_done) {
    Lock lock(_mutex);
    const auto it = cursor.packed_after ? _packed.upper_bound(*cursor.packed_after) : _packed.begin();
    if (it != _packed.end()) {
      cursor.packed_after = it->first;
      return it->first;
    }
    cursor.packed_done = true;
  }

  std::error_code error;
  if (!cursor.loose_initialized) {
    cursor.iterator = std::filesystem::recursive_directory_iterator(_objects, std::filesystem::directory_options::skip_permission_denied, error);
    cursor.loose_initialized = true;
    if (error) {
      cursor = {};
      exhausted = true;
      return std::nullopt;
    }
  }
  const std::filesystem::recursive_directory_iterator end;
  while (cursor.iterator != end) {
    const auto entry = *cursor.iterator;
    cursor.iterator.increment(error);
    if (error) {
      cursor = {};
      exhausted = true;
      return std::nullopt;
    }
    std::error_code type_error;
    if (!entry.is_regular_file(type_error) || type_error) continue;
    const auto name = entry.path().filename().string();
    if (name.size() != 68 || name.compare(64, 4, ".obj") != 0) continue;
    const auto bytes = dannan::identity::unhex(name.substr(0, 64));
    if (!bytes || bytes->size() != 32) continue;
    ObjectId id;
    std::copy(bytes->begin(), bytes->end(), id.bytes.begin());
    return id;
  }
  cursor = {};
  exhausted = true;
  return std::nullopt;
}

std::filesystem::path LocalStore::object_path(const ObjectId& id) const {
  Lock lock(_mutex);
  if (const auto found = _packed.find(id); found != _packed.end()) return found->second.file;
  return path(id);
}

uint64_t LocalStore::stored_size(const ObjectId& id) const {
  ObjectLock object_guard(_object_locks.mutex(id));
  {
    Lock lock(_mutex);
    if (const auto found = _packed.find(id); found != _packed.end()) return found->second.record_size;
  }
  std::error_code error;
  const auto size = std::filesystem::file_size(path(id), error);
  return error ? 0 : size;
}

std::filesystem::file_time_type LocalStore::last_write(const ObjectId& id) const {
  ObjectLock object_guard(_object_locks.mutex(id));
  {
    Lock lock(_mutex);
    if (const auto found = _packed.find(id); found != _packed.end()) return file_time_from_unix_ms(found->second.touched_unix_ms);
  }
  std::error_code error;
  const auto time = std::filesystem::last_write_time(path(id), error);
  return error ? std::filesystem::file_time_type::min() : time;
}

void LocalStore::touch(const ObjectId& id) {
  ObjectLock object_guard(_object_locks.mutex(id));
  Lock lock(_mutex);
  if (_packed.contains(id)) {
    uint64_t record_size = 0;
    PackEntry record;
    const auto touched = _wall.unix_ms();
    if (_mode != LocalStoreMode::ephemeral) ensure_accounting_dirty(lock);
    append_pack_record_locked(pack_touch, id, {}, touched, &record, &record_size, lock);
    // The append released _mutex: find the entry again.
    if (const auto found = _packed.find(id); found != _packed.end()) found->second.touched_unix_ms = touched;
    _pack_dead_bytes += record_size;
    _used.fetch_add(record_size, std::memory_order_relaxed);
    if (_mode != LocalStoreMode::ephemeral && _durability_domain) {
      const auto generation = _durability_domain->complete_mutation(record.file, _packs);
      _last_mutation_generation = std::max(_last_mutation_generation, generation);
    }
    return;
  }
  lock.unlock();
  std::error_code error;
  std::filesystem::last_write_time(path(id), wall_file_time(), error);
}

bool LocalStore::older_than(const ObjectId& id, std::chrono::milliseconds age) const {
  ObjectLock object_guard(_object_locks.mutex(id));
  {
    Lock lock(_mutex);
    if (const auto found = _packed.find(id); found != _packed.end()) {
      const auto now = _wall.unix_ms();
      return now >= found->second.touched_unix_ms && now - found->second.touched_unix_ms > static_cast<uint64_t>(age.count());
    }
  }
  std::error_code error;
  const auto time = std::filesystem::last_write_time(path(id), error);
  if (error) return false;
  return age_ms(time) > age.count();
}

bool LocalStore::is_packed(const ObjectId& id) const {
  ObjectLock object_guard(_object_locks.mutex(id));
  Lock lock(_mutex);
  return _packed.contains(id);
}

std::filesystem::file_time_type LocalStore::wall_file_time() const { return file_time_from_unix_ms(_wall.unix_ms()); }

int64_t LocalStore::age_ms(std::filesystem::file_time_type time) const { return static_cast<int64_t>(_wall.unix_ms()) - unix_ms_of(time); }

bool LocalStore::compact_packs_locked(Lock& lock) {
  if (!_pack_threshold || _pack_dead_bytes == 0) return true;

  struct PackUsage {
    std::filesystem::path file;
    uint64_t size = 0;
    uint64_t live = 0;
    uint64_t dead = 0;
  };

  std::map<std::filesystem::path, uint64_t> live_by_pack;
  for (const auto& [id, entry] : _packed) {
    auto& live = live_by_pack[entry.file];
    if (entry.record_size > std::numeric_limits<uint64_t>::max() - live) throw std::runtime_error("pack compaction live-size overflow");
    live += entry.record_size;
  }
  const auto packed_snapshot = _packed;

  std::vector<PackUsage> packs;
  uint64_t total_dead = 0;
  std::optional<PackUsage> victim;
  bool space_available = true;
  {
    Unlocked unlocked(lock, _mutex);
    for (const auto& [file, size] : _files.list(_packs)) {
      if (!pack_sequence(file.filename().string())) continue;
      const auto found = live_by_pack.find(file);
      const uint64_t live = found == live_by_pack.end() ? 0 : found->second;
      if (live > size) throw std::runtime_error("pack live accounting exceeds physical pack size");
      const uint64_t dead = size - live;
      if (dead > std::numeric_limits<uint64_t>::max() - total_dead) throw std::runtime_error("pack dead-size overflow");
      total_dead += dead;
      packs.push_back({file, size, live, dead});
    }

    // The cached counter is only an admission hint; recompute from the index
    // so an interrupted compaction cannot leave dead bytes hidden.
    const auto most_dead = std::max_element(packs.begin(), packs.end(), [](const PackUsage& a, const PackUsage& b) {
      if (a.dead != b.dead) return a.dead < b.dead;
      return a.size < b.size;
    });
    if (most_dead != packs.end() && most_dead->dead != 0) {
      victim = *most_dead;
      // At most one source pack per call, so temporary space is bounded by
      // that pack's live bytes.
      std::error_code space_error;
      const auto space = std::filesystem::space(_root, space_error);
      if (space_error) throw std::runtime_error("cannot inspect free space for pack compaction: " + space_error.message());
      space_available = victim->live <= space.available && _reserve_free <= space.available - victim->live;
    }
  }
  if (!victim) {
    _pack_dead_bytes = 0;
    return true;
  }
  if (!space_available) return false;

  std::vector<std::pair<ObjectId, PackEntry>> victim_entries;
  for (const auto& [id, entry] : packed_snapshot)
    if (entry.file == victim->file) victim_entries.push_back({id, entry});

  std::filesystem::path replacement;
  uint64_t replacement_size = 0;
  std::map<ObjectId, PackEntry> rebuilt;
  if (!victim_entries.empty()) {
    Unlocked unlocked(lock, _mutex);
    std::filesystem::path temp;
    int fd = -1;
    try {
      const auto sequence = _next_pack_sequence++;
      replacement = _packs / pack_name(sequence);
      temp = _packs / (".compact-" + std::to_string(::getpid()) + "-" + std::to_string(sequence) + ".tmp");
      fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (fd < 0) throw std::runtime_error("cannot create compacted pack: " + std::string(std::strerror(errno)));

      for (const auto& [id, old_entry] : victim_entries) {
        if (old_entry.payload_offset < pack_header_size) throw std::runtime_error("invalid packed object offset during compaction");
        const auto record_offset = old_entry.payload_offset - pack_header_size;
        const auto bytes = _files.read_at(victim->file, record_offset, static_cast<size_t>(old_entry.record_size));
        if (!bytes) throw std::runtime_error("short live pack record during compaction");
        write_all(fd, *bytes);

        auto next = old_entry;
        next.file = replacement;
        next.payload_offset = replacement_size + pack_header_size;
        rebuilt.emplace(id, std::move(next));
        replacement_size += old_entry.record_size;
      }

      if (::fsync(fd) != 0) throw std::runtime_error("cannot sync compacted pack: " + std::string(std::strerror(errno)));
      _own_syncs.fetch_add(1, std::memory_order_relaxed);
      if (::close(fd) != 0) {
        fd = -1;
        throw std::runtime_error("cannot close compacted pack: " + std::string(std::strerror(errno)));
      }
      fd = -1;
      if (::rename(temp.c_str(), replacement.c_str()) != 0) throw std::runtime_error("cannot install compacted pack: " + std::string(std::strerror(errno)));
      temp.clear();
      sync_directory(_packs);
    } catch (...) {
      if (fd >= 0) ::close(fd);
      if (!temp.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
      }
      throw;
    }
  }

  if (_mode != LocalStoreMode::ephemeral) ensure_accounting_dirty(lock);

  const auto before = _used.load(std::memory_order_relaxed);
  if (replacement_size > std::numeric_limits<uint64_t>::max() - before) throw std::runtime_error("pack compaction accounting overflow");
  _used.store(before + replacement_size, std::memory_order_relaxed);

  // The installed replacement has a strictly newer sequence than every
  // existing pack. Point the live index at it before removing the victim; a
  // failed unlink then leaves only harmless dead duplicate bytes.
  for (auto& [id, entry] : rebuilt) _packed[id] = std::move(entry);

  if (!replacement.empty() && replacement_size < _pack_target_size) {
    _active_pack = replacement;
    _active_pack_size = replacement_size;
  } else {
    _active_pack.clear();
    _active_pack_size = 0;
  }

  // Readers that selected the victim before the index switch may not have
  // opened it yet; no new reader can select it after the switch above.
  _pack_readers_cv.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) {
    const auto active = _active_pack_readers.find(victim->file);
    return active == _active_pack_readers.end() || active->second == 0;
  });

  std::error_code remove_error;
  bool removed = false;
  uint64_t generation = 0;
  {
    Unlocked unlocked(lock, _mutex);
    removed = std::filesystem::remove(victim->file, remove_error);
    if (remove_error || !removed) {
      // Every record in the old victim is now superseded by the newer copy
      // (or was already dead), so the whole old pack is dead and a later
      // bounded compaction retries it.
      _logger.warn("cannot remove compacted source pack path=" + victim->file.string() + (remove_error ? " error=" + remove_error.message() : ""));
    }
    sync_directory(_packs);
    if (_mode != LocalStoreMode::ephemeral && _durability_domain) generation = _durability_domain->complete_mutation({}, _packs);
  }
  if (remove_error || !removed) {
    _pack_dead_bytes = total_dead - victim->dead + victim->size;
  } else {
    const auto current_used = _used.fetch_sub(victim->size, std::memory_order_relaxed);
    if (victim->size > current_used) {
      _used.fetch_add(victim->size, std::memory_order_relaxed);
      throw std::runtime_error("pack compaction accounting underflow");
    }
    _pack_dead_bytes = total_dead - victim->dead;
  }
  if (generation) _last_mutation_generation = std::max(_last_mutation_generation, generation);
  return !remove_error && removed;
}

bool LocalStore::compact_packs(std::stop_token stop) {
  Lock pack_io_lock(_pack_io_mutex);
  Lock lock(_mutex);
  if (!wait_for_accounting(lock, stop, true)) return false;
  return compact_packs_locked(lock);
}

void LocalStore::wait_for_accounting(Lock& lock) const { (void)wait_for_accounting(lock, {}); }

void LocalStore::await_accounting() const {
  // Before the object's lock, never under it: the scan takes object locks as
  // it walks, so a writer waiting for the scan while holding one (as Macha's
  // put and remove do) waits for ever once the scan reaches that object.
  Lock lock(_mutex);
  wait_for_accounting(lock);
}

bool LocalStore::wait_for_accounting(Lock& lock, std::stop_token stop, bool exact) const {
  // Ordinary puts and removes go on with a checkpoint's estimate; only a
  // caller that needs the reconciled figure (compaction) waits for the walk.
  if (!exact && _accounting_estimate.load(std::memory_order_acquire) && !_scan_failed.load(std::memory_order_acquire)) return true;
  std::stop_callback wake_waiter(stop, [this] { _accounting_cv.notify_all(); });
  _accounting_cv.wait(lock.native(), [this, &stop] {
    return _scan_complete.load(std::memory_order_acquire) || _scan_failed.load(std::memory_order_acquire) || stop.stop_requested();
  });
  if (stop.stop_requested()) return false;
  if (_scan_failed.load(std::memory_order_acquire)) throw std::runtime_error("storage accounting reconciliation failed");
  return true;
}

LocalStore::VerifyResult LocalStore::verify_step(size_t directories) {
  VerifyResult result;
  if (!_ledger || !_ledger->seeded()) {
    result.complete = true;
    return result;
  }
  std::vector<ObjectId> missing;
  uint32_t next = _ledger->verify_next();
  while (result.directories < directories && !result.complete) {
    const auto prefix = static_cast<uint16_t>(next);
    next = (next + 1) & 0xffff;
    result.complete = next == 0;
    ++result.directories;
    ObjectId sample;
    sample.bytes[0] = static_cast<uint8_t>(prefix >> 8);
    sample.bytes[1] = static_cast<uint8_t>(prefix & 0xff);
    const auto directory = path(sample).parent_path();
    std::vector<ObjectId> files;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
      if (const auto id = object_id_from_file_name(it->path().filename().string())) files.push_back(*id);
    // A listing that failed part way proves nothing; an absent directory
    // holds nothing.
    if (error && error != std::errc::no_such_file_or_directory) continue;
    std::sort(files.begin(), files.end());
    const auto listed = _ledger->held_with_prefix(prefix);
    std::vector<ObjectId> unlisted;
    std::set_difference(files.begin(), files.end(), listed.begin(), listed.end(), std::back_inserter(unlisted));
    std::set_difference(listed.begin(), listed.end(), files.begin(), files.end(), std::back_inserter(missing));
    for (const auto& id : unlisted) {
      // As the scan does: under the object's lock, and only if the file is
      // still there and not empty.
      ObjectLock object_guard(_object_locks.mutex(id));
      std::error_code size_error;
      const auto size = std::filesystem::file_size(path(id), size_error);
      if (size_error || size == 0) continue;
      Lock lock(_mutex);
      _ledger->record(id, true);
      ++result.recorded;
    }
  }
  // Every change already made is installed, so a removal that raced the
  // listing reads as not held below.
  flush_ledger();
  for (const auto& id : missing) {
    ObjectLock object_guard(_object_locks.mutex(id));
    if (!_ledger->held(id).value_or(false)) continue;
    std::error_code exists_error;
    if (std::filesystem::exists(path(id), exists_error) || exists_error) continue;
    Lock lock(_mutex);
    // A packed object has no file of its own.
    if (_packed.contains(id)) continue;
    _ledger->record(id, false);
    ++result.lost;
    _losses.fetch_add(1, std::memory_order_release);
  }
  flush_ledger();
  _ledger->set_verify_next(static_cast<uint16_t>(next));
  if (result.recorded || result.lost)
    _logger.warn("storage held ledger corrected path=" + _root.string() + " recorded=" + std::to_string(result.recorded) +
                 " lost=" + std::to_string(result.lost));
  return result;
}

void LocalStore::warm_presence_index(std::stop_token stop) {
  // Until a walk completes, has() asks the device on a miss; a failed walk is
  // retried, backing off to five minutes, so that stays temporary. The
  // backoff is on the real clock: it paces a retry, it decides nothing.
  auto backoff = std::chrono::seconds(1);
  while (!stop.stop_requested()) {
    const auto started = dannan::contract::Clock::now();
    std::error_code error;
    std::vector<ObjectId> batch;
    batch.reserve(4096);
    uint64_t entries = 0;
    const auto flush = [&] {
      if (batch.empty()) return;
      Lock lock(_mutex);
      _presence.listed(batch);
      entries += batch.size();
      batch.clear();
    };
    // Directory names only: readdir supplies the file type, so nothing here
    // stats an object. The scan (when it runs) does the same as it walks.
    for (auto it = std::filesystem::recursive_directory_iterator(_objects, error); !error && it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
      if (stop.stop_requested()) return;
      if (it.depth() < 2) continue;
      if (const auto id = object_id_from_file_name(it->path().filename().string())) {
        batch.push_back(*id);
        if (batch.size() >= 4096) flush();
      }
    }
    flush();
    _presence_index_entries.store(entries, std::memory_order_relaxed);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(dannan::contract::Clock::now() - started).count();
    if (!error) {
      {
        Lock lock(_mutex);
        _presence.warmed();
      }
      _logger.debug("storage presence index warmed path=" + _root.string() + " objects=" + std::to_string(entries) +
                    " elapsed_ms=" + std::to_string(elapsed_ms));
      if (_ledger) seed_ledger();
      return;
    }
    _logger.warn("storage presence index walk failed path=" + _root.string() + " error=" + error.message() + " retry_s=" + std::to_string(backoff.count()));
    // A stoppable sleep: the mutex guards nothing.
    Mutex wait_mutex;
    std::condition_variable_any wait_cv;
    Lock wait_lock(wait_mutex);
    wait_cv.wait_for(wait_lock.native(), stop, backoff, [] { return false; });
    backoff = std::min<std::chrono::seconds>(backoff * 2, std::chrono::minutes(5));
  }
}

void LocalStore::scan(std::stop_token stop) {
  _logger.debug("storage accounting scan begin path=" + _root.string());
  uint64_t total = 0;
  std::error_code error;
  for (const auto& root : {_objects, _packs}) {
    for (auto it = std::filesystem::recursive_directory_iterator(root, error); !error && it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
      if (stop.stop_requested()) return;
      // A long scan of a big store is progress, not a stall.
      _progress.note();
      if (!it->is_regular_file()) continue;
      const auto name = it->path().filename().string();
      if (name.find(".tmp") != std::string::npos || name.starts_with(".compact-")) {
        std::error_code remove_error;
        std::filesystem::remove(it->path(), remove_error);
        continue;
      }
      std::error_code size_error;
      const auto size = it->file_size(size_error);
      if (!size_error) total += size;
      if (root == _objects) {
        if (const auto id = object_id_from_file_name(name)) {
          if (!size_error && size == 0) {
            ObjectLock object_guard(_object_locks.mutex(*id));
            (void)prune_empty_loose(*id, it->path());
          } else if (size > 0) {
            // Under the object's lock and only if the file is still there: a
            // remove between this listing and here must not be undone once
            // presence is authoritative.
            ObjectLock object_guard(_object_locks.mutex(*id));
            std::error_code exists_error;
            if (std::filesystem::exists(it->path(), exists_error)) {
              Lock lock(_mutex);
              _presence.observed(*id);
              if (_ledger) _ledger->record(*id, true);
            }
          }
        }
      }
    }
    if (error) break;
  }
  if (error) {
    mark_scan_failed();
    _logger.warn("storage accounting scan failed path=" + _root.string() + " error=" + error.message());
    return;
  }
  if (_mode == LocalStoreMode::authoritative && _durability_domain) {
    try {
      const auto baseline = _durability_domain->complete_mutation();
      _durability_domain->await_durable(baseline, DurabilityUrgency::immediate);
      Lock lock(_mutex);
      _last_mutation_generation = std::max(_last_mutation_generation, baseline);
    } catch (const std::exception& ex) {
      mark_scan_failed();
      _logger.warn("storage accounting baseline durability failed path=" + _root.string() + " error=" + ex.what());
      return;
    }
  }
  {
    Lock lock(_mutex);
    // Writes admitted against the estimate during the walk may or may not
    // have been seen by it; keep the larger figure (over-counting is the safe
    // direction, and bounded by what was written meanwhile).
    const auto running = _used.load(std::memory_order_relaxed);
    _used.store(_accounting_estimate.load(std::memory_order_acquire) ? std::max(total, running) : total, std::memory_order_relaxed);
    _accounting_estimate.store(false, std::memory_order_release);
    _accounting_trusted.store(true, std::memory_order_release);
    if (_mode != LocalStoreMode::ephemeral) {
      // The clean checkpoint is written without _mutex, as
      // ensure_accounting_dirty writes its marker: a writer meanwhile finds
      // the store clean, waits for this write, then marks it dirty.
      _accounting_state_cv.wait(lock.native(), [this]() DANNAN_REQUIRES(_mutex) { return !_accounting_dirty_in_progress; });
      _accounting_dirty_in_progress = true;
      _accounting_dirty = false;
      const auto used = _used.load(std::memory_order_relaxed);
      lock.unlock();
      std::exception_ptr failure;
      try {
        persist_accounting(used, accounting_clean, ObjectId{}, 0, true);
      } catch (...) {
        failure = std::current_exception();
      }
      lock.lock();
      if (failure) _accounting_dirty = true;
      _accounting_dirty_in_progress = false;
      _accounting_state_cv.notify_all();
      if (failure) std::rethrow_exception(failure);
    }
    // Under the mutex a waiter checks it under, or the wake can fall between
    // a waiter's check and its sleep and be lost (Macha sets and notifies
    // outside it, so a put could wait for ever on a finished scan).
    _scan_complete.store(true, std::memory_order_release);
  }
  _accounting_cv.notify_all();
  flush_ledger();
  _logger.debug("storage accounting scan complete path=" + _root.string() + " used=" + std::to_string(total));
}

LocalStoreDiagnostics LocalStore::diagnostics() const noexcept {
  LocalStoreDiagnostics out;
  out.loose_reaffirmation_fast_paths = _loose_reaffirmation_fast_paths.load(std::memory_order_relaxed);
  out.loose_reaffirmation_full_validations = _loose_reaffirmation_full_validations.load(std::memory_order_relaxed);
  out.presence_index_entries = _presence_index_entries.load(std::memory_order_relaxed);
  out.pack_recovery_truncated_tails = _pack_recovery_truncated_tails.load(std::memory_order_relaxed);
  out.pack_recovery_skipped_regions = _pack_recovery_skipped_regions.load(std::memory_order_relaxed);
  out.pack_recovery_skipped_bytes = _pack_recovery_skipped_bytes.load(std::memory_order_relaxed);
  out.own_syncs = _own_syncs.load(std::memory_order_relaxed);
  return out;
}

}  // namespace dagda::store
