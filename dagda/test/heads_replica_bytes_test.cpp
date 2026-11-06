//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The replica as bytes: frames two real Macha 0.93.4 nodes wrote (commit 9a08846) open
// and replay through Dagda's code, Macha's magics and v18's associated data given as
// parameters. A few states are checked in as hex under dagda/test/data/replica-0.93.4;
// the rest are read from Macha's fixtures and skip, saying so, on a machine without them.
//
// What the owner's is stays opaque. The codec below holds a payload as bytes and reads
// only its head section's layout: the clock and the merge parents. It cannot apply a
// delta, which only Macha's own codec can (the plan's section 4.3): a delta frame is
// opened, framed and linked here, and materialised only where the committed record in
// checkpoint.meta gives its result, which the replica's replay then holds to the
// frame's hash. A delta frame with no such record is counted, not claimed.
//
// A real state with a damaged region does not exist among the fixtures: 0.93.4 cut such
// a file at open, and no fixture ran 0.93.5. So the damage below is a test's, done to a
// copy of real bytes, as Macha's 9153011 reads it.
//
// No frame of journal.log is proved here: 0.93.4 writes that file only empty, so no
// fixture holds one (Macha's metadata.cpp:4039, :4070). The reader's bytes rest on
// Macha's two cases, at the bytes they build.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/heads/replica.h"
#include "dagda/heads/space.h"
#include "dagda/test/fixtures/heads.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"

namespace dagda::heads {
namespace {

using dannan::identity::hex;
using dannan::identity::unhex;
using dannan::net::Reader;
using dannan::net::Writer;

constexpr Magic checkpoint_magic{'D', 'H', 'T', 'M', 'D', 'B', '0', '1'};
constexpr Magic journal_magic{'D', 'H', 'T', 'M', 'J', 'N', 'L', '1'};
constexpr Magic history_magic{'D', 'H', 'T', 'M', 'H', 'S', 'T', '1'};
constexpr Magic heads_magic{'D', 'H', 'T', 'M', 'A', 'C', 'C', '1'};
constexpr Magic author_magic{'D', 'H', 'T', 'M', 'A', 'U', 'T', '1'};
constexpr Magic set_aside_magic{'D', 'H', 'T', 'M', 'S', 'A', 'S', '1'};
constexpr Magic seed_magic{'D', 'H', 'T', 'C', 'M', 'E', 'T', '1'};
const std::string genesis_hash = "87ad4c58cd1ccdd1326698d186eeb0ce54e5bf0cb8b1829b688a54584b6d7f06";

const std::filesystem::path checked_in = std::filesystem::path(DAGDA_SOURCE_DIR) / "dagda/test/data/replica-0.93.4";

std::filesystem::path macha_fixtures() {
  if (const char* given = std::getenv("DAGDA_MACHA_FIXTURES")) return given;
  return "/Users/tom/devroot/macha/build/macha-fixtures";
}

std::shared_ptr<const domain::StorageKey> fixture_key() {
  dannan::test::TempDir dir;
  {
    std::ofstream key(dir.path() / "cluster.key", std::ios::binary);
    key << "macha-fixture-cluster-key\n";
  }
  return std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_v1(dannan::identity::load_cluster_key(dir.path() / "cluster.key")));
}

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), {});
}

