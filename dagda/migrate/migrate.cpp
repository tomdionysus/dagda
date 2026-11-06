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
#include <chrono>
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
#include "dagda/migrate/retag.h"
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

// The store ages a loose object by its modification time (older_than,
// remove_if_older_than), so a move that only rewrites a tag leaves it as it was.
void keep_modified_time(int fd, const struct stat& before, const std::filesystem::path& path) {
  struct timespec times[2]{};
  times[0].tv_nsec = UTIME_OMIT;
#if defined(__APPLE__)
  times[1] = before.st_mtimespec;
#else
  times[1] = before.st_mtim;
#endif
  if (::futimens(fd, times) != 0) throw std::runtime_error("cannot keep the time of " + path.string() + ": " + std::strerror(errno));
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
  // A journal of nothing but a torn append holds nothing, under any key.
  if (ledger::SealedJournal::holds_only_a_torn_frame(path, max_read_frame)) return Ledger::empty;
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
Ledger migrate_sightings(const std::filesystem::path& path, const domain::StorageKey& from, const domain::StorageKey& to, MigrationReport& report,
                         const MigrationOptions& options) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Ledger::empty;
  const store::Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.empty()) return Ledger::empty;
  const auto refused = [&]() -> MigrationRefused {
    return MigrationRefused("sightings file " + path.string() + " is neither v18's table nor sealed under the node-local key: damaged, or another node's");
  };
  std::map<store::ObjectId, uint64_t> table;
  if (options.reverse) {
    // Back to the clear: sealed is what moves, a table that decodes is done.
    try {
      table = ledger::open_unreferenced_since(from, bytes);
    } catch (const std::exception&) {
      try {
        (void)ledger::decode_unreferenced_since(bytes);
        return Ledger::already;
      } catch (const std::exception&) {
        throw refused();
      }
    }
  } else {
    try {
      (void)ledger::open_unreferenced_since(to, bytes);
      return Ledger::already;
    } catch (const std::exception&) {
    }
    try {
      table = ledger::decode_unreferenced_since(bytes);
    } catch (const std::exception&) {
      throw refused();
    }
  }
  report.bytes_read += bytes.size();
  if (options.dry_run) return Ledger::migrated;
  const auto written = options.reverse ? ledger::encode_unreferenced_since(table) : ledger::seal_unreferenced_since(to, table);
  dannan::identity::durable_replace_file(path, std::string_view(reinterpret_cast<const char*>(written.data()), written.size()));
  report.bytes_written += written.size();
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

// The ledgers, by the names the store gives them, as Macha's does. Nothing
// else under the state is read: metadata/ holds a journal.log and is not a
// trie, and the rest of a Macha state is not the store's.
struct KnownLedgers {
  std::vector<std::filesystem::path> tries;
  std::vector<std::filesystem::path> journals;
  std::vector<std::filesystem::path> sightings;
};

KnownLedgers known_ledgers(const std::filesystem::path& state) {
  KnownLedgers out;
  std::error_code ec;
  const auto keep = [&](std::vector<std::filesystem::path>& into, const std::filesystem::path& path) {
    if (std::filesystem::exists(path, ec)) into.push_back(path);
  };
  const auto ledger = state / "ledger";
  const auto retention = state / "retention";
  keep(out.tries, ledger / "control");
  if (std::filesystem::is_directory(ledger, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(ledger, ec))
      if (entry.is_directory() && entry.path().filename().string().starts_with("data-")) out.tries.push_back(entry.path());
    if (ec) throw std::runtime_error("cannot list " + ledger.string() + ": " + ec.message());
  }
  keep(out.tries, ledger / "referenced" / "extents");
  keep(out.tries, ledger / "referenced" / "nodes");
  keep(out.tries, retention / "ledger-data");
  keep(out.tries, retention / "ledger-control");
  keep(out.journals, retention / "claims.log");
  keep(out.sightings, retention / "unreferenced-data.bin");
  keep(out.sightings, retention / "unreferenced-control.bin");
  std::sort(out.tries.begin(), out.tries.end());
  return out;
}

