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
// No frame of journal.log is proved here: 0.93.4 writes that file only empty, so no
// fixture holds one (Macha's metadata.cpp:4039, :4070). The reader's bytes rest on
// Macha's two cases, at the bytes they build.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/heads/replica.h"
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
  return "../../macha/build/macha-fixtures";
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
  EXPECT_EQ(counts.heads, 1U);
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
  }
}

INSTANTIATE_TEST_SUITE_P(Macha0934, HeadsReplicaCheckedIn, testing::Values("state-scratch", "branch", "delete", "seed"));

struct FixtureState {
  const char* fixture;
  const char* node;
  size_t frames;
  size_t delta;
  size_t deltas_replayed;
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
  EXPECT_EQ(counts.heads, 1U);
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
INSTANTIATE_TEST_SUITE_P(Macha0934, HeadsReplicaFixture,
                         testing::Values(FixtureState{"state-scratch-0.93.4", "a", 13, 1, 0}, FixtureState{"state-scratch-0.93.4", "b", 13, 1, 0},
                                         FixtureState{"state-scratch-0.93.4-branch", "a", 7, 2, 1}, FixtureState{"state-scratch-0.93.4-branch", "b", 7, 2, 1},
                                         FixtureState{"state-scratch-0.93.4-delete", "a", 9, 1, 1}, FixtureState{"state-scratch-0.93.4-delete", "b", 9, 1, 1},
                                         FixtureState{"state-scratch-0.93.4-markers", "a", 22, 4, 0},
                                         FixtureState{"state-scratch-0.93.4-markers", "b", 14, 3, 0},
                                         FixtureState{"state-scratch-0.93.4-truncated", "a", 4, 0, 0},
                                         FixtureState{"state-scratch-0.93.4-truncated", "b", 51, 0, 0}, FixtureState{"state-scratch-0.93.4-seed", "a", 9, 0, 0},
                                         FixtureState{"state-scratch-0.93.4-seed", "b", 9, 0, 0}),
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

}  // namespace
}  // namespace dagda::heads