void write_file(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

using SmallFiles = std::map<std::string, Bytes>;

SmallFiles read_checked_in(const std::string& name) {
  SmallFiles out;
  std::ifstream in(checked_in / (name + ".txt"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto gap = line.find(' ');
    const auto value = unhex(line.substr(gap + 1));
    if (!value) throw std::runtime_error("bad hex in " + name);
    out[line.substr(0, gap)] = *value;
  }
  return out;
}

// What the opaque owner holds: the payload.
struct OpaqueState {
  Bytes payload;
};

// The head section as Macha's SM forms lay it out, read as far as the merge parents.
// Nothing past it is parsed. Every fixture's status and reset sections are empty, and
// one that is not refuses, rather than being misread. The floor sits past the owner's
// conflicts: a whole record's is non-zero by Macha's own decoder, so one is said.
HeadFields opaque_fields(std::span<const uint8_t> payload) {
  static const Bytes genesis{'D', 'H', 'T', 'M', 'G', 'E', 'N', '1'};
  if (std::equal(payload.begin(), payload.end(), genesis.begin(), genesis.end())) return {};
  HeadFields out;
  out.floor = 1;
  Reader in(payload);
  const auto magic = in.view(8);
  const std::string form(magic.begin(), magic.end());
  if (form != "DHTMETB4" && form != "DHTMETB6" && form != "DHTMETB7" && form != "DHTMETB9") throw DecodeError("not a whole record");
  const auto voters = in.u32();
  (void)in.view(size_t{voters} * 16);
  (void)in.view(4 + 8);
  out.clock = decode_clock(in);
  (void)in.view(32);
  if (in.u8() != 0) (void)in.view(32);
  const auto garbage = in.u32();
  (void)in.view(size_t{garbage} * 56);
  if (in.u32() != 0 || in.u32() != 0) throw DecodeError("a status or reset section this test does not read");
  out.merge_parents = decode_merge_parents(in);
  return out;
}

class OpaqueCodec final : public Codec {
 public:
  // The payload a delta reaches where something beside the frame says, by the delta's hash.
  using Known = std::map<Hash256, Bytes>;
  explicit OpaqueCodec(Known known = {}) : _known(std::move(known)) {}

  Bytes genesis() const override { return Bytes{'D', 'H', 'T', 'M', 'G', 'E', 'N', '1'}; }
  State decode_full(std::span<const uint8_t> payload) const override {
    (void)opaque_fields(payload);
    return std::make_shared<const OpaqueState>(OpaqueState{Bytes(payload.begin(), payload.end())});
  }
  Bytes encode_full(const State& state) const override { return as_state(state).payload; }
  std::optional<Bytes> encode_delta(const State&, const State&) const override { return std::nullopt; }
  void check_delta(std::span<const uint8_t> delta) const override {
    if (form_version(delta, "DHTMDLT") < 5) throw DecodeError("bad metadata delta");
  }
  State apply_delta(const State&, std::span<const uint8_t> delta, TreeNodeStore*) const override {
    const auto found = _known.find(dannan::identity::sha256(delta));
    if (found == _known.end()) throw DecodeError("only the owner's codec can apply this delta");
    return std::make_shared<const OpaqueState>(OpaqueState{found->second});
  }
  Bytes encode_after_delta(std::span<const uint8_t>, const State& state) const override { return as_state(state).payload; }
  HeadFields head_fields(const State& state) const override { return opaque_fields(as_state(state).payload); }
  uint64_t resident_bytes(const State& state) const override { return as_state(state).payload.size(); }

 private:
  static const OpaqueState& as_state(const State& state) { return *static_cast<const OpaqueState*>(state.get()); }
  const Known _known;
};

const ReplicaMagics& machas_magics() {
  static const ReplicaMagics magics{checkpoint_magic, journal_magic, history_magic, heads_magic, author_magic, set_aside_magic};
  return magics;
}

// v18's rule: the magic alone.
AadRule machas_aad() {
  return [](const Magic& magic) { return Bytes(magic.begin(), magic.end()); };
}

struct Frame {
  uint64_t offset = 0;
  uint64_t bytes = 0;
  HistoryEntry entry;
};

// Every frame, each checked byte for byte: re-encoding the entry and re-sealing it
// under its own nonce gives the frame Macha wrote.
std::vector<Frame> walk_frames(const domain::StorageKey& key, std::span<const uint8_t> history) {
  std::vector<Frame> out;
  size_t offset = 0;
  while (offset < history.size()) {
    Reader header(history.subspan(offset, 4));
    const auto length = header.u32();
    const auto frame_bytes = history.subspan(offset, 4 + size_t{length});
    const auto envelope = parse_frame_envelope(history.subspan(offset + 4, length));
    const auto plaintext = open_frame_envelope(key, history_magic, envelope);
    Frame frame;
    frame.offset = offset;
    frame.bytes = frame_bytes.size();
    frame.entry = decode_history_entry(plaintext);
    EXPECT_EQ(encode_history_entry(frame.entry), plaintext) << "offset " << offset;

    const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), std::span<const uint8_t, 12>(envelope.nonce), plaintext, history_magic);
    EXPECT_EQ(sealed.tag, envelope.tag);
    EXPECT_EQ(sealed.ciphertext, envelope.ciphertext);
    Writer inner;
    inner.fixed(envelope.nonce);
    inner.fixed(envelope.tag);
    inner.bytes(envelope.ciphertext);
    const auto payload = inner.take();
    Writer outer;
    outer.u32(static_cast<uint32_t>(payload.size()));
    outer.raw(payload);
    EXPECT_EQ(outer.data(), Bytes(frame_bytes.begin(), frame_bytes.end())) << "offset " << offset;

    if (frame.entry.body == HistoryEntry::Body::full) {
      EXPECT_EQ(record_hash(frame.entry.generation, frame.entry.previous, frame.entry.payload), frame.entry.hash) << "offset " << offset;
      if (frame.entry.generation == 1) {
        EXPECT_EQ(hex(frame.entry.hash), genesis_hash);
      }
    }
    out.push_back(std::move(frame));
    offset += 4 + size_t{length};
  }
  EXPECT_EQ(offset, history.size());
  return out;
}

// A sealed file re-sealed under its own nonce: the same bytes.
void expect_seal_reproduces(const domain::StorageKey& key, const Magic& magic, std::span<const uint8_t> file, std::span<const uint8_t> plaintext) {
  Reader reader(file);
  (void)reader.view(8);
  const auto nonce = reader.fixed<12>();
  const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), std::span<const uint8_t, 12>(nonce), plaintext, magic);
  Writer writer;
  writer.raw(magic);
  writer.fixed(sealed.nonce);
  writer.fixed(sealed.tag);
  writer.bytes(sealed.ciphertext);
  EXPECT_EQ(writer.data(), Bytes(file.begin(), file.end()));
}

// The listing macha-metadata-dump printed, to hold the walk to: offset, generation,
// hash and previous prefixes, known, body, payload size, merge parents.
void expect_matches_dump(const std::vector<Frame>& frames, const std::filesystem::path& dump) {
  std::ifstream in(dump);
  ASSERT_TRUE(in.good()) << dump;
  std::string line;
  size_t at = 0;
  while (std::getline(in, line)) {
    if (line.rfind("offset=", 0) != 0) {
      if (line.rfind("history:", 0) == 0) break;
      continue;
    }
    ASSERT_LT(at, frames.size()) << line;
    const auto& frame = frames[at++];
    std::ostringstream expected;
    expected << "offset=" << frame.offset << " gen=" << frame.entry.generation << " hash=" << hex(frame.entry.hash).substr(0, 16)
             << " prev=" << hex(frame.entry.previous).substr(0, 16) << " prev_known=" << (frame.entry.previous_known ? 1 : 0)
             << " body=" << (frame.entry.body == HistoryEntry::Body::full ? "full" : "delta") << " payload=" << frame.entry.payload.size()
             << " merge_parents=" << frame.entry.merge_parents.size();
    for (const auto& parent : frame.entry.merge_parents) expected << " mp=" << hex(parent).substr(0, 16);
    EXPECT_EQ(line, expected.str());
  }
  EXPECT_EQ(at, frames.size());
}

struct ProofCounts {
  size_t frames = 0;
  size_t full = 0;
  size_t delta = 0;
  size_t deltas_replayed = 0;
  size_t heads = 0;
};

