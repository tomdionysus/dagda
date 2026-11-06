//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/migrate/manifest.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/sealed_journal.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/migrate/legacy_v18.h"
#include "dagda/migrate/manifest_format.h"
#include "dagda/migrate/migrate.h"
#include "dagda/store/local_format.h"
#include "dagda/store/local_store.h"
#include "dagda/store/sealing.h"
#include "dagda/store/state_layout.h"
#include "dagda/store/storage_pool.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/node_identity.h"
#include "dannan/net/codec.h"

namespace dagda::migrate {

std::shared_ptr<MetadataManifestReader> metadata_manifest_reader() {
  // Heads session S7 returns its reader here. Until then there is none, and the
  // comparison reports heads, certificates and history as NOT CHECKED.
  return nullptr;
}

namespace {

using namespace manifest_format;
using namespace store::local_format;
using dannan::identity::Hash256;
using dannan::identity::NodeId;
using dannan::identity::sha256;

// The order keys. Sections are compared by merging on these, so the writer
// emits them strictly increasing; a store's label comes between separators that
// sort before any character of a path.
constexpr char sep = '\x01';
constexpr uint64_t block_target = 1024 * 1024;
// As the migration: this many records failing with none opened is the wrong key.
constexpr uint64_t probe_limit = 8;
constexpr uint32_t max_read_frame = 64U * 1024U * 1024U;
// Dannan's state lock (dannan/identity/node_identity.h), where a node's state and
// Dannan's are one directory.
constexpr std::string_view dannan_lock_name = ".dannan.lock";

std::string read_text_or_empty(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), {});
}

std::string trimmed(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  return text;
}

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  Bytes bytes(std::istreambuf_iterator<char>(in), {});
  if (in.bad()) throw std::runtime_error("cannot read " + path.string());
  return bytes;
}

Bytes bytes_of(std::string_view text) { return Bytes(text.begin(), text.end()); }

struct FileStat {
  uint64_t size = 0;
  int64_t mtime_ns = 0;
};

FileStat stat_of(const std::filesystem::path& path) {
  struct stat value{};
  if (::lstat(path.c_str(), &value) != 0 || value.st_size < 0) throw std::runtime_error("cannot stat " + path.string() + ": " + std::strerror(errno));
#if defined(__APPLE__)
  const auto mtime = static_cast<int64_t>(value.st_mtimespec.tv_sec) * 1000000000LL + value.st_mtimespec.tv_nsec;
#else
  const auto mtime = static_cast<int64_t>(value.st_mtim.tv_sec) * 1000000000LL + value.st_mtim.tv_nsec;
#endif
  return {static_cast<uint64_t>(value.st_size), mtime};
}

