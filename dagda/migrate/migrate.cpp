//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/migrate/migrate.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>

#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/sealed_journal.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/migrate/legacy_v18.h"
#include "dagda/store/local_format.h"
#include "dagda/store/local_store.h"
#include "dagda/store/state_layout.h"
#include "dagda/store/storage_pool.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"
#include "dannan/status/log.h"

namespace dagda::migrate {
namespace {

using namespace store::local_format;
using dannan::identity::NodeId;

// How many records may fail to open before the key is refused, when none
// has opened yet: a damaged record is not a wrong key.
constexpr uint64_t probe_limit = 8;
// Journals are read up to an older writer's limit.
constexpr uint32_t max_read_frame = 64U * 1024U * 1024U;

// The ledger loggers write through the run's progress line.
class SayingSink final : public dannan::status::LogSink {
 public:
  explicit SayingSink(std::function<void(std::string_view)> say) : _say(std::move(say)) {}
  bool enabled(dannan::status::LogLevel level) const noexcept override { return _say && level >= dannan::status::LogLevel::info; }
  void write(dannan::status::LogLevel, std::string_view scope, std::string_view message) override {
    if (_say) _say(std::string(scope) + ": " + std::string(message));
  }

 private:
  std::function<void(std::string_view)> _say;
};

void say(const MigrationOptions& options, const std::string& line) {
  if (options.say) options.say(line);
}

void step(const MigrationOptions& options, const MigrationReport& report) {
  if (options.after_step) options.after_step(report);
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  std::string text((std::istreambuf_iterator<char>(in)), {});
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  return text;
}

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  return Bytes(std::istreambuf_iterator<char>(in), {});
}

void write_at(int fd, std::span<const uint8_t> bytes, uint64_t offset, const std::filesystem::path& path) {
  size_t done = 0;
  while (done < bytes.size()) {
    const auto n = ::pwrite(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("cannot write " + path.string() + ": " + std::strerror(errno));
    done += static_cast<size_t>(n);
  }
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

void sync_fd(int fd, const std::filesystem::path& path) {
  int rc;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) throw std::runtime_error("cannot sync " + path.string() + ": " + std::strerror(errno));
}

class Fd {
 public:
  Fd(const std::filesystem::path& path, int flags) : _fd(::open(path.c_str(), flags | O_CLOEXEC)) {
    if (_fd < 0) throw std::runtime_error("cannot open " + path.string() + ": " + std::strerror(errno));
  }
  ~Fd() { ::close(_fd); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const noexcept { return _fd; }

 private:
  int _fd;
};

// A record that opens under neither layout: left as it is, listed; the key
// is refused once enough have failed with none opened.
void unreadable(MigrationReport& report, bool proved, uint64_t& counter, const std::string& what) {
  ++counter;
  report.unreadable.push_back(what);
  if (!proved && report.loose_unreadable + report.pack_records_unreadable >= probe_limit)
    throw MigrationRefused("the key opens none of the first " + std::to_string(probe_limit) + " records found, under either layout (" + what +
                           "): wrong cluster key, or not a Dagda disk");
}

// ---- ledgers ------------------------------------------------------------------

enum class Ledger { skipped, migrated, already, empty };

Ledger migrate_journal(const std::filesystem::path& path, std::array<uint8_t, 8> aad, const domain::StorageKey& from, const domain::StorageKey& to,
                       MigrationReport& report, const MigrationOptions& options) {
  const auto under_to = ledger::SealedJournal::probe(path, to, aad, max_read_frame);
  if (!under_to) return Ledger::empty;
  if (*under_to) return Ledger::already;
  if (ledger::SealedJournal::probe(path, from, aad, max_read_frame) != std::optional<bool>(true))
    throw MigrationRefused("ledger " + path.string() + " opens under neither the node-local key nor the domain key: damaged, or another node's");
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  report.bytes_read += ec ? 0 : size;
  if (options.dry_run) return Ledger::migrated;
  const auto done = ledger::SealedJournal::rekey(path, from, to, aad, max_read_frame);
  report.ledger_tail_bytes_dropped += done.dropped_bytes;
  report.bytes_written += ec ? 0 : size - done.dropped_bytes;
  return Ledger::migrated;
}

Ledger migrate_trie(const std::filesystem::path& dir, std::shared_ptr<const domain::StorageKey> from, std::shared_ptr<const domain::StorageKey> to,
                    MigrationReport& report, const MigrationOptions& options) {
  const auto under_to = ledger::ObjectTrie::probe(dir, *to);
  if (!under_to) return Ledger::empty;
  if (*under_to) return Ledger::already;
  if (ledger::ObjectTrie::probe(dir, *from) != std::optional<bool>(true))
    throw MigrationRefused("ledger " + dir.string() + " opens under neither the node-local key nor the domain key: damaged, or another node's");
  uint64_t before = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    if (entry.is_regular_file()) before += entry.file_size(ec);
  report.bytes_read += before;
  if (options.dry_run) return Ledger::migrated;
  ledger::ObjectTrie trie(dir, std::move(from), {}, dannan::status::ScopedLogger(std::make_shared<SayingSink>(options.say), "migrate.trie"));
  trie.rekey(std::move(to));
  report.bytes_written += trie.stats().node_file_bytes;
  return Ledger::migrated;
}

// A sightings file (`retention/unreferenced-*.bin`): v18 wrote the table in
// the clear; v19 seals it under the node-local key. Rewritten whole through
// a sibling and a rename, as a journal is.
Ledger migrate_sightings(const std::filesystem::path& path, const domain::StorageKey& to, MigrationReport& report, const MigrationOptions& options) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Ledger::empty;
  const store::Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.empty()) return Ledger::empty;
  try {
    (void)ledger::open_unreferenced_since(to, bytes);
    return Ledger::already;
  } catch (const std::exception&) {
  }
  std::map<store::ObjectId, uint64_t> table;
  try {
    table = ledger::decode_unreferenced_since(bytes);
  } catch (const std::exception&) {
    throw MigrationRefused("sightings file " + path.string() + " is neither v18's table nor sealed under the node-local key: damaged, or another node's");
  }
  report.bytes_read += bytes.size();
  if (options.dry_run) return Ledger::migrated;
  const auto sealed = ledger::seal_unreferenced_since(to, table);
  dannan::identity::durable_replace_file(path, std::string_view(reinterpret_cast<const char*>(sealed.data()), sealed.size()));
  report.bytes_written += sealed.size();
  return Ledger::migrated;
}

void count(Ledger outcome, MigrationReport& report) {
  switch (outcome) {
    case Ledger::migrated:
      ++report.ledgers_migrated;
      break;
    case Ledger::already:
      ++report.ledgers_already;
      break;
    case Ledger::empty:
      ++report.ledgers_empty;
      break;
    case Ledger::skipped:
      break;
  }
}

bool is_trie_dir(const std::filesystem::path& dir) { return std::filesystem::exists(dir / "root") || std::filesystem::exists(dir / "journal.log"); }

bool under(const std::filesystem::path& path, const std::filesystem::path& root) {
  const auto p = path.lexically_normal();
  const auto r = root.lexically_normal();
  auto pi = p.begin();
  for (auto ri = r.begin(); ri != r.end(); ++ri, ++pi)
    if (pi == p.end() || *pi != *ri) return false;
  return true;
}

void migrate_ledgers(const std::filesystem::path& state, const std::vector<std::filesystem::path>& skip, std::shared_ptr<const domain::StorageKey> from,
                     std::shared_ptr<const domain::StorageKey> to, MigrationReport& report, const MigrationOptions& options) {
  std::vector<std::filesystem::path> tries;
  std::vector<std::filesystem::path> journals;
  std::vector<std::filesystem::path> sightings;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(state, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    const auto& path = it->path();
    if (std::any_of(skip.begin(), skip.end(), [&](const auto& s) { return under(path, s); })) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_directory()) {
      if (is_trie_dir(path)) {
        tries.push_back(path);
        it.disable_recursion_pending();
      }
      continue;
    }
    if (path.filename() == "claims.log") journals.push_back(path);
    if (path.parent_path().filename() == "retention" && path.filename().string().starts_with("unreferenced-") && path.extension() == ".bin")
      sightings.push_back(path);
  }
  if (ec) throw std::runtime_error("cannot walk " + state.string() + ": " + ec.message());
  std::sort(tries.begin(), tries.end());
  std::sort(journals.begin(), journals.end());
  for (const auto& path : journals) {
    const auto outcome = migrate_journal(path, ledger::retention_format::journal_aad, *from, *to, report, options);
    count(outcome, report);
    say(options, "ledger " + path.string() + ": " + (outcome == Ledger::migrated ? "re-sealed" : outcome == Ledger::already ? "node-local already" : "empty"));
    if (outcome == Ledger::migrated) step(options, report);
  }
  std::sort(sightings.begin(), sightings.end());
  for (const auto& path : sightings) {
    const auto outcome = migrate_sightings(path, *to, report, options);
    count(outcome, report);
    say(options, "ledger " + path.string() + ": " + (outcome == Ledger::migrated ? "sealed" : outcome == Ledger::already ? "node-local already" : "empty"));
    if (outcome == Ledger::migrated) step(options, report);
  }
  for (const auto& dir : tries) {
    const auto outcome = migrate_trie(dir, from, to, report, options);
    count(outcome, report);
    say(options, "ledger " + dir.string() + ": " + (outcome == Ledger::migrated ? "re-sealed" : outcome == Ledger::already ? "node-local already" : "empty"));
    if (outcome == Ledger::migrated) step(options, report);
  }
}