// Everything a state's files say, through Dagda's code. `directory` holds the five
// files and is the replica's own, so a state may be reopened after.
ProofCounts prove_state(const domain::StorageKey& key, const std::shared_ptr<const domain::StorageKey>& shared_key, const std::filesystem::path& directory) {
  ProofCounts counts;
  const auto history = read_file(directory / "history.log");
  const auto checkpoint_file = read_file(directory / "checkpoint.meta");
  const auto heads_file = read_file(directory / "heads.meta");

  const auto frames = walk_frames(key, history);
  counts.frames = frames.size();
  std::set<Hash256> seen;
  std::map<Hash256, const Frame*> by_hash;
  for (const auto& frame : frames) {
    if (frame.entry.body == HistoryEntry::Body::delta) {
      EXPECT_TRUE(seen.contains(frame.entry.previous)) << "a delta's parent comes first";
      EXPECT_TRUE(frame.entry.previous_known);
      ++counts.delta;
    } else {
      ++counts.full;
    }
    seen.insert(frame.entry.hash);
    by_hash[frame.entry.hash] = &frame;
  }

  // The checkpoint and the certificates, opened and re-sealed to the same bytes.
  const auto checkpoint_plain = open_file(key, checkpoint_magic, checkpoint_magic, checkpoint_file);
  const auto committed = decode_record(checkpoint_plain);
  EXPECT_EQ(encode_record(committed), checkpoint_plain);
  expect_seal_reproduces(key, checkpoint_magic, checkpoint_file, checkpoint_plain);
  const auto heads_plain = open_file(key, heads_magic, heads_magic, heads_file);
  const auto certificates = decode_acceptance_set(heads_plain);
  EXPECT_EQ(encode_acceptance_set(certificates), heads_plain);
  expect_seal_reproduces(key, heads_magic, heads_file, heads_plain);
  counts.heads = certificates.size();

  // The head's delta, if it ends on one, reaches the checkpoint's bytes.
  OpaqueCodec::Known known;
  if (const auto head = by_hash.find(committed.hash); head != by_hash.end() && head->second->entry.body == HistoryEntry::Body::delta)
    known[dannan::identity::sha256(head->second->entry.payload)] = Bytes(committed.payload.begin(), committed.payload.end());

  ReplicaOptions options;
  options.directory = directory;
  options.content_key = shared_key;
  options.local_key = shared_key;
  options.magics = machas_magics();
  options.aad = machas_aad();
  options.codec = std::make_shared<OpaqueCodec>(std::move(known));
  options.logger = test::quiet_logger();
  Replica replica(std::move(options));

  EXPECT_FALSE(replica.recovery_required());
  EXPECT_EQ(replica.committed().hash, committed.hash);
  EXPECT_EQ(replica.committed().payload, committed.payload);
  EXPECT_EQ(replica.committed_generation(), committed.generation);
  EXPECT_EQ(replica.accepted_head_certificates(), certificates);
  EXPECT_TRUE(replica.unreconstructable_heads().empty());
  std::vector<Hash256> certified;
  for (const auto& certificate : certificates) certified.push_back(certificate.hash);
  std::sort(certified.begin(), certified.end());
  EXPECT_EQ(test::hashes_of(replica.accepted_heads()), certified);

  const auto diagnostics = replica.diagnostics();
  EXPECT_EQ(diagnostics.history_records, frames.size());
  EXPECT_EQ(diagnostics.history_file_bytes, history.size());

  for (const auto& frame : frames) {
    const auto entry = replica.history_entry(frame.entry.hash);
    if (!entry) {
      ADD_FAILURE() << "no entry at offset " << frame.offset;
      continue;
    }
    if (by_hash.at(frame.entry.hash) != &frame) continue;
    EXPECT_EQ(*entry, frame.entry);
    const auto links = replica.history_links(frame.entry.hash);
    if (links) {
      EXPECT_EQ(links->generation, frame.entry.generation);
      EXPECT_EQ(links->merge_parents, frame.entry.merge_parents);
    } else {
      ADD_FAILURE() << "no links at offset " << frame.offset;
    }
    if (frame.entry.previous_known) {
      EXPECT_TRUE(replica.history_is_ancestor(frame.entry.previous, frame.entry.hash));
    } else if (frame.entry.previous != Hash256{}) {
      // A compacted root's parent hash is ancestry, though not traversable.
      EXPECT_TRUE(replica.history_is_ancestor(frame.entry.previous, frame.entry.hash));
    }
    for (const auto& parent : frame.entry.merge_parents) EXPECT_TRUE(replica.history_is_ancestor(parent, frame.entry.hash));

    const auto replayed = replica.historical(frame.entry.hash);
    if (frame.entry.body == HistoryEntry::Body::full) {
      EXPECT_TRUE(replayed.has_value());
      if (replayed) {
        EXPECT_EQ(replayed->hash, frame.entry.hash);
        EXPECT_EQ(replayed->payload, frame.entry.payload);
      }
    } else if (replayed) {
      // Held to the frame's hash by the replica's own replay.
      EXPECT_EQ(replayed->hash, frame.entry.hash);
      ++counts.deltas_replayed;
    }
  }

  // The replica materialises every head, so nothing about opening needed a write.
  EXPECT_EQ(read_file(directory / "history.log"), history);
  EXPECT_EQ(read_file(directory / "checkpoint.meta"), checkpoint_file);
  EXPECT_EQ(read_file(directory / "heads.meta"), heads_file);

  // The author continues: same id, the next sequence, no rotation (section 4.2, item 10).
  if (std::filesystem::exists(directory / "author.meta")) {
    const auto author_plain = open_file(key, author_magic, author_magic, read_file(directory / "author.meta"));
    Reader plain(author_plain);
    const NodeId id{plain.fixed<16>()};
    const auto reserved = plain.u64();
    (void)plain.u64();
    const auto past = plain.u32();
    (void)plain.view(size_t{past} * 16);
    plain.finish();
    expect_seal_reproduces(key, author_magic, read_file(directory / "author.meta"), author_plain);
    const auto clock = opaque_fields(committed.payload).clock;
    const auto observed = clock.contains(id) ? clock.at(id) : uint64_t{0};
    const auto dot = replica.reserve_mutation_dot(id, clock);
    EXPECT_EQ(dot.author, id);
    EXPECT_EQ(dot.sequence, std::max(reserved, observed) + 1);
    EXPECT_EQ(replica.author_ids(id, clock).front(), id);
  }
  return counts;
}

// Copies a state's metadata files beside a key, as a replica's directory.
void copy_state(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::filesystem::create_directories(to);
  for (const auto& entry : std::filesystem::directory_iterator(from)) {
    if (entry.is_regular_file()) std::filesystem::copy_file(entry.path(), to / entry.path().filename());
  }
}

class HeadsReplicaCheckedIn : public testing::TestWithParam<std::string> {};