void migrate_ledgers(const std::filesystem::path& state, std::shared_ptr<const domain::StorageKey> from, std::shared_ptr<const domain::StorageKey> to,
                     MigrationReport& report, const MigrationOptions& options) {
  const auto ledgers = known_ledgers(state);
  for (const auto& path : ledgers.journals) {
    const auto outcome = migrate_journal(path, ledger::retention_format::journal_aad, *from, *to, report, options);
    count(outcome, report);
    say(options, "ledger " + path.string() + ": " + (outcome == Ledger::migrated ? "re-sealed" : outcome == Ledger::already ? "node-local already" : "empty"));
    if (outcome == Ledger::migrated) step(options, report);
  }
  for (const auto& path : ledgers.sightings) {
    const auto outcome = migrate_sightings(path, *from, *to, report, options);
    count(outcome, report);
    say(options, "ledger " + path.string() + ": " + (outcome == Ledger::migrated ? "sealed" : outcome == Ledger::already ? "node-local already" : "empty"));
    if (outcome == Ledger::migrated) step(options, report);
  }
  for (const auto& dir : ledgers.tries) {
    const auto outcome = migrate_trie(dir, from, to, report, options);
    count(outcome, report);
    say(options, "ledger " + dir.string() + ": " + (outcome == Ledger::migrated ? "re-sealed" : outcome == Ledger::already ? "node-local already" : "empty"));
    if (outcome == Ledger::migrated) step(options, report);
  }
}

// What RetentionStore would refuse to open once migrated, refused while
// nothing has been written. An unmarked trie with records is its "hold claims
// but no ledger marker"; a probe cannot tell an empty trie, so an unmarked
// one that has been written to refuses too and the operator decides.
// The token a backend marker of either layout names; nothing for neither.
std::optional<NodeId> marker_token(const std::string& text) {
  if (const auto v18 = parse_backend_marker_v18(text)) return v18->token;
  std::vector<std::string> fields;
  std::istringstream in(text);
  for (std::string field; in >> field;) fields.push_back(field);
  if (fields.size() != 5 || fields[0] != store::backend_marker_v19) return std::nullopt;
  return NodeId::parse(fields[2]);
}

// A backend the state's mirrors name and the run was not given would keep its
// data v18 under a state marked v19, where no later run looks: its mirror moved
// with the rest, and the next run says nothing to do. Refused before any write.
void refuse_unnamed_backends(const std::filesystem::path& state, const std::vector<std::filesystem::path>& backends) {
  std::vector<NodeId> named;
  for (const auto& backend : backends)
    if (const auto token = marker_token(read_text(backend / store::backend_marker_name))) named.push_back(*token);
  const auto identities = state / "backend-identities";
  std::error_code ec;
  if (!std::filesystem::is_directory(identities, ec)) return;
  for (const auto& entry : std::filesystem::directory_iterator(identities, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".id") continue;
    const auto token = marker_token(read_text(entry.path()));
    if (!token || std::find(named.begin(), named.end(), *token) != named.end()) continue;
    throw MigrationRefused("this state knows a backend with token " + dannan::identity::to_string(*token) + " (" + entry.path().string() +
                           ") that no --backend names. Give every backend the node has; if that disk is gone for good, remove its mirror first");
  }
  if (ec) throw std::runtime_error("cannot list " + identities.string() + ": " + ec.message());
}

// Where the migration makes replacements under the state.
std::vector<std::filesystem::path> known_ledgers_dirs(const std::filesystem::path& state) {
  std::vector<std::filesystem::path> dirs{state, state / "backend-identities", state / "retention"};
  std::error_code ec;
  const auto holding = [&](const std::filesystem::path& dir) {
    if (std::filesystem::is_directory(dir, ec)) dirs.push_back(dir);
  };
  const auto ledger = state / "ledger";
  holding(ledger / "control");
  holding(ledger / "referenced" / "extents");
  holding(ledger / "referenced" / "nodes");
  holding(state / "retention" / "ledger-data");
  holding(state / "retention" / "ledger-control");
  if (std::filesystem::is_directory(ledger, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(ledger, ec))
      if (entry.is_directory() && entry.path().filename().string().starts_with("data-")) dirs.push_back(entry.path());
    if (ec) throw std::runtime_error("cannot list " + ledger.string() + ": " + ec.message());
  }
  return dirs;
}