// ---- objects ------------------------------------------------------------------

void migrate_objects(const std::filesystem::path& root, const domain::StorageKey& key, const store::SealingScheme& to, MigrationReport& report, bool& proved,
                     const MigrationOptions& options) {
  std::error_code ec;
  const auto objects = root / "objects";
  std::vector<std::pair<std::filesystem::path, ObjectId>> loose;
  if (std::filesystem::is_directory(objects, ec)) {
    for (auto it = std::filesystem::recursive_directory_iterator(objects, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file()) continue;
      if (const auto id = object_id_from_file_name(it->path().filename().string())) loose.emplace_back(it->path(), *id);
    }
    if (ec) throw std::runtime_error("cannot walk " + objects.string() + ": " + ec.message());
  }
  std::sort(loose.begin(), loose.end());
  for (const auto& [file, id] : loose) migrate_loose_file(file, id, key, to, report, proved, options);

  const auto packs = root / "packs";
  std::vector<std::pair<uint64_t, std::filesystem::path>> files;
  if (std::filesystem::is_directory(packs, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(packs, ec)) {
      if (!entry.is_regular_file()) continue;
      if (const auto sequence = pack_sequence(entry.path().filename().string())) files.emplace_back(*sequence, entry.path());
    }
    if (ec) throw std::runtime_error("cannot list " + packs.string() + ": " + ec.message());
  }
  std::sort(files.begin(), files.end());
  for (const auto& [sequence, pack] : files) migrate_pack_file(pack, key, to, report, proved, options);
}