Hash256 digest_of_span(int fd, uint64_t offset, uint64_t length, const std::filesystem::path& path) {
  dannan::identity::Sha256Hasher hasher;
  Bytes chunk(std::min<uint64_t>(length, 1024 * 1024));
  while (length) {
    const auto want = static_cast<size_t>(std::min<uint64_t>(length, chunk.size()));
    const auto n = ::pread(fd, chunk.data(), want, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("cannot read " + path.string());
    hasher.update(std::span<const uint8_t>(chunk.data(), static_cast<size_t>(n)));
    offset += static_cast<uint64_t>(n);
    length -= static_cast<uint64_t>(n);
  }
  return hasher.finish();
}

std::optional<Bytes> read_at(int fd, size_t size, uint64_t offset) {
  Bytes out(size);
  size_t done = 0;
  while (done < size) {
    const auto n = ::pread(fd, out.data() + done, size - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return std::nullopt;
    done += static_cast<size_t>(n);
  }
  return out;
}

// A path as a store's root is compared: no dots, no trailing separator.
std::filesystem::path normal(const std::filesystem::path& path) {
  const auto out = path.lexically_normal();
  return out.has_filename() ? out : out.parent_path();
}

// A shared flock on each lock file there is. A node's exclusive lock refuses
// this and this refuses a node starting; a file is never created, so a store
// with none has its absence checked again at the end.
class ReadOnlyLocks {
 public:
  ReadOnlyLocks() = default;
  ~ReadOnlyLocks() {
    for (const int fd : _held) ::close(fd);
  }
  ReadOnlyLocks(const ReadOnlyLocks&) = delete;
  ReadOnlyLocks& operator=(const ReadOnlyLocks&) = delete;

  void hold(const std::filesystem::path& root, std::string_view name) {
    const auto path = root / std::string(name);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      if (errno != ENOENT) throw std::runtime_error("cannot open " + path.string() + ": " + std::strerror(errno));
      _absent.push_back(path);
      return;
    }
    _held.push_back(fd);
    if (::flock(fd, LOCK_SH | LOCK_NB) != 0) throw MigrationRefused(root.string() + " is in use (is the node running?): " + std::strerror(errno));
  }

  void check_none_appeared() const {
    std::error_code ec;
    for (const auto& path : _absent)
      if (std::filesystem::exists(path, ec))
        throw std::runtime_error(path.string() + " appeared during the pass: a node may have started, and the manifest is not trusted");
  }

 private:
  std::vector<int> _held;
  std::vector<std::filesystem::path> _absent;
};

// What a stopped pass left, to go on from.
struct OpenSection {
  Begin begin;
  uint64_t count = 0;
  Bytes last_key;
};

struct Replay {
  // The last put or tombstone of each id in a store's packs, as the store's
  // recovery replays them: live, or the tombstone's time.
  std::map<ObjectId, std::optional<uint64_t>> last;
  std::optional<std::pair<uint64_t, uint64_t>> next_position;
};

struct Resumed {
  uint64_t size = 0;
  Hash256 chain{};
  uint64_t items = 0;
  uint64_t sections = 0;
  std::set<std::string> closed;
  std::optional<OpenSection> open;
  bool proved = false;
  uint64_t unreadable = 0;
  // Items of the sections that say whether the state has changed.
  std::map<std::string, std::vector<std::pair<Bytes, Bytes>>> recorded;
  std::map<std::string, Replay> replays;
};

// The file, a block at a time, and the order it must hold.
class ManifestFile {
 public:
  ManifestFile(std::filesystem::path path, const Header& header, const Resumed* resumed) : _path(std::move(path)) {
    if (resumed) {
      _fd = ::open(_path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
      if (_fd < 0) throw std::runtime_error("cannot open " + _path.string() + ": " + std::strerror(errno));
      if (::ftruncate(_fd, static_cast<off_t>(resumed->size)) != 0) throw std::runtime_error("cannot truncate " + _path.string() + ": " + std::strerror(errno));
      _chain = resumed->chain;
      _items = resumed->items;
      _sections = resumed->sections;
      _closed = resumed->closed;
      _open = resumed->open;
      return;
    }
    _fd = ::open(_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
    if (_fd < 0) throw std::runtime_error("cannot create " + _path.string() + ": " + std::strerror(errno));
    Bytes head(magic.begin(), magic.end());
    dannan::net::Writer version;
    version.u32(format_version);
    head.insert(head.end(), version.data().begin(), version.data().end());
    write_out(head);
    Record first;
    first.type = RecordType::header;
    first.header = header;
    add(first);
  }
  ~ManifestFile() {
    if (_fd >= 0) ::close(_fd);
  }
  ManifestFile(const ManifestFile&) = delete;
  ManifestFile& operator=(const ManifestFile&) = delete;

  bool closed(const std::string& order) const { return _closed.contains(order); }

  // Opens a section, or finds it open from a stopped pass.
  void begin(const std::string& order, const std::string& display, SectionKind kind, const std::string& not_checked = {}) {
    if (_open && _open->begin.order == order) return;
    if (_open) throw std::logic_error("a manifest section left open: " + _open->begin.display);
    if (!_last_order.empty() && !(_last_order < order)) throw std::logic_error("manifest sections out of order: " + display);
    if (_closed.contains(order)) throw std::logic_error("manifest section written twice: " + display);
    Record record;
    record.type = RecordType::begin;
    record.begin = {order, display, kind, not_checked};
    add(record);
    _open = OpenSection{record.begin, 0, {}};
    _last_order = order;
    ++_sections;
  }

  void item(Bytes key, Bytes value) {
    if (!_open) throw std::logic_error("a manifest item outside a section");
    if (_open->count && !(_open->last_key < key)) throw std::logic_error("manifest items out of order in " + _open->begin.display);
    _open->last_key = key;
    ++_open->count;
    ++_items;
    Record record;
    record.type = RecordType::item;
    record.key = std::move(key);
    record.value = std::move(value);
    add(record);
  }

  uint64_t end() {
    if (!_open) throw std::logic_error("no manifest section to end");
    Record record;
    record.type = RecordType::end;
    record.count = _open->count;
    add(record);
    const auto count = _open->count;
    _closed.insert(_open->begin.order);
    _last_order = _open->begin.order;
    _open.reset();
    return count;
  }

  uint64_t open_count() const { return _open ? _open->count : 0; }

  // Everything so far in a block of its own.
  void flush(bool durable) {
    if (!_block.empty()) {
      dannan::net::Writer framing;
      framing.u32(static_cast<uint32_t>(_block.size()));
      dannan::identity::Sha256Hasher hasher;
      hasher.update(_chain.bytes);
      hasher.update(_block);
      _chain = hasher.finish();
      write_out(framing.data());
      write_out(_block);
      write_out(_chain.bytes);
      _block.clear();
    }
    if (durable) sync();
  }

  void finish() {
    Record record;
    record.type = RecordType::trailer;
    record.count = _items;
    record.sections = _sections;
    add(record);
    flush(true);
  }

  uint64_t items() const noexcept { return _items; }
  uint64_t written() const noexcept { return _written; }
  void sync() {
    int rc;
    do {
      rc = ::fsync(_fd);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) throw std::runtime_error("cannot sync " + _path.string() + ": " + std::strerror(errno));
  }

 private:
  void add(const Record& record) {
    const auto encoded = encode_record(record);
    _block.insert(_block.end(), encoded.begin(), encoded.end());
    if (_block.size() >= block_target) flush(false);
  }

  void write_out(std::span<const uint8_t> bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
      const auto n = ::write(_fd, bytes.data() + done, bytes.size() - done);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::runtime_error("cannot write " + _path.string() + ": " + std::strerror(errno));
      done += static_cast<size_t>(n);
    }
    _written += bytes.size();
  }

  const std::filesystem::path _path;
  int _fd = -1;
  Bytes _block;
  Hash256 _chain{};
  uint64_t _items = 0;
  uint64_t _sections = 0;
  uint64_t _written = 0;
  std::string _last_order;
  std::set<std::string> _closed;
  std::optional<OpenSection> _open;
};

// What a partial holds, up to the last block a pass can go on from: inside the
// loose or pack records of a store, or between two of the sections a store
// starts with. Everything else is cheap enough to read again.
std::optional<Resumed> read_partial(const std::filesystem::path& partial, const Header& header) {
  const auto is_store = [](const std::string& order) { return order.starts_with("1"); };
  uint64_t safe = 0;
  {
    ManifestReader reader(partial, true);
    std::optional<SectionKind> open;
    std::optional<SectionKind> last_closed;
    bool any = false;
    while (const auto record = reader.next()) {
      if (record->type == RecordType::header) {
        if (record->header != header) throw MigrationRefused(partial.string() + " was begun over another state or other stores: start the pass again");
        any = true;
      } else if (record->type == RecordType::begin) {
        if (!is_store(record->begin.order)) break;
        open = record->begin.kind;
      } else if (record->type == RecordType::end) {
        last_closed = open;
        open.reset();
      } else if (record->type == RecordType::trailer) {
        break;
      }
      if (!reader.at_block_end() || !any) continue;
      const bool in_records = open == SectionKind::loose || open == SectionKind::pack_records;
      const bool between = !open && last_closed != SectionKind::removed;
      if (in_records || between) safe = reader.block_end();
    }
  }
  if (!safe) return std::nullopt;

  Resumed out;
  out.size = safe;
  ManifestReader reader(partial, true);
  reader.limit_to(safe);
  std::string order;
  std::string store;
  SectionKind kind = SectionKind::bytes;
  bool open = false;
  while (const auto record = reader.next()) {
    switch (record->type) {
      case RecordType::begin:
        order = record->begin.order;
        kind = record->begin.kind;
        store = order.substr(0, order.rfind(sep));
        out.open = OpenSection{record->begin, 0, {}};
        ++out.sections;
        open = true;
        break;
      case RecordType::item:
        if (!open) throw MigrationRefused(partial.string() + " holds an item outside a section: start the pass again");
        out.open->count += 1;
        out.open->last_key = record->key;
        ++out.items;
        if (kind == SectionKind::loose || kind == SectionKind::pack_files) out.recorded[order].emplace_back(record->key, record->value);
        if (kind == SectionKind::loose) {
          const auto value = decode_loose_value(record->value);
          if (value.layout == record_v18 || value.layout == record_v19) out.proved = true;
          if (value.layout == record_unreadable) ++out.unreadable;
        }
        if (kind == SectionKind::pack_records) {
          const auto value = decode_pack_value(record->value);
          auto& replay = out.replays[store];
          const auto sequence = number_of(std::span<const uint8_t>(record->key).first(8));
          const auto offset = number_of(std::span<const uint8_t>(record->key).subspan(8));
          replay.next_position = std::make_pair(sequence, offset + value.total());
          if (value.span == span_record && value.header.type == pack_put) {
            replay.last[value.header.id] = std::nullopt;
            if (value.layout == record_v18 || value.layout == record_v19) out.proved = true;
            if (value.layout == record_unreadable) ++out.unreadable;
          } else if (value.span == span_record && value.header.type == pack_remove) {
            replay.last[value.header.id] = value.header.touched_ms;
          }
        }
        break;
      case RecordType::end:
        out.closed.insert(order);
        out.open.reset();
        open = false;
        break;
      default:
        break;
    }
  }
  out.chain = reader.chain();
  return out;
}

class Pass {
 public:
  Pass(const std::filesystem::path& state, const dannan::identity::ClusterKey& cluster, const dannan::identity::KeyCustody& custody,
       const ManifestOptions& options, ManifestSummary& summary)
      : _state(state),
        _options(options),
        _summary(summary),
        _domain_key(std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_v1(cluster))),
        _node_local(std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_node_local(custody))),
        _scheme(store::SealingScheme::v19(domain::domain_v1)) {}

  void run(const std::filesystem::path& manifest);

 private:
  struct Store {
    std::string label;
    std::string kind;
    std::filesystem::path root;
    struct Loose {
      Bytes key;
      ObjectId id;
      std::string rel;
      FileStat stat;
    };
    struct Pack {
      uint64_t sequence = 0;
      std::filesystem::path path;
      FileStat stat;
    };
    std::vector<Loose> loose;
    std::vector<Pack> packs;
  };

  void say(const std::string& line) const {
    if (_options.say) _options.say(line);
  }
  std::string store_order(const Store& store, char sub) const { return std::string("1") + sep + store.label + sep + sub; }

  void find_stores();
  void take_locks();
  Header make_header();
  void list(Store& store);
  void read_store(Store& store, ManifestFile& file);
  void read_loose(Store& store, ManifestFile& file);
  void read_packs(Store& store, ManifestFile& file, Replay& replay);
  void read_one_pack(const Store::Pack& pack, uint64_t from, ManifestFile& file, Replay& replay);
  void write_removed_and_accounting(Store& store, ManifestFile& file, const Replay& replay);
  void read_markers(ManifestFile& file);
  void read_ledgers(ManifestFile& file);
  void read_trie(ManifestFile& file, const std::string& order, const std::string& display, SectionKind kind, const std::filesystem::path& dir);
  void read_claim_frames(ManifestFile& file);
  void read_sightings(ManifestFile& file, const std::string& name);
  void read_others(ManifestFile& file);
  void read_metadata(ManifestFile& file);
  void write_not_checked(ManifestFile& file);
  uint8_t open_layout(const ObjectId& id, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext,
                      uint64_t plain_size) const;
  void note_unreadable(const std::string& what);
  void note_record(uint64_t bytes);
  void checkpoint_if_due(ManifestFile& file);
  void not_checked(const std::string& name, const std::string& why) {
    _not_checked[name] = why;
    say("NOT CHECKED " + name + ": " + why);
  }
  void category(const std::string& display, uint64_t count, const std::string& why = {}) {
    _summary.categories.push_back({display, count, why.empty(), why});
    _summary.items += count;
    say(display + ": sized " + std::to_string(count) + (why.empty() ? "" : ", NOT CHECKED: " + why));
  }
  const domain::StorageKey& ledger_key() const { return _layout == 18 ? *_domain_key : *_node_local; }

  const std::filesystem::path _state;
  const ManifestOptions& _options;
  ManifestSummary& _summary;
  const std::shared_ptr<const domain::StorageKey> _domain_key;
  const std::shared_ptr<const domain::StorageKey> _node_local;
  const store::SealingScheme _scheme;
  uint32_t _layout = 0;
  std::vector<Store> _stores;
  std::filesystem::path _control;
  ReadOnlyLocks _locks;
  std::optional<Resumed> _resumed;
  std::map<std::string, std::string> _not_checked;
  bool _proved = false;
  uint64_t _unreadable = 0;
  uint64_t _records = 0;
  uint64_t _since_checkpoint_records = 0;
  uint64_t _since_checkpoint_bytes = 0;
  uint64_t _total_bytes = 0;
  uint64_t _temporaries = 0;
  std::chrono::steady_clock::time_point _started = std::chrono::steady_clock::now();
  uint64_t _bytes_this_run = 0;
};