TEST_P(HeadsReplicaCheckedIn, EveryFrameOpensAndReplays) {
  dannan::test::TempDir dir;
  for (const auto& [name, bytes] : read_checked_in(GetParam())) {
    if (name != "current.meta") write_file(dir.path() / name, bytes);
  }
  const auto key = fixture_key();
  const auto counts = prove_state(*key, key, dir.path());
  EXPECT_GT(counts.frames, 0U);
  EXPECT_EQ(counts.heads, GetParam() == "twoheads" ? 2U : 1U);
  if (GetParam() == "state-scratch") {
    // Macha's README: 13 frames, 12 full and 1 delta, generation 8.
    EXPECT_EQ(counts.frames, 13U);
    EXPECT_EQ(counts.full, 12U);
    EXPECT_EQ(counts.delta, 1U);
  } else if (GetParam() == "branch") {
    // A fork at generation 5, the head a delta with one extra parent, replayed.
    EXPECT_EQ(counts.frames, 7U);
    EXPECT_EQ(counts.delta, 2U);
    EXPECT_EQ(counts.deltas_replayed, 1U);
  } else if (GetParam() == "delete") {
    // A delta naming a batch, DLTB, heads the chain, replayed.
    EXPECT_EQ(counts.frames, 9U);
    EXPECT_EQ(counts.delta, 1U);
    EXPECT_EQ(counts.deltas_replayed, 1U);
  } else if (GetParam() == "twoheads") {
    // Macha's README: a crash with two accepted heads at generation 4, five whole frames.
    EXPECT_EQ(counts.frames, 5U);
    EXPECT_EQ(counts.delta, 0U);
  }
}

INSTANTIATE_TEST_SUITE_P(Macha0934, HeadsReplicaCheckedIn, testing::Values("state-scratch", "branch", "delete", "seed", "twoheads"));

struct FixtureState {
  const char* fixture;
  const char* node;
  size_t frames;
  size_t delta;
  size_t deltas_replayed;
  size_t heads = 1;
};

class HeadsReplicaFixture : public testing::TestWithParam<FixtureState> {};

TEST_P(HeadsReplicaFixture, EveryFrameOpensAndReplays) {
  const auto root = macha_fixtures() / GetParam().fixture;
  const auto metadata = root / GetParam().node / "metadata";
  if (!std::filesystem::exists(metadata)) GTEST_SKIP() << GetParam().fixture << " is not here (DAGDA_MACHA_FIXTURES)";
  dannan::test::TempDir dir;
  const auto directory = dir.path() / "state";
  copy_state(metadata, directory);
  const auto key = fixture_key();
  const auto counts = prove_state(*key, key, directory);
  EXPECT_EQ(counts.frames, GetParam().frames);
  EXPECT_EQ(counts.delta, GetParam().delta);
  EXPECT_EQ(counts.deltas_replayed, GetParam().deltas_replayed);
  EXPECT_EQ(counts.heads, GetParam().heads);
  const auto frames = walk_frames(*key, read_file(directory / "history.log"));
  expect_matches_dump(frames, root / (std::string("macha-metadata-dump-") + GetParam().node + ".txt"));
}