// ---- markers ------------------------------------------------------------------

std::string v19_marker_for(const BackendMarkerV18& old, const domain::StorageKey& key, const store::SealingScheme& to) {
  return store::backend_marker_text(to, old.node, old.token, store::backend_key_check(key, to.domain, old.token)) + "\n";
}

}  // namespace

RecordLayout record_layout(const domain::StorageKey& key, const store::SealingScheme& to, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                           std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size) {
  try {
    (void)store::open_object(key, to, id, nonce, tag, ciphertext, plain_size);
    return RecordLayout::v19;
  } catch (const std::exception&) {
  }
  try {
    (void)open_object_v18(key, id, nonce, tag, ciphertext, plain_size);
    return RecordLayout::v18;
  } catch (const std::exception&) {
  }
  return RecordLayout::neither;
}

std::array<uint8_t, 16> migrated_tag(const domain::StorageKey& key, const store::SealingScheme& to, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                                     std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size) {
  const auto plain = open_object_v18(key, id, nonce, tag, ciphertext, plain_size);
  const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), nonce, plain, to.associated_data(id));
  if (sealed.ciphertext.size() != ciphertext.size() || !std::equal(sealed.ciphertext.begin(), sealed.ciphertext.end(), ciphertext.begin()))
    throw std::runtime_error("the ciphertext changed under the same key and nonce: the cipher is not AES-GCM");
  return sealed.tag;
}

