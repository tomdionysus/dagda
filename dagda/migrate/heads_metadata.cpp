//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/migrate/heads_metadata.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>

#include "dagda/heads/record.h"
#include "dagda/migrate/retag.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"
#include "dannan/net/codec.h"

namespace dagda::migrate {

heads::ReplicaMagics macha_metadata::magics() {
  return heads::ReplicaMagics{checkpoint_magic, journal_magic, history_magic, heads_magic, author_magic, set_aside_magic};
}

namespace {

using dannan::identity::hex;
using dannan::identity::sha256;
using dannan::net::Reader;
using dannan::net::Writer;
using heads::Magic;

constexpr const char* history_name = "history.log";
constexpr const char* journal_name = "journal.log";
constexpr const char* checkpoint_name = "checkpoint.meta";
constexpr const char* heads_name = "heads.meta";
constexpr const char* author_name = "author.meta";
constexpr const char* set_aside_name = "set-aside.meta";
constexpr const char* seed_v18_name = "current.meta";

// Room kept beyond the largest file written whole beside itself.
constexpr uint64_t space_margin = 1024 * 1024;

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  Bytes out((std::istreambuf_iterator<char>(in)), {});
  if (in.bad()) throw std::runtime_error("cannot read " + path.string());
  return out;
}

std::string digest_text(std::span<const uint8_t> bytes) { return std::to_string(bytes.size()) + " bytes sha256 " + hex(sha256(bytes)); }

void sync_directory(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot open " + path.string() + ": " + std::strerror(errno));
  int rc;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  const auto error = errno;
  ::close(fd);
  if (rc != 0) throw std::runtime_error("cannot sync " + path.string() + ": " + std::strerror(error));
}

class Fd {
 public:
  Fd(const std::filesystem::path& path, int flags, mode_t mode = 0600) : _fd(::open(path.c_str(), flags | O_CLOEXEC, mode)) {
    if (_fd < 0) throw std::runtime_error("cannot open " + path.string() + ": " + std::strerror(errno));
  }
  ~Fd() { ::close(_fd); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const noexcept { return _fd; }

 private:
  int _fd;
};

void write_at(int fd, std::span<const uint8_t> bytes, uint64_t offset, const std::filesystem::path& path) {
  size_t done = 0;
  while (done < bytes.size()) {
    const auto n = ::pwrite(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("cannot write " + path.string() + ": " + std::strerror(errno));
    done += static_cast<size_t>(n);
  }
}

void sync_fd(int fd, const std::filesystem::path& path) {
  int rc;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) throw std::runtime_error("cannot sync " + path.string() + ": " + std::strerror(errno));
}

// What dagda-migrate's second run removes (migration_temporary).
std::filesystem::path temporary_beside(const std::filesystem::path& path) {
  static std::atomic_uint64_t next{0};
  return path.string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(next.fetch_add(1));
}

// A sealed file's parts: magic, nonce, tag, then the ciphertext as a u32-length blob.
struct SealedParts {
  std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> tag{};
  Bytes ciphertext;
};

SealedParts parse_sealed(const Magic& magic, std::span<const uint8_t> file) {
  Reader reader(file);
  const auto found = reader.view(magic.size());
  if (!std::equal(found.begin(), found.end(), magic.begin())) throw std::runtime_error("bad file header");
  SealedParts out;
  out.nonce = reader.fixed<12>();
  out.tag = reader.fixed<16>();
  out.ciphertext = reader.bytes();
  reader.finish();
  return out;
}

Bytes sealed_bytes(const Magic& magic, const SealedParts& parts) {
  Writer writer;
  writer.raw(magic);
  writer.fixed(parts.nonce);
  writer.fixed(parts.tag);
  writer.bytes(parts.ciphertext);
  return writer.take();
}

std::optional<Bytes> try_open(const domain::StorageKey& key, const Magic& magic, std::span<const uint8_t> aad, std::span<const uint8_t> file) {
  try {
    return heads::open_file(key, magic, aad, file);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void step(const MigrationOptions& options, const MigrationReport& report) {
  if (options.after_step) options.after_step(report);
}

void say(const MigrationOptions& options, const std::string& line) {
  if (options.say) options.say(line);
}

// One layout's forms for a space: the key and associated data of each file.
struct Forms {
  heads::SpaceId space{};
  bool v19 = false;
  const domain::StorageKey* content = nullptr;
  const domain::StorageKey* standing = nullptr;
  Bytes aad(const Magic& magic) const { return v19 ? v19_aad(space, magic) : v18_aad(magic); }
  Bytes seed_aad(const Magic& magic) const { return v19 ? v19_seed_aad(space, magic) : v18_aad(magic); }
};

// ---- The pass -------------------------------------------------------------------

enum class Form : uint8_t { absent, from, to, neither };

struct FramesPlan {
  std::filesystem::path path;
  Magic magic{};
  uint64_t size = 0;
  uint64_t from = 0;
  uint64_t to = 0;
  uint64_t damaged = 0;
  uint64_t damaged_bytes = 0;
  uint64_t tail_bytes = 0;
  std::vector<std::string> carried;
};

struct SealedPlan {
  std::filesystem::path path;
  Magic magic{};
  Form form = Form::absent;
};

class HeadsPass {
 public:
  HeadsPass(const std::filesystem::path& state, const domain::StorageKey& domain_key, const domain::StorageKey& node_local, const MigrationOptions& options,
            MigrationReport& report)
      : _options(options),
        _report(report),
        _places(heads_places(state, options.cache, options.space_name)),
        _state(state),
        _domain_key(domain_key),
        _deriver(domain_key) {
    _v18.space = _v19.space = _places.space;
    _v18.content = _v19.content = &domain_key;
    _v18.standing = &domain_key;
    _v19.v19 = true;
    _v19.standing = &node_local;
    _from = options.reverse ? &_v19 : &_v18;
    _to = options.reverse ? &_v18 : &_v19;
    _from_dir = options.reverse ? _places.v19_directory : _places.v18_directory;
    _to_dir = options.reverse ? _places.v18_directory : _places.v19_directory;
    _from_seed = options.reverse ? _places.v19_seed : _places.v18_seed;
    _to_seed = options.reverse ? _places.v18_seed : _places.v19_seed;
  }

  void plan();
  void run();

 private:
  std::string direction() const { return _options.reverse ? "back to v18" : "to v19"; }
  void classify_frames(FramesPlan& plan);
  void classify_sealed(SealedPlan& plan);
  void classify_set_aside();
  void classify_seed();
  void note_frames(const FramesPlan& plan);
  void rewrite_frames(const FramesPlan& plan);
  void move_checkpoint();
  void reseal(const SealedPlan& plan);
  void move_set_aside();
  void move_seed();
  void move_directory();

  const MigrationOptions& _options;
  MigrationReport& _report;
  const HeadsPlaces _places;
  const std::filesystem::path _state;
  const domain::StorageKey& _domain_key;
  const TagDeriver _deriver;
  Forms _v18;
  Forms _v19;
  const Forms* _from = nullptr;
  const Forms* _to = nullptr;
  std::filesystem::path _from_dir;
  std::filesystem::path _to_dir;
  std::filesystem::path _from_seed;
  std::filesystem::path _to_seed;

  bool _have_from_dir = false;
  bool _have_to_dir = false;
  // The directory the files are in, whichever of the two.
  std::filesystem::path _work_dir;
  bool _proved = false;
  bool _sealed_content = false;
  FramesPlan _history;
  FramesPlan _journal;
  SealedPlan _checkpoint;
  SealedPlan _heads;
  SealedPlan _author;
  Form _set_aside = Form::absent;
  Form _seed_from = Form::absent;
  Form _seed_to = Form::absent;
  uint64_t _peak = 0;
};

void HeadsPass::classify_frames(FramesPlan& plan) {
  std::error_code ec;
  if (!std::filesystem::exists(plan.path, ec)) return;
  plan.size = std::filesystem::file_size(plan.path);
  if (plan.size) _sealed_content = true;
  const std::vector<Bytes> aads{_from->aad(plan.magic), _to->aad(plan.magic)};
  walk_frames(plan.path, _domain_key, aads, [&](const FrameSpan& span, const Bytes*) {
    switch (span.kind) {
      case FrameSpan::Kind::frame:
        ++(span.opened_by == 0 ? plan.from : plan.to);
        break;
      case FrameSpan::Kind::damaged:
        ++plan.damaged;
        plan.damaged_bytes += span.length;
        plan.carried.push_back(plan.path.filename().string() + " offset " + std::to_string(span.offset) + ": " + std::to_string(span.length) +
                               " damaged bytes between frames, carried as they are");
        break;
      case FrameSpan::Kind::tail:
        plan.tail_bytes += span.length;
        plan.carried.push_back(plan.path.filename().string() + " offset " + std::to_string(span.offset) + ": a torn tail of " + std::to_string(span.length) +
                               " bytes, carried as it is for the reader to cut");
        break;
    }
  });
  if (plan.from + plan.to) _proved = true;
  if (plan.from && plan.to)
    throw MigrationRefused(plan.path.string() + " holds frames of both layouts (" + std::to_string(plan.from) + " and " + std::to_string(plan.to) +
                           "): no run of this tool leaves that, so it is not this tool's to judge");
}

void HeadsPass::classify_sealed(SealedPlan& plan) {
  std::error_code ec;
  if (!std::filesystem::exists(plan.path, ec)) return;
  const auto bytes = read_file(plan.path);
  _sealed_content = true;
  const bool content = plan.magic == macha_metadata::checkpoint_magic;
  const auto& from_key = content ? *_from->content : *_from->standing;
  const auto& to_key = content ? *_to->content : *_to->standing;
  if (try_open(from_key, plan.magic, _from->aad(plan.magic), bytes)) {
    plan.form = Form::from;
  } else if (try_open(to_key, plan.magic, _to->aad(plan.magic), bytes)) {
    plan.form = Form::to;
  } else {
    plan.form = Form::neither;
    return;
  }
  _proved = true;
}

void HeadsPass::classify_set_aside() {
  const auto path = _work_dir / set_aside_name;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return;
  const auto bytes = read_file(path);
  // Sealed is v19's form; anything else is Macha's plain bytes, carried whatever
  // they hold.
  const bool sealed = try_open(*_v19.standing, macha_metadata::set_aside_magic, _v19.aad(macha_metadata::set_aside_magic), bytes).has_value();
  if (sealed) _proved = true;
  _set_aside = sealed == _options.reverse ? Form::from : Form::to;
}

void HeadsPass::classify_seed() {
  std::error_code ec;
  const auto form_of = [&](const std::filesystem::path& path, const Forms& forms) {
    if (path.empty() || !std::filesystem::exists(path, ec)) return Form::absent;
    const auto bytes = read_file(path);
    if (try_open(*forms.content, macha_metadata::seed_magic, forms.seed_aad(macha_metadata::seed_magic), bytes)) {
      _proved = true;
      return Form::from;
    }
    return Form::neither;
  };
  _seed_from = form_of(_from_seed, *_from);
  const auto to = form_of(_to_seed, *_to);
  _seed_to = to == Form::from ? Form::to : to;
}

void HeadsPass::plan() {
  std::error_code ec;
  _have_from_dir = std::filesystem::is_directory(_from_dir, ec);
  _have_to_dir = std::filesystem::is_directory(_to_dir, ec);
  if (_have_from_dir && _have_to_dir)
    throw MigrationRefused("both " + _from_dir.string() + " and " + _to_dir.string() +
                           " exist: something wrote the space's new place before its metadata moved there. Nothing is written; compare the two by hand");
  // Files are judged where they are: a run stopped before the directory moved left
  // some of them moved in the old place, and one stopped going back left some moved
  // back in the new. Only the directory's place says which way is done.
  _work_dir = _have_from_dir ? _from_dir : _to_dir;
  const auto in_work = [&](const char* name) { return _work_dir / name; };
  _history.path = in_work(history_name);
  _history.magic = macha_metadata::history_magic;
  _journal.path = in_work(journal_name);
  _journal.magic = macha_metadata::journal_magic;
  _checkpoint = {in_work(checkpoint_name), macha_metadata::checkpoint_magic, Form::absent};
  _heads = {in_work(heads_name), macha_metadata::heads_magic, Form::absent};
  _author = {in_work(author_name), macha_metadata::author_magic, Form::absent};
  if (_have_from_dir || _have_to_dir) {
    classify_frames(_history);
    classify_frames(_journal);
    for (auto* plan : {&_checkpoint, &_heads, &_author}) {
      classify_sealed(*plan);
      if (plan->form == Form::neither)
        throw MigrationRefused(plan->path.string() + " opens under neither layout's key and associated data" +
                               (_options.reverse
                                    ? std::string(": nothing is written. Open the node once on Dagda, which recovers it from the seed or from peers, "
                                                  "and run again")
                                    : std::string(": nothing is written. Open it once under Macha 0.93.4, which recovers from the seed or from "
                                                  "peers, and run again")));
    }
    classify_set_aside();
  }
  classify_seed();
  _report.metadata_started = _have_to_dir || _history.to || _journal.to || _checkpoint.form == Form::to || _heads.form == Form::to ||
                             _author.form == Form::to || _set_aside == Form::to || _seed_to == Form::to;
  if (_sealed_content && !_proved)
    throw MigrationRefused("nothing in " + _work_dir.string() + " opens under the cluster key or the node's custody: the wrong key, or not this node's state");
  // The one file written whole beside itself at a time.
  for (const auto* frames : {&_history, &_journal})
    if (frames->from) _peak = std::max(_peak, frames->size);
  if (_peak && !_options.dry_run) {
    const auto room = std::filesystem::space(_work_dir, ec);
    if (ec) throw MigrationRefused("cannot tell the free space under " + _work_dir.string() + ": " + ec.message());
    if (room.available < _peak + space_margin)
      throw MigrationRefused(_work_dir.string() + " has " + std::to_string(room.available) + " bytes free and the metadata pass needs " +
                             std::to_string(_peak + space_margin) + ": one history file beside itself");
  }
}

void HeadsPass::note_frames(const FramesPlan& plan) {
  _report.metadata_frames_migrated += plan.from;
  _report.metadata_frames_already += plan.to;
  _report.metadata_damaged_spans += plan.damaged;
  _report.metadata_damaged_bytes += plan.damaged_bytes;
  _report.metadata_tail_bytes += plan.tail_bytes;
  for (const auto& what : plan.carried) _report.unreadable.push_back(what);
}

// The file written whole beside itself, every frame's tag moved, then read again
// as the replica would read it under the new layout: the same frames at the same
// offsets and the same bytes between them, or nothing is renamed.
void HeadsPass::rewrite_frames(const FramesPlan& plan) {
  const auto from_aad = _from->aad(plan.magic);
  const auto to_aad = _to->aad(plan.magic);
  const auto temporary = temporary_beside(plan.path);
  try {
    {
      Fd in(plan.path, O_RDONLY);
      Fd out(temporary, O_WRONLY | O_CREAT | O_EXCL);
      Bytes buffer(1024 * 1024);
      uint64_t copied = 0;
      for (;;) {
        const auto n = ::read(in.get(), buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw std::runtime_error("cannot read " + plan.path.string() + ": " + std::strerror(errno));
        if (n == 0) break;
        write_at(out.get(), std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n)), copied, temporary);
        copied += static_cast<uint64_t>(n);
      }
      if (copied != plan.size) throw std::runtime_error(plan.path.string() + " changed size while it was read");
      _report.bytes_read += copied;
    }
    struct Seen {
      FrameSpan::Kind kind;
      uint64_t offset;
      uint64_t length;
      bool operator==(const Seen&) const = default;
    };
    std::vector<Seen> before;
    {
      Fd out(temporary, O_RDWR);
      walk_frames(plan.path, _domain_key, {from_aad, to_aad}, [&](const FrameSpan& span, const Bytes*) {
        before.push_back({span.kind, span.offset, span.length});
        if (span.kind != FrameSpan::Kind::frame) return;
        if (span.opened_by != 0) throw std::runtime_error(plan.path.string() + " gained a frame of the other layout while it was read");
        const auto tag = _deriver.derive(span.tag, from_aad, to_aad, span.ciphertext_bytes);
        write_at(out.get(), tag, span.offset + 4 + 12, temporary);
        _report.bytes_written += tag.size();
      });
      sync_fd(out.get(), temporary);
    }
    std::vector<Seen> after;
    walk_frames(temporary, _domain_key, {to_aad}, [&](const FrameSpan& span, const Bytes*) { after.push_back({span.kind, span.offset, span.length}); });
    if (before != after) throw std::runtime_error(plan.path.string() + " would not read under the new layout as it read under the old: left as it was");
    std::filesystem::rename(temporary, plan.path);
    sync_directory(plan.path.parent_path());
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

void HeadsPass::move_checkpoint() {
  const auto& magic = _checkpoint.magic;
  auto parts = parse_sealed(magic, read_file(_checkpoint.path));
  const auto from_aad = _from->aad(magic);
  const auto to_aad = _to->aad(magic);
  parts.tag = _deriver.derive(parts.tag, from_aad, to_aad, parts.ciphertext.size());
  const auto bytes = sealed_bytes(magic, parts);
  // Opened once more under the new form before it replaces the old.
  (void)heads::open_file(*_to->content, magic, to_aad, bytes);
  dannan::identity::durable_replace_file(_checkpoint.path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  _report.bytes_written += bytes.size();
}

void HeadsPass::reseal(const SealedPlan& plan) {
  const auto plaintext = heads::open_file(*_from->standing, plan.magic, _from->aad(plan.magic), read_file(plan.path));
  const auto bytes = heads::seal_file(*_to->standing, plan.magic, _to->aad(plan.magic), plaintext);
  dannan::identity::durable_replace_file(plan.path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  _report.bytes_written += bytes.size();
}

void HeadsPass::move_set_aside() {
  const auto path = _work_dir / set_aside_name;
  const auto& magic = macha_metadata::set_aside_magic;
  Bytes bytes;
  if (_options.reverse) {
    bytes = heads::open_file(*_v19.standing, magic, _v19.aad(magic), read_file(path));
  } else {
    bytes = heads::seal_file(*_v19.standing, magic, _v19.aad(magic), read_file(path));
  }
  dannan::identity::durable_replace_file(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  _report.bytes_written += bytes.size();
}

void HeadsPass::move_seed() {
  const auto& magic = macha_metadata::seed_magic;
  auto parts = parse_sealed(magic, read_file(_from_seed));
  const auto from_aad = _from->seed_aad(magic);
  const auto to_aad = _to->seed_aad(magic);
  parts.tag = _deriver.derive(parts.tag, from_aad, to_aad, parts.ciphertext.size());
  const auto bytes = sealed_bytes(magic, parts);
  (void)heads::open_file(*_to->content, magic, to_aad, bytes);
  dannan::identity::durable_replace_file(_to_seed, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  _report.bytes_written += bytes.size();
}

void HeadsPass::move_directory() {
  const auto parent = _to_dir.parent_path();
  std::filesystem::create_directories(parent);
  sync_directory(parent.parent_path());
  std::filesystem::rename(_from_dir, _to_dir);
  sync_directory(parent);
  sync_directory(_from_dir.parent_path());
  if (_options.reverse) {
    // Going back leaves no empty heads/ for Macha to find.
    std::error_code ec;
    const auto heads_root = _from_dir.parent_path();
    if (std::filesystem::is_directory(heads_root, ec) && std::filesystem::is_empty(heads_root, ec)) {
      std::filesystem::remove(heads_root, ec);
      if (!ec) sync_directory(heads_root.parent_path());
    }
  }
}

void HeadsPass::run() {
  plan();
  const bool write = !_options.dry_run;
  _report.heads_space = _options.space_name + " " + hex(_places.space);
  _report.metadata_peak_extra_bytes = std::max(_report.metadata_peak_extra_bytes, _peak);

  // The seed, beside the cache: written at its new name, then the old removed.
  if (_seed_from == Form::from) {
    say(_options, "metadata seed " + _from_seed.string() + " to " + _to_seed.string());
    if (write) move_seed();
    _report.metadata_seed = "migrated";
    step(_options, _report);
    if (write) {
      std::filesystem::remove(_from_seed);
      sync_directory(_from_seed.parent_path());
    }
    step(_options, _report);
  } else if (_seed_from == Form::neither) {
    _report.metadata_seed = "unreadable";
    _report.unreadable.push_back(_from_seed.string() + ": a seed that opens under neither layout, left as it was");
  } else if (_seed_to == Form::to) {
    _report.metadata_seed = "already";
  } else if (_seed_to == Form::neither) {
    _report.metadata_seed = "unreadable";
    _report.unreadable.push_back(_to_seed.string() + ": a seed that opens under neither layout, left as it was");
  } else {
    _report.metadata_seed = "none";
  }

  if (!_have_from_dir && !_have_to_dir) {
    _report.metadata_directory = "none";
    return;
  }
  say(_options, "metadata " + _work_dir.string() + " " + direction());
  for (const auto* frames : {&_history, &_journal}) {
    note_frames(*frames);
    if (!frames->from) continue;
    if (write) rewrite_frames(*frames);
    ++_report.metadata_files_migrated;
    step(_options, _report);
  }
  for (const auto* frames : {&_history, &_journal})
    if (!frames->from && frames->size) ++_report.metadata_files_already;
  for (const auto* plan : {&_checkpoint, &_heads, &_author}) {
    if (plan->form == Form::to) ++_report.metadata_files_already;
    if (plan->form != Form::from) continue;
    if (write) {
      if (plan == &_checkpoint)
        move_checkpoint();
      else
        reseal(*plan);
    }
    ++_report.metadata_files_migrated;
    step(_options, _report);
  }
  if (_set_aside == Form::to) ++_report.metadata_files_already;
  if (_set_aside == Form::from) {
    if (write) move_set_aside();
    ++_report.metadata_files_migrated;
    step(_options, _report);
  }
  // Last, the directory: present at its new place is done.
  if (!_have_from_dir) {
    _report.metadata_directory = "already";
    return;
  }
  if (write) move_directory();
  _report.metadata_directory = "moved";
  say(_options, "metadata directory " + _from_dir.string() + " is now " + _to_dir.string());
  step(_options, _report);
}

// ---- The reader -----------------------------------------------------------------

struct Indexed {
  uint64_t generation = 0;
  heads::Hash256 previous{};
  bool full = false;
};

std::string hash_text(const heads::Hash256& hash) { return hex(hash); }

std::string zero_padded(uint64_t value) {
  std::string text = std::to_string(value);
  return std::string(20 - std::min<size_t>(20, text.size()), '0') + text;
}

MetadataFact fact(std::string kind, std::string name, std::string value, bool unknown = false) {
  MetadataFact out;
  out.kind = std::move(kind);
  out.name = std::move(name);
  out.value = Bytes(value.begin(), value.end());
  out.unknown = unknown;
  return out;
}

// Every span of a file of frames, and for history the index replay would build.
void read_frames(const std::string& kind, const std::filesystem::path& path, const domain::StorageKey& key, const Bytes& aad, std::vector<MetadataFact>& out,
                 std::map<heads::Hash256, Indexed>* index) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return;
  walk_frames(path, key, {aad}, [&](const FrameSpan& span, const Bytes* plaintext) {
    const auto at = zero_padded(span.offset);
    if (span.kind != FrameSpan::Kind::frame) {
      std::ifstream in(path, std::ios::binary);
      in.seekg(static_cast<std::streamoff>(span.offset));
      Bytes bytes(static_cast<size_t>(span.length));
      if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("cannot read " + path.string());
      if (span.kind == FrameSpan::Kind::damaged) {
        out.push_back(fact(kind, at + " damaged", "damaged bytes between frames, " + digest_text(bytes), true));
      } else {
        out.push_back(fact(kind, at + " tail", "a torn tail the reader cuts, " + digest_text(bytes)));
      }
      return;
    }
    std::ostringstream text;
    try {
      const auto entry = heads::decode_history_entry(*plaintext);
      text << "generation " << entry.generation << " hash " << hash_text(entry.hash) << " previous " << hash_text(entry.previous) << " known "
           << (entry.previous_known ? 1 : 0) << " body " << (entry.body == heads::HistoryEntry::Body::full ? "full" : "delta") << " parents";
      for (const auto& parent : entry.merge_parents) text << " " << hash_text(parent);
      text << " payload " << digest_text(entry.payload) << "; ";
      bool indexed = false;
      if (entry.body == heads::HistoryEntry::Body::full) {
        if (heads::record_hash(entry.generation, entry.previous, entry.payload) == entry.hash) {
          text << "its hash recomputes";
          indexed = true;
        } else {
          text << "its hash does not recompute";
        }
      } else if (index) {
        const auto parent = index->find(entry.previous);
        if (entry.previous_known && entry.generation > 1 && parent != index->end() &&
            heads::delta_succession_valid(parent->second.generation, entry.generation)) {
          text << "a delta on a parent held before it";
          indexed = true;
        } else {
          text << "a delta with no parent before it: the reader skips it";
        }
      } else {
        text << "a delta";
      }
      if (index && indexed) {
        auto& slot = (*index)[entry.hash];
        // A whole body is preferred over a delta of the same hash, as the reader's.
        if (!slot.full) slot = {entry.generation, entry.previous, entry.body == heads::HistoryEntry::Body::full};
      }
    } catch (const std::exception& error) {
      text << "authenticates and does not decode (" << error.what() << "): " << digest_text(*plaintext);
    }
    out.push_back(fact(kind, at + " frame", text.str()));
  });
}

// How history replays a head, as far as Dagda can tell without the owner's codec:
// the chain of deltas down to a whole record.
std::string replay_text(const heads::Hash256& head, const std::map<heads::Hash256, Indexed>& index) {
  auto at = head;
  uint64_t deltas = 0;
  for (size_t guard = 0; guard <= index.size(); ++guard) {
    const auto found = index.find(at);
    if (found == index.end()) {
      if (deltas == 0) return "not in history";
      return "its chain breaks at " + hash_text(at) + " after " + std::to_string(deltas) + " deltas";
    }
    if (found->second.full) return "replays from the whole record " + hash_text(at) + " through " + std::to_string(deltas) + " deltas";
    ++deltas;
    at = found->second.previous;
  }
  return "its chain loops";
}

}  // namespace

HeadsPlaces heads_places(const std::filesystem::path& state, const std::filesystem::path& cache, std::string_view space_name) {
  HeadsPlaces out;
  out.space = heads::space_id(domain::domain_v1, space_name);
  out.v18_directory = state / "metadata";
  out.v19_directory = heads::space_directory(state, out.space);
  if (!cache.empty()) {
    out.v18_seed = cache / "metadata" / seed_v18_name;
    out.v19_seed = cache / "metadata" / (hex(out.space) + ".meta");
  }
  return out;
}

Bytes v18_aad(const heads::Magic& magic) { return Bytes(magic.begin(), magic.end()); }

Bytes v19_aad(const heads::SpaceId& space, const heads::Magic& magic) { return heads::replica_aad(domain::domain_v1, space)(magic); }

Bytes v19_seed_aad(const heads::SpaceId& space, const heads::Magic& magic) { return heads::seed_aad(domain::domain_v1, space, magic); }

void walk_frames(const std::filesystem::path& path, const domain::StorageKey& key, const std::vector<Bytes>& aads,
                 const std::function<void(const FrameSpan&, const Bytes* plaintext)>& visit) {
  const uint64_t size = std::filesystem::file_size(path);
  std::ifstream stream(path, std::ios::binary);
  std::ifstream scan(path, std::ios::binary);
  if (!stream || !scan) throw std::runtime_error("cannot open " + path.string());
  size_t opened_by = 0;
  const auto open_any = [&](std::span<const uint8_t> envelope) -> std::optional<Bytes> {
    for (size_t i = 0; i < aads.size(); ++i) {
      if (auto plaintext = heads::open_frame(key, aads[i], envelope)) {
        opened_by = i;
        return plaintext;
      }
    }
    return std::nullopt;
  };
  uint64_t offset = 0;
  while (offset + 4 <= size) {
    std::array<uint8_t, 4> word{};
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream.read(reinterpret_cast<char*>(word.data()), word.size())) throw std::runtime_error("cannot read " + path.string());
    Reader header(word);
    const uint64_t length = header.u32();
    std::optional<Bytes> plaintext;
    Bytes frame;
    if (length <= heads::max_history_frame_bytes && offset + 4 + length <= size) {
      frame.resize(static_cast<size_t>(length));
      if (length && !stream.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(length)))
        throw std::runtime_error("cannot read " + path.string());
      plaintext = open_any(frame);
    }
    if (!plaintext) {
      bool read_failed = false;
      const auto next = heads::next_frame_from(scan, size, offset + 1, heads::max_history_frame_bytes, open_any, read_failed);
      if (read_failed) throw std::runtime_error("cannot read frames from " + path.string());
      FrameSpan span;
      span.offset = offset;
      if (!next) {
        span.kind = FrameSpan::Kind::tail;
        span.length = size - offset;
        visit(span, nullptr);
        return;
      }
      span.kind = FrameSpan::Kind::damaged;
      span.length = *next - offset;
      visit(span, nullptr);
      offset = *next;
      continue;
    }
    const auto envelope = heads::parse_frame_envelope(frame);
    FrameSpan span;
    span.kind = FrameSpan::Kind::frame;
    span.offset = offset;
    span.length = 4 + length;
    span.opened_by = opened_by;
    span.nonce = envelope.nonce;
    span.tag = envelope.tag;
    span.ciphertext_bytes = envelope.ciphertext.size();
    visit(span, &*plaintext);
    offset += 4 + length;
  }
  if (offset < size) {
    FrameSpan span;
    span.kind = FrameSpan::Kind::tail;
    span.offset = offset;
    span.length = size - offset;
    visit(span, nullptr);
  }
}

void migrate_heads_metadata(const std::filesystem::path& state, const domain::StorageKey& domain_key, const domain::StorageKey& node_local,
                            const MigrationOptions& options, MigrationReport& report, bool plan_only) {
  HeadsPass pass(state, domain_key, node_local, options, report);
  if (plan_only) {
    pass.plan();
    return;
  }
  pass.run();
}

bool heads_metadata_pending(const std::filesystem::path& state, const std::filesystem::path& cache, std::string_view space_name) {
  const auto places = heads_places(state, cache, space_name);
  std::error_code ec;
  return std::filesystem::exists(places.v18_directory, ec) || (!places.v18_seed.empty() && std::filesystem::exists(places.v18_seed, ec));
}

HeadsMetadataReader::HeadsMetadataReader(std::string space_name)
    : _space_name(std::move(space_name)), _space_hex(hex(heads::space_id(domain::domain_v1, _space_name))) {}

bool HeadsMetadataReader::covers(const std::filesystem::path&, const std::filesystem::path& relative) const {
  auto part = relative.begin();
  if (part == relative.end()) return false;
  // The state's metadata/, and the cache's, where its seed is.
  if (*part == "metadata") return true;
  if (*part != "heads") return false;
  ++part;
  return part != relative.end() && *part == _space_hex;
}

void HeadsMetadataReader::read(const MetadataManifestInput& input, const std::function<void(const MetadataFact&)>& emit) {
  if (!input.domain_key || !input.node_local_key) throw std::invalid_argument("the metadata reader needs both keys");
  const auto places = heads_places(input.state, input.cache, _space_name);
  std::error_code ec;
  const bool v19 = std::filesystem::is_directory(places.v19_directory, ec);
  const bool v18 = std::filesystem::is_directory(places.v18_directory, ec);
  if (v19 && v18) throw std::runtime_error("both " + places.v18_directory.string() + " and " + places.v19_directory.string() + " exist");
  Forms forms;
  forms.space = places.space;
  forms.v19 = v19;
  forms.content = input.domain_key.get();
  forms.standing = v19 ? input.node_local_key.get() : input.domain_key.get();
  std::vector<MetadataFact> facts;

  const auto sealed = [&](const std::string& kind, const std::filesystem::path& path, const Magic& magic, const domain::StorageKey& key,
                          const Bytes& aad) -> std::optional<Bytes> {
    const auto bytes = read_file(path);
    if (auto plaintext = try_open(key, magic, aad, bytes)) return plaintext;
    facts.push_back(fact(kind, path.filename().string(), "opens under neither form, " + digest_text(bytes), true));
    return std::nullopt;
  };

  if (v19 || v18) {
    const auto dir = v19 ? places.v19_directory : places.v18_directory;
    using namespace macha_metadata;
    if (std::filesystem::exists(dir / author_name)) {
      if (const auto plain = sealed("author", dir / author_name, author_magic, *forms.standing, forms.aad(author_magic)))
        facts.push_back(fact("author", author_name, hex(*plain)));
    }
    std::vector<heads::Acceptance> certificates;
    if (std::filesystem::exists(dir / heads_name)) {
      if (const auto plain = sealed("certificates", dir / heads_name, heads_magic, *forms.standing, forms.aad(heads_magic))) {
        try {
          certificates = heads::decode_acceptance_set(*plain);
        } catch (const std::exception& error) {
          facts.push_back(fact("certificates", heads_name, std::string("opens and does not decode (") + error.what() + "): " + hex(*plain)));
        }
      }
    }
    for (const auto& certificate : certificates) {
      std::ostringstream text;
      text << "generation " << certificate.generation << " required " << certificate.required << " replicas";
      for (const auto& replica : certificate.replicas) text << " " << dannan::identity::to_string(replica);
      facts.push_back(fact("certificates", hash_text(certificate.hash), text.str()));
    }
    if (std::filesystem::exists(dir / checkpoint_name)) {
      if (const auto plain = sealed("checkpoint", dir / checkpoint_name, checkpoint_magic, *forms.content, forms.aad(checkpoint_magic))) {
        try {
          const auto record = heads::decode_record(*plain);
          facts.push_back(fact("checkpoint", checkpoint_name,
                               "generation " + std::to_string(record.generation) + " hash " + hash_text(record.hash) + " previous " +
                                   hash_text(record.previous) + " payload " + digest_text(record.payload.span())));
        } catch (const std::exception& error) {
          facts.push_back(fact("checkpoint", checkpoint_name, std::string("opens and does not decode (") + error.what() + "): " + hex(*plain)));
        }
      }
    }
    std::map<heads::Hash256, Indexed> index;
    read_frames("history", dir / history_name, *forms.content, forms.aad(history_magic), facts, &index);
    for (const auto& certificate : certificates) facts.push_back(fact("heads", hash_text(certificate.hash), replay_text(certificate.hash, index)));
    read_frames("journal", dir / journal_name, *forms.content, forms.aad(journal_magic), facts, nullptr);
    std::vector<std::filesystem::path> others;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_directory()) continue;
      const auto relative = it->path().lexically_relative(dir);
      const auto name = relative.generic_string();
      if (name == history_name || name == journal_name || name == checkpoint_name || name == heads_name || name == author_name || name == set_aside_name)
        continue;
      others.push_back(relative);
    }
    if (ec) throw std::runtime_error("cannot walk " + dir.string() + ": " + ec.message());
    for (const auto& relative : others) {
      if (!std::filesystem::is_regular_file(dir / relative) || std::filesystem::is_symlink(dir / relative)) {
        facts.push_back(fact("markers", relative.generic_string(), "not a regular file", true));
        continue;
      }
      facts.push_back(fact("markers", relative.generic_string(), digest_text(read_file(dir / relative))));
    }
    if (std::filesystem::exists(dir / set_aside_name)) {
      if (v19) {
        if (const auto plain = sealed("set-aside", dir / set_aside_name, set_aside_magic, *forms.standing, forms.aad(set_aside_magic)))
          facts.push_back(fact("set-aside", set_aside_name, hex(*plain)));
      } else {
        facts.push_back(fact("set-aside", set_aside_name, hex(read_file(dir / set_aside_name))));
      }
    }
  }

  // The seed, at either name, and anything else beside it.
  if (!input.cache.empty()) {
    const auto seed_dir = input.cache / "metadata";
    bool named = false;
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(seed_dir, ec)) {
      for (const auto& entry : std::filesystem::directory_iterator(seed_dir, ec)) files.push_back(entry.path());
      if (ec) throw std::runtime_error("cannot list " + seed_dir.string() + ": " + ec.message());
    }
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
      const auto name = path.filename().string();
      if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(path)) {
        facts.push_back(fact("seed", "file " + name, "not a regular file", true));
        continue;
      }
      const auto bytes = read_file(path);
      const bool old_name = path == places.v18_seed;
      const bool new_name = path == places.v19_seed;
      if (!old_name && !new_name) {
        facts.push_back(fact("seed", "file " + name, digest_text(bytes)));
        continue;
      }
      const auto aad = old_name ? v18_aad(macha_metadata::seed_magic) : v19_seed_aad(places.space, macha_metadata::seed_magic);
      const auto plain = try_open(*input.domain_key, macha_metadata::seed_magic, aad, bytes);
      if (!plain) {
        facts.push_back(fact("seed", "file " + name, "opens under neither form, " + digest_text(bytes), true));
        continue;
      }
      if (named) throw std::runtime_error("the seed is under both names in " + seed_dir.string() + ": a migration stopped part way");
      named = true;
      try {
        const auto record = heads::decode_record(*plain);
        facts.push_back(fact("seed", "seed",
                             "generation " + std::to_string(record.generation) + " hash " + hash_text(record.hash) + " previous " + hash_text(record.previous) +
                                 " payload " + digest_text(record.payload.span())));
      } catch (const std::exception& error) {
        facts.push_back(fact("seed", "seed", std::string("opens and does not decode (") + error.what() + "): " + hex(*plain)));
      }
    }
  }

  const auto& order = metadata_kinds();
  const auto rank = [&](const std::string& kind) { return std::find(order.begin(), order.end(), kind) - order.begin(); };
  std::stable_sort(facts.begin(), facts.end(), [&](const MetadataFact& a, const MetadataFact& b) {
    if (a.kind != b.kind) return rank(a.kind) < rank(b.kind);
    return a.name < b.name;
  });
  for (const auto& each : facts) emit(each);
}

}  // namespace dagda::migrate