std::string fixture_name(const testing::TestParamInfo<FixtureState>& info) {
  std::string name = std::string(info.param.fixture) + "_" + info.param.node;
  for (auto& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  return name;
}

// The counts are Macha's READMEs' and the dumps'. deltas_replayed: only a delta at a
// head can be, its result being in checkpoint.meta; the rest are the owner's to replay.
INSTANTIATE_TEST_SUITE_P(
    Macha0934, HeadsReplicaFixture,
    testing::Values(FixtureState{"state-scratch-0.93.4", "a", 13, 1, 0}, FixtureState{"state-scratch-0.93.4", "b", 13, 1, 0},
                    FixtureState{"state-scratch-0.93.4-branch", "a", 7, 2, 1}, FixtureState{"state-scratch-0.93.4-branch", "b", 7, 2, 1},
                    FixtureState{"state-scratch-0.93.4-delete", "a", 9, 1, 1}, FixtureState{"state-scratch-0.93.4-delete", "b", 9, 1, 1},
                    FixtureState{"state-scratch-0.93.4-markers", "a", 22, 4, 0}, FixtureState{"state-scratch-0.93.4-markers", "b", 14, 3, 0},
                    FixtureState{"state-scratch-0.93.4-truncated", "a", 4, 0, 0}, FixtureState{"state-scratch-0.93.4-truncated", "b", 51, 0, 0},
                    FixtureState{"state-scratch-0.93.4-seed", "a", 9, 0, 0}, FixtureState{"state-scratch-0.93.4-seed", "b", 9, 0, 0},
                    // Node a of -recovery has no heads.meta: its own cases, below.
                    FixtureState{"state-scratch-0.93.4-recovery", "b", 9, 0, 0}, FixtureState{"state-scratch-0.93.4-twoheads", "a", 5, 0, 0, 2},
                    FixtureState{"state-scratch-0.93.4-twoheads", "b", 4, 1, 1}),
    fixture_name);

// A real seed opens by Macha's rule and recovers a state whose checkpoint is damaged,
// unaccepted: the record the checkpoint named, no certificate.
TEST(HeadsReplicaBytes, AMachaSeedOpensAndRecovers) {
  const auto files = read_checked_in("seed");
  const auto key = fixture_key();
  const auto seed = decode_record(open_file(*key, seed_magic, seed_magic, files.at("current.meta")));
  EXPECT_EQ(encode_record(seed), open_file(*key, seed_magic, seed_magic, files.at("current.meta")));
  expect_seal_reproduces(*key, seed_magic, files.at("current.meta"), encode_record(seed));
  EXPECT_EQ(files.at("current.meta").size(), 294U);

  dannan::test::TempDir dir;
  for (const auto& [name, bytes] : files) {
    if (name != "current.meta") write_file(dir.path() / name, bytes);
  }
  auto damaged = read_file(dir.path() / "checkpoint.meta");
  damaged[40] ^= 0x5a;
  write_file(dir.path() / "checkpoint.meta", damaged);

  ReplicaOptions options;
  options.directory = dir.path();
  options.content_key = key;
  options.local_key = key;
  options.magics = machas_magics();
  options.aad = machas_aad();
  options.codec = std::make_shared<OpaqueCodec>();
  options.logger = test::quiet_logger();
  options.recovery_seed = seed;
  Replica replica(std::move(options));
  EXPECT_TRUE(replica.recovery_required());
  EXPECT_EQ(replica.committed().hash, seed.hash);
  EXPECT_EQ(replica.committed().payload, seed.payload);
  EXPECT_TRUE(replica.accepted_heads().empty());
  EXPECT_FALSE(replica.acceptance(seed.hash).has_value());
}

// The other node's seed, from Macha's fixtures.
TEST(HeadsReplicaBytes, BothFixtureSeedsOpen) {
  const auto root = macha_fixtures() / "state-scratch-0.93.4-seed";
  if (!std::filesystem::exists(root / "cache-b")) GTEST_SKIP() << "state-scratch-0.93.4-seed is not here (DAGDA_MACHA_FIXTURES)";
  const auto key = fixture_key();
  for (const char* node : {"a", "b"}) {
    const auto file = read_file(root / (std::string("cache-") + node) / "metadata/current.meta");
    const auto seed = decode_record(open_file(*key, seed_magic, seed_magic, file));
    EXPECT_EQ(seed.generation, 9U);
    EXPECT_EQ(hex(seed.hash).substr(0, 16), "f2d9a033310a2912");
  }
}

// Macha's state goes on working: a commit extends it, and all of it reopens.
TEST(HeadsReplicaBytes, ACommitExtendsMachasStateAndItAllReopens) {
  dannan::test::TempDir dir;
  for (const auto& [name, bytes] : read_checked_in("state-scratch")) write_file(dir.path() / name, bytes);
  const auto key = fixture_key();
  const auto open = [&] {
    ReplicaOptions options;
    options.directory = dir.path();
    options.content_key = key;
    options.local_key = key;
    options.magics = machas_magics();
    options.aad = machas_aad();
    options.codec = std::make_shared<OpaqueCodec>();
    options.logger = test::quiet_logger();
    return std::make_unique<Replica>(std::move(options));
  };
  Record next;
  {
    auto replica = open();
    const auto head = replica->committed();
    EXPECT_EQ(head.generation, 13U);
    next = test::record_after(head, Bytes(head.payload.begin(), head.payload.end()));
    ASSERT_TRUE(replica->store_commit(next));
    ASSERT_TRUE(replica->accept_commit({next.generation, next.hash, 1, {test::author(1)}}));
    EXPECT_EQ(replica->committed().hash, next.hash);
  }
  auto reopened = open();
  EXPECT_EQ(reopened->committed().hash, next.hash);
  EXPECT_EQ(reopened->diagnostics().history_records, 14U);
  EXPECT_FALSE(reopened->recovery_required());
  const auto frames = walk_frames(*key, read_file(dir.path() / "history.log"));
  ASSERT_EQ(frames.size(), 14U);
  EXPECT_EQ(frames.front().entry.generation, 1U);
  EXPECT_EQ(frames.back().entry.hash, next.hash);
}

// The clock section's place in a whole record, as opaque_fields reads it.
size_t clock_offset(std::span<const uint8_t> payload) {
  Reader in(payload);
  (void)in.view(8);
  const auto voters = in.u32();
  return 8 + 4 + size_t{voters} * 16 + 4 + 8;
}

// A mutation that says nothing but its clock, as Macha's mutate_delta would on a commit
// that changes nothing (R1): the rest of the record is carried as bytes, torrent
// sections and all, and Dagda parses none of it. It has no delta, so its frame is whole.
class ClockOnly final : public Mutation {
 public:
  explicit ClockOnly(const State& parent) : _payload(static_cast<const OpaqueState*>(parent.get())->payload) {}
  void edit(const MutationContext&) override {}
  void stamp(const MutationContext&) override {}
  void advance_clock(const MutationContext& context) override {
    const auto at = clock_offset(_payload);
    Reader in(std::span<const uint8_t>(_payload).subspan(at));
    auto clock = decode_clock(in);
    const auto old_bytes = _payload.size() - at - in.remaining();
    clock[context.dot.author] = context.dot.sequence;
    Writer out;
    encode_clock(out, clock);
    const auto bytes = out.take();
    Bytes next(_payload.begin(), _payload.begin() + static_cast<std::ptrdiff_t>(at));
    next.insert(next.end(), bytes.begin(), bytes.end());
    next.insert(next.end(), _payload.begin() + static_cast<std::ptrdiff_t>(at + old_bytes), _payload.end());
    _payload = std::move(next);
  }
  void apply(const MutationContext&) override {}
  State state() const override { return std::make_shared<const OpaqueState>(OpaqueState{_payload}); }
  Bytes delta() const override { return {}; }
  Introduced introduced(const MutationContext&) const override { return {}; }

 private:
  Bytes _payload;
};

// S4 on Macha's own state: a space's commit extends the head two real nodes left, with
// the next dot of the author in author.meta. The frame seals as Macha's, the whole chain
// still reads frame by frame, and it all reopens. Macha's own codec is what a real commit
// runs at the port; this holds only what is Dagda's.
TEST(HeadsReplicaBytes, ASpaceCommitExtendsMachasStateWithTheNextDot) {
  dannan::test::TempDir dir;
  for (const auto& [name, bytes] : read_checked_in("state-scratch")) write_file(dir.path() / name, bytes);
  const auto key = fixture_key();
  const auto open = [&] {
    ReplicaOptions replica;
    replica.directory = dir.path();
    replica.content_key = key;
    replica.local_key = key;
    replica.magics = machas_magics();
    replica.aad = machas_aad();
    replica.codec = std::make_shared<OpaqueCodec>();
    replica.logger = test::quiet_logger();
    SpaceSpec spec;
    spec.name = "macha/namespace";
    spec.codec = replica.codec;
    SpaceServices services;
    services.self = test::author(1);
    services.commit_nodes = [] { return std::make_unique<MemoryTreeNodeStore>(); };
    services.read_nodes = [] { return std::make_unique<MemoryTreeNodeStore>(); };
    return std::make_unique<Space>(std::move(spec), std::move(replica), std::move(services));
  };
  const MutationFactory mutation = [](const State& parent) { return std::make_unique<ClockOnly>(parent); };

  Record first;
  Dot dot;
  {
    auto space = open();
    const auto head = space->replica().committed();
    ASSERT_EQ(head.generation, 13U);
    const auto parent_clock = opaque_fields(head.payload).clock;
    const auto outcome = space->mutate_delta(mutation);
    ASSERT_EQ(outcome.kind, CommitOutcome::Kind::committed);
    first = outcome.record;
    dot = *outcome.dot;
    EXPECT_EQ(first.generation, 14U);
    EXPECT_EQ(first.previous, head.hash);
    EXPECT_EQ(outcome.body, HistoryEntry::Body::full);
    EXPECT_EQ(space->replica().committed().hash, first.hash);

    // The clock moved by that one entry, and the author continues what author.meta held.
    auto expected = parent_clock;
    expected[dot.author] = dot.sequence;
    EXPECT_EQ(opaque_fields(first.payload).clock, expected);
    const auto at = parent_clock.find(dot.author);
    ASSERT_NE(at, parent_clock.end()) << "the fixture's own author";
    EXPECT_GT(dot.sequence, at->second);
    // Everything past the clock is the parent's, byte for byte.
    const auto tail = head.payload.size() - clock_offset(head.payload) - (4 + parent_clock.size() * 24);
    EXPECT_TRUE(
        std::equal(head.payload.end() - static_cast<std::ptrdiff_t>(tail), head.payload.end(), first.payload.end() - static_cast<std::ptrdiff_t>(tail)));
  }

  const auto frames = walk_frames(*key, read_file(dir.path() / "history.log"));
  ASSERT_EQ(frames.size(), 14U);
  EXPECT_EQ(frames.back().entry.hash, first.hash);
  EXPECT_EQ(frames.back().entry.body, HistoryEntry::Body::full);

  auto reopened = open();
  EXPECT_EQ(reopened->replica().committed().hash, first.hash);
  EXPECT_FALSE(reopened->replica().recovery_required());
  const auto next = reopened->mutate_delta(mutation);
  EXPECT_EQ(next.dot->author, dot.author);
  EXPECT_EQ(next.dot->sequence, dot.sequence + 1);
  EXPECT_EQ(next.record.previous, first.hash);
}

std::unique_ptr<Replica> open_macha_state(const std::filesystem::path& directory, const std::shared_ptr<const domain::StorageKey>& key,
                                          OpaqueCodec::Known known = {}) {
  ReplicaOptions options;
  options.directory = directory;
  options.content_key = key;
  options.local_key = key;
  options.magics = machas_magics();
  options.aad = machas_aad();
  options.codec = std::make_shared<OpaqueCodec>(std::move(known));
  options.logger = test::quiet_logger();
  return std::make_unique<Replica>(std::move(options));
}

// A node recovered from its cache seed, as 0.93.4 left it: the marker holds, the seed
// grants no authority, and what was set aside is left as it lies. Macha's own dump tool
// aborted here (exit -6, "truncated input"); the node opened, and so does this.
TEST(HeadsReplicaBytes, AMachaNodeInRecoveryOpensStillInRecovery) {
  const auto files = read_checked_in("recovery");
  dannan::test::TempDir dir;
  for (const auto& [name, bytes] : files) write_file(dir.path() / name, bytes);
  EXPECT_FALSE(files.contains("heads.meta"));
  const auto key = fixture_key();
  const auto stamp = std::string(".corrupt.1793982986266051000");

  const auto frames = walk_frames(*key, files.at("history.log"));
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].entry.generation, 9U);
  EXPECT_EQ(frames[0].entry.body, HistoryEntry::Body::full);
  EXPECT_FALSE(frames[0].entry.previous_known);
  EXPECT_EQ(hex(frames[0].entry.hash).substr(0, 16), "28597164ce6669b2");

  // What was set aside: the flipped checkpoint keeps its damage, the rest still opens.
  EXPECT_ANY_THROW((void)open_file(*key, checkpoint_magic, checkpoint_magic, files.at("checkpoint.meta" + stamp)));
  const auto old_heads = decode_acceptance_set(open_file(*key, heads_magic, heads_magic, files.at("heads.meta" + stamp)));
  ASSERT_EQ(old_heads.size(), 1U);
  EXPECT_EQ(old_heads[0].hash, frames[0].entry.hash);
  const auto old_frames = walk_frames(*key, files.at("history.log" + stamp));
  ASSERT_EQ(old_frames.size(), 9U);
  EXPECT_EQ(old_frames.back().entry.hash, frames[0].entry.hash);
  EXPECT_TRUE(files.at("journal.log" + stamp).empty());

  const auto author_plain = open_file(*key, author_magic, author_magic, files.at("author.meta"));
  Reader author_reader(author_plain);
  const NodeId author{author_reader.fixed<16>()};
  for (int run = 0; run < 2; ++run) {
    auto replica = open_macha_state(dir.path(), key);
    EXPECT_TRUE(replica->recovery_required());
    EXPECT_EQ(replica->committed().hash, frames[0].entry.hash);
    EXPECT_EQ(replica->committed().generation, 9U);
    EXPECT_TRUE(replica->accepted_heads().empty());
    EXPECT_FALSE(replica->acceptance(frames[0].entry.hash).has_value());
    EXPECT_EQ(replica->diagnostics().history_records, 1U);
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "recovery.required"));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "author-chain-broken"));
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "heads.meta"));
    for (const auto& [name, bytes] : files) EXPECT_EQ(read_file(dir.path() / name), bytes) << name;
  }
  // The marked chain: this node's next commit is a new author's, as Macha's is.
  auto replica = open_macha_state(dir.path(), key);
  const auto clock = opaque_fields(replica->committed().payload).clock;
  const auto dot = replica->reserve_mutation_dot(author, clock);
  EXPECT_NE(dot.author, author);
  EXPECT_EQ(dot.sequence, 1U);
}