void check_retention(const std::filesystem::path& state, const domain::StorageKey& from, const domain::StorageKey& to) {
  const auto root = state / "retention";
  std::error_code ec;
  if (std::filesystem::exists(root / "ledger", ec)) return;
  if (ledger::retention_holds_unmoved_checkpoint(root))
    throw MigrationRefused("retention claims at " + root.string() +
                           " are in a checkpoint from before the claims ledger (claims.meta or claims.current), which Dagda does not read");
  for (const auto* name : {"ledger-data", "ledger-control"}) {
    const auto dir = root / name;
    if (!std::filesystem::exists(dir, ec)) continue;
    if (ledger::ObjectTrie::probe(dir, to).has_value() || ledger::ObjectTrie::probe(dir, from).has_value())
      throw MigrationRefused("retention claims at " + root.string() + " hold claims but no ledger marker; Dagda will not rebuild them empty");
  }
}

// ---- objects ------------------------------------------------------------------

std::array<uint8_t, 16> derive_v19_tag(const TagDeriver& deriver, const store::SealingScheme& to, const ObjectId& id, std::span<const uint8_t, 16> tag,
                                       uint64_t ciphertext_size) {
  return deriver.derive(tag, associated_data_v18(id), to.associated_data(id), ciphertext_size);
}

// The tag in the run's direction.
std::array<uint8_t, 16> moved_tag(const TagDeriver& deriver, const store::SealingScheme& v19, const ObjectId& id, std::span<const uint8_t, 16> tag,
                                  uint64_t ciphertext_size, bool reverse) {
  if (!reverse) return derive_v19_tag(deriver, v19, id, tag, ciphertext_size);
  return deriver.derive(tag, v19.associated_data(id), associated_data_v18(id), ciphertext_size);
}

// What a record is, trying first what the run expects to move, so a record
// not yet moved is opened once.
RecordLayout classify(const domain::StorageKey& key, const store::SealingScheme& v19, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                      std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size, bool reverse) {
  if (!reverse) return record_layout(key, v19, id, nonce, tag, ciphertext, plain_size);
  try {
    (void)store::open_object(key, v19, id, nonce, tag, ciphertext, plain_size);
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

// A tag the power cut off in the middle of its rewrite. The tag on the disk is
// the old tag's bytes on one side of the cut and the new tag's on the other, and
// old and new differ by a value that depends only on the associated data and the
// length (migrate/retag.h). So the tags it can have been are few: the cut at each
// byte, front first or back first, with the bytes on one side moved back. Each
// is tried under both layouts, which for a record that is simply damaged costs
// these opens and finds nothing, since no tag opens a changed ciphertext. Nothing
// is believed but a tag that authenticates.
struct TornTag {
  RecordLayout layout = RecordLayout::neither;
  std::array<uint8_t, 16> tag{};
};

std::optional<TornTag> find_torn_tag(const domain::StorageKey& key, const TagDeriver& deriver, const store::SealingScheme& v19, const ObjectId& id,
                                     std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> found, std::span<const uint8_t> ciphertext,
                                     uint64_t plain_size) {
  const std::array<uint8_t, 16> zero{};
  const auto difference = deriver.derive(zero, associated_data_v18(id), v19.associated_data(id), ciphertext.size());
  std::array<uint8_t, 16> candidate{};
  const auto opens = [&](RecordLayout& layout) {
    try {
      (void)open_object_v18(key, id, nonce, candidate, ciphertext, plain_size);
      layout = RecordLayout::v18;
      return true;
    } catch (const std::exception&) {
    }
    try {
      (void)store::open_object(key, v19, id, nonce, candidate, ciphertext, plain_size);
      layout = RecordLayout::v19;
      return true;
    } catch (const std::exception&) {
    }
    return false;
  };
  for (size_t cut = 0; cut <= 16; ++cut) {
    // Bytes before the cut moved back or forward; the rest as they are. A cut
    // of none is the tag as found: whole, with only the checksum after it torn.
    for (int front = 1; front >= 0; --front) {
      if (cut == 0 && !front) continue;
      for (size_t i = 0; i < 16; ++i) candidate[i] = static_cast<uint8_t>(found[i] ^ (((i < cut) == (front == 1)) ? difference[i] : 0));
      RecordLayout layout = RecordLayout::neither;
      if (opens(layout)) return TornTag{layout, candidate};
    }
  }
  return std::nullopt;
}

// A line every so many records: on a 4 TB disk the operator sees it moving.
// The clock is only ever printed.
void progress(MigrationReport& report, const MigrationOptions& options) {
  ++report.records_seen;
  if (!options.say || !options.progress_every || report.records_seen % options.progress_every != 0) return;
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - report.started).count();
  constexpr double mib = 1024.0 * 1024.0;
  const auto read = static_cast<double>(report.bytes_read) / mib;
  std::ostringstream out;
  out << "progress: " << report.records_seen << " records, " << static_cast<uint64_t>(read) << " MiB read, "
      << static_cast<uint64_t>(seconds > 0 ? read / seconds : 0) << " MiB/s";
  options.say(out.str());
}

void migrate_objects(const std::filesystem::path& root, const domain::StorageKey& key, const TagDeriver& deriver, const store::SealingScheme& to,
                     MigrationReport& report, bool& proved, const MigrationOptions& options) {
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
  for (const auto& [file, id] : loose) {
    if (options.probe_only && proved) return;
    migrate_loose_file(file, id, key, to, report, proved, options, &deriver);
  }

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
  for (const auto& [sequence, pack] : files) {
    if (options.probe_only && proved) return;
    migrate_pack_file(pack, key, to, report, proved, options, &deriver);
  }
}

// ---- markers ------------------------------------------------------------------

std::string v19_marker_for(const BackendMarkerV18& old, const domain::StorageKey& key, const store::SealingScheme& to) {
  return store::backend_marker_text(to, old.node, old.token, store::backend_key_check(key, to.domain, old.token)) + "\n";
}

// What a crash left beside what a replacement was replacing: as large as it, and
// nothing else ever removes it.
void remove_temporaries(const std::filesystem::path& state, const std::vector<std::filesystem::path>& backends, const MigrationOptions& options) {
  if (options.dry_run) return;
  auto dirs = known_ledgers_dirs(state);
  dirs.insert(dirs.end(), backends.begin(), backends.end());
  std::error_code ec;
  for (const auto& dir : dirs) {
    if (!std::filesystem::is_directory(dir, ec)) continue;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (!migration_temporary(entry.path().filename().string())) continue;
      std::filesystem::remove(entry.path(), ec);
      if (ec) throw std::runtime_error("cannot remove " + entry.path().string() + ": " + ec.message());
      say(options, "removed what a crash left: " + entry.path().string());
    }
    if (ec) throw std::runtime_error("cannot list " + dir.string() + ": " + ec.message());
  }
}

}  // namespace

