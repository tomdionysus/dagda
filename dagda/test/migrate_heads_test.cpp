//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Macha's metadata moved by dagda-migrate (the heads plan's section 4, cases C41 to
// C46): forward, at every step a crash can stop it, back, over real states Macha
// 0.93.4 wrote and over a state holding every file kind; history damaged between
// frames carried byte for byte; and the comparison's metadata, with a damage test
// per item. Real states are read from Macha's fixtures and skip, saying so, on a
// machine without them; the checked-in hex states always run.
//
// Macha's records stay opaque: an owner that reads a payload's clock and merge
// parents and no more (fixtures/macha_opaque.h). No torrent or filesystem form is
// parsed here.

#include <gtest/gtest.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "dagda/heads/replica.h"
#include "dagda/heads/sealed_file.h"
#include "dagda/migrate/heads_metadata.h"
#include "dagda/migrate/manifest.h"
#include "dagda/migrate/migrate.h"
#include "dagda/store/state_layout.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/macha_opaque.h"
#include "dagda/test/fixtures/migrate.h"
#include "dannan/test/fixtures/child_process.h"

namespace dagda::migrate {
namespace {

using namespace std::chrono_literals;
using dannan::identity::hex;
using dannan::net::Reader;
using dannan::net::Writer;
using heads::Magic;
using test::v18::bytes_of;
using test::v18::custody_source;
using test::v18::read_text;
namespace macha = test::macha;
namespace mm = macha_metadata;

const heads::SpaceId& machas_space() {
  static const auto space = heads::space_id(domain::domain_v1, mm::space_name);
  return space;
}

std::string v19_marker() { return store::state_layout_marker(store::SealingScheme::v19(domain::domain_v1)) + "\n"; }

// ---- A node's state, for the metadata pass ----------------------------------------

struct HeadsBed {
  explicit HeadsBed(std::string_view marker = "v19")
      : root(dir.path() / "node"),
        state(root / "state"),
        node_state(root / "node"),
        cache(root / "cache"),
        cluster_key_file(root / "cluster.key"),
        manifests(dir.path() / "manifests") {
    std::filesystem::create_directories(state);
    std::filesystem::create_directories(cache);
    std::filesystem::create_directories(manifests);
    (void)test::v18::cluster_key_from(cluster_key_file, macha::fixture_cluster_key);
    (void)dannan::identity::KeyCustody::create(node_state, custody_source());
    std::ofstream(node_state / "node-id") << dannan::identity::to_string(node) << "\n";
    if (marker == "v19") macha::write_file(state / "storage-layout", bytes_of(v19_marker()));
    if (marker == "v18") macha::write_file(state / "storage-layout", bytes_of("macha-state-layout-v18\n"));
    domain_key = std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_v1(cluster()));
    node_local = std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_node_local(custody()));
  }

  dannan::identity::ClusterKey cluster() const { return dannan::identity::load_cluster_key(cluster_key_file); }
  dannan::identity::KeyCustody custody() const { return dannan::identity::KeyCustody::open(node_state, custody_source()); }

  std::filesystem::path v18_dir() const { return state / "metadata"; }
  std::filesystem::path v19_dir() const { return heads::space_directory(state, machas_space()); }
  std::filesystem::path v18_seed() const { return cache / "metadata" / "current.meta"; }
  std::filesystem::path v19_seed() const { return cache / "metadata" / (hex(machas_space()) + ".meta"); }

  void write_metadata(const std::map<std::string, Bytes>& files) const {
    for (const auto& [name, bytes] : files) {
      if (name == "current.meta") {
        macha::write_file(v18_seed(), bytes);
      } else {
        macha::write_file(v18_dir() / name, bytes);
      }
    }
  }

  MigrationOptions options(bool heads = true) const {
    MigrationOptions out;
    out.cache = cache;
    out.heads = heads;
    out.node = node;
    return out;
  }
  MigrationReport forward(MigrationOptions given) const { return migrate_state(state, backends, cluster(), custody(), given); }
  MigrationReport forward() const { return forward(options()); }
  MigrationReport back(MigrationOptions given) const { return migrate_state_back(state, backends, cluster(), custody(), given); }
  MigrationReport back() const { return back(options(false)); }

  ManifestSummary pass(const std::string& name) const {
    ManifestOptions options;
    options.backends = backends;
    options.cache = cache;
    options.node = node;
    options.metadata = metadata_manifest_reader();
    return write_manifest(manifests / name, state, cluster(), custody(), options);
  }
  ComparisonReport compare(const std::string& before, const std::string& after, uint32_t from, uint32_t to) const {
    ComparisonOptions options;
    options.before_layout = from;
    options.after_layout = to;
    return compare_manifests(manifests / before, manifests / after, options);
  }

  std::map<std::filesystem::path, std::string> digest() const { return test::legacy::tree_digest(root); }

  dannan::test::TempDir dir;
  const std::filesystem::path root;
  const std::filesystem::path state;
  const std::filesystem::path node_state;
  const std::filesystem::path cache;
  const std::filesystem::path cluster_key_file;
  const std::filesystem::path manifests;
  dannan::identity::NodeId node = test::v18::filled(0xab);
  std::vector<std::filesystem::path> backends;
  std::shared_ptr<const domain::StorageKey> domain_key;
  std::shared_ptr<const domain::StorageKey> node_local;
};

// Two journal frames as an older Macha wrote them (0.93.4 writes the file only
// empty): the migration carries frames it does not read.
Bytes journal_frames(const domain::StorageKey& key) {
  Bytes out;
  for (const auto* text : {"a journal frame of an older version", "and a second"}) {
    const auto frame = heads::seal_frame(key, v18_aad(mm::journal_magic), bytes_of(text));
    out.insert(out.end(), frame.begin(), frame.end());
  }
  return out;
}

Bytes set_aside_plain(uint8_t fill, uint64_t since) {
  Writer writer;
  writer.u32(1);
  heads::Hash256 hash;
  hash.bytes.fill(fill);
  writer.fixed(hash.bytes);
  writer.u64(since);
  return writer.take();
}

// Macha's state-scratch node a, checked in, with every other file kind of the
// plan's section 4.1 added: journal frames, a plain set-aside.meta, the two text
// markers, a quarantine, and a real seed beside the cache.
std::map<std::string, Bytes> every_kind(const domain::StorageKey& key) {
  auto files = macha::read_checked_in("state-scratch");
  files["journal.log"] = journal_frames(key);
  files["set-aside.meta"] = set_aside_plain(0x77, 1793700000000ULL);
  files["recovery.required"] = bytes_of("metadata recovery: a reason, as text");
  files["author-chain-broken"] = bytes_of("the head set was replaced");
  files["history.log.corrupt.1793982986266051000"] = bytes_of("a quarantined tail Macha cut");
  files["current.meta"] = macha::read_checked_in("seed").at("current.meta");
  return files;
}

struct Sealed {
  std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> tag{};
  Bytes ciphertext;
};

Sealed sealed_parts(std::span<const uint8_t> file) {
  Reader reader(file);
  (void)reader.view(8);
  Sealed out;
  out.nonce = reader.fixed<12>();
  out.tag = reader.fixed<16>();
  out.ciphertext = reader.bytes();
  reader.finish();
  return out;
}

std::vector<FrameSpan> spans_of(const std::filesystem::path& path, const domain::StorageKey& key, const Bytes& aad) {
  std::vector<FrameSpan> out;
  walk_frames(path, key, {aad}, [&](const FrameSpan& span, const Bytes*) { out.push_back(span); });
  return out;
}

// A file of frames moved: the same size, every frame at its offset with its length,
// nonce and ciphertext, its tag the only bytes changed, and it opens under v19's
// associated data where it opened under v18's. Damaged spans and a tail are the same
// bytes at the same offsets. Returns the frames.
size_t expect_frames_moved(const Bytes& old_bytes, const std::filesystem::path& moved, const Magic& magic, const domain::StorageKey& key,
                           const std::filesystem::path& scratch) {
  macha::write_file(scratch, old_bytes);
  const auto before = spans_of(scratch, key, v18_aad(magic));
  const auto after = spans_of(moved, key, v19_aad(machas_space(), magic));
  const auto new_bytes = macha::read_file(moved);
  EXPECT_EQ(new_bytes.size(), old_bytes.size()) << moved;
  EXPECT_EQ(before.size(), after.size()) << moved;
  std::set<uint64_t> tag_bytes;
  size_t frames = 0;
  for (size_t i = 0; i < std::min(before.size(), after.size()); ++i) {
    EXPECT_EQ(before[i].kind, after[i].kind) << moved << " span " << i;
    EXPECT_EQ(before[i].offset, after[i].offset) << moved << " span " << i;
    EXPECT_EQ(before[i].length, after[i].length) << moved << " span " << i;
    if (before[i].kind != FrameSpan::Kind::frame) continue;
    ++frames;
    EXPECT_EQ(before[i].nonce, after[i].nonce);
    EXPECT_NE(before[i].tag, after[i].tag);
    for (uint64_t at = before[i].offset + 16; at < before[i].offset + 32; ++at) tag_bytes.insert(at);
  }
  for (size_t at = 0; at < std::min(old_bytes.size(), new_bytes.size()); ++at) {
    if (tag_bytes.contains(at)) continue;
    if (old_bytes[at] != new_bytes[at]) {
      ADD_FAILURE() << moved << ": byte " << at << " changed outside every tag";
      break;
    }
  }
  return frames;
}