void migrate_loose_file(const std::filesystem::path& file, const ObjectId& id, const domain::StorageKey& key, const store::SealingScheme& to,
                        MigrationReport& report, bool& proved, const MigrationOptions& options) {
  const auto bytes = read_file(file);
  report.bytes_read += bytes.size();
  if (bytes.empty()) {
    // A crash's empty file; the store prunes it.
    return;
  }
  LooseObject loose;
  try {
    loose = decode_loose(bytes);
  } catch (const std::exception& error) {
    unreadable(report, proved, report.loose_unreadable, file.string() + ": " + error.what());
    return;
  }
  const auto layout = record_layout(key, to, id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size);
  if (layout == RecordLayout::neither) {
    unreadable(report, proved, report.loose_unreadable, file.string() + ": opens under neither layout");
    return;
  }
  proved = true;
  if (layout == RecordLayout::v19) {
    ++report.loose_already;
    return;
  }
  const auto tag = migrated_tag(key, to, id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size);
  if (!options.dry_run) {
    Fd fd(file, O_RDWR);
    write_at(fd.get(), tag, loose_tag_offset, file);
    sync_fd(fd.get(), file);
    report.bytes_written += tag.size();
  }
  ++report.loose_migrated;
  step(options, report);
}

void migrate_pack_file(const std::filesystem::path& pack, const domain::StorageKey& key, const store::SealingScheme& to, MigrationReport& report, bool& proved,
                       const MigrationOptions& options) {
  ++report.packs;
  Fd fd(pack, options.dry_run ? O_RDONLY : O_RDWR);
  struct stat st{};
  if (::fstat(fd.get(), &st) != 0 || st.st_size < 0) throw std::runtime_error("cannot stat " + pack.string() + ": " + std::strerror(errno));
  const auto file_size = static_cast<uint64_t>(st.st_size);
  report.bytes_read += file_size;
  uint64_t offset = 0;
  bool written = false;
  while (offset < file_size) {
    if (file_size - offset < pack_header_size) {
      report.pack_tail_bytes += file_size - offset;
      break;
    }
    const auto bytes = read_at(fd.get(), pack_header_size, offset);
    if (!bytes) throw std::runtime_error("cannot read " + pack.string());
    const auto header = decode_pack_header(*bytes);
    if (!header) {
      // As the store's recovery: a torn tail is left for it to truncate, a
      // damaged span inside the pack is skipped.
      const auto next = find_next_pack_header(fd.get(), offset + 1, file_size);
      if (!next) {
        report.pack_tail_bytes += file_size - offset;
        break;
      }
      ++report.pack_regions_skipped;
      offset = *next;
      continue;
    }
    const uint64_t total = pack_header_size + header->payload_size;
    if (total > file_size - offset) {
      report.pack_tail_bytes += file_size - offset;
      break;
    }
    if (header->type != pack_put) {
      ++report.pack_records_unsealed;
      offset += total;
      continue;
    }
    const auto payload = read_at(fd.get(), static_cast<size_t>(header->payload_size), offset + pack_header_size);
    if (!payload) throw std::runtime_error("cannot read " + pack.string());
    const auto layout = record_layout(key, to, header->id, header->nonce, header->tag, *payload, header->plain_size);
    if (layout == RecordLayout::neither) {
      unreadable(report, proved, report.pack_records_unreadable, pack.string() + " record " + store::to_string(header->id) + " at " + std::to_string(offset));
    } else {
      proved = true;
      if (layout == RecordLayout::v19) {
        ++report.pack_records_already;
      } else {
        auto rewritten = *header;
        rewritten.tag = migrated_tag(key, to, header->id, header->nonce, header->tag, *payload, header->plain_size);
        const auto encoded = encode_pack_header(rewritten);
        // The tag and the checksum after it, one write: a header is whole
        // or it is not, and the store's recovery tells.
        if (!options.dry_run) {
          write_at(fd.get(), std::span<const uint8_t>(encoded).subspan(pack_tag_offset), offset + pack_tag_offset, pack);
          written = true;
          report.bytes_written += encoded.size() - pack_tag_offset;
        }
        ++report.pack_records_migrated;
        // Reported once the record is on the device, where a test may end
        // the process; the pack's sync follows the last record.
        if (written) sync_fd(fd.get(), pack);
        step(options, report);
      }
    }
    offset += total;
  }
  (void)written;
}