bool migration_temporary(std::string_view file_name) {
  const auto at = file_name.rfind(".tmp.");
  if (at == std::string_view::npos) return false;
  const auto suffix = file_name.substr(at + 5);
  const auto dot = suffix.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == suffix.size()) return false;
  const auto digits = [](std::string_view text) { return std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }); };
  if (!digits(suffix.substr(0, dot)) || !digits(suffix.substr(dot + 1))) return false;
  const auto base = file_name.substr(0, at);
  return base == "storage-layout" || base == store::backend_marker_name || base == "claims.log" || base == "unreferenced-data.bin" ||
         base == "unreferenced-control.bin" || base == "root" || base == "journal.log" || base.ends_with(".id");
}

RecordLayout record_layout(const domain::StorageKey& key, const store::SealingScheme& to, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                           std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size) {
  // v18 first: a record not yet migrated is opened once, and only what a
  // resumed run already did pays for a second attempt.
  try {
    (void)open_object_v18(key, id, nonce, tag, ciphertext, plain_size);
    return RecordLayout::v18;
  } catch (const std::exception&) {
  }
  try {
    (void)store::open_object(key, to, id, nonce, tag, ciphertext, plain_size);
    return RecordLayout::v19;
  } catch (const std::exception&) {
  }
  return RecordLayout::neither;
}

std::array<uint8_t, 16> migrated_tag(const domain::StorageKey& key, const store::SealingScheme& to, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                                     std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size) {
  (void)open_object_v18(key, id, nonce, tag, ciphertext, plain_size);
  return derive_v19_tag(TagDeriver(key), to, id, tag, ciphertext.size());
}