void Pass::find_stores() {
  std::error_code ec;
  if (!std::filesystem::is_directory(_state, ec)) throw MigrationRefused("state " + _state.string() + " is not a directory");
  const auto layout_marker = _state / "storage-layout";
  if (!std::filesystem::exists(layout_marker)) throw MigrationRefused("state " + _state.string() + " has no storage-layout marker: not a Dagda or Macha state");
  const auto layout = trimmed(read_text_or_empty(layout_marker));
  if (layout == state_layout_v18) {
    _layout = 18;
  } else if (layout == store::state_layout_marker(_scheme)) {
    _layout = 19;
  } else {
    throw MigrationRefused("state " + _state.string() + " is \"" + layout + "\": neither layout v18 nor this domain's v19, so nothing can be compared");
  }
  _summary.layout = _layout;
  for (const auto& backend : _options.backends) {
    if (!std::filesystem::is_directory(backend, ec)) throw MigrationRefused("backend " + backend.string() + " is not a directory");
    _stores.push_back({"backend:" + backend.lexically_normal().string(), "backend", backend, {}, {}});
  }
  _control = _options.control.empty() ? _state / "metadata-objects" : _options.control;
  if (std::filesystem::is_directory(_control, ec)) _stores.push_back({"control:" + _control.lexically_normal().string(), "control", _control, {}, {}});
  if (!_options.cache.empty()) {
    if (!std::filesystem::is_directory(_options.cache, ec)) throw MigrationRefused("cache " + _options.cache.string() + " is not a directory");
    _stores.push_back({"cache:" + _options.cache.lexically_normal().string(), "cache", _options.cache, {}, {}});
  }
  std::sort(_stores.begin(), _stores.end(), [](const Store& a, const Store& b) { return a.label < b.label; });
  for (size_t i = 1; i < _stores.size(); ++i)
    if (_stores[i - 1].label == _stores[i].label) throw MigrationRefused("store named twice: " + _stores[i].label);
}