std::string MigrationReport::summary() const {
  std::ostringstream out;
  if (already_v19) {
    out << "state is layout v19 already: nothing to do\n";
    return out.str();
  }
  if (dry_run) out << "dry run: nothing written\n";
  out << "loose objects: " << loose_migrated << " migrated, " << loose_already << " v19 already, " << loose_unreadable << " unreadable\n";
  out << "pack records: " << pack_records_migrated << " migrated, " << pack_records_already << " v19 already, " << pack_records_unreadable << " unreadable, "
      << pack_records_unsealed << " unsealed (touches and tombstones), in " << packs << " packs; " << pack_regions_skipped << " damaged spans skipped, "
      << pack_tail_bytes << " torn tail bytes left\n";
  out << "ledgers: " << ledgers_migrated << " re-sealed under the node-local key, " << ledgers_already << " already, " << ledgers_empty << " empty; "
      << ledger_tail_bytes_dropped << " torn tail bytes dropped\n";
  out << "markers rewritten: " << markers_rewritten << "\n";
  out << "bytes read: " << bytes_read << ", written: " << bytes_written << "\n";
  for (const auto& what : unreadable) out << "left as it was: " << what << "\n";
  return out.str();
}

MigrationReport migrate_state(const std::filesystem::path& state, const std::vector<std::filesystem::path>& backends,
                              const dannan::identity::ClusterKey& cluster, const dannan::identity::KeyCustody& custody, const MigrationOptions& options) {
  MigrationReport report;
  report.dry_run = options.dry_run;
  const store::SealingScheme to = store::SealingScheme::v19(domain::domain_v1);

  // What is there, before any lock or key: refused in the operator's terms.
  std::error_code ec;
  if (!std::filesystem::is_directory(state, ec)) throw MigrationRefused("state " + state.string() + " is not a directory");
  const auto layout_marker = state / "storage-layout";
  if (!std::filesystem::exists(layout_marker)) throw MigrationRefused("state " + state.string() + " has no storage-layout marker: not a Dagda or Macha state");
  const auto layout = read_text(layout_marker);
  if (layout == store::state_layout_marker(to)) {
    report.already_v19 = true;
    say(options, report.summary());
    return report;
  }
  if (layout != state_layout_v18) throw MigrationRefused("state " + state.string() + " is \"" + layout + "\": neither layout v18 nor this domain's v19");

  struct BackendFound {
    std::filesystem::path path;
    std::optional<BackendMarkerV18> v18;
  };
  std::vector<BackendFound> found;
  for (const auto& backend : backends) {
    if (!std::filesystem::is_directory(backend, ec)) throw MigrationRefused("backend " + backend.string() + " is not a directory");
    const auto marker = backend / store::backend_marker_name;
    if (!std::filesystem::exists(marker))
      throw MigrationRefused("backend " + backend.string() + " has no " + std::string(store::backend_marker_name) + " marker");
    const auto text = read_text(marker);
    if (const auto v18 = parse_backend_marker_v18(text)) {
      found.push_back({backend, v18});
      continue;
    }
    if (text.starts_with(store::backend_marker_v19) && text.find(domain::to_string(to.domain)) != std::string::npos) {
      found.push_back({backend, std::nullopt});
      continue;
    }
    throw MigrationRefused("backend " + backend.string() + " marker is \"" + text + "\": neither layout v18 nor domain v1's v19");
  }
  const auto control = options.control.empty() ? state / "metadata-objects" : options.control;
  const bool have_control = std::filesystem::is_directory(control, ec);
  const bool have_cache = !options.cache.empty() && std::filesystem::is_directory(options.cache, ec);

  // Every store's lock: a running node is refused, never migrated under.
  std::vector<std::unique_ptr<store::StorageLock>> locks;
  const auto lock = [&](const std::filesystem::path& root) {
    try {
      locks.push_back(std::make_unique<store::StorageLock>(root));
    } catch (const std::exception& error) {
      throw MigrationRefused(root.string() + " is in use (is the node running?): " + error.what());
    }
  };
  lock(state);
  for (const auto& backend : found) lock(backend.path);
  if (have_control) lock(control);
  if (have_cache) lock(options.cache);

  const auto domain_key = std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_v1(cluster));
  const auto node_local = std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_node_local(custody));

  // Objects first: a backend, the control store and the cache are each a
  // tree of loose files and packs.
  bool proved = false;
  for (const auto& backend : found) {
    say(options, "backend " + backend.path.string());
    migrate_objects(backend.path, *domain_key, to, report, proved, options);
  }
  if (have_control) {
    say(options, "control store " + control.string());
    migrate_objects(control, *domain_key, to, report, proved, options);
  }
  if (have_cache) {
    say(options, "cache " + options.cache.string());
    migrate_objects(options.cache, *domain_key, to, report, proved, options);
  }

  // Then every ledger under the state: the held tries, the claims and
  // their tries, the reference counts.
  std::vector<std::filesystem::path> skip{state / "backend-identities", control};
  if (have_cache) skip.push_back(options.cache);
  for (const auto& backend : found) skip.push_back(backend.path);
  migrate_ledgers(state, skip, domain_key, node_local, report, options);

  // Markers last: the backends', their mirrors, then the state's.
  const auto rewrite = [&](const std::filesystem::path& path, const std::string& text) {
    if (!options.dry_run) dannan::identity::durable_replace_file(path, text);
    ++report.markers_rewritten;
    report.bytes_written += options.dry_run ? 0 : text.size();
    say(options, "marker " + path.string());
    step(options, report);
  };
  for (const auto& backend : found) {
    if (!backend.v18) continue;
    rewrite(backend.path / store::backend_marker_name, v19_marker_for(*backend.v18, *domain_key, to));
  }
  const auto identities = state / "backend-identities";
  if (std::filesystem::is_directory(identities, ec)) {
    std::vector<std::filesystem::path> mirrors;
    for (const auto& entry : std::filesystem::directory_iterator(identities, ec))
      if (entry.is_regular_file() && entry.path().extension() == ".id") mirrors.push_back(entry.path());
    std::sort(mirrors.begin(), mirrors.end());
    for (const auto& mirror : mirrors)
      if (const auto v18 = parse_backend_marker_v18(read_text(mirror))) rewrite(mirror, v19_marker_for(*v18, *domain_key, to));
  }
  rewrite(layout_marker, store::state_layout_marker(to) + "\n");
  say(options, report.summary());
  return report;
}

}  // namespace dagda::migrate