// The same state in Macha's tree: the short listing, the seed it came from, and the peer.
TEST(HeadsReplicaBytes, AMachaNodeInRecoveryAgainstItsDumpSeedAndPeer) {
  const auto root = macha_fixtures() / "state-scratch-0.93.4-recovery";
  if (!std::filesystem::exists(root / "a/metadata")) GTEST_SKIP() << "state-scratch-0.93.4-recovery is not here (DAGDA_MACHA_FIXTURES)";
  const auto key = fixture_key();
  const auto frames = walk_frames(*key, read_file(root / "a/metadata/history.log"));
  expect_matches_dump(frames, root / "macha-metadata-dump-a.txt");
  ASSERT_EQ(frames.size(), 1U);
  const auto seed = decode_record(open_file(*key, seed_magic, seed_magic, read_file(root / "cache-a/metadata/current.meta")));
  EXPECT_EQ(seed.hash, frames[0].entry.hash);
  // Node a's new frame holds what its peer's frame at generation 9 holds.
  const auto peer = walk_frames(*key, read_file(root / "b/metadata/history.log"));
  ASSERT_EQ(peer.size(), 9U);
  EXPECT_EQ(peer.back().entry.hash, frames[0].entry.hash);
  EXPECT_EQ(peer.back().entry.payload, frames[0].entry.payload);
}