void migrate_loose_file(const std::filesystem::path& file, const ObjectId& id, const domain::StorageKey& key, const store::SealingScheme& to,
                        MigrationReport& report, bool& proved, const MigrationOptions& options, const TagDeriver* deriver) {
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
  std::optional<TagDeriver> own;
  if (!deriver) deriver = &own.emplace(key);
  auto layout = classify(key, to, id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size, options.reverse);
  std::array<uint8_t, 16> found = loose.tag;
  bool torn = false;
  if (layout == RecordLayout::neither) {
    if (const auto tear = find_torn_tag(key, *deriver, to, id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size)) {
      layout = tear->layout;
      found = tear->tag;
      torn = true;
    }
  }
  if (layout == RecordLayout::neither) {
    unreadable(report, proved, report.loose_unreadable, file.string() + ": opens under neither layout");
    return;
  }
  proved = true;
  const auto target = options.reverse ? RecordLayout::v18 : RecordLayout::v19;
  if (layout == target && !torn) {
    ++report.loose_already;
    return;
  }
  if (torn) ++report.loose_torn_finished;
  if (!options.dry_run) {
    const auto tag = layout == target ? found : moved_tag(*deriver, to, id, found, loose.ciphertext.size(), options.reverse);
    Fd fd(file, O_RDWR);
    struct stat before{};
    if (::fstat(fd.get(), &before) != 0) throw std::runtime_error("cannot stat " + file.string() + ": " + std::strerror(errno));
    write_at(fd.get(), tag, loose_tag_offset, file);
    keep_modified_time(fd.get(), before, file);
    sync_fd(fd.get(), file);
    report.bytes_written += tag.size();
  }
  ++report.loose_migrated;
  progress(report, options);
  step(options, report);
}