// What a replica opens to, for comparing two of them.
struct Opened {
  heads::Hash256 committed{};
  uint64_t generation = 0;
  std::vector<heads::Acceptance> certificates;
  uint64_t history_records = 0;
  bool recovery = false;
  std::vector<heads::Hash256> accepted;
  std::optional<heads::Dot> next_dot;
  std::string failure;
  bool operator==(const Opened&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Opened& opened) {
  return out << "committed " << hex(opened.committed) << " generation " << opened.generation << " certificates " << opened.certificates.size() << " history "
             << opened.history_records << " recovery " << opened.recovery << " failure \"" << opened.failure << "\"";
}

heads::ReplicaOptions macha_options(const std::filesystem::path& directory, bool v19, const HeadsBed& bed, const std::shared_ptr<const heads::Codec>& codec) {
  heads::ReplicaOptions options;
  options.directory = directory;
  options.content_key = bed.domain_key;
  options.local_key = v19 ? bed.node_local : bed.domain_key;
  options.magics = mm::magics();
  if (v19) {
    options.aad = heads::replica_aad(domain::domain_v1, machas_space());
  } else {
    options.aad = [](const Magic& magic) { return v18_aad(magic); };
  }
  options.codec = codec;
  options.logger = test::quiet_logger();
  return options;
}

void copy_tree(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::filesystem::create_directories(to);
  std::filesystem::copy(from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
}

// Opens a replica as Dagda would at that layout, over a copy, since opening writes
// (a tail cut, an author reserved), and asks the author for its next dot: the same
// author and sequence on both sides is "the author continues".
Opened open_replica(const std::filesystem::path& source, bool v19, const HeadsBed& bed) {
  static std::atomic_uint64_t copies{0};
  const auto directory = bed.dir.path() / ("replica-" + std::to_string(copies.fetch_add(1)));
  copy_tree(source, directory);
  Opened out;
  try {
    heads::Replica replica(macha_options(directory, v19, bed, std::make_shared<macha::OpaqueCodec>()));
    out.committed = replica.committed().hash;
    out.generation = replica.committed_generation();
    out.certificates = replica.accepted_head_certificates();
    out.history_records = replica.diagnostics().history_records;
    out.recovery = replica.recovery_required();
    out.accepted = test::hashes_of(replica.accepted_heads());
    if (std::filesystem::exists(directory / "author.meta")) {
      const auto plain =
          v19 ? heads::open_file(*bed.node_local, mm::author_magic, v19_aad(machas_space(), mm::author_magic), macha::read_file(directory / "author.meta"))
              : heads::open_file(*bed.domain_key, mm::author_magic, v18_aad(mm::author_magic), macha::read_file(directory / "author.meta"));
      Reader reader(plain);
      const heads::NodeId author{reader.fixed<16>()};
      heads::HeadClock clock;
      try {
        clock = macha::opaque_fields(replica.committed().payload).clock;
      } catch (const std::exception&) {
      }
      out.next_dot = replica.reserve_mutation_dot(author, clock);
      // A broken chain (author-chain-broken) takes a random new author: that it did is
      // what compares.
      if (out.next_dot->author != author) out.next_dot = heads::Dot{};
    }
  } catch (const std::exception& error) {
    // Its text names the copy's path: whether it failed is what compares.
    std::cout << "[ replica ] " << source << " did not open here: " << error.what() << std::endl;
    out.failure = "did not open";
  }
  return out;
}

// ---- C41: forward ------------------------------------------------------------------

// Every file kind of section 4.1 on a state already v19 (stage one done): the
// metadata pass alone.
TEST(MigrateHeads, ForwardKeepsNonceAndCiphertextAndEveryFrameOpens) {
  HeadsBed bed;
  const auto files = every_kind(*bed.domain_key);
  bed.write_metadata(files);
  const auto report = bed.forward();

  EXPECT_FALSE(report.already_v19);
  EXPECT_EQ(read_text(bed.state / "storage-layout"), v19_marker());
  EXPECT_FALSE(std::filesystem::exists(bed.v18_dir()));
  ASSERT_TRUE(std::filesystem::is_directory(bed.v19_dir()));
  EXPECT_EQ(report.metadata_directory, "moved");
  EXPECT_EQ(report.metadata_seed, "migrated");
  EXPECT_EQ(report.metadata_frames_migrated, 13u + 2u);
  EXPECT_EQ(report.metadata_frames_already, 0u);
  EXPECT_EQ(report.metadata_files_migrated, 6u);
  EXPECT_EQ(report.metadata_damaged_spans, 0u);
  // Peak extra space: one history file beside itself.
  EXPECT_EQ(report.metadata_peak_extra_bytes, files.at("history.log").size());

  const auto scratch = bed.dir.path() / "scratch";
  EXPECT_EQ(expect_frames_moved(files.at("history.log"), bed.v19_dir() / "history.log", mm::history_magic, *bed.domain_key, scratch / "h"), 13u);
  EXPECT_EQ(expect_frames_moved(files.at("journal.log"), bed.v19_dir() / "journal.log", mm::journal_magic, *bed.domain_key, scratch / "j"), 2u);

  // The checkpoint and the seed: one record each, nonce and ciphertext kept.
  for (const auto& [old_bytes, path, magic, aad] : std::vector<std::tuple<Bytes, std::filesystem::path, Magic, Bytes>>{
           {files.at("checkpoint.meta"), bed.v19_dir() / "checkpoint.meta", mm::checkpoint_magic, v19_aad(machas_space(), mm::checkpoint_magic)},
           {files.at("current.meta"), bed.v19_seed(), mm::seed_magic, v19_seed_aad(machas_space(), mm::seed_magic)}}) {
    const auto moved = macha::read_file(path);
    EXPECT_EQ(moved.size(), old_bytes.size()) << path;
    EXPECT_EQ(sealed_parts(moved).nonce, sealed_parts(old_bytes).nonce) << path;
    EXPECT_EQ(sealed_parts(moved).ciphertext, sealed_parts(old_bytes).ciphertext) << path;
    EXPECT_EQ(heads::open_file(*bed.domain_key, magic, aad, moved), heads::open_file(*bed.domain_key, magic, v18_aad(magic), old_bytes)) << path;
  }
  EXPECT_FALSE(std::filesystem::exists(bed.v18_seed()));

  // This node's standing, under its own key, the plaintext byte for byte.
  for (const auto& [name, magic] : std::vector<std::pair<std::string, Magic>>{{"heads.meta", mm::heads_magic}, {"author.meta", mm::author_magic}}) {
    const auto moved = macha::read_file(bed.v19_dir() / name);
    EXPECT_EQ(heads::open_file(*bed.node_local, magic, v19_aad(machas_space(), magic), moved),
              heads::open_file(*bed.domain_key, magic, v18_aad(magic), files.at(name)))
        << name;
    EXPECT_THROW((void)heads::open_file(*bed.domain_key, magic, v19_aad(machas_space(), magic), moved), std::exception) << name;
  }
  // The set-aside times sealed for the first time.
  EXPECT_EQ(
      heads::open_file(*bed.node_local, mm::set_aside_magic, v19_aad(machas_space(), mm::set_aside_magic), macha::read_file(bed.v19_dir() / "set-aside.meta")),
      files.at("set-aside.meta"));
  // The rest, byte for byte.
  for (const auto* name : {"recovery.required", "author-chain-broken", "history.log.corrupt.1793982986266051000"})
    EXPECT_EQ(macha::read_file(bed.v19_dir() / name), files.at(name)) << name;

  // A second run has nothing to do.
  const auto again = bed.forward();
  EXPECT_TRUE(again.already_v19) << again.summary();
}

// Dagda's replica reads the moved state as it read Macha's: the same head,
// certificates, history and author.
TEST(MigrateHeads, AReplicaOpensTheMovedStateAsItOpenedMachas) {
  HeadsBed bed;
  const auto files = macha::read_checked_in("state-scratch");
  bed.write_metadata(files);
  const auto original = bed.dir.path() / "original";
  copy_tree(bed.v18_dir(), original);
  (void)bed.forward();
  const auto before = open_replica(original, false, bed);
  const auto after = open_replica(bed.v19_dir(), true, bed);
  EXPECT_TRUE(before.failure.empty()) << before;
  EXPECT_EQ(before, after);
  EXPECT_EQ(after.history_records, 13u);
  ASSERT_TRUE(after.next_dot.has_value());
}

// ---- C42: a crash at every step -------------------------------------------------------

// The pass's steps on a state with every file kind: the seed written and the old
// one removed; history, journal, checkpoint, heads, author and set-aside (the plain
// file sealed); the directory renamed; the state's marker. Pinned, so a step added
// fails here until it has its case.
constexpr size_t heads_steps = 2 + 6 + 1 + 1;

struct CrashHeads {
  CrashHeads() {
    files = every_kind(*bed.domain_key);
    bed.write_metadata(files);
    (void)bed.pass("before");
  }

  // A child runs the pass and ends the process after `step` steps. True if it died
  // there.
  bool die_after(size_t step, bool going_back) const {
    const auto child = ::fork();
    if (child < 0) {
      ADD_FAILURE() << "cannot fork";
      return false;
    }
    if (child == 0) {
      try {
        auto options = bed.options(!going_back);
        size_t seen = 0;
        options.after_step = [&seen, step](const MigrationReport&) {
          if (++seen == step) ::_exit(0);
        };
        if (going_back)
          (void)bed.back(options);
        else
          (void)bed.forward(options);
        ::_exit(21);
      } catch (...) {
        ::_exit(22);
      }
    }
    const auto code = test::exit_code(child);
    EXPECT_TRUE(code == std::optional<int>(0) || code == std::optional<int>(21)) << "the child ended with " << code.value_or(-1);
    return code == std::optional<int>(0);
  }

  // A state stopped part way opens as nothing: the store refuses its marker, and
  // Macha, which reads only "macha-state-layout-v18", refuses it too.
  void expect_nobody_opens() const {
    const auto marker = read_text(bed.state / "storage-layout");
    EXPECT_NE(marker, "macha-state-layout-v18\n");
    EXPECT_THROW(store::open_state_layout(bed.state, store::SealingScheme::v19(domain::domain_v1)), std::runtime_error) << marker;
  }

  HeadsBed bed;
  std::map<std::string, Bytes> files;
};

TEST(MigrateHeadsCrash, AProcessThatDiesAfterAnyStepIsFinishedByASecondRun) {
  size_t died = 0;
  for (size_t step = 1;; ++step) {
    SCOPED_TRACE("died after step " + std::to_string(step));
    CrashHeads crash;
    if (!crash.die_after(step, false)) break;
    ++died;
    if (step < heads_steps) crash.expect_nobody_opens();
    // A run without --heads finishes it too: a stopped pass is finished whatever.
    const auto report = crash.bed.forward(crash.bed.options(step % 2 == 0));
    EXPECT_EQ(report.metadata_damaged_spans, 0u);
    EXPECT_EQ(read_text(crash.bed.state / "storage-layout"), v19_marker());
    EXPECT_FALSE(std::filesystem::exists(crash.bed.v18_dir()));
    const auto after = open_replica(crash.bed.v19_dir(), true, crash.bed);
    EXPECT_EQ(after.history_records, 13u) << after;
    (void)crash.bed.pass("after");
    const auto comparison = crash.bed.compare("before", "after", 19, 19);
    EXPECT_TRUE(comparison.passed()) << comparison.summary();
  }
  EXPECT_EQ(died, heads_steps);
}

TEST(MigrateHeadsCrash, AProcessThatDiesAfterAnyStepCanBeTakenBack) {
  size_t died = 0;
  for (size_t step = 1;; ++step) {
    SCOPED_TRACE("died after step " + std::to_string(step));
    CrashHeads crash;
    if (!crash.die_after(step, false)) break;
    ++died;
    (void)crash.bed.back();
    EXPECT_EQ(read_text(crash.bed.state / "storage-layout"), "macha-state-layout-v18\n");
    for (const auto* name : {"history.log", "journal.log", "checkpoint.meta", "set-aside.meta", "recovery.required"})
      EXPECT_EQ(macha::read_file(crash.bed.v18_dir() / name), crash.files.at(name)) << name;
    EXPECT_EQ(macha::read_file(crash.bed.v18_seed()), crash.files.at("current.meta"));
    (void)crash.bed.pass("back");
    const auto comparison = crash.bed.compare("before", "back", 19, 18);
    EXPECT_TRUE(comparison.passed()) << comparison.summary();
  }
  EXPECT_EQ(died, heads_steps);
}

TEST(MigrateHeadsCrash, AProcessThatDiesGoingBackAfterAnyStepIsFinishedByASecondRun) {
  size_t died = 0;
  for (size_t step = 1;; ++step) {
    SCOPED_TRACE("died going back after step " + std::to_string(step));
    CrashHeads crash;
    (void)crash.bed.forward();
    if (!crash.die_after(step, true)) break;
    ++died;
    if (step < heads_steps) crash.expect_nobody_opens();
    (void)crash.bed.back();
    EXPECT_EQ(read_text(crash.bed.state / "storage-layout"), "macha-state-layout-v18\n");
    EXPECT_FALSE(std::filesystem::exists(crash.bed.state / "heads"));
    for (const auto* name : {"history.log", "journal.log", "checkpoint.meta", "set-aside.meta", "author-chain-broken"})
      EXPECT_EQ(macha::read_file(crash.bed.v18_dir() / name), crash.files.at(name)) << name;
    EXPECT_EQ(macha::read_file(crash.bed.v18_seed()), crash.files.at("current.meta"));
    const auto opened = open_replica(crash.bed.v18_dir(), false, crash.bed);
    EXPECT_EQ(opened.history_records, 13u) << opened;
  }
  // Going back: the seed's two, the six files, the directory, and the marker.
  EXPECT_EQ(died, heads_steps);
}

// A crash in the middle of writing history beside itself leaves the sibling, as
// large as the file; the old file is whole, and a second run removes the sibling.
TEST(MigrateHeadsCrash, AHistoryWrittenPartWayBesideItselfIsRemovedAndRedone) {
  HeadsBed bed;
  const auto files = every_kind(*bed.domain_key);
  bed.write_metadata(files);
  const auto sibling = bed.v18_dir() / "history.log.tmp.4242.0";
  const auto& history = files.at("history.log");
  macha::write_file(sibling, std::span<const uint8_t>(history).first(history.size() / 2));
  macha::write_file(bed.v18_dir() / "heads.meta.tmp.4242.1", bytes_of("half a certificate"));
  macha::write_file(bed.v18_seed().parent_path() / "current.meta.tmp.4242.2", bytes_of("half a seed"));
  macha::write_file(bed.state / "storage-layout", bytes_of(std::string(store::state_layout_going_forward_heads) + "\n"));
  const auto report = bed.forward(bed.options(false));
  EXPECT_EQ(report.metadata_directory, "moved");
  EXPECT_FALSE(std::filesystem::exists(bed.v19_dir() / "history.log.tmp.4242.0"));
  EXPECT_FALSE(std::filesystem::exists(bed.v19_dir() / "heads.meta.tmp.4242.1"));
  EXPECT_FALSE(std::filesystem::exists(bed.v18_seed().parent_path() / "current.meta.tmp.4242.2"));
  EXPECT_EQ(expect_frames_moved(history, bed.v19_dir() / "history.log", mm::history_magic, *bed.domain_key, bed.dir.path() / "h"), 13u);
}

// Stage one's run never meets metadata part way: a v18 state whose --heads run
// stopped refuses a run without it, which would leave history Macha's replica
// would cut as one torn tail.
TEST(MigrateHeadsCrash, AFirstStageRunRefusesMetadataLeftPartWay) {
  HeadsBed bed("v18");
  bed.write_metadata(macha::read_checked_in("state-scratch"));
  size_t seen = 0;
  auto stopping = bed.options(true);
  struct Stop {};
  stopping.after_step = [&](const MigrationReport&) {
    // Past the ledgers and the history's move: the metadata is part way.
    if (++seen == 1) throw Stop{};
  };
  EXPECT_THROW((void)bed.forward(stopping), Stop);
  EXPECT_EQ(read_text(bed.state / "storage-layout"), std::string(store::state_layout_going_forward) + "\n");
  const auto digest = bed.digest();
  EXPECT_THROW((void)bed.forward(bed.options(false)), MigrationRefused);
  EXPECT_EQ(bed.digest(), digest);
  // With --heads it finishes; back, it is Macha's again.
  (void)bed.forward(bed.options(true));
  EXPECT_TRUE(std::filesystem::is_directory(bed.v19_dir()));
  (void)bed.back();
  EXPECT_EQ(macha::read_file(bed.v18_dir() / "history.log"), macha::read_checked_in("state-scratch").at("history.log"));
}

// ---- C43: back -------------------------------------------------------------------------

TEST(MigrateHeads, ReverseRestoresWhatTheStoreReads) {
  HeadsBed bed;
  const auto files = every_kind(*bed.domain_key);
  bed.write_metadata(files);
  (void)bed.pass("before");
  (void)bed.forward();
  const auto report = bed.back();
  EXPECT_EQ(report.metadata_directory, "moved") << report.summary();
  EXPECT_EQ(report.metadata_frames_migrated, 15u);
  EXPECT_EQ(read_text(bed.state / "storage-layout"), "macha-state-layout-v18\n");
  EXPECT_FALSE(std::filesystem::exists(bed.state / "heads"));
  EXPECT_FALSE(std::filesystem::exists(bed.v19_seed()));
  // Macha's bytes for frames, the checkpoint, the seed and the plain files.
  for (const auto& [name, bytes] : files) {
    if (name == "heads.meta" || name == "author.meta") continue;
    const auto path = name == "current.meta" ? bed.v18_seed() : bed.v18_dir() / name;
    EXPECT_EQ(macha::read_file(path), bytes) << name;
  }
  // Re-sealed under the domain key and the magic, a fresh nonce: the plaintext.
  for (const auto& [name, magic] : std::vector<std::pair<std::string, Magic>>{{"heads.meta", mm::heads_magic}, {"author.meta", mm::author_magic}})
    EXPECT_EQ(heads::open_file(*bed.domain_key, magic, v18_aad(magic), macha::read_file(bed.v18_dir() / name)),
              heads::open_file(*bed.domain_key, magic, v18_aad(magic), files.at(name)))
        << name;
  (void)bed.pass("back");
  const auto comparison = bed.compare("before", "back", 19, 18);
  EXPECT_TRUE(comparison.passed()) << comparison.summary();
  // And forward again.
  (void)bed.forward();
  EXPECT_TRUE(std::filesystem::is_directory(bed.v19_dir()));
}

// What Dagda wrote while it ran goes back with the rest: a commit on the moved
// state, then the way back, and Macha's form reads it.
TEST(MigrateHeads, WhatDagdaWroteGoesBackWithTheRest) {
  HeadsBed bed;
  bed.write_metadata(macha::read_checked_in("state-scratch"));
  (void)bed.forward();
  heads::Record extended;
  {
    heads::Replica replica(macha_options(bed.v19_dir(), true, bed, std::make_shared<macha::OpaqueCodec>()));
    const auto head = replica.committed();
    extended = test::record_after(head, Bytes(head.payload.begin(), head.payload.end()));
    test::commit_record(replica, extended);
  }
  (void)bed.back();
  const auto opened = open_replica(bed.v18_dir(), false, bed);
  EXPECT_EQ(opened.committed, extended.hash) << opened;
  EXPECT_EQ(opened.history_records, 14u);
}

// ---- C44 ----------------------------------------------------------------------------------

TEST(MigrateHeads, RefusesBeforeTheFirstWrite) {
  const auto refused = [](const std::function<void(HeadsBed&)>& hurt, const std::string& why) {
    SCOPED_TRACE(why);
    HeadsBed bed;
    bed.write_metadata(every_kind(*bed.domain_key));
    hurt(bed);
    const auto digest = bed.digest();
    try {
      (void)bed.forward();
      ADD_FAILURE() << "not refused";
    } catch (const MigrationRefused& error) {
      EXPECT_NE(std::string(error.what()).find(why), std::string::npos) << error.what();
    }
    EXPECT_EQ(bed.digest(), digest);
  };
  refused([](HeadsBed& bed) { test::flip_byte_at(bed.v18_dir() / "heads.meta", 40); }, "heads.meta opens under neither");
  refused([](HeadsBed& bed) { test::flip_byte_at(bed.v18_dir() / "checkpoint.meta", 40); }, "checkpoint.meta opens under neither");
  refused([](HeadsBed& bed) { test::flip_byte_at(bed.v18_dir() / "author.meta", 40); }, "Open it once under Macha 0.93.4");
  refused([](HeadsBed& bed) { std::filesystem::create_directories(bed.v19_dir()); }, "both");
  // Another cluster's key: nothing opens.
  refused([](HeadsBed& bed) { (void)test::v18::cluster_key_from(bed.cluster_key_file, "another cluster\n"); }, "opens under neither");
  // A node running: its lock is held.
  {
    HeadsBed bed;
    bed.write_metadata(every_kind(*bed.domain_key));
    store::StorageLock held(bed.state);
    const auto digest = bed.digest();
    EXPECT_THROW((void)bed.forward(), MigrationRefused);
    EXPECT_EQ(bed.digest(), digest);
  }
}

// A frame that does not authenticate is listed and carried; every frame that does
// moves.
TEST(MigrateHeads, AnUnreadableFrameIsListedAndCarried) {
  HeadsBed bed;
  auto files = macha::read_checked_in("state-scratch");
  bed.write_metadata(files);
  // The fifth frame's last byte.
  const auto spans = spans_of(bed.v18_dir() / "history.log", *bed.domain_key, v18_aad(mm::history_magic));
  ASSERT_EQ(spans.size(), 13u);
  test::flip_byte_at(bed.v18_dir() / "history.log", spans[4].offset + spans[4].length - 1);
  const auto damaged = macha::read_file(bed.v18_dir() / "history.log");
  const auto report = bed.forward();
  EXPECT_EQ(report.metadata_damaged_spans, 1u);
  EXPECT_EQ(report.metadata_damaged_bytes, spans[4].length);
  EXPECT_EQ(report.metadata_frames_migrated, 12u);
  EXPECT_TRUE(std::any_of(report.unreadable.begin(), report.unreadable.end(), [&](const std::string& line) {
    return line.find("offset " + std::to_string(spans[4].offset)) != std::string::npos;
  })) << report.summary();
  EXPECT_EQ(expect_frames_moved(damaged, bed.v19_dir() / "history.log", mm::history_magic, *bed.domain_key, bed.dir.path() / "h"), 12u);
}

// Stage one leaves the metadata; the later stage's run moves it under v19's marker.
TEST(MigrateHeads, ARunUnderV19StillDoesThePass) {
  HeadsBed bed;
  const auto files = macha::read_checked_in("state-scratch");
  bed.write_metadata(files);
  std::vector<std::string> said;
  auto first = bed.options(false);
  first.say = [&](std::string_view line) { said.emplace_back(line); };
  const auto stage_one = bed.forward(first);
  EXPECT_TRUE(stage_one.already_v19);
  EXPECT_TRUE(std::any_of(said.begin(), said.end(), [](const std::string& line) { return line.find("--heads moves it") != std::string::npos; }));
  for (const auto& [name, bytes] : files) EXPECT_EQ(macha::read_file(bed.v18_dir() / name), bytes) << name;
  const auto later = bed.forward(bed.options(true));
  EXPECT_FALSE(later.already_v19);
  EXPECT_EQ(later.metadata_directory, "moved");
  EXPECT_TRUE(std::filesystem::is_directory(bed.v19_dir()));
}

// And from v18 in one run: objects, ledgers, then the metadata, then the markers.
TEST(MigrateHeads, AV18StateMovesItsMetadataInTheSameRun) {
  test::StoreBed store;
  test::v18::V18State v18(store);
  // The state's objects are under its own known-answer key, so its metadata is
  // written under that key too, by a replica given v18's forms.
  heads::ReplicaOptions options;
  options.directory = v18.state / "metadata";
  options.content_key = store.key;
  options.local_key = store.key;
  options.magics = mm::magics();
  options.aad = [](const Magic& magic) { return v18_aad(magic); };
  options.codec = std::make_shared<test::NeutralCodec>();
  options.logger = test::quiet_logger();
  heads::Hash256 head;
  {
    heads::Replica replica(options);
    const auto next = test::record_after(replica.committed(), test::neutral_payload(test::head_with({"a"})));
    test::commit_record(replica, next);
    head = next.hash;
    (void)replica.reserve_mutation_dot(test::author(1), {});
  }
  auto run = MigrationOptions{};
  run.heads = true;
  const auto report = v18.migrate(run);
  EXPECT_EQ(report.metadata_directory, "moved") << report.summary();
  test::v18::expect_v19_opens(store, v18);
  options.directory = heads::space_directory(v18.state, machas_space());
  options.local_key = v18.node_local();
  options.aad = heads::replica_aad(domain::domain_v1, machas_space());
  heads::Replica replica(options);
  EXPECT_EQ(replica.committed().hash, head);
}

TEST(MigrateHeads, TheDryRunWritesNothing) {
  HeadsBed bed;
  bed.write_metadata(every_kind(*bed.domain_key));
  const auto digest = bed.digest();
  auto options = bed.options();
  options.dry_run = true;
  const auto report = bed.forward(options);
  EXPECT_EQ(bed.digest(), digest);
  EXPECT_EQ(report.metadata_frames_migrated, 15u);
  EXPECT_EQ(report.metadata_files_migrated, 6u);
  EXPECT_EQ(report.metadata_seed, "migrated");
  EXPECT_NE(report.summary().find("dry run"), std::string::npos);
}

TEST(MigrateHeads, ASeedThatOpensUnderNeitherIsListedAndLeft) {
  HeadsBed bed;
  bed.write_metadata(every_kind(*bed.domain_key));
  test::flip_byte_at(bed.v18_seed(), 60);
  const auto seed = macha::read_file(bed.v18_seed());
  const auto report = bed.forward();
  EXPECT_EQ(report.metadata_seed, "unreadable");
  EXPECT_EQ(report.metadata_directory, "moved");
  EXPECT_EQ(macha::read_file(bed.v18_seed()), seed);
  EXPECT_FALSE(std::filesystem::exists(bed.v19_seed()));
  EXPECT_TRUE(
      std::any_of(report.unreadable.begin(), report.unreadable.end(), [](const std::string& line) { return line.find("current.meta") != std::string::npos; }));
}

// ---- Damage between good frames, on a copy of a real state --------------------------

// Macha 0.93.5 leaves damaged bytes between frames that authenticate. The pass moves
// every frame that authenticates and carries every other byte where it was; a torn
// tail likewise, for the reader to cut. The replica then reads the moved file as it
// read the damaged one, and the comparison names the damaged span NOT CHECKED, its
// bytes the same, as a damaged span of a pack is.
TEST(MigrateHeads, ADamagedRealHistoryIsCarriedByteForByte) {
  const auto files = macha::read_checked_in("state-scratch");
  HeadsBed probe;
  const auto spans = [&] {
    macha::write_file(probe.dir.path() / "h", files.at("history.log"));
    return spans_of(probe.dir.path() / "h", *probe.domain_key, v18_aad(mm::history_magic));
  }();
  ASSERT_EQ(spans.size(), 13u);
  enum class Kind { flip, length, inserted, torn };
  size_t runs = 0;
  for (const auto kind : {Kind::flip, Kind::length, Kind::inserted, Kind::torn}) {
    for (size_t frame = 0; frame < spans.size(); ++frame) {
      if (kind == Kind::torn && frame + 1 != spans.size()) continue;
      SCOPED_TRACE("damage " + std::to_string(static_cast<int>(kind)) + " at frame " + std::to_string(frame));
      HeadsBed bed;
      auto damaged = files;
      auto& history = damaged["history.log"];
      const auto& span = spans[frame];
      switch (kind) {
        case Kind::flip:
          history[span.offset + span.length - 1] ^= 0x5a;
          break;
        case Kind::length:
          history[span.offset] ^= 0x40;
          break;
        case Kind::inserted: {
          const Bytes garbage(37, 0xc3);
          history.insert(history.begin() + static_cast<std::ptrdiff_t>(span.offset), garbage.begin(), garbage.end());
          break;
        }
        case Kind::torn:
          history.resize(span.offset + span.length / 2);
          break;
      }
      bed.write_metadata(damaged);
      const auto copy = bed.dir.path() / "damaged-copy";
      copy_tree(bed.v18_dir(), copy);
      (void)bed.pass("before");
      const auto report = bed.forward();
      const auto moved = expect_frames_moved(history, bed.v19_dir() / "history.log", mm::history_magic, *bed.domain_key, bed.dir.path() / "h");
      EXPECT_EQ(report.metadata_frames_migrated, moved);
      EXPECT_GE(moved, kind == Kind::inserted ? 13u : 12u);
      // The replica reads the moved file as it read the damaged one.
      const auto before = open_replica(copy, false, bed);
      const auto after = open_replica(bed.v19_dir(), true, bed);
      EXPECT_EQ(before, after);
      (void)bed.pass("after");
      const auto comparison = bed.compare("before", "after", 19, 19);
      EXPECT_EQ(comparison.difference_count, 0u) << comparison.summary();
      if (kind == Kind::torn || (kind != Kind::inserted && frame + 1 == spans.size())) {
        // Nothing authenticates after it: a tail, which the reader cuts.
        EXPECT_TRUE(comparison.passed()) << comparison.summary();
      } else {
        EXPECT_EQ(comparison.not_checked_count, 1u) << comparison.summary();
        EXPECT_NE(comparison.summary().find("damaged: damaged bytes between frames"), std::string::npos) << comparison.summary();
      }
      ++runs;
    }
  }
  EXPECT_EQ(runs, 13u * 3 + 1);
}

// ---- C45 ------------------------------------------------------------------------------------

// The neutral owner's chain written as Macha's v18 files, moved, reopened: the same
// head, a commit extends it, the author continues.
TEST(HeadsReplica, AChainThroughAMigratedStateIsTheSameHead) {
  HeadsBed bed;
  const auto codec = std::make_shared<test::NeutralCodec>();
  const auto first = test::head_with({"a"}, {{test::author(1), 1}});
  const auto second = test::head_with({"a", "b"}, {{test::author(1), 2}});
  heads::Record one;
  heads::Record two;
  heads::Dot dot;
  {
    heads::Replica replica(macha_options(bed.v18_dir(), false, bed, codec));
    one = test::record_after(replica.committed(), test::neutral_payload(first));
    test::commit_record(replica, one);
    two = test::record_after(one, test::neutral_payload(second));
    test::commit_record(replica, two, test::delta_between(first, second));
    dot = replica.reserve_mutation_dot(test::author(1), second.clock);
    replica.note_author_accepted(dot);
  }
  (void)bed.forward();
  {
    heads::Replica replica(macha_options(bed.v19_dir(), true, bed, codec));
    EXPECT_EQ(replica.committed().hash, two.hash);
    ASSERT_TRUE(replica.history_entry(two.hash).has_value());
    EXPECT_EQ(replica.history_entry(two.hash)->body, heads::HistoryEntry::Body::delta);
    EXPECT_EQ(test::hashes_of(replica.accepted_heads()), std::vector<heads::Hash256>{two.hash});
    const auto next = replica.reserve_mutation_dot(test::author(1), second.clock);
    EXPECT_EQ(next.author, dot.author);
    EXPECT_EQ(next.sequence, dot.sequence + 1);
    const auto third = test::head_with({"a", "b", "c"}, {{test::author(1), next.sequence}});
    const auto three = test::record_after(two, test::neutral_payload(third));
    test::commit_record(replica, three, test::delta_between(second, third));
  }
  heads::Replica reopened(macha_options(bed.v19_dir(), true, bed, codec));
  EXPECT_EQ(reopened.committed().generation, 4u);
  EXPECT_EQ(reopened.diagnostics().history_records, 4u);
}

// ---- The comparison over the moved state, and a damage test per item ---------------

struct CompareBed {
  CompareBed() {
    files = every_kind(*bed.domain_key);
    bed.write_metadata(files);
    (void)bed.pass("before");
    (void)bed.forward();
    (void)bed.pass("after");
  }
  ComparisonReport after_damage() const {
    (void)bed.pass("damaged");
    return bed.compare("before", "damaged", 19, 19);
  }
  void reseal(const std::string& name, const Magic& magic, std::span<const uint8_t> plaintext) const {
    macha::write_file(bed.v19_dir() / name, heads::seal_file(*bed.node_local, magic, v19_aad(machas_space(), magic), plaintext));
  }
  Bytes opened(const std::string& name, const Magic& magic) const {
    return heads::open_file(*bed.node_local, magic, v19_aad(machas_space(), magic), macha::read_file(bed.v19_dir() / name));
  }
  HeadsBed bed;
  std::map<std::string, Bytes> files;
};

void expect_names(const ComparisonReport& report, std::string_view text) {
  EXPECT_FALSE(report.passed());
  EXPECT_TRUE(report.names(text)) << "no difference names \"" << text << "\" in:\n" << report.summary();
}

TEST(MigrateHeadsCompare, TheMovedStatePassesWithEveryMetadataCategorySized) {
  CompareBed bed;
  const auto report = bed.bed.compare("before", "after", 19, 19);
  EXPECT_TRUE(report.passed()) << report.summary();
  const auto sized = [&](const std::string& name, uint64_t count) {
    const auto* category = report.category("metadata " + name);
    ASSERT_NE(category, nullptr) << name;
    EXPECT_EQ(category->before, count) << name;
    EXPECT_EQ(category->after, count) << name;
  };
  sized("author", 1);
  sized("certificates", 1);
  sized("checkpoint", 1);
  sized("heads", 1);
  sized("history", 13);
  sized("journal", 2);
  sized("markers", 3);
  sized("seed", 1);
  sized("set-aside", 1);
}

TEST(MigrateHeadsCompareDamage, TheAuthorChanged) {
  CompareBed bed;
  auto plain = bed.opened("author.meta", mm::author_magic);
  plain[20] ^= 1;
  bed.reseal("author.meta", mm::author_magic, plain);
  expect_names(bed.after_damage(), "metadata author: author.meta has changed");
}

TEST(MigrateHeadsCompareDamage, ACertificateChanged) {
  CompareBed bed;
  auto certificates = heads::decode_acceptance_set(bed.opened("heads.meta", mm::heads_magic));
  ASSERT_EQ(certificates.size(), 1u);
  certificates[0].replicas.push_back(test::author(9));
  bed.reseal("heads.meta", mm::heads_magic, heads::encode_acceptance_set(certificates));
  expect_names(bed.after_damage(), "metadata certificates: " + hex(certificates[0].hash) + " has changed");
}

TEST(MigrateHeadsCompareDamage, ACertificateLost) {
  CompareBed bed;
  const auto certificates = heads::decode_acceptance_set(bed.opened("heads.meta", mm::heads_magic));
  bed.reseal("heads.meta", mm::heads_magic, heads::encode_acceptance_set({}));
  const auto report = bed.after_damage();
  expect_names(report, "metadata certificates: " + hex(certificates[0].hash) + " is lost");
  expect_names(report, "metadata heads: " + hex(certificates[0].hash) + " is lost");
}

TEST(MigrateHeadsCompareDamage, TheCheckpointChanged) {
  CompareBed bed;
  const auto seed = heads::open_file(*bed.bed.domain_key, mm::seed_magic, v19_seed_aad(machas_space(), mm::seed_magic), macha::read_file(bed.bed.v19_seed()));
  macha::write_file(bed.bed.v19_dir() / "checkpoint.meta",
                    heads::seal_file(*bed.bed.domain_key, mm::checkpoint_magic, v19_aad(machas_space(), mm::checkpoint_magic), seed));
  expect_names(bed.after_damage(), "metadata checkpoint: checkpoint.meta has changed");
}

TEST(MigrateHeadsCompareDamage, AHistoryFrameDamaged) {
  CompareBed bed;
  const auto spans = spans_of(bed.bed.v19_dir() / "history.log", *bed.bed.domain_key, v19_aad(machas_space(), mm::history_magic));
  test::flip_byte_at(bed.bed.v19_dir() / "history.log", spans[3].offset + 40);
  const auto report = bed.after_damage();
  expect_names(report,
               "metadata history: " + std::string(20 - std::to_string(spans[3].offset).size(), '0') + std::to_string(spans[3].offset) + " frame is lost");
  expect_names(report, "damaged is new");
}

TEST(MigrateHeadsCompareDamage, AHeadWhoseHistoryIsLost) {
  CompareBed bed;
  std::filesystem::resize_file(bed.bed.v19_dir() / "history.log", 0);
  const auto report = bed.after_damage();
  expect_names(report, "frame is lost");
  const auto certificates = heads::decode_acceptance_set(bed.opened("heads.meta", mm::heads_magic));
  expect_names(report, "metadata heads: " + hex(certificates[0].hash) + " has changed");
}

TEST(MigrateHeadsCompareDamage, AJournalFrameLost) {
  CompareBed bed;
  const auto spans = spans_of(bed.bed.v19_dir() / "journal.log", *bed.bed.domain_key, v19_aad(machas_space(), mm::journal_magic));
  ASSERT_EQ(spans.size(), 2u);
  std::filesystem::resize_file(bed.bed.v19_dir() / "journal.log", spans[1].offset);
  expect_names(bed.after_damage(), "metadata journal: ");
}

TEST(MigrateHeadsCompareDamage, AMarkerLost) {
  CompareBed bed;
  std::filesystem::remove(bed.bed.v19_dir() / "recovery.required");
  expect_names(bed.after_damage(), "metadata markers: recovery.required is lost");
}

TEST(MigrateHeadsCompareDamage, AQuarantineChanged) {
  CompareBed bed;
  macha::write_file(bed.bed.v19_dir() / "history.log.corrupt.1793982986266051000", bytes_of("another tail"));
  expect_names(bed.after_damage(), "metadata markers: history.log.corrupt.1793982986266051000 has changed");
}

TEST(MigrateHeadsCompareDamage, TheSeedLost) {
  CompareBed bed;
  std::filesystem::remove(bed.bed.v19_seed());
  expect_names(bed.after_damage(), "metadata seed: seed is lost");
}

TEST(MigrateHeadsCompareDamage, TheSetAsideChanged) {
  CompareBed bed;
  bed.reseal("set-aside.meta", mm::set_aside_magic, set_aside_plain(0x77, 1793700000001ULL));
  expect_names(bed.after_damage(), "metadata set-aside: set-aside.meta has changed");
}

TEST(MigrateHeadsCompareDamage, AFileThatNoLongerOpensIsNamed) {
  CompareBed bed;
  test::flip_byte_at(bed.bed.v19_dir() / "author.meta", 40);
  const auto report = bed.after_damage();
  expect_names(report, "metadata author: author.meta has changed");
  expect_names(report, "opens under neither form");
}

// ---- Every real state Macha's fixtures hold ----------------------------------------------

struct RealNode {
  const char* fixture;
  const char* node;
};

std::string real_name(const testing::TestParamInfo<RealNode>& info) {
  std::string name = std::string(info.param.fixture) + "_" + info.param.node;
  for (auto& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  return name;
}

// A real node's whole state_path, copied: its control store, ledgers, markers and
// metadata, and its cache's seed where the fixture kept one. The DATA backends were
// not copied into the fixtures, so the mirrors naming them are left out: the
// metadata is what this proves.
struct RealBed {
  explicit RealBed(const RealNode& real) : bed("none") {
    source = macha::fixtures() / real.fixture / real.node;
    if (!std::filesystem::exists(source / "metadata")) return;
    present = true;
    std::filesystem::remove_all(bed.state);
    copy_tree(source, bed.state);
    std::filesystem::remove_all(bed.state / "backend-identities");
    const auto seed = macha::fixtures() / real.fixture / (std::string("cache-") + real.node) / "metadata";
    if (std::filesystem::exists(seed)) copy_tree(seed, bed.cache / "metadata");
    const auto id = read_text(bed.state / "node-id");
    std::ofstream(bed.node_state / "node-id", std::ios::trunc) << id;
    bed.node = dannan::identity::NodeId::parse(std::string(id.begin(), id.begin() + 32)).value();
  }
  HeadsBed bed;
  std::filesystem::path source;
  bool present = false;
};

std::vector<RealNode> every_real_node() {
  std::vector<RealNode> out;
  for (const char* fixture : {"state-scratch", "state-scratch-0.93.4", "state-scratch-0.93.4-branch", "state-scratch-0.93.4-delete",
                              "state-scratch-0.93.4-markers", "state-scratch-0.93.4-recovery", "state-scratch-0.93.4-seed", "state-scratch-0.93.4-torrent",
                              "state-scratch-0.93.4-truncated", "state-scratch-0.93.4-twoheads"})
    for (const char* node : {"a", "b"}) out.push_back({fixture, node});
  return out;
}

class MigrateHeadsReal : public testing::TestWithParam<RealNode> {};

// C41 and C43 over every real state: the first stage, then the later stage's pass,
// compared from Macha's v18 to v19 with nothing lost; the replica opens it as it
// opened Macha's; then all the way back, compared again, Macha's bytes restored.
TEST_P(MigrateHeadsReal, EveryRealStateMovesComparesAndComesBack) {
  RealBed real(GetParam());
  if (!real.present) GTEST_SKIP() << GetParam().fixture << " is not here (DAGDA_MACHA_FIXTURES)";
  auto& bed = real.bed;
  const auto original = bed.dir.path() / "original";
  copy_tree(bed.v18_dir(), original);
  const auto seed_before = std::filesystem::exists(bed.v18_seed()) ? std::optional<Bytes>(macha::read_file(bed.v18_seed())) : std::nullopt;

  (void)bed.pass("before");
  const auto stage_one = bed.forward(bed.options(false));
  EXPECT_FALSE(stage_one.already_v19);
  for (const auto& entry : std::filesystem::directory_iterator(original))
    EXPECT_EQ(macha::read_file(bed.v18_dir() / entry.path().filename()), macha::read_file(entry.path())) << "stage one moved " << entry.path().filename();
  const auto report = bed.forward(bed.options(true));
  EXPECT_EQ(report.metadata_directory, "moved") << report.summary();
  EXPECT_EQ(report.metadata_damaged_spans, 0u);
  EXPECT_EQ(report.metadata_seed, seed_before ? "migrated" : "none");
  const auto history = macha::read_file(original / "history.log");
  const auto frames = expect_frames_moved(history, bed.v19_dir() / "history.log", mm::history_magic, *bed.domain_key, bed.dir.path() / "h");
  EXPECT_EQ(frames, report.metadata_frames_migrated);

  const auto before = open_replica(original, false, bed);
  const auto after = open_replica(bed.v19_dir(), true, bed);
  EXPECT_EQ(before, after);

  (void)bed.pass("after");
  const auto comparison = bed.compare("before", "after", 18, 19);
  EXPECT_TRUE(comparison.passed()) << comparison.summary();
  std::ostringstream counts;
  for (const auto& kind : metadata_kinds())
    if (const auto* category = comparison.category("metadata " + kind)) counts << " " << kind << "=" << category->after;
  std::cout << "[ real ] " << GetParam().fixture << "/" << GetParam().node << ": frames " << frames << ", replica: " << after << ";" << counts.str()
            << std::endl;

  const auto back = bed.back();
  EXPECT_EQ(read_text(bed.state / "storage-layout"), "macha-state-layout-v18\n");
  for (const auto& entry : std::filesystem::directory_iterator(original)) {
    const auto name = entry.path().filename().string();
    if (name == "heads.meta" || name == "author.meta") continue;
    EXPECT_EQ(macha::read_file(bed.v18_dir() / name), macha::read_file(entry.path())) << name;
  }
  if (seed_before) {
    EXPECT_EQ(macha::read_file(bed.v18_seed()), *seed_before);
  }
  EXPECT_EQ(open_replica(bed.v18_dir(), false, bed), before);
  (void)bed.pass("back");
  const auto there_and_back = bed.compare("before", "back", 18, 18);
  EXPECT_TRUE(there_and_back.passed()) << there_and_back.summary();
  (void)back;
}

INSTANTIATE_TEST_SUITE_P(Macha0934, MigrateHeadsReal, testing::ValuesIn(every_real_node()), real_name);

// ---- C46 --------------------------------------------------------------------------------------

// Macha's bytes as opaque vectors, over every fixture: each frame's layout, its seal
// re-made under its own nonce, the entry re-encoded, a whole record's hash
// recomputed, genesis the constant; heads.meta and checkpoint.meta likewise. The
// forms are counted by their magics, never parsed: what was shown on real bytes is
// printed and the torrent fixture's deltas pinned to its README. The branch half
// (boundaries for the four domain strings, MNB1 and MNY1 re-encoded equal) is
// HeadsTreeVectors.*, over Macha's cut vectors and the scratch state's own nodes.
TEST(HeadsConformance, MachasFramesCertificatesAndBranchesAsOpaqueBytes) {
  const auto root = macha::fixtures();
  if (!std::filesystem::exists(root / "state-scratch-0.93.4-torrent")) GTEST_SKIP() << "Macha's fixtures are not here (DAGDA_MACHA_FIXTURES)";
  HeadsBed keys;
  const auto& key = *keys.domain_key;
  std::map<std::string, size_t> census;
  size_t states = 0;
  for (const auto& real : every_real_node()) {
    const auto directory = root / real.fixture / real.node / "metadata";
    if (!std::filesystem::exists(directory)) continue;
    SCOPED_TRACE(std::string(real.fixture) + "/" + real.node);
    ++states;
    std::map<std::string, size_t> forms;
    const auto history = macha::read_file(directory / "history.log");
    size_t offset = 0;
    while (offset < history.size()) {
      Reader header(std::span<const uint8_t>(history).subspan(offset, 4));
      const auto length = header.u32();
      const auto envelope = heads::parse_frame_envelope(std::span<const uint8_t>(history).subspan(offset + 4, length));
      const auto plaintext = heads::open_frame_envelope(key, v18_aad(mm::history_magic), envelope);
      const auto entry = heads::decode_history_entry(plaintext);
      EXPECT_EQ(heads::encode_history_entry(entry), plaintext);
      const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), std::span<const uint8_t, 12>(envelope.nonce), plaintext, v18_aad(mm::history_magic));
      EXPECT_EQ(sealed.tag, envelope.tag);
      EXPECT_EQ(sealed.ciphertext, envelope.ciphertext);
      if (entry.body == heads::HistoryEntry::Body::full) {
        EXPECT_EQ(heads::record_hash(entry.generation, entry.previous, entry.payload), entry.hash);
        // The constant is 0.93's (766617e); the older state-scratch began at the old one.
        if (entry.generation == 1 && std::string(real.fixture).starts_with("state-scratch-0.93.4")) {
          EXPECT_EQ(hex(entry.hash), "87ad4c58cd1ccdd1326698d186eeb0ce54e5bf0cb8b1829b688a54584b6d7f06");
        }
      }
      const std::string form(entry.payload.begin(), entry.payload.begin() + std::min<size_t>(8, entry.payload.size()));
      ++forms[(entry.body == heads::HistoryEntry::Body::full ? "full " : "delta ") + form];
      offset += 4 + length;
    }
    EXPECT_EQ(offset, history.size());
    if (std::filesystem::exists(directory / "heads.meta")) {
      const auto file = macha::read_file(directory / "heads.meta");
      const auto plain = heads::open_file(key, mm::heads_magic, v18_aad(mm::heads_magic), file);
      EXPECT_EQ(heads::encode_acceptance_set(heads::decode_acceptance_set(plain)), plain);
      const auto parts = sealed_parts(file);
      EXPECT_EQ(dannan::identity::aes_gcm_seal(key.bytes(), std::span<const uint8_t, 12>(parts.nonce), plain, v18_aad(mm::heads_magic)).tag, parts.tag);
    }
    const auto checkpoint =
        heads::decode_record(heads::open_file(key, mm::checkpoint_magic, v18_aad(mm::checkpoint_magic), macha::read_file(directory / "checkpoint.meta")));
    ++forms["checkpoint " + std::string(checkpoint.payload.begin(), checkpoint.payload.begin() + std::min<size_t>(8, checkpoint.payload.size()))];
    std::ostringstream line;
    for (const auto& [form, count] : forms) {
      line << " " << form << "=" << count;
      census[form] += count;
    }
    std::cout << "[ forms ] " << real.fixture << "/" << real.node << ":" << line.str() << std::endl;
    // The README's DLT9 15, DLT5 10, DLT10 3, DLT11 1 are frame-forms-a.txt's, the
    // head's chain of 29; history holds both branches' frames, counted here.
    if (std::string(real.fixture) == "state-scratch-0.93.4-torrent" && std::string(real.node) == "a") {
      EXPECT_EQ(forms["delta DHTMDLT9"], 18u);
      EXPECT_EQ(forms["delta DHTMDLT5"], 17u);
      EXPECT_EQ(forms["delta DHTMDLTA"], 5u);
      EXPECT_EQ(forms["delta DHTMDLTB"], 1u);
      EXPECT_EQ(forms["checkpoint DHTMETB9"], 1u);
    }
  }
  std::ostringstream all;
  for (const auto& [form, count] : census) all << " " << form << "=" << count;
  std::cout << "[ forms ] every fixture:" << all.str() << std::endl;
  EXPECT_EQ(states, 20u);
}

// ---- The tool -----------------------------------------------------------------------------

std::pair<std::optional<int>, std::string> run_tool(const HeadsBed& bed, std::vector<std::string> arguments, const std::string& log) {
  const std::string tool = std::string(DAGDA_BINARY_DIR) + "/dagda-migrate";
  arguments.insert(arguments.begin(), tool);
  dannan::test::ChildProcess child(arguments, bed.dir.path() / log);
  const auto deadline = std::chrono::steady_clock::now() + 120s;
  const auto output = child.read_all(deadline);
  const auto code = child.wait(deadline);
  return {code, output};
}

std::vector<std::string> common_arguments(const HeadsBed& bed) {
  std::vector<std::string> out{
      "--state",          bed.state.string(),      "--node-state", bed.node_state.string(), "--cluster-key", bed.cluster_key_file.string(),
      "--passphrase-env", "DAGDA_TEST_PASSPHRASE", "--cache",      bed.cache.string()};
  for (const auto& backend : bed.backends) {
    out.push_back("--backend");
    out.push_back(backend.string());
  }
  return out;
}

std::vector<std::string> with(std::vector<std::string> all, std::initializer_list<std::string> more) {
  all.insert(all.end(), more);
  return all;
}

// A state of more than 256 records, its pass killed after a durable checkpoint and
// resumed from the partial it left, through the command line: the same manifest as a
// pass that was never stopped.
TEST(MigrateHeadsTool, APassKilledMidWayResumesFromItsPartial) {
  HeadsBed bed("v18");
  ::setenv("DAGDA_TEST_PASSPHRASE", "dagda", 1);
  // 600 loose objects in the control store, sealed as Macha's v18.
  const auto control = bed.state / "metadata-objects";
  for (uint32_t i = 0; i < 600; ++i) {
    const auto data = test::pattern(200 + i, static_cast<uint8_t>(i));
    test::legacy::write_loose_v18(control, *bed.domain_key, store::object_id(data), data);
  }
  bed.write_metadata(every_kind(*bed.domain_key));
  const auto whole = (bed.manifests / "whole").string();
  const auto resumed = (bed.manifests / "resumed").string();
  {
    const auto [code, output] = run_tool(bed, with(common_arguments(bed), {"--write-manifest", whole}), "whole.log");
    ASSERT_EQ(code, std::optional<int>(0)) << read_text(bed.dir.path() / "whole.log");
    EXPECT_NE(output.find("objects control:"), std::string::npos) << output;
  }
  {
    ::setenv("DAGDA_MIGRATE_TEST_HOLD_AFTER_CHECKPOINTS", "2", 1);
    const std::string tool = std::string(DAGDA_BINARY_DIR) + "/dagda-migrate";
    auto arguments = with(common_arguments(bed), {"--write-manifest", resumed});
    arguments.insert(arguments.begin(), tool);
    dannan::test::ChildProcess child(arguments, bed.dir.path() / "killed.log");
    ::unsetenv("DAGDA_MIGRATE_TEST_HOLD_AFTER_CHECKPOINTS");
    bool holding = false;
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (const auto line = child.line(deadline)) {
      if (line->find("holding after checkpoint 2") != std::string::npos) {
        holding = true;
        break;
      }
    }
    ASSERT_TRUE(holding) << read_text(bed.dir.path() / "killed.log");
    child.signal(SIGKILL);
    EXPECT_EQ(child.wait(std::chrono::steady_clock::now() + 30s), std::optional<int>(128 + SIGKILL));
  }
  EXPECT_FALSE(std::filesystem::exists(resumed));
  ASSERT_TRUE(std::filesystem::exists(resumed + ".partial"));
  EXPECT_GT(std::filesystem::file_size(resumed + ".partial"), 0u);
  {
    // Without --resume the partial is refused and kept.
    const auto [code, output] = run_tool(bed, with(common_arguments(bed), {"--write-manifest", resumed}), "refused.log");
    EXPECT_EQ(code, std::optional<int>(1));
    EXPECT_NE(read_text(bed.dir.path() / "refused.log").find("--resume"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(resumed + ".partial"));
  }
  {
    const auto [code, output] = run_tool(bed, with(common_arguments(bed), {"--write-manifest", resumed, "--resume"}), "resume.log");
    ASSERT_EQ(code, std::optional<int>(0)) << read_text(bed.dir.path() / "resume.log");
    EXPECT_NE(output.find("of them from the pass that stopped"), std::string::npos) << output;
    EXPECT_NE(output.find("resuming from"), std::string::npos) << output;
    // At least two checkpoints of 256 records were kept.
    const auto at = output.find("items, ");
    ASSERT_NE(at, std::string::npos) << output;
    const auto held = std::stoull(output.substr(at + 7));
    EXPECT_GE(held, 512u) << output;
  }
  EXPECT_FALSE(std::filesystem::exists(resumed + ".partial"));
  {
    const auto [code, output] = run_tool(bed, {"--compare-before", whole, "--compare-after", resumed, "--expect-after", "18"}, "compare.log");
    EXPECT_EQ(code, std::optional<int>(0)) << output;
    EXPECT_NE(output.find("RESULT: PASS"), std::string::npos) << output;
    EXPECT_NE(output.find("objects control:" + control.string() + " loose: sized 600 before, 600 after: SAME"), std::string::npos) << output;
  }
}

// The later stage and the way back through the command line, on a copy of a real
// state with its seed: --heads moves it, the comparison passes, --reverse takes it all
// back to Macha's bytes.
TEST(MigrateHeadsTool, TheLaterStageAndBackOnARealState) {
  RealBed real({"state-scratch-0.93.4-seed", "a"});
  if (!real.present) GTEST_SKIP() << "state-scratch-0.93.4-seed is not here (DAGDA_MACHA_FIXTURES)";
  auto& bed = real.bed;
  ::setenv("DAGDA_TEST_PASSPHRASE", "dagda", 1);
  const auto original = bed.dir.path() / "original";
  copy_tree(bed.v18_dir(), original);
  const auto seed = macha::read_file(bed.v18_seed());
  const auto manifest = [&](const std::string& name) {
    const auto [code, output] = run_tool(bed, with(common_arguments(bed), {"--write-manifest", (bed.manifests / name).string()}), name + ".log");
    EXPECT_EQ(code, std::optional<int>(0)) << read_text(bed.dir.path() / (name + ".log"));
    return output;
  };
  (void)manifest("before");
  {
    const auto [code, output] = run_tool(bed, common_arguments(bed), "stage-one.log");
    ASSERT_EQ(code, std::optional<int>(0)) << read_text(bed.dir.path() / "stage-one.log");
    EXPECT_TRUE(std::filesystem::exists(bed.v18_dir()));
  }
  {
    const auto [code, output] = run_tool(bed, with(common_arguments(bed), {"--heads"}), "heads.log");
    ASSERT_EQ(code, std::optional<int>(0)) << read_text(bed.dir.path() / "heads.log");
    EXPECT_NE(output.find("metadata of space macha/namespace"), std::string::npos) << output;
    EXPECT_NE(output.find("seed migrated"), std::string::npos) << output;
    EXPECT_TRUE(std::filesystem::is_directory(bed.v19_dir()));
  }
  const auto after = manifest("after");
  EXPECT_NE(after.find("metadata history: sized 9"), std::string::npos) << after;
  {
    const auto [code, output] =
        run_tool(bed, {"--compare-before", (bed.manifests / "before").string(), "--compare-after", (bed.manifests / "after").string()}, "compare.log");
    EXPECT_EQ(code, std::optional<int>(0)) << output;
    EXPECT_NE(output.find("metadata history: sized 9 before, 9 after: SAME"), std::string::npos) << output;
    EXPECT_NE(output.find("metadata seed: sized 1 before, 1 after: SAME"), std::string::npos) << output;
  }
  {
    const auto [code, output] = run_tool(bed, with(common_arguments(bed), {"--reverse"}), "reverse.log");
    ASSERT_EQ(code, std::optional<int>(0)) << read_text(bed.dir.path() / "reverse.log");
  }
  EXPECT_EQ(read_text(bed.state / "storage-layout"), "macha-state-layout-v18\n");
  for (const auto& entry : std::filesystem::directory_iterator(original)) {
    const auto name = entry.path().filename().string();
    if (name == "heads.meta" || name == "author.meta") continue;
    EXPECT_EQ(macha::read_file(bed.v18_dir() / name), macha::read_file(entry.path())) << name;
  }
  EXPECT_EQ(macha::read_file(bed.v18_seed()), seed);
  (void)manifest("back");
  const auto [code, output] =
      run_tool(bed, {"--compare-before", (bed.manifests / "before").string(), "--compare-after", (bed.manifests / "back").string(), "--expect-after", "18"},
               "compare-back.log");
  EXPECT_EQ(code, std::optional<int>(0)) << output;
}

}  // namespace
}  // namespace dagda::migrate