// A crash between two heads and their merge: both certificates stand, neither is the
// other's ancestor, and the preferred one is the higher hash (R4).
TEST(HeadsReplicaBytes, TwoAcceptedHeadsAtAGenerationStayTwo) {
  dannan::test::TempDir dir;
  for (const auto& [name, bytes] : read_checked_in("twoheads")) write_file(dir.path() / name, bytes);
  const auto key = fixture_key();
  const auto replica = open_macha_state(dir.path(), key);
  const auto heads = replica->accepted_heads();
  ASSERT_EQ(heads.size(), 2U);
  EXPECT_EQ(heads[0].generation, 4U);
  EXPECT_EQ(heads[1].generation, 4U);
  std::set<std::string> prefixes;
  for (const auto& head : heads) prefixes.insert(hex(head.hash).substr(0, 8));
  EXPECT_EQ(prefixes, (std::set<std::string>{"095a7813", "3dfada38"}));
  EXPECT_EQ(hex(replica->committed().hash).substr(0, 8), "3dfada38");
  EXPECT_FALSE(replica->recovery_required());
  EXPECT_FALSE(replica->history_is_ancestor(heads[0].hash, heads[1].hash));
  EXPECT_FALSE(replica->history_is_ancestor(heads[1].hash, heads[0].hash));
  const auto base = replica->history_common_ancestor(heads[0].hash, heads[1].hash);
  ASSERT_TRUE(base.has_value());
  EXPECT_EQ(hex(*base).substr(0, 8), "e33439ac");
}

// The six kinds of damage Macha's test does to a frame.
using RealDamage = void (*)(std::span<uint8_t> file, const Frame& frame);

void put_u32(std::span<uint8_t> file, uint64_t at, uint32_t value) {
  file[at] = static_cast<uint8_t>(value >> 24);
  file[at + 1] = static_cast<uint8_t>(value >> 16);
  file[at + 2] = static_cast<uint8_t>(value >> 8);
  file[at + 3] = static_cast<uint8_t>(value);
}

const std::array<std::pair<const char*, RealDamage>, 6> real_damages{{
    {"past_the_end", [](std::span<uint8_t> file, const Frame& frame) { put_u32(file, frame.offset, 0x10000000); }},
    {"over_any_record", [](std::span<uint8_t> file, const Frame& frame) { put_u32(file, frame.offset, 0xFFFFFFFF); }},
    {"short_inside", [](std::span<uint8_t> file, const Frame& frame) { put_u32(file, frame.offset, static_cast<uint32_t>((frame.bytes - 4) / 2)); }},
    {"long_inside", [](std::span<uint8_t> file, const Frame& frame) { put_u32(file, frame.offset, static_cast<uint32_t>(frame.bytes - 4 + 9)); }},
    {"zero", [](std::span<uint8_t> file, const Frame& frame) { put_u32(file, frame.offset, 0); }},
    {"body_byte", [](std::span<uint8_t> file, const Frame& frame) { file[frame.offset + 4 + (frame.bytes - 4) / 2] ^= 0x5a; }},
}};