void migrate_pack_file(const std::filesystem::path& pack, const domain::StorageKey& key, const store::SealingScheme& to, MigrationReport& report, bool& proved,
                       const MigrationOptions& options, const TagDeriver* deriver) {
  ++report.packs;
  Fd fd(pack, options.dry_run ? O_RDONLY : O_RDWR);
  struct stat st{};
  if (::fstat(fd.get(), &st) != 0 || st.st_size < 0) throw std::runtime_error("cannot stat " + pack.string() + ": " + std::strerror(errno));
  const auto file_size = static_cast<uint64_t>(st.st_size);
  std::optional<TagDeriver> own;
  if (!deriver) deriver = &own.emplace(key);
  uint64_t offset = 0;
  // Written and not yet on the device. A record is whole v18 or whole v19
  // either way, so a crash costs only the re-doing; the last sync is before
  // the markers can follow.
  uint64_t unsynced = 0;
  constexpr uint64_t sync_every = 256;
  while (offset < file_size) {
    if (file_size - offset < pack_header_size) {
      report.pack_tail_bytes += file_size - offset;
      break;
    }
    const auto bytes = read_at(fd.get(), pack_header_size, offset);
    if (!bytes) throw std::runtime_error("cannot read " + pack.string());
    const auto header = decode_pack_header(*bytes);
    if (!header) {
      // A rewrite of this record's tag and checksum cut short? Its first 77
      // bytes are as Macha wrote them, so it still says what it is.
      if (const auto torn = decode_pack_header_unchecked(*bytes);
          torn && torn->type == pack_put && pack_header_size + torn->payload_size <= file_size - offset) {
        const auto payload = read_at(fd.get(), static_cast<size_t>(torn->payload_size), offset + pack_header_size);
        if (!payload) throw std::runtime_error("cannot read " + pack.string());
        if (const auto tear = find_torn_tag(key, *deriver, to, torn->id, torn->nonce, torn->tag, *payload, torn->plain_size)) {
          proved = true;
          ++report.pack_records_torn_finished;
          ++report.pack_records_migrated;
          const auto target = options.reverse ? RecordLayout::v18 : RecordLayout::v19;
          if (!options.dry_run) {
            auto rewritten = *torn;
            rewritten.tag = tear->layout == target ? tear->tag : moved_tag(*deriver, to, torn->id, tear->tag, payload->size(), options.reverse);
            const auto encoded = encode_pack_header(rewritten);
            write_at(fd.get(), std::span<const uint8_t>(encoded).subspan(pack_tag_offset), offset + pack_tag_offset, pack);
            report.bytes_written += encoded.size() - pack_tag_offset;
            sync_fd(fd.get(), pack);
          }
          step(options, report);
          if (options.probe_only) return;
          const uint64_t total = pack_header_size + torn->payload_size;
          offset += total;
          report.bytes_read += total;
          progress(report, options);
          continue;
        }
      }
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
    const auto layout = classify(key, to, header->id, header->nonce, header->tag, *payload, header->plain_size, options.reverse);
    if (layout == RecordLayout::neither) {
      unreadable(report, proved, report.pack_records_unreadable, pack.string() + " record " + store::to_string(header->id) + " at " + std::to_string(offset));
    } else {
      proved = true;
      if (layout == (options.reverse ? RecordLayout::v18 : RecordLayout::v19)) {
        ++report.pack_records_already;
      } else {
        // The tag and the checksum after it, one write: a header is whole
        // or it is not, and the store's recovery tells.
        if (!options.dry_run) {
          auto rewritten = *header;
          rewritten.tag = moved_tag(*deriver, to, header->id, header->tag, payload->size(), options.reverse);
          const auto encoded = encode_pack_header(rewritten);
          write_at(fd.get(), std::span<const uint8_t>(encoded).subspan(pack_tag_offset), offset + pack_tag_offset, pack);
          report.bytes_written += encoded.size() - pack_tag_offset;
          ++unsynced;
        }
        ++report.pack_records_migrated;
        // Reported once the record is on the device where a test may end the
        // process.
        if (unsynced && (options.after_step || unsynced >= sync_every)) {
          sync_fd(fd.get(), pack);
          unsynced = 0;
        }
        step(options, report);
      }
    }
    if (options.probe_only && proved) return;
    offset += total;
    report.bytes_read += total;
    progress(report, options);
  }
  if (unsynced) sync_fd(fd.get(), pack);
}

std::string MigrationReport::summary() const {
  std::ostringstream out;
  if (already_v19) {
    out << "state is layout v19 already: nothing to do\n";
    return out.str();
  }
  if (dry_run) out << "dry run: nothing written\n";
  if (reverse) out << "back to layout v18\n";
  const char* done = reverse ? " v18 already, " : " v19 already, ";
  out << "loose objects: " << loose_migrated << " migrated, " << loose_already << done << loose_unreadable << " unreadable\n";
  if (loose_torn_finished + pack_records_torn_finished)
    out << "cut short by a crash and finished: " << loose_torn_finished << " loose objects, " << pack_records_torn_finished << " pack records\n";
  out << "pack records: " << pack_records_migrated << " migrated, " << pack_records_already << done << pack_records_unreadable << " unreadable, "
      << pack_records_unsealed << " unsealed (touches and tombstones), in " << packs << " packs; " << pack_regions_skipped << " damaged spans skipped, "
      << pack_tail_bytes << " torn tail bytes left\n";
  out << "ledgers: " << ledgers_migrated << (reverse ? " re-sealed under the domain key, " : " re-sealed under the node-local key, ") << ledgers_already
      << " already, " << ledgers_empty << " empty; " << ledger_tail_bytes_dropped << " torn tail bytes dropped\n";
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
    // Every backend it was given must be there too: one still v18 under a state
    // marked v19 is a disk no run would otherwise come back to.
    for (const auto& backend : backends) {
      if (std::filesystem::exists(backend / store::backend_marker_name) && parse_backend_marker_v18(read_text(backend / store::backend_marker_name)))
        throw MigrationRefused("state " + state.string() + " is layout v19 and backend " + backend.string() +
                               " is v18: run --reverse with every backend named, then forward with every backend named");
    }
    report.already_v19 = true;
    say(options, report.summary());
    return report;
  }
  if (layout == store::state_layout_going_back)
    throw MigrationRefused("state " + state.string() + " stopped on its way back to layout v18: finish that first, with --reverse");
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
  refuse_unnamed_backends(state, backends);
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

  // Every refusal before the first write: the node id, the key, what the
  // store would refuse to open, and every ledger.
  if (options.node) {
    for (const auto& backend : found) {
      if (backend.v18 && backend.v18->node != *options.node)
        throw MigrationRefused("backend " + backend.path.string() + " belongs to node " + dannan::identity::to_string(backend.v18->node) +
                               " but this node is " + dannan::identity::to_string(*options.node) +
                               ": the node would refuse the disk. Run with the --node-state whose node-id is the one Macha's state holds");
    }
  }
  const TagDeriver deriver(*domain_key);
  bool proved = false;
  {
    MigrationOptions quiet;
    quiet.dry_run = true;
    MigrationReport scratch;
    MigrationOptions probe = quiet;
    probe.probe_only = true;
    for (const auto& backend : found) migrate_objects(backend.path, *domain_key, deriver, to, scratch, proved, probe);
    if (have_control) migrate_objects(control, *domain_key, deriver, to, scratch, proved, probe);
    if (have_cache) migrate_objects(options.cache, *domain_key, deriver, to, scratch, proved, probe);
    check_retention(state, *domain_key, *node_local);
    migrate_ledgers(state, domain_key, node_local, scratch, quiet);
  }

  std::vector<std::filesystem::path> backend_paths;
  for (const auto& backend : found) backend_paths.push_back(backend.path);
  remove_temporaries(state, backend_paths, options);

  // Objects first: a backend, the control store and the cache are each a
  // tree of loose files and packs.
  for (const auto& backend : found) {
    say(options, "backend " + backend.path.string());
    migrate_objects(backend.path, *domain_key, deriver, to, report, proved, options);
  }
  if (have_control) {
    say(options, "control store " + control.string());
    migrate_objects(control, *domain_key, deriver, to, report, proved, options);
  }
  if (have_cache) {
    say(options, "cache " + options.cache.string());
    migrate_objects(options.cache, *domain_key, deriver, to, report, proved, options);
  }

  // Then every ledger: the held tries, the claims and their tries, the
  // reference counts.
  migrate_ledgers(state, domain_key, node_local, report, options);

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

MigrationReport migrate_state_back(const std::filesystem::path& state, const std::vector<std::filesystem::path>& backends,
                                   const dannan::identity::ClusterKey& cluster, const dannan::identity::KeyCustody& custody, const MigrationOptions& given) {
  MigrationOptions options = given;
  options.reverse = true;
  MigrationReport report;
  report.dry_run = options.dry_run;
  report.reverse = true;
  const store::SealingScheme v19 = store::SealingScheme::v19(domain::domain_v1);
  const auto domain_key = std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_v1(cluster));
  const auto node_local = std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_node_local(custody));

  std::error_code ec;
  if (!std::filesystem::is_directory(state, ec)) throw MigrationRefused("state " + state.string() + " is not a directory");
  const auto layout_marker = state / "storage-layout";
  if (!std::filesystem::exists(layout_marker)) throw MigrationRefused("state " + state.string() + " has no storage-layout marker: not a Dagda or Macha state");
  // Either marker: a forward run that crashed left a v18 marker over records
  // already v19, and this finishes going back from it. Every record is looked
  // at, so a disk already all v18 is reported as such.
  const auto layout = read_text(layout_marker);
  if (layout != store::state_layout_marker(v19) && layout != state_layout_v18 && layout != store::state_layout_going_back)
    throw MigrationRefused("state " + state.string() + " is \"" + layout + "\": neither layout v18 nor domain v1's v19; only Macha's domain goes back");

  const auto v18_marker = [](const dannan::identity::NodeId& node, const dannan::identity::NodeId& token) {
    return std::string(backend_marker_v18) + " " + dannan::identity::to_string(node) + " " + dannan::identity::to_string(token) + "\n";
  };
  // A v19 marker of domain v1: its node, its token, and the key proved by its
  // check, before any write.
  const auto parse_v19 = [&](const std::string& text, const std::filesystem::path& where) -> std::optional<BackendMarkerV18> {
    std::vector<std::string> fields;
    std::istringstream in(text);
    for (std::string field; in >> field;) fields.push_back(field);
    if (fields.size() != 5 || fields[0] != store::backend_marker_v19) return std::nullopt;
    if (fields[3] != domain::to_string(v19.domain)) throw MigrationRefused(where.string() + " is domain " + fields[3] + ": only Macha's domain goes back");
    const auto node = dannan::identity::NodeId::parse(fields[1]);
    const auto token = dannan::identity::NodeId::parse(fields[2]);
    if (!node || !token) return std::nullopt;
    if (fields[4] != store::backend_key_check(*domain_key, v19.domain, *token))
      throw MigrationRefused(where.string() + " does not match the key: wrong cluster key, or another cluster's disk");
    return BackendMarkerV18{*node, *token};
  };

  struct BackendFound {
    std::filesystem::path path;
    BackendMarkerV18 marker;
    bool v18 = false;
  };
  std::vector<BackendFound> found;
  for (const auto& backend : backends) {
    if (!std::filesystem::is_directory(backend, ec)) throw MigrationRefused("backend " + backend.string() + " is not a directory");
    const auto marker = backend / store::backend_marker_name;
    if (!std::filesystem::exists(marker))
      throw MigrationRefused("backend " + backend.string() + " has no " + std::string(store::backend_marker_name) + " marker");
    const auto text = read_text(marker);
    if (const auto v18 = parse_backend_marker_v18(text)) {
      found.push_back({backend, *v18, true});
    } else if (const auto parsed = parse_v19(text, marker)) {
      found.push_back({backend, *parsed, false});
    } else {
      throw MigrationRefused("backend " + backend.string() + " marker is \"" + text + "\": neither layout v18 nor domain v1's v19");
    }
  }
  refuse_unnamed_backends(state, backends);
  const auto control = options.control.empty() ? state / "metadata-objects" : options.control;
  const bool have_control = std::filesystem::is_directory(control, ec);
  const bool have_cache = !options.cache.empty() && std::filesystem::is_directory(options.cache, ec);

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

  if (options.node) {
    for (const auto& backend : found) {
      if (backend.marker.node != *options.node)
        throw MigrationRefused("backend " + backend.path.string() + " belongs to node " + dannan::identity::to_string(backend.marker.node) +
                               " but this node is " + dannan::identity::to_string(*options.node) + ": run with the --node-state whose node-id the disks carry");
    }
  }
  const TagDeriver deriver(*domain_key);
  bool proved = false;
  {
    MigrationOptions quiet;
    quiet.dry_run = true;
    quiet.reverse = true;
    MigrationReport scratch;
    MigrationOptions probe = quiet;
    probe.probe_only = true;
    for (const auto& backend : found) migrate_objects(backend.path, *domain_key, deriver, v19, scratch, proved, probe);
    if (have_control) migrate_objects(control, *domain_key, deriver, v19, scratch, proved, probe);
    if (have_cache) migrate_objects(options.cache, *domain_key, deriver, v19, scratch, proved, probe);
    migrate_ledgers(state, node_local, domain_key, scratch, quiet);
  }
  std::vector<std::filesystem::path> backend_paths;
  for (const auto& backend : found) backend_paths.push_back(backend.path);
  remove_temporaries(state, backend_paths, options);
  // From the first write to the last the state is part v18 and part v19. Under
  // either marker Macha or the store would open it and read the other part as
  // corrupt, and a forward run would call it done.
  if (!options.dry_run) dannan::identity::durable_replace_file(layout_marker, std::string(store::state_layout_going_back) + "\n");
  for (const auto& backend : found) {
    say(options, "backend " + backend.path.string());
    migrate_objects(backend.path, *domain_key, deriver, v19, report, proved, options);
  }
  if (have_control) {
    say(options, "control store " + control.string());
    migrate_objects(control, *domain_key, deriver, v19, report, proved, options);
  }
  if (have_cache) {
    say(options, "cache " + options.cache.string());
    migrate_objects(options.cache, *domain_key, deriver, v19, report, proved, options);
  }
  migrate_ledgers(state, node_local, domain_key, report, options);

  // Markers last, as going forward: a disk is v19 until everything is v18.
  const auto rewrite = [&](const std::filesystem::path& path, const std::string& text) {
    if (!options.dry_run) dannan::identity::durable_replace_file(path, text);
    ++report.markers_rewritten;
    report.bytes_written += options.dry_run ? 0 : text.size();
    say(options, "marker " + path.string());
    step(options, report);
  };
  for (const auto& backend : found)
    if (!backend.v18) rewrite(backend.path / store::backend_marker_name, v18_marker(backend.marker.node, backend.marker.token));
  const auto identities = state / "backend-identities";
  if (std::filesystem::is_directory(identities, ec)) {
    std::vector<std::filesystem::path> mirrors;
    for (const auto& entry : std::filesystem::directory_iterator(identities, ec))
      if (entry.is_regular_file() && entry.path().extension() == ".id") mirrors.push_back(entry.path());
    std::sort(mirrors.begin(), mirrors.end());
    for (const auto& mirror : mirrors) {
      const auto text = read_text(mirror);
      if (parse_backend_marker_v18(text)) continue;
      if (const auto parsed = parse_v19(text, mirror)) rewrite(mirror, v18_marker(parsed->node, parsed->token));
    }
  }
  if (layout != state_layout_v18) {
    rewrite(layout_marker, std::string(state_layout_v18) + "\n");
  } else if (!options.dry_run) {
    // v18 when the run began (a forward run's crash, or nothing to do): it says
    // so again, which is no change to report.
    dannan::identity::durable_replace_file(layout_marker, std::string(state_layout_v18) + "\n");
  }
  say(options, report.summary());
  return report;
}

}  // namespace dagda::migrate