void Pass::take_locks() {
  _locks.hold(_state, store::storage_lock_name);
  _locks.hold(_state, dannan_lock_name);
  for (const auto& store : _stores) _locks.hold(store.root, store::storage_lock_name);
  if (!_options.node_state.empty()) _locks.hold(_options.node_state, dannan_lock_name);
}

Header Pass::make_header() {
  Header header;
  header.layout = _layout;
  header.domain = domain::to_string(_scheme.domain);
  header.node = _options.node ? dannan::identity::to_string(*_options.node) : std::string();
  header.key_check = store::backend_key_check(*_domain_key, _scheme.domain, NodeId{});
  for (const auto& store : _stores) {
    header.stores.push_back(store.label);
    std::error_code ec;
    const auto accounting = store.root / ".macha.accounting";
    header.accounting_digests.push_back(std::filesystem::exists(accounting, ec) ? dannan::identity::hex(sha256(read_file(accounting))) : std::string("none"));
  }
  return header;
}

void Pass::list(Store& store) {
  std::error_code ec;
  const auto objects = store.root / "objects";
  if (std::filesystem::is_directory(objects, ec)) {
    for (auto it = std::filesystem::recursive_directory_iterator(objects, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const auto id = object_id_from_file_name(it->path().filename().string());
      if (!id) continue;
      const auto stat = stat_of(it->path());
      Store::Loose loose{{}, *id, {}, stat};
      loose.key.assign(id->bytes.begin(), id->bytes.end());
      if (it->path() != loose_path(objects, *id)) {
        loose.rel = it->path().lexically_relative(store.root).generic_string();
        loose.key.insert(loose.key.end(), loose.rel.begin(), loose.rel.end());
      }
      store.loose.push_back(std::move(loose));
    }
    if (ec) throw std::runtime_error("cannot walk " + objects.string() + ": " + ec.message());
  }
  std::sort(store.loose.begin(), store.loose.end(), [](const Store::Loose& a, const Store::Loose& b) { return a.key < b.key; });
  for (const auto& loose : store.loose) _total_bytes += loose.stat.size;
  const auto packs = store.root / "packs";
  if (std::filesystem::is_directory(packs, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(packs, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      if (const auto sequence = pack_sequence(entry.path().filename().string())) store.packs.push_back({*sequence, entry.path(), stat_of(entry.path())});
    }
    if (ec) throw std::runtime_error("cannot list " + packs.string() + ": " + ec.message());
  }
  std::sort(store.packs.begin(), store.packs.end(), [](const Store::Pack& a, const Store::Pack& b) { return a.sequence < b.sequence; });
  for (const auto& pack : store.packs) _total_bytes += pack.stat.size;
}

// The layout the record opens under, the pass's own tried first so a record is
// opened once; a record of the other is what a half-migrated state holds, and
// the comparison names it.
uint8_t Pass::open_layout(const ObjectId& id, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext,
                          uint64_t plain_size) const {
  const auto try_v18 = [&] {
    try {
      (void)open_object_v18(*_domain_key, id, nonce, tag, ciphertext, plain_size);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };
  const auto try_v19 = [&] {
    try {
      (void)store::open_object(*_domain_key, _scheme, id, nonce, tag, ciphertext, plain_size);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };
  if (_layout == 18) {
    if (try_v18()) return record_v18;
    if (try_v19()) return record_v19;
  } else {
    if (try_v19()) return record_v19;
    if (try_v18()) return record_v18;
  }
  return record_unreadable;
}

void Pass::note_unreadable(const std::string& what) {
  ++_unreadable;
  say("unreadable: " + what);
  if (!_proved && _unreadable >= probe_limit)
    throw MigrationRefused("the key opens none of the first " + std::to_string(probe_limit) + " records found, under either layout (" + what +
                           "): wrong cluster key, or not a Dagda disk");
}

void Pass::note_record(uint64_t bytes) {
  ++_records;
  ++_since_checkpoint_records;
  _since_checkpoint_bytes += bytes;
  _bytes_this_run += bytes;
  _summary.bytes_read += bytes;
  if (!_options.say || !_options.progress_every || _records % _options.progress_every != 0) return;
  constexpr double mib = 1024.0 * 1024.0;
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - _started).count();
  const auto rate = seconds > 0 ? static_cast<double>(_bytes_this_run) / seconds : 0.0;
  const auto left = _total_bytes > _summary.bytes_read ? static_cast<double>(_total_bytes - _summary.bytes_read) : 0.0;
  std::ostringstream out;
  out << "progress: " << _records << " records, " << static_cast<uint64_t>(static_cast<double>(_summary.bytes_read) / mib) << " of "
      << static_cast<uint64_t>(static_cast<double>(_total_bytes) / mib) << " MiB, " << static_cast<uint64_t>(rate / mib) << " MiB/s, about "
      << static_cast<uint64_t>(rate > 0 ? left / rate / 60 : 0) << " min left";
  say(out.str());
}

void Pass::checkpoint_if_due(ManifestFile& file) {
  if (_since_checkpoint_records < _options.checkpoint_records && _since_checkpoint_bytes < _options.checkpoint_bytes) return;
  _since_checkpoint_records = 0;
  _since_checkpoint_bytes = 0;
  file.flush(true);
  if (_options.after_checkpoint) _options.after_checkpoint();
}

void Pass::read_loose(Store& store, ManifestFile& file) {
  const auto order = store_order(store, '1');
  const auto display = "objects " + store.label + " loose";
  // A record the partial holds is not read again, but what it says of the file
  // must still be so: a state that changed since is refused.
  const std::vector<std::pair<Bytes, Bytes>>* recorded = nullptr;
  if (_resumed) {
    const auto found = _resumed->recorded.find(order);
    if (found != _resumed->recorded.end()) recorded = &found->second;
  }
  const bool closed = file.closed(order);
  const auto changed = [&](const std::string& what) {
    return MigrationRefused("the state has changed since the pass began (" + what + "): start the pass again");
  };
  size_t held = 0;
  if (!closed) file.begin(order, display, SectionKind::loose);
  for (const auto& loose : store.loose) {
    if (recorded && held < recorded->size()) {
      const auto& [key, encoded] = (*recorded)[held];
      if (key != loose.key) throw changed(store.label + " loose object " + store::to_string(loose.id));
      const auto value = decode_loose_value(encoded);
      if (value.file_size != loose.stat.size || value.mtime_ns != loose.stat.mtime_ns)
        throw changed(store.label + " loose object " + store::to_string(loose.id));
      ++held;
      continue;
    }
    if (closed) throw changed(store.label + " has a loose object it did not have: " + store::to_string(loose.id));
    const auto path = loose.rel.empty() ? loose_path(store.root / "objects", loose.id) : store.root / loose.rel;
    const auto bytes = read_file(path);
    LooseValue value;
    value.file_size = bytes.size();
    value.mtime_ns = loose.stat.mtime_ns;
    if (bytes.empty()) {
      value.layout = record_empty;
    } else {
      try {
        const auto decoded = decode_loose(bytes);
        value.plain_size = decoded.plain_size;
        value.nonce = decoded.nonce;
        value.tag = decoded.tag;
        value.layout = open_layout(loose.id, decoded.nonce, decoded.tag, decoded.ciphertext, decoded.plain_size);
      } catch (const std::exception&) {
        value.layout = record_unreadable;
      }
      if (value.layout == record_unreadable)
        note_unreadable(path.string());
      else
        _proved = true;
    }
    file.item(loose.key, encode(value));
    note_record(bytes.size());
    checkpoint_if_due(file);
  }
  if (recorded && held != recorded->size()) throw changed(store.label + " has lost a loose object");
  if (!closed) {
    file.end();
  }
  category(display, recorded && closed ? recorded->size() : store.loose.size());
}

void Pass::read_one_pack(const Store::Pack& pack, uint64_t from, ManifestFile& file, Replay& replay) {
  const int fd = ::open(pack.path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot open " + pack.path.string() + ": " + std::strerror(errno));
  const struct Closing {
    int fd;
    ~Closing() { ::close(fd); }
  } closing{fd};
  const uint64_t file_size = pack.stat.size;
  uint64_t offset = from;
  const auto span = [&](uint8_t kind, uint64_t length) {
    PackValue value;
    value.span = kind;
    value.length = length;
    value.digest = digest_of_span(fd, offset, length, pack.path);
    file.item(pack_key(pack.sequence, offset), encode(value));
    note_record(length);
    if (kind == span_tail) _summary.tail_bytes += length;
    offset += length;
  };
  while (offset < file_size) {
    if (file_size - offset < pack_header_size) {
      span(span_tail, file_size - offset);
      break;
    }
    const auto bytes = read_at(fd, pack_header_size, offset);
    if (!bytes) throw std::runtime_error("cannot read " + pack.path.string());
    const auto header = decode_pack_header(*bytes);
    if (!header) {
      // As the store's recovery: a torn tail, or damage with records after it.
      const auto next = find_next_pack_header(fd, offset + 1, file_size);
      span(next ? span_damaged : span_tail, (next ? *next : file_size) - offset);
      if (next) note_unreadable(pack.path.string() + " damaged span");
      checkpoint_if_due(file);
      continue;
    }
    const uint64_t total = pack_header_size + header->payload_size;
    if (total > file_size - offset) {
      span(span_tail, file_size - offset);
      break;
    }
    PackValue value;
    value.header = *header;
    if (header->type == pack_put) {
      const auto payload = read_at(fd, static_cast<size_t>(header->payload_size), offset + pack_header_size);
      if (!payload) throw std::runtime_error("cannot read " + pack.path.string());
      value.layout = open_layout(header->id, header->nonce, header->tag, *payload, header->plain_size);
      if (value.layout == record_unreadable)
        note_unreadable(pack.path.string() + " record " + store::to_string(header->id) + " at " + std::to_string(offset));
      else
        _proved = true;
      replay.last[header->id] = std::nullopt;
    } else if (header->type == pack_remove) {
      replay.last[header->id] = header->touched_ms;
    }
    file.item(pack_key(pack.sequence, offset), encode(value));
    offset += total;
    note_record(total);
    checkpoint_if_due(file);
  }
}

void Pass::read_packs(Store& store, ManifestFile& file, Replay& replay) {
  const auto files_order = store_order(store, '2');
  const auto files_display = "objects " + store.label + " pack files";
  const auto changed = [&](const std::string& what) {
    return MigrationRefused("the state has changed since the pass began (" + what + "): start the pass again");
  };
  const auto encode_pack = [](const Store::Pack& pack) {
    dannan::net::Writer writer;
    writer.u64(pack.stat.size);
    writer.u64(static_cast<uint64_t>(pack.stat.mtime_ns));
    return writer.take();
  };
  if (file.closed(files_order)) {
    const auto& recorded = _resumed->recorded[files_order];
    if (recorded.size() != store.packs.size()) throw changed(store.label + " has another number of packs");
    for (size_t i = 0; i < recorded.size(); ++i)
      if (recorded[i].first != number_key(store.packs[i].sequence) || recorded[i].second != encode_pack(store.packs[i]))
        throw changed(store.packs[i].path.string());
    category(files_display, recorded.size());
  } else {
    file.begin(files_order, files_display, SectionKind::pack_files);
    for (const auto& pack : store.packs) file.item(number_key(pack.sequence), encode_pack(pack));
    category(files_display, file.end());
  }

  const auto order = store_order(store, '3');
  const auto display = "objects " + store.label + " pack records";
  if (file.closed(order)) return;
  file.begin(order, display, SectionKind::pack_records);
  for (const auto& pack : store.packs) {
    uint64_t from = 0;
    if (replay.next_position) {
      if (pack.sequence < replay.next_position->first) continue;
      if (pack.sequence == replay.next_position->first) from = replay.next_position->second;
    }
    read_one_pack(pack, from, file, replay);
  }
  file.flush(true);
  category(display, file.end());
}

void Pass::write_removed_and_accounting(Store& store, ManifestFile& file, const Replay& replay) {
  std::set<ObjectId> loose_ids;
  for (const auto& loose : store.loose)
    if (loose.stat.size) loose_ids.insert(loose.id);
  const auto order = store_order(store, '4');
  const auto display = "removed " + store.label;
  // Flushed before, so a stopped pass goes on from a store's sealed records.
  file.flush(true);
  file.begin(order, display, SectionKind::removed);
  uint64_t live = loose_ids.size();
  for (const auto& [id, tombstone] : replay.last) {
    if (!tombstone) {
      if (!loose_ids.contains(id)) ++live;
      continue;
    }
    if (loose_ids.contains(id)) continue;
    file.item(Bytes(id.bytes.begin(), id.bytes.end()), number_key(*tombstone));
  }
  category(display, file.end());
  say("objects " + store.label + ": " + std::to_string(live) + " distinct live");

  const auto accounting_order = store_order(store, '5');
  const auto accounting_display = "accounting " + store.label;
  file.begin(accounting_order, accounting_display, SectionKind::accounting);
  const auto path = store.root / ".macha.accounting";
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    const auto bytes = read_file(path);
    AccountingValue value;
    value.digest = sha256(bytes);
    for (size_t slot = 0; slot < 2 && (slot + 1) * accounting_slot_size <= bytes.size(); ++slot) {
      const auto decoded = decode_accounting(std::span<const uint8_t>(bytes).subspan(slot * accounting_slot_size, accounting_slot_size));
      if (decoded && (!value.decoded || decoded->sequence > value.sequence)) {
        value.decoded = true;
        value.sequence = decoded->sequence;
        value.used = decoded->used;
        value.operation = decoded->operation;
      }
    }
    file.item(bytes_of("accounting"), encode(value));
  }
  category(accounting_display, file.end());
}

void Pass::read_store(Store& store, ManifestFile& file) {
  const auto last_order = store_order(store, '5');
  if (file.closed(last_order)) {
    // Done before the pass stopped; what it recorded must still be so.
    read_loose(store, file);
    Replay none;
    read_packs(store, file, none);
    return;
  }
  say(store.kind + " " + store.root.string());
  Replay replay;
  if (_resumed) {
    const auto found = _resumed->replays.find(std::string("1") + sep + store.label);
    if (found != _resumed->replays.end()) replay = found->second;
  }
  read_loose(store, file);
  read_packs(store, file, replay);
  write_removed_and_accounting(store, file, replay);
}

// What a marker says. A marker the key does not prove reads as neither layout.
MarkerValue marker_value(const std::string& text, const domain::StorageKey& key, const domain::DomainId& domain) {
  MarkerValue value;
  value.text = text;
  if (const auto v18 = parse_backend_marker_v18(text)) {
    value.layout = 18;
    value.node = dannan::identity::to_string(v18->node);
    value.token = dannan::identity::to_string(v18->token);
    return value;
  }
  std::vector<std::string> fields;
  std::istringstream in(text);
  for (std::string field; in >> field;) fields.push_back(field);
  if (fields.size() != 5 || fields[0] != store::backend_marker_v19 || fields[3] != domain::to_string(domain)) return value;
  const auto token = NodeId::parse(fields[2]);
  if (!token || !NodeId::parse(fields[1]) || fields[4] != store::backend_key_check(key, domain, *token)) return value;
  value.layout = 19;
  value.node = fields[1];
  value.token = fields[2];
  return value;
}

void Pass::read_markers(ManifestFile& file) {
  file.begin("2", "markers", SectionKind::markers);
  std::set<std::string> tokens;
  std::map<Bytes, Bytes> items;
  const auto layout_text = trimmed(read_text_or_empty(_state / "storage-layout"));
  MarkerValue layout;
  layout.layout = static_cast<uint8_t>(_layout);
  layout.text = layout_text;
  items[bytes_of("state storage-layout")] = encode(layout);
  std::error_code ec;
  for (const auto& store : _stores) {
    if (store.kind != "backend") continue;
    const auto path = store.root / store::backend_marker_name;
    if (!std::filesystem::exists(path, ec)) {
      not_checked("markers " + path.string(), "the backend has no marker, so its disk cannot be told from any other");
      continue;
    }
    const auto value = marker_value(trimmed(read_text_or_empty(path)), *_domain_key, _scheme.domain);
    if (!value.token.empty()) tokens.insert(value.token);
    if (_options.node && !value.node.empty() && value.node != dannan::identity::to_string(*_options.node))
      not_checked("markers " + path.string(), "belongs to node " + value.node + ", not this node");
    items[bytes_of("backend " + store.label)] = encode(value);
  }
  const auto identities = _state / "backend-identities";
  if (std::filesystem::is_directory(identities, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(identities, ec)) {
      if (!entry.is_regular_file(ec) || entry.path().extension() != ".id") continue;
      const auto value = marker_value(trimmed(read_text_or_empty(entry.path())), *_domain_key, _scheme.domain);
      // A backend the node knows and the pass was not told of holds data no
      // pass reads: nothing may pass over it.
      if (!value.token.empty() && !tokens.contains(value.token))
        not_checked("backend with token " + value.token, "the mirror " + entry.path().filename().string() + " names it and no --backend does");
      items[bytes_of("mirror " + entry.path().filename().string())] = encode(value);
    }
    if (ec) throw std::runtime_error("cannot list " + identities.string() + ": " + ec.message());
  }
  for (auto& [key, value] : items) file.item(key, std::move(value));
  category("markers", file.end());

  file.begin("3", "retention marker", SectionKind::bytes);
  const auto retention_marker = _state / "retention" / "ledger";
  if (std::filesystem::exists(retention_marker, ec)) file.item(bytes_of("retention/ledger"), read_file(retention_marker));
  category("retention marker", file.end());
}

void Pass::read_trie(ManifestFile& file, const std::string& order, const std::string& display, SectionKind kind, const std::filesystem::path& dir) {
  file.begin(order, display, kind);
  try {
    const auto walk = ledger::ObjectTrie::read_only(dir, ledger_key(), [&](const ObjectId& id, std::span<const uint8_t> value) {
      file.item(Bytes(id.bytes.begin(), id.bytes.end()), Bytes(value.begin(), value.end()));
    });
    _summary.tail_bytes += walk.journal_damaged ? 0 : walk.journal_tail_bytes;
    if (walk.journal_damaged) not_checked(display, "its journal holds a frame that does not open under the expected key");
  } catch (const std::exception& error) {
    not_checked(display, std::string("does not read under the ") + (_layout == 18 ? "domain" : "node-local") + " key: " + error.what());
  }
  category(display, file.end());
}

void Pass::read_claim_frames(ManifestFile& file) {
  const std::string display = "claims retention/claims.log";
  file.begin("5\x01z", display, SectionKind::claim_frames);
  const auto path = _state / "retention" / "claims.log";
  try {
    uint64_t index = 0;
    const auto scan =
        ledger::SealedJournal::read_only(path, ledger_key(), ledger::retention_format::journal_aad, max_read_frame,
                                         [&](std::span<const uint8_t> plain) { file.item(number_key(index++), Bytes(plain.begin(), plain.end())); });
    if (scan.stopped_at_a_whole_frame)
      not_checked(display, "frame " + std::to_string(scan.frames) + " does not open under the " + (_layout == 18 ? "domain" : "node-local") + " key");
    else
      _summary.tail_bytes += scan.tail_bytes;
  } catch (const std::exception& error) {
    not_checked(display, error.what());
  }
  category(display, file.end());
}

void Pass::read_sightings(ManifestFile& file, const std::string& name) {
  const auto display = "sightings " + name;
  file.begin("7\x01" + name, display, SectionKind::sightings);
  const auto path = _state / "retention" / ("unreferenced-" + name + ".bin");
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    try {
      const auto bytes = read_file(path);
      std::map<ObjectId, uint64_t> table;
      if (!bytes.empty()) table = _layout == 18 ? ledger::decode_unreferenced_since(bytes) : ledger::open_unreferenced_since(*_node_local, bytes);
      for (const auto& [id, first] : table) file.item(Bytes(id.bytes.begin(), id.bytes.end()), number_key(first));
    } catch (const std::exception& error) {
      not_checked(display, std::string("does not read as this layout's table: ") + error.what());
    }
  }
  category(display, file.end());
}

void Pass::read_ledgers(ManifestFile& file) {
  const auto rel = [&](const std::filesystem::path& dir) { return dir.lexically_relative(_state).generic_string(); };
  // The fixed names always, so that a ledger not there says ZERO.
  std::set<std::filesystem::path> held{_state / "ledger" / "control"};
  for (const auto& dir : known_ledgers(_state).tries)
    if (dir.parent_path() == _state / "ledger" && dir.filename().string().starts_with("data-")) held.insert(dir);
  std::error_code ec;
  for (const auto& dir : held) {
    const auto name = rel(dir);
    read_trie(file, "4\x01" + name + "\x01" + "1", "held " + name, SectionKind::held, dir);
    file.begin("4\x01" + name + "\x01" + "2", "held " + name + " markers", SectionKind::bytes);
    for (const char* marker : {"seeded", "verify-next"})
      if (std::filesystem::exists(dir / marker, ec)) file.item(bytes_of(marker), read_file(dir / marker));
    category("held " + name + " markers", file.end());
  }
  for (const auto* name : {"ledger-control", "ledger-data"})
    read_trie(file, std::string("5\x01") + name, std::string("claims retention/") + name, SectionKind::claims, _state / "retention" / name);
  read_claim_frames(file);
  read_trie(file,
            "6\x01"
            "1",
            "reference counts ledger/referenced/extents", SectionKind::counts, _state / "ledger" / "referenced" / "extents");
  read_trie(file,
            "6\x01"
            "2",
            "reference counts ledger/referenced/nodes", SectionKind::counts, _state / "ledger" / "referenced" / "nodes");
  file.begin(
      "6\x01"
      "3",
      "reference counts ledger/referenced/state", SectionKind::bytes);
  if (std::filesystem::exists(_state / "ledger" / "referenced" / "state", ec))
    file.item(bytes_of("state"), read_file(_state / "ledger" / "referenced" / "state"));
  category("reference counts ledger/referenced/state", file.end());
  read_sightings(file, "control");
  read_sightings(file, "data");
}

void Pass::read_others(ManifestFile& file) {
  struct Root {
    std::string label;
    std::filesystem::path path;
    const Store* store;
  };
  std::vector<Root> roots;
  for (const auto& store : _stores) roots.push_back({store.label, store.root, &store});
  roots.push_back({"state", _state, nullptr});
  std::sort(roots.begin(), roots.end(), [](const Root& a, const Root& b) { return a.label < b.label; });
  std::vector<std::filesystem::path> nested;
  for (const auto& store : _stores) nested.push_back(normal(store.root));
  const auto known_tries = known_ledgers(_state);
  std::set<std::filesystem::path> trie_dirs(known_tries.tries.begin(), known_tries.tries.end());
  trie_dirs.insert(_state / "ledger" / "control");

  const auto covered_in_store = [](const std::filesystem::path& rel) {
    const auto text = rel.generic_string();
    if (text == ".macha.backend" || text == ".macha.accounting" || text == store::storage_lock_name) return true;
    if (text.starts_with("objects/") && store::local_format::object_id_from_file_name(rel.filename().string())) return true;
    if (text.starts_with("packs/") && rel.parent_path() == "packs" && pack_sequence(rel.filename().string())) return true;
    return false;
  };
  const auto covered_in_state = [&](const std::filesystem::path& rel, const std::filesystem::path& full) {
    const auto text = rel.generic_string();
    if (text == "storage-layout" || text == store::storage_lock_name || text == dannan_lock_name) return true;
    if (text == "retention/ledger" || text == "retention/claims.log" || text == "retention/unreferenced-data.bin" ||
        text == "retention/unreferenced-control.bin" || text == "ledger/referenced/state")
      return true;
    if (rel.parent_path() == "backend-identities" && rel.extension() == ".id") return true;
    if (trie_dirs.contains(full.parent_path())) {
      const auto name = rel.filename().string();
      if (name == "root" || name == "journal.log" || (name.starts_with("nodes-") && name.ends_with(".bin"))) return true;
      if (name == "seeded" || name == "verify-next") return rel.parent_path().parent_path() == "ledger";
    }
    return false;
  };

  for (const auto& root : roots) {
    const auto display = "other files " + root.label;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root.path, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
      const auto full = it->path();
      if (!root.store) {
        // Another store inside the state is its own root's.
        if (std::find(nested.begin(), nested.end(), normal(full)) != nested.end()) {
          it.disable_recursion_pending();
          continue;
        }
      }
      if (it->is_directory(ec) && !it->is_symlink(ec)) continue;
      const auto rel = full.lexically_relative(root.path);
      if (migration_temporary(full.filename().string())) {
        ++_temporaries;
        say("temporary, not compared: " + full.string());
        continue;
      }
      if (root.store ? covered_in_store(rel) : covered_in_state(rel, full)) continue;
      if (_options.metadata && _options.metadata->covers(root.path, rel)) continue;
      if (!it->is_regular_file(ec) || it->is_symlink(ec)) {
        not_checked(display + " " + rel.generic_string(), "not a regular file");
        continue;
      }
      files.push_back(rel);
    }
    if (ec) throw std::runtime_error("cannot walk " + root.path.string() + ": " + ec.message());
    std::sort(files.begin(), files.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) { return a.generic_string() < b.generic_string(); });
    file.begin("8\x01" + root.label, display, SectionKind::other);
    for (const auto& rel : files) {
      const auto bytes = read_file(root.path / rel);
      OtherValue value;
      value.size = bytes.size();
      value.digest = sha256(bytes);
      file.item(bytes_of(rel.generic_string()), encode(value));
    }
    category(display, file.end());
  }
}