// A real state: a name under dagda/test/data, or "fixture/node" in Macha's tree (empty
// when that is not here).
SmallFiles load_real_state(const std::string& name) {
  if (name.find('/') == std::string::npos) return read_checked_in(name);
  SmallFiles out;
  const auto directory = macha_fixtures() / name / "metadata";
  if (!std::filesystem::exists(directory)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    if (entry.is_regular_file()) out[entry.path().filename().string()] = read_file(entry.path());
  return out;
}

// What a reader holds of these frames with frame `lost` gone, by Macha's rule: a whole
// frame stands alone, a delta needs its parent, and a hash's whole body is preferred.
std::map<Hash256, size_t> held_without(const std::vector<Frame>& frames, std::optional<size_t> lost) {
  std::map<Hash256, size_t> held;
  for (size_t i = 0; i < frames.size(); ++i) {
    if (lost && i == *lost) continue;
    const auto& entry = frames[i].entry;
    if (entry.body == HistoryEntry::Body::full) {
      held[entry.hash] = i;
    } else if (held.contains(entry.previous)) {
      held.emplace(entry.hash, i);
    }
  }
  return held;
}

void expect_held(const Replica& replica, const std::vector<Frame>& frames, const std::map<Hash256, size_t>& held, const Hash256& committed) {
  for (size_t i = 0; i < frames.size(); ++i) {
    const auto& frame = frames[i].entry;
    const auto entry = replica.history_entry(frame.hash);
    const auto at = held.find(frame.hash);
    if (at == held.end()) {
      // The committed head alone comes back, re-rooted from the checkpoint.
      if (frame.hash != committed) {
        EXPECT_FALSE(entry.has_value()) << "frame " << i;
      }
      continue;
    }
    ASSERT_TRUE(entry.has_value()) << "lost frame " << i;
    if (at->second == i && frame.hash != committed) {
      EXPECT_EQ(*entry, frame) << "frame " << i;
    }
  }
}

struct RealDamageCase {
  const char* state;
  size_t damage;
};

class HeadsReplicaDamagedCopy : public testing::TestWithParam<RealDamageCase> {};

// Each frame of a real history.log in turn, one kind of damage: what the reader still
// holds is everything but that frame and any delta that needed it, the bytes stay, a
// node opens without recovery, and a commit past the damage survives another open. The
// last frame has nothing readable after it, so it is the torn tail, cut and kept aside.
TEST_P(HeadsReplicaDamagedCopy, EveryOtherRealFrameSurvives) {
  const auto files = load_real_state(GetParam().state);
  if (files.empty()) GTEST_SKIP() << GetParam().state << " is not here (DAGDA_MACHA_FIXTURES)";
  const auto key = fixture_key();
  const auto& history = files.at("history.log");
  const auto frames = walk_frames(*key, history);
  ASSERT_GT(frames.size(), 1U);
  const auto committed = decode_record(open_file(*key, checkpoint_magic, checkpoint_magic, files.at("checkpoint.meta")));

  OpaqueCodec::Known known;
  for (const auto& frame : frames)
    if (frame.entry.hash == committed.hash && frame.entry.body == HistoryEntry::Body::delta)
      known[dannan::identity::sha256(frame.entry.payload)] = Bytes(committed.payload.begin(), committed.payload.end());

  for (size_t lost = 0; lost < frames.size(); ++lost) {
    SCOPED_TRACE("frame " + std::to_string(lost) + " at offset " + std::to_string(frames[lost].offset));
    const bool last = lost + 1 == frames.size();
    dannan::test::TempDir dir;
    for (const auto& [name, bytes] : files)
      if (name != "current.meta" && name != "history.log") write_file(dir.path() / name, bytes);
    auto damaged = history;
    real_damages[GetParam().damage].second(damaged, frames[lost]);
    const auto log = dir.path() / "history.log";
    write_file(log, damaged);

    const auto held = held_without(frames, lost);
    Record next;
    {
      auto replica = open_macha_state(dir.path(), key, known);
      EXPECT_FALSE(replica->recovery_required());
      EXPECT_EQ(replica->committed().hash, committed.hash);
      expect_held(*replica, frames, held, committed.hash);

      // Nothing before the end moved. Past it only a re-rooted head may have been added.
      const auto kept = read_file(log);
      const auto unchanged = last ? frames[lost].offset : damaged.size();
      ASSERT_GE(kept.size(), unchanged);
      EXPECT_TRUE(std::equal(damaged.begin(), damaged.begin() + static_cast<std::ptrdiff_t>(unchanged), kept.begin()));
      bool kept_aside = false;
      for (const auto& item : std::filesystem::directory_iterator(dir.path()))
        kept_aside |= item.path().filename().string().rfind("history.log.corrupt.", 0) == 0;
      EXPECT_EQ(kept_aside, last);

      next = test::record_after(committed, Bytes(committed.payload.begin(), committed.payload.end()));
      ASSERT_TRUE(replica->store_commit(next));
      ASSERT_TRUE(replica->accept_commit({next.generation, next.hash, 1, {test::author(1)}}));
    }
    auto again = open_macha_state(dir.path(), key, known);
    EXPECT_EQ(again->committed().hash, next.hash);
    EXPECT_TRUE(again->history_entry(next.hash).has_value());
    expect_held(*again, frames, held, committed.hash);
  }
}

std::vector<RealDamageCase> every_real_damage() {
  std::vector<RealDamageCase> out;
  for (const char* state : {"state-scratch", "branch", "delete", "seed", "twoheads", "state-scratch-0.93.4-markers/a", "state-scratch-0.93.4-markers/b",
                            "state-scratch-0.93.4-truncated/b"})
    for (size_t damage = 0; damage < real_damages.size(); ++damage) out.push_back({state, damage});
  return out;
}

std::string real_damage_name(const testing::TestParamInfo<RealDamageCase>& info) {
  std::string name = std::string(info.param.state) + "_" + real_damages[info.param.damage].first;
  for (auto& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  return name;
}

INSTANTIATE_TEST_SUITE_P(Macha0934, HeadsReplicaDamagedCopy, testing::ValuesIn(every_real_damage()), real_damage_name);

// A region bigger than the scan's window, in the middle of a real file, whatever it
// holds: every real frame on both sides of it stays where it is read.
TEST(HeadsReplicaBytes, ADamagedRegionInARealHistoryLosesNoFrame) {
  const auto files = read_checked_in("state-scratch");
  const auto key = fixture_key();
  const auto& history = files.at("history.log");
  const auto frames = walk_frames(*key, history);
  ASSERT_EQ(frames.size(), 13U);
  const auto committed = decode_record(open_file(*key, checkpoint_magic, checkpoint_magic, files.at("checkpoint.meta")));

  std::mt19937 random(9153011);
  Bytes region(2 * 1024 * 1024 + 512 * 1024);
  for (auto& byte : region) byte = static_cast<uint8_t>(random());
  const auto cut = static_cast<std::ptrdiff_t>(frames[6].offset);
  Bytes damaged(history.begin(), history.begin() + cut);
  damaged.insert(damaged.end(), region.begin(), region.end());
  damaged.insert(damaged.end(), history.begin() + cut, history.end());

  dannan::test::TempDir dir;
  for (const auto& [name, bytes] : files)
    if (name != "history.log" && name != "current.meta") write_file(dir.path() / name, bytes);
  write_file(dir.path() / "history.log", damaged);
  const auto replica = open_macha_state(dir.path(), key);
  EXPECT_FALSE(replica->recovery_required());
  EXPECT_EQ(replica->committed().hash, committed.hash);
  expect_held(*replica, frames, held_without(frames, std::nullopt), committed.hash);
  EXPECT_EQ(replica->diagnostics().history_records, frames.size());
  EXPECT_EQ(read_file(dir.path() / "history.log"), damaged);

  // The same scan with no replica, under v18's associated data: what S7's tools call.
  const auto next = next_frame_from(dir.path() / "history.log", damaged.size(), frames[6].offset + 1, *key, history_magic);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(*next, frames[6].offset + region.size());
}

}  // namespace
}  // namespace dagda::heads