void Pass::read_metadata(ManifestFile& file) {
  const std::vector<std::string> kinds{"certificates", "heads", "history"};
  if (!_options.metadata) {
    const std::string why = "no metadata reader in this build: heads session S7 adds it";
    for (const auto& kind : kinds) {
      file.begin("9\x01" + kind, "metadata " + kind, SectionKind::metadata, why);
      file.end();
      category("metadata " + kind, 0, why);
      _summary.not_checked.push_back("metadata " + kind + ": " + why);
    }
    return;
  }
  MetadataManifestInput input;
  input.state = _state;
  input.cache = _options.cache;
  input.domain_key = _domain_key;
  input.node_local_key = _node_local;
  input.layout = _layout;
  size_t at = 0;
  bool open = false;
  const auto close = [&] {
    if (open) category("metadata " + kinds[at], file.end());
    open = false;
  };
  const auto open_kind = [&](size_t index) {
    file.begin("9\x01" + kinds[index], "metadata " + kinds[index], SectionKind::metadata);
    open = true;
    at = index;
  };
  open_kind(0);
  _options.metadata->read(input, [&](const MetadataFact& fact) {
    const auto found = std::find(kinds.begin(), kinds.end(), fact.kind);
    if (found == kinds.end()) throw std::runtime_error("metadata reader emitted a fact of unknown kind " + fact.kind);
    const auto index = static_cast<size_t>(found - kinds.begin());
    if (index < at) throw std::runtime_error("metadata reader emitted its facts out of order");
    while (at < index) {
      close();
      open_kind(at + 1);
    }
    file.item(bytes_of(fact.name), fact.value);
  });
  while (at + 1 < kinds.size()) {
    close();
    open_kind(at + 1);
  }
  close();
}

void Pass::write_not_checked(ManifestFile& file) {
  file.begin("a", "not checked", SectionKind::not_checked);
  for (const auto& [name, why] : _not_checked) {
    file.item(bytes_of(name), bytes_of(why));
    _summary.not_checked.push_back(name + ": " + why);
  }
  file.end();
}

void Pass::run(const std::filesystem::path& manifest) {
  find_stores();
  take_locks();

  // Nothing of the pass is ever inside what it reads, and an earlier manifest
  // is never overwritten.
  const auto where = std::filesystem::weakly_canonical(manifest);
  const auto inside = [&](const std::filesystem::path& root) {
    const auto canonical = std::filesystem::weakly_canonical(root);
    const auto relative = where.lexically_relative(canonical);
    return !relative.empty() && *relative.begin() != "..";
  };
  if (inside(_state)) throw MigrationRefused("manifest " + manifest.string() + " is inside the state: a pass writes nothing there");
  for (const auto& store : _stores)
    if (inside(store.root)) throw MigrationRefused("manifest " + manifest.string() + " is inside " + store.root.string() + ": a pass writes nothing there");
  std::error_code ec;
  if (std::filesystem::exists(manifest, ec)) throw MigrationRefused("manifest " + manifest.string() + " exists: a pass never overwrites one");
  const auto partial = std::filesystem::path(manifest.string() + ".partial");
  const bool had_partial = std::filesystem::exists(partial, ec);
  if (had_partial && !_options.resume)
    throw MigrationRefused(partial.string() + " exists from a pass that stopped: give --resume to go on from it, or remove it to start again");

  const auto header = make_header();
  if (had_partial) _resumed = read_partial(partial, header);
  if (had_partial && !_resumed) std::filesystem::remove(partial);
  if (_resumed) {
    _proved = _resumed->proved;
    _unreadable = _resumed->unreadable;
    _summary.resumed_items = _resumed->items;
    say("resuming from " + partial.string() + ": " + std::to_string(_resumed->items) + " items held");
  }

  for (auto& store : _stores) list(store);
  try {
    ManifestFile file(partial, header, _resumed ? &*_resumed : nullptr);
    for (auto& store : _stores) read_store(store, file);
    file.flush(true);
    read_markers(file);
    read_ledgers(file);
    read_others(file);
    read_metadata(file);
    write_not_checked(file);
    file.finish();
  } catch (const MigrationRefused&) {
    // A refusal is not a pass that stopped: nothing here to go on from.
    if (!_resumed) std::filesystem::remove(partial, ec);
    throw;
  }

  _locks.check_none_appeared();
  if (_temporaries) say("temporaries not compared: " + std::to_string(_temporaries));
  std::filesystem::rename(partial, manifest);
  const int dir = ::open(where.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir >= 0) {
    (void)::fsync(dir);
    ::close(dir);
  }
  _summary.manifest_bytes = std::filesystem::file_size(manifest);
}

}  // namespace

std::string ManifestSummary::summary() const {
  std::ostringstream out;
  out << "manifest of layout v" << layout << ": " << manifest_bytes << " bytes, " << items << " items";
  if (resumed_items) out << ", " << resumed_items << " of them from the pass that stopped";
  out << "; " << bytes_read << " bytes read this run, " << tail_bytes << " tail bytes the store's recovery drops\n";
  for (const auto& category : categories) {
    out << category.name << ": ";
    if (!category.checked)
      out << "NOT CHECKED: " << category.why;
    else if (category.items == 0)
      out << "ZERO";
    else
      out << "sized " << category.items;
    out << "\n";
  }
  for (const auto& what : not_checked) out << "NOT CHECKED " << what << "\n";
  return out.str();
}

ManifestSummary write_manifest(const std::filesystem::path& manifest, const std::filesystem::path& state, const dannan::identity::ClusterKey& cluster,
                               const dannan::identity::KeyCustody& custody, const ManifestOptions& options) {
  ManifestSummary summary;
  Pass pass(state, cluster, custody, options, summary);
  pass.run(manifest);
  return summary;
}

}  // namespace dagda::migrate
