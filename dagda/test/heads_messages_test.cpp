//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The heads' messages: their registration (the plan's 2.3, C14), and their bytes. Three proofs
// of the bytes, each independent of the code it tests:
//
// - Known answers written out here by hand from Macha's encoders at 9a08846 (metadata.cpp:1742,
//   :1751, :1811, :1859; metadata_server.cpp:15; cluster.cpp:743).
// - What two real Macha 0.93.4 nodes wrote: heads.meta's plaintext is the certificate set a
//   metadata_heads_reply carries, a history frame's the entry a put carries, checkpoint.meta's the
//   record a metadata_reply carries (metadata.cpp:2431, :2731). A few are checked in as hex, and the
//   rest are read from Macha's fixtures and skip, saying so, where those are absent. They are
//   opaque: the owner's payload inside is never parsed.
// - The same real history carried between two Dagda nodes over Dannan, on an owner's numbers, and
//   captured at the receiver: the payloads on the wire are those bytes, with nothing after them.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/heads/exchange.h"
#include "dagda/heads/messages.h"
#include "dagda/heads/sealed_file.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/heads_space.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/net/routes.h"
#include "dannan/test/fixtures/gate.h"

namespace dagda::heads {
namespace {

using namespace std::chrono_literals;
using dannan::identity::hex;
using dannan::identity::unhex;
using dannan::net::Lane;
using dannan::net::Reader;
using dannan::net::RouteTable;
using dannan::sched::FrameType;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::wait_until;

Bytes bytes_of(const std::string& hexed) {
  const auto out = unhex(hexed);
  EXPECT_TRUE(out.has_value()) << hexed;
  return out.value_or(Bytes{});
}

// Hash256 of one repeated byte.
Hash256 filled(uint8_t byte) {
  Hash256 out;
  out.bytes.fill(byte);
  return out;
}

std::string repeated(const std::string& pair, size_t times) {
  std::string out;
  for (size_t i = 0; i < times; ++i) out += pair;
  return out;
}

struct Recorder final : dannan::Routes {
  explicit Recorder(RouteTable& t) : table(t) {}
  RouteTable& table;
  void add(dannan::net::MessageType type, dannan::net::Registration registration, dannan::net::RequestHandler) override { table.add(type, registration); }
  void retire(dannan::net::MessageType) override {}
  void bind(dannan::net::MessageType, dannan::net::RequestHandler) override {}
  void unbind(dannan::net::MessageType) override {}
  std::shared_ptr<dannan::net::CallHandle> call(const dannan::membership::NodeRecord&, dannan::net::MessageType, dannan::net::Bytes, FrameType,
                                                std::chrono::milliseconds, boost::asio::any_io_executor, Done, dannan::net::CallWatch) override {
    return {};
  }
  std::shared_ptr<dannan::net::CallHandle> call(const dannan::net::Endpoint&, dannan::net::MessageType, dannan::net::Bytes, FrameType,
                                                std::chrono::milliseconds, boost::asio::any_io_executor, Done, dannan::net::CallWatch) override {
    return {};
  }
  dannan::net::CallOutcome call_and_wait(const dannan::membership::NodeRecord&, dannan::net::MessageType, dannan::net::Bytes, FrameType,
                                         std::chrono::milliseconds, dannan::net::CallWatch) override {
    return {};
  }
  dannan::net::CallOutcome call_and_wait(const dannan::net::Endpoint&, dannan::net::MessageType, dannan::net::Bytes, FrameType, std::chrono::milliseconds,
                                         dannan::net::CallWatch) override {
    return {};
  }
};

std::vector<dannan::net::MessageType> all_of(const message::Numbers& n) {
  return {n.put_history_entry,
          n.put_commit,
          n.accept_commit,
          n.get_history_entry,
          n.has_history_entry,
          n.get_heads,
          n.get_history_record,
          n.get_committed,
          n.get_current,
          n.get_identity,
          n.notice,
          n.record_reply,
          n.identity_reply,
          n.history_entry_reply,
          n.heads_reply};
}

// C14. Three mutations ordered, per peer (H26), and no other, on Dagda's numbers and an owner's.
TEST(HeadsMessages, TheThreeMutationsAreOrderedAndNoOther) {
  for (const auto& numbers : {message::Numbers{}, test::owner_numbers()}) {
    auto table = RouteTable::with_dannan_types();
    Recorder routes(table);
    message::register_types(routes, numbers);
    const std::set<dannan::net::MessageType> ordered{numbers.put_history_entry, numbers.put_commit, numbers.accept_commit};
    for (const auto type : all_of(numbers)) {
      EXPECT_EQ(table.is_ordered(type), ordered.contains(type)) << table.name(type);
      EXPECT_EQ(table.is_notification(type), type == numbers.notice) << table.name(type);
      EXPECT_FALSE(table.is_fast_control(type)) << table.name(type);
    }
  }
}

// C14. The CONTROL lane in any class, as lane_for puts them (net.cpp:2027-2037); a number taken
// refuses, as Dannan refuses it; and the plan's table is what an owner's numbers come to. Which
// numbers a node runs on is the owner's to say, and Dagda's own overlap neither the store's nor
// an owner's.
TEST(HeadsMessages, LanesFramesAndADuplicateRegistration) {
  const auto numbers = test::owner_numbers();
  auto table = RouteTable::with_dannan_types();
  Recorder routes(table);
  message::register_types(routes, numbers);
  for (const auto type : all_of(numbers)) {
    for (const auto frame : {FrameType::control, FrameType::read_ahead, FrameType::speculative}) {
      EXPECT_TRUE(table.allowed(type, Lane::control, frame)) << table.name(type);
      EXPECT_FALSE(table.allowed(type, Lane::data, frame)) << table.name(type);
    }
  }
  EXPECT_THROW(message::register_types(routes, numbers), std::invalid_argument);

  const std::map<dannan::net::MessageType, uint16_t> plan{
      {numbers.put_history_entry, 540},  {numbers.put_commit, 542},        {numbers.accept_commit, 543},
      {numbers.get_history_entry, 538},  {numbers.has_history_entry, 539}, {numbers.get_heads, 541},
      {numbers.get_history_record, 553}, {numbers.get_committed, 522},     {numbers.notice, 521},
      {numbers.record_reply, 617},       {numbers.identity_reply, 620},    {numbers.history_entry_reply, 623},
      {numbers.heads_reply, 624},        {numbers.get_current, 518},       {numbers.get_identity, 533},
  };
  EXPECT_EQ(plan.size(), all_of(numbers).size());
  for (const auto& [type, number] : plan) EXPECT_EQ(static_cast<uint16_t>(type), number);
  // The five retired numbers are registered by nobody: their slots stay free.
  for (const int retired : {7, 8, 11, 17, 18}) EXPECT_EQ(table.find(dannan::net::MessageType{static_cast<uint16_t>(512 + retired)}), nullptr) << retired;

  // Dagda's own and the store's, in one table, as a node with no owner's numbers registers them.
  auto own_table = RouteTable::with_dannan_types();
  Recorder own_routes(own_table);
  store::message::register_types(own_routes);
  EXPECT_NO_THROW(message::register_types(own_routes, message::Numbers{}));
}

// A node's registration under `heads` off and on, on a real node. Off stands for the first
// stage of the port, whose owner registers these numbers itself, ordered: nothing here collides.
TEST(HeadsMessages, OffRegistersNothingSoAnOwnersOwnRegistrationStands) {
  Cluster cluster;
  ClusterNodeOptions off;
  off.heads = false;
  auto& a = cluster.add("a", off);
  a.open();
  const auto numbers = test::owner_numbers();
  // The owner's own, at those numbers: the node does not take them.
  EXPECT_NO_THROW(message::register_types(a.context().routes(), numbers));
  a.start();

  ClusterNodeOptions on;
  on.exchange.numbers = numbers;
  auto& b = cluster.add("b", on);
  b.open();
  // One registration of a number is all Dannan allows.
  EXPECT_THROW(message::register_types(b.context().routes(), numbers), std::invalid_argument);
  b.start();
}

// ---- Known answers ----------------------------------------------------------

TEST(HeadsWire, EveryPayloadIsTheFormsKnownAnswer) {
  // A history entry: u8 body, u64 generation, previous, hash, u8 previous_known, u32 parents and
  // each, then the payload as a u32-prefixed blob.
  HistoryEntry entry;
  entry.body = HistoryEntry::Body::delta;
  entry.generation = 0x0102030405060708ULL;
  entry.previous = filled(0xAA);
  entry.hash = filled(0xBB);
  entry.previous_known = true;
  entry.merge_parents = {filled(0xCC)};
  entry.payload = {0x11, 0x22, 0x33};
  const auto entry_bytes = bytes_of(
      "02"
      "0102030405060708" +
      repeated("aa", 32) + repeated("bb", 32) +
      "01"
      "00000001" +
      repeated("cc", 32) +
      "00000003"
      "112233");
  EXPECT_EQ(encode_history_entry(entry), entry_bytes);
  EXPECT_EQ(decode_history_entry(entry_bytes), entry);

  // A certificate: u64 generation, hash, u32 required, u32 count, each replica sorted.
  NodeId low{};
  low.bytes.fill(0x01);
  NodeId high{};
  high.bytes.fill(0x02);
  const Acceptance acceptance{7, filled(0xDD), 2, {high, low}};
  const auto acceptance_bytes = bytes_of("0000000000000007" + repeated("dd", 32) +
                                         "00000002"
                                         "00000002" +
                                         repeated("01", 16) + repeated("02", 16));
  EXPECT_EQ(encode_acceptance(acceptance), acceptance_bytes);
  EXPECT_EQ(decode_acceptance(acceptance_bytes).replicas, (std::vector<NodeId>{low, high}));

  // The set: u32 count, each a u32-prefixed certificate.
  // 8 + 32 + 4 + 4 + 16 + 16 = 80.
  EXPECT_EQ(encode_acceptance_set({acceptance}), bytes_of("00000001"
                                                          "00000050" +
                                                          hex(acceptance_bytes)));

  // A record: u64 generation, previous, hash, then the payload as a blob.
  const auto payload = Bytes{0x41, 0x42};
  auto record = test::make_record(3, filled(0xEE), payload);
  EXPECT_EQ(encode_record(record), bytes_of("0000000000000003" + repeated("ee", 32) + hex(record.hash.bytes) +
                                            "00000002"
                                            "4142"));

  // The identity reply: u64 generation, hash; 40 bytes, as SM:4206 counts them.
  const auto identity = message::encode_identity({9, filled(0x77)});
  EXPECT_EQ(identity, bytes_of("0000000000000009" + repeated("77", 32)));
  EXPECT_EQ(identity.size(), 40u);
  EXPECT_EQ(message::decode_identity(identity), (HeadIdentity{9, filled(0x77)}));

  // A notice: the generation alone. A hash request: the hash alone.
  EXPECT_EQ(message::encode_notice(0x10), bytes_of("0000000000000010"));
  EXPECT_EQ(message::decode_notice(bytes_of("0000000000000010")), 0x10u);
  EXPECT_EQ(message::encode_hash(filled(0x55)), bytes_of(repeated("55", 32)));
  EXPECT_EQ(message::decode_hash(bytes_of(repeated("55", 32))), filled(0x55));

  // A yes or a no, the store's: one byte.
  EXPECT_EQ(store::message::encode_bool(true), Bytes{1});
  EXPECT_EQ(store::message::encode_bool(false), Bytes{0});
}

// Macha's decoders call finish(): nothing may trail a form on its one space.
TEST(HeadsWire, NoFormTakesATrailingByteAndAnIdIsOnlyAnId) {
  const auto hash = message::encode_hash(filled(0x55));
  const auto trailing = [](Bytes bytes) {
    bytes.push_back(0);
    return bytes;
  };
  EXPECT_THROW((void)message::decode_hash(trailing(hash)), DecodeError);
  EXPECT_THROW((void)message::decode_notice(trailing(message::encode_notice(1))), DecodeError);
  EXPECT_THROW((void)message::decode_identity(trailing(message::encode_identity({1, filled(1)}))), DecodeError);

  const auto space = filled(0x99);
  const auto decode = [](std::span<const uint8_t> bytes) { return message::decode_hash(bytes); };
  // Unaddressed, the bytes go straight through.
  const auto plain = message::addressed(hash, decode);
  EXPECT_EQ(plain.form, filled(0x55));
  EXPECT_FALSE(plain.space.has_value());
  // Addressed, exactly the id follows.
  const auto with = message::with_space(hash, space);
  EXPECT_EQ(with.size(), hash.size() + 32);
  const auto named = message::addressed(with, decode);
  EXPECT_EQ(named.form, filled(0x55));
  EXPECT_EQ(named.space, space);
  EXPECT_EQ(message::with_space(hash, std::nullopt), hash);
  // Anything else is refused, not guessed at.
  EXPECT_THROW((void)message::addressed(trailing(with), decode), DecodeError);
  EXPECT_THROW((void)message::addressed(Bytes(5, 0), decode), DecodeError);
  EXPECT_FALSE(message::space_of_empty({}).has_value());
  EXPECT_EQ(message::space_of_empty(message::with_space({}, space)), space);
  EXPECT_THROW((void)message::space_of_empty(Bytes(5, 0)), DecodeError);
}

// ---- What real Macha nodes wrote --------------------------------------------

constexpr Magic checkpoint_magic{'D', 'H', 'T', 'M', 'D', 'B', '0', '1'};
constexpr Magic journal_magic{'D', 'H', 'T', 'M', 'J', 'N', 'L', '1'};
constexpr Magic history_magic{'D', 'H', 'T', 'M', 'H', 'S', 'T', '1'};
constexpr Magic heads_magic{'D', 'H', 'T', 'M', 'A', 'C', 'C', '1'};
constexpr Magic author_magic{'D', 'H', 'T', 'M', 'A', 'U', 'T', '1'};
constexpr Magic set_aside_magic{'D', 'H', 'T', 'M', 'S', 'A', 'S', '1'};

std::shared_ptr<const domain::StorageKey> fixture_key() {
  dannan::test::TempDir dir;
  {
    std::ofstream key(dir.path() / "cluster.key", std::ios::binary);
    key << "macha-fixture-cluster-key\n";
  }
  return std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_v1(dannan::identity::load_cluster_key(dir.path() / "cluster.key")));
}

const std::filesystem::path checked_in = std::filesystem::path(DAGDA_SOURCE_DIR) / "dagda/test/data/replica-0.93.4";

std::filesystem::path macha_fixtures() {
  if (const char* given = std::getenv("DAGDA_MACHA_FIXTURES")) return given;
  return "../../macha/build/macha-fixtures";
}

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), {});
}

using SmallFiles = std::map<std::string, Bytes>;

SmallFiles read_checked_in(const std::string& name) {
  SmallFiles out;
  std::ifstream in(checked_in / (name + ".txt"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto gap = line.find(' ');
    out[line.substr(0, gap)] = bytes_of(line.substr(gap + 1));
  }
  return out;
}

// Each history frame's plaintext.
std::vector<Bytes> history_plaintexts(std::span<const uint8_t> history, const domain::StorageKey& key) {
  std::vector<Bytes> out;
  size_t at = 0;
  while (at < history.size()) {
    Reader length(history.subspan(at, 4));
    const auto size = length.u32();
    const auto envelope = parse_frame_envelope(history.subspan(at + 4, size));
    out.push_back(open_frame_envelope(key, history_magic, envelope));
    at += 4 + size;
  }
  return out;
}

// What the files of one fixture node's metadata carry on the wire when served: a set, a record and
// entries. Each re-encodes to the bytes a real Macha node wrote.
void expect_wire_forms_are_real(const std::filesystem::path& state_name, const Bytes& heads_file, const Bytes& checkpoint_file, const Bytes& history_file,
                                const domain::StorageKey& key) {
  const auto set_plain = open_file(key, heads_magic, heads_magic, heads_file);
  const auto set = decode_acceptance_set(set_plain);
  EXPECT_FALSE(set.empty()) << state_name;
  EXPECT_EQ(encode_acceptance_set(set), set_plain) << state_name;
  const auto served = message::addressed(set_plain, [](std::span<const uint8_t> bytes) { return decode_acceptance_set(bytes); });
  EXPECT_FALSE(served.space.has_value()) << state_name;
  for (const auto& certificate : set) {
    const auto alone = encode_acceptance(certificate);
    EXPECT_EQ(decode_acceptance(alone), certificate) << state_name;
    EXPECT_FALSE(message::addressed(alone, [](std::span<const uint8_t> bytes) { return decode_acceptance(bytes); }).space.has_value()) << state_name;
  }

  const auto record_plain = open_file(key, checkpoint_magic, checkpoint_magic, checkpoint_file);
  const auto record = decode_record(record_plain);
  EXPECT_EQ(encode_record(record), record_plain) << state_name;

  size_t frames = 0;
  for (const auto& plain : history_plaintexts(history_file, key)) {
    const auto entry = decode_history_entry(plain);
    EXPECT_EQ(encode_history_entry(entry), plain) << state_name;
    EXPECT_FALSE(message::addressed(plain, [](std::span<const uint8_t> bytes) { return decode_history_entry(bytes); }).space.has_value()) << state_name;
    ++frames;
  }
  EXPECT_GT(frames, 0u) << state_name;
}

TEST(HeadsWireCheckedIn, TheFormsAreWhatRealNodesWrote) {
  const auto key = fixture_key();
  for (const auto* state : {"state-scratch", "branch", "delete"}) {
    const auto files = read_checked_in(state);
    expect_wire_forms_are_real(state, files.at("heads.meta"), files.at("checkpoint.meta"), files.at("history.log"), *key);
  }
}

class HeadsWireFixture : public testing::Test {
 protected:
  void SetUp() override {
    if (!std::filesystem::exists(macha_fixtures())) GTEST_SKIP() << "Macha's fixtures are not here: " << macha_fixtures();
  }
};

// Every state, a two-head heads.meta among them: a survey's reply is exactly that file's plaintext.
TEST_F(HeadsWireFixture, TheFormsAreWhatRealNodesWroteInEveryState) {
  const auto key = fixture_key();
  for (const auto* state : {"state-scratch-0.93.4", "state-scratch-0.93.4-branch", "state-scratch-0.93.4-delete", "state-scratch-0.93.4-markers",
                            "state-scratch-0.93.4-truncated", "state-scratch-0.93.4-seed", "state-scratch-0.93.4-twoheads"}) {
    for (const auto* node : {"a", "b"}) {
      const auto directory = macha_fixtures() / state / node / "metadata";
      if (!std::filesystem::exists(directory / "heads.meta") || !std::filesystem::exists(directory / "checkpoint.meta") ||
          !std::filesystem::exists(directory / "history.log"))
        continue;
      expect_wire_forms_are_real(std::string(state) + "/" + node, read_file(directory / "heads.meta"), read_file(directory / "checkpoint.meta"),
                                 read_file(directory / "history.log"), *key);
    }
  }
  const auto two =
      decode_acceptance_set(open_file(*key, heads_magic, heads_magic, read_file(macha_fixtures() / "state-scratch-0.93.4-twoheads/a/metadata/heads.meta")));
  EXPECT_EQ(two.size(), 2u);
  EXPECT_EQ(two.front().generation, 4u);
}

// ---- Real history over Dannan -----------------------------------------------

// An owner's record held as bytes: the clock and the merge parents read from Macha's head section,
// nothing past it parsed. A delta is applied only where a whole frame of the same hash gives its
// result, as heads_replica_bytes_test.cpp does it (the plan's section 4.3).
struct OpaqueState {
  Bytes payload;
};

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
  Bytes genesis() const override { return Bytes{'D', 'H', 'T', 'M', 'G', 'E', 'N', '1'}; }
  State decode_full(std::span<const uint8_t> payload) const override {
    (void)opaque_fields(payload);
    return std::make_shared<const OpaqueState>(OpaqueState{Bytes(payload.begin(), payload.end())});
  }
  Bytes encode_full(const State& state) const override { return std::static_pointer_cast<const OpaqueState>(state)->payload; }
  std::optional<Bytes> encode_delta(const State&, const State&) const override { return std::nullopt; }
  void check_delta(std::span<const uint8_t>) const override {}
  State apply_delta(const State&, std::span<const uint8_t>, TreeNodeStore*) const override {
    throw DecodeError("only the owner's codec applies an owner's delta");
  }
  Bytes encode_after_delta(std::span<const uint8_t>, const State&) const override { throw DecodeError("only the owner's codec applies an owner's delta"); }
  HeadFields head_fields(const State& state) const override { return opaque_fields(std::static_pointer_cast<const OpaqueState>(state)->payload); }
  uint64_t resident_bytes(const State& state) const override { return std::static_pointer_cast<const OpaqueState>(state)->payload.size(); }
};

// Macha's magics are the parameters they are, here in a test.
ReplicaMagics owner_magics() { return {checkpoint_magic, journal_magic, history_magic, heads_magic, author_magic, set_aside_magic}; }

SpaceSpec opaque_spec() {
  SpaceSpec spec;
  spec.name = "test/opaque";
  spec.codec = std::make_shared<OpaqueCodec>();
  spec.magics = owner_magics();
  spec.seed_magic = Magic{'D', 'H', 'T', 'C', 'M', 'E', 'T', '1'};
  spec.unaddressed = true;
  return spec;
}

struct CapturedRequests {
  std::mutex mutex;
  std::map<dannan::net::MessageType, std::vector<Bytes>> asked;
  test::Interpose hook() {
    return [this](dannan::net::MessageType type, const dannan::membership::NodeRecord&,
                  const dannan::net::Message& request) -> std::optional<dannan::net::Handled> {
      std::lock_guard lock(mutex);
      asked[type].push_back(request.payload);
      return std::nullopt;
    };
  }
  std::vector<Bytes> of(dannan::net::MessageType type) {
    std::lock_guard lock(mutex);
    return asked[type];
  }
};

// The real history of each node of a state, whole frames carried between two Dagda nodes on an
// owner's numbers and in the one space that carries no id, captured at the receiver: what goes
// over Dannan is the frame's plaintext and a real certificate, byte for byte. `-truncated`'s two
// nodes hold differing histories; each frame the receiver asked for or was pushed is that frame.
TEST_F(HeadsWireFixture, RealHistoryCrossesTheWireAsItIs) {
  const auto key = fixture_key();
  for (const auto* state : {"state-scratch-0.93.4-truncated", "state-scratch-0.93.4"}) {
    for (const auto* node : {"a", "b"}) {
      const auto directory = macha_fixtures() / state / node / "metadata";
      if (!std::filesystem::exists(directory / "history.log")) continue;
      std::map<Hash256, Bytes> frames;
      std::map<Hash256, HistoryEntry> entries;
      // In the file's order, as a node's own history is.
      std::vector<Hash256> in_order;
      for (const auto& plain : history_plaintexts(read_file(directory / "history.log"), *key)) {
        auto entry = decode_history_entry(plain);
        frames[entry.hash] = plain;
        in_order.push_back(entry.hash);
        entries[entry.hash] = std::move(entry);
      }
      const auto certificates = decode_acceptance_set(open_file(*key, heads_magic, heads_magic, read_file(directory / "heads.meta")));
      ASSERT_FALSE(certificates.empty());
      // A head reached by whole frames alone, so an opaque owner can hold it.
      std::optional<Acceptance> usable;
      for (const auto& certificate : certificates) {
        const auto found = entries.find(certificate.hash);
        if (found != entries.end() && found->second.body == HistoryEntry::Body::full) usable = certificate;
      }
      if (!usable) continue;

      const auto numbers = test::owner_numbers();
      CapturedRequests captured;
      Cluster cluster;
      ClusterNodeOptions options;
      options.spaces = {opaque_spec()};
      options.exchange.numbers = numbers;
      options.exchange.write_copies = 1;
      auto& from = cluster.add("from", options);
      auto& to = cluster.add("to", options);
      cluster.start_all();
      ASSERT_TRUE(wait_until([&] { return from.context().nodes().active().size() >= 2 && to.context().nodes().active().size() >= 2; }));
      to.interpose(captured.hook());

      auto& source = *from.dagda_node().spaces()->exchange(from.api().heads().id_of("test/opaque"));
      auto& target = *to.dagda_node().spaces()->exchange(to.api().heads().id_of("test/opaque"));
      // The node's own history, whole frames only, under this node's own seal.
      std::set<Hash256> held_before;
      std::map<Hash256, bool> parent_was_held;
      for (const auto& hash : in_order) {
        const auto& entry = entries.at(hash);
        if (entry.body != HistoryEntry::Body::full) continue;
        parent_was_held[hash] = held_before.contains(entry.previous);
        ASSERT_TRUE(source.space().replica().import_history(entry)) << hex(hash.bytes);
        held_before.insert(hash);
      }
      Acceptance certificate = *usable;
      ASSERT_TRUE(source.space().accept(certificate));
      ASSERT_EQ(source.space().replica().committed().hash, certificate.hash);

      // A publish: a put of the frame, then the certificate.
      dannan::membership::NodeRecord peer;
      for (const auto& record : from.context().nodes().active())
        if (record.id == to.id()) peer = record;
      const auto record = source.space().replica().committed();
      const auto published = source.publish_commit({from.context().nodes().self(), peer}, record, {}, FrameType::control);
      EXPECT_FALSE(published.stored_on.empty());
      // write_copies 1: this node alone. A repair offers it to every node present.
      source.repair_once();

      const auto has = captured.of(numbers.has_history_entry);
      for (const auto& asked : has) EXPECT_EQ(asked.size(), 32u) << "a has carries a hash and nothing after it";
      const auto puts = captured.of(numbers.put_history_entry);
      ASSERT_FALSE(puts.empty()) << state << "/" << node;
      for (const auto& put : puts) {
        const auto entry = decode_history_entry(put);
        const auto real = frames.find(entry.hash);
        ASSERT_NE(real, frames.end());
        auto expected = real->second;
        // The one byte Macha's import changes on the way in (metadata.cpp's import_history): a
        // whole record does not claim a predecessor it does not hold, and this opaque owner
        // leaves deltas out.
        constexpr size_t previous_known_at = 1 + 8 + 32 + 32;
        if (!parent_was_held.at(entry.hash)) expected[previous_known_at] = 0;
        EXPECT_EQ(put, expected) << "a put is the frame's plaintext";
      }
      const auto accepts = captured.of(numbers.accept_commit);
      ASSERT_FALSE(accepts.empty());
      EXPECT_EQ(decode_acceptance(accepts.back()).hash, certificate.hash);
      for (const auto& accept : accepts) EXPECT_EQ(encode_acceptance(decode_acceptance(accept)), accept);

      EXPECT_EQ(target.space().replica().committed().hash, certificate.hash);
      EXPECT_EQ(target.space().replica().committed().payload, record.payload);
    }
  }
}

// The survey, on the wire, for a two-head state: the reply is the file's plaintext, and each head is
// asked for by hash. The heads stay two, as this space's owner gave no merge.
TEST_F(HeadsWireFixture, ASurveyOfTwoRealHeadsImportsBoth) {
  const auto key = fixture_key();
  const auto directory = macha_fixtures() / "state-scratch-0.93.4-twoheads/a/metadata";
  if (!std::filesystem::exists(directory / "heads.meta")) GTEST_SKIP() << "no -twoheads fixture";
  const auto set_plain = open_file(*key, heads_magic, heads_magic, read_file(directory / "heads.meta"));
  const auto certificates = decode_acceptance_set(set_plain);
  ASSERT_EQ(certificates.size(), 2u);
  std::map<Hash256, HistoryEntry> entries;
  for (const auto& plain : history_plaintexts(read_file(directory / "history.log"), *key)) {
    auto entry = decode_history_entry(plain);
    entries[entry.hash] = std::move(entry);
  }
  for (const auto& certificate : certificates) {
    ASSERT_TRUE(entries.contains(certificate.hash));
    ASSERT_EQ(entries.at(certificate.hash).body, HistoryEntry::Body::full) << "the fixture's README says a has 5 full frames";
  }

  const auto numbers = test::owner_numbers();
  CapturedRequests at_from;
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces = {opaque_spec()};
  options.exchange.numbers = numbers;
  options.exchange.write_copies = 1;
  auto& from = cluster.add("from", options);
  auto& to = cluster.add("to", options);
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return from.context().nodes().active().size() >= 2 && to.context().nodes().active().size() >= 2; }));

  auto& source = *from.dagda_node().spaces()->exchange(from.api().heads().id_of("test/opaque"));
  auto& target = *to.dagda_node().spaces()->exchange(to.api().heads().id_of("test/opaque"));
  for (const auto& [hash, entry] : entries) {
    if (entry.body == HistoryEntry::Body::full) {
      ASSERT_TRUE(source.space().replica().import_history(entry)) << hex(hash.bytes);
    }
  }
  for (const auto& certificate : certificates) ASSERT_TRUE(source.space().accept(certificate));
  ASSERT_EQ(source.space().replica().accepted_heads().size(), 2u);
  from.interpose(at_from.hook());

  // A read on the other node: the survey and the import, then a read that falls back to
  // the own head with two standing, as Macha's does while its merge cannot be made.
  try {
    (void)target.read_base();
    ADD_FAILURE() << "two heads stand and no merge was given";
  } catch (const HeadsNotReady& error) {
    EXPECT_NE(std::string(error.what()).find("gave no merge"), std::string::npos) << error.what();
  }
  EXPECT_EQ(target.space().replica().accepted_heads().size(), 2u);
  const auto asked = at_from.of(numbers.get_heads);
  ASSERT_FALSE(asked.empty());
  EXPECT_TRUE(asked.front().empty()) << "a survey carries nothing";
  std::set<Hash256> fetched;
  for (const auto& request : at_from.of(numbers.get_history_entry)) {
    EXPECT_EQ(request.size(), 32u);
    fetched.insert(message::decode_hash(request));
  }
  for (const auto& certificate : certificates) EXPECT_TRUE(fetched.contains(certificate.hash));
}

// An owner's merge whose answer is what a real Macha node's merge came to: the reconciler over it
// has only the two heads and those bytes, and what it mints is measured against what Macha minted.
class ReplayedMerge final : public heads::Merge {
 public:
  ReplayedMerge(Bytes payload, Bytes delta) : _payload(std::move(payload)), _delta(std::move(delta)) {}
  Result merge(const std::optional<State>&, const heads::MergeHeads&) const override {
    Result out;
    out.state = OpaqueCodec().decode_full(_payload);
    out.delta = _delta;
    return out;
  }

 private:
  const Bytes _payload;
  const Bytes _delta;
};

// The fold against Macha's own bytes. -branch: a fork merged by a delta. -markers: a standing conflict
// and a merge stored whole. Given the two heads and the merge's payload, the reconciler mints Macha's
// commit: its generation, its previous as the lower hash, so its hash, and one merge parent, the
// higher. The merge itself is the owner's; nothing here parses it past the fields it must name.
TEST_F(HeadsWireFixture, TheFoldMintsMachasOwnMergeFromItsTwoHeads) {
  const auto key = fixture_key();
  size_t proved = 0;
  for (const auto* state : {"state-scratch-0.93.4-branch", "state-scratch-0.93.4-markers"}) {
    for (const auto* node : {"a", "b"}) {
      const auto directory = macha_fixtures() / state / node / "metadata";
      if (!std::filesystem::exists(directory / "history.log")) continue;
      std::map<Hash256, HistoryEntry> entries;
      const HistoryEntry* found = nullptr;
      for (const auto& plain : history_plaintexts(read_file(directory / "history.log"), *key)) {
        auto entry = decode_history_entry(plain);
        entries[entry.hash] = std::move(entry);
      }
      for (const auto& [hash, entry] : entries)
        if (!entry.merge_parents.empty() && (!found || entry.generation > found->generation)) found = &entry;
      if (!found) continue;
      ASSERT_EQ(found->merge_parents.size(), 1u) << state << " " << node << ": Macha's merge names one merge parent";
      EXPECT_LT(found->previous, found->merge_parents.front()) << state << " " << node << ": its previous is the lower hash";
      if (!entries.contains(found->previous) || !entries.contains(found->merge_parents.front())) continue;
      const auto& primary = entries.at(found->previous);
      const auto& secondary = entries.at(found->merge_parents.front());
      EXPECT_EQ(found->generation, std::max(primary.generation, secondary.generation) + 1) << state << " " << node;
      // The merge's record whole somewhere: its frame, or the checkpoint when it is the head.
      Bytes payload;
      Bytes delta;
      if (found->body == HistoryEntry::Body::full) {
        payload.assign(found->payload.begin(), found->payload.end());
      } else {
        delta.assign(found->payload.begin(), found->payload.end());
        const auto committed = decode_record(open_file(*key, checkpoint_magic, checkpoint_magic, read_file(directory / "checkpoint.meta")));
        if (committed.hash != found->hash) continue;
        EXPECT_LT(delta.size(), committed.payload.size()) << state << " " << node;
        payload.assign(committed.payload.begin(), committed.payload.end());
      }
      // A delta only applies under the owner's codec, so minting it here takes two whole parents.
      if (primary.body != HistoryEntry::Body::full || secondary.body != HistoryEntry::Body::full) continue;

      auto spec = opaque_spec();
      spec.merge = std::make_shared<ReplayedMerge>(payload, delta);
      ClusterNodeOptions options;
      options.spaces = {spec};
      options.exchange.numbers = test::owner_numbers();
      options.exchange.write_copies = 1;
      options.exchange.view_ttl = std::chrono::milliseconds(0);
      Cluster cluster;
      auto& from = cluster.add("from", options);
      from.start();
      auto& exchange = *from.dagda_node().spaces()->exchange(from.api().heads().id_of("test/opaque"));
      auto& replica = exchange.space().replica();
      for (const auto* parent : {&primary, &secondary}) {
        ASSERT_TRUE(replica.import_history(*parent)) << state << " " << node;
        ASSERT_TRUE(exchange.space().accept(Acceptance{parent->generation, parent->hash, 1, {from.id()}})) << state << " " << node;
      }
      ASSERT_EQ(replica.accepted_heads().size(), 2u) << state << " " << node;
      const auto minted = exchange.read_base();
      EXPECT_EQ(minted.generation, found->generation) << state << " " << node;
      EXPECT_EQ(minted.previous, found->previous) << state << " " << node;
      EXPECT_EQ(minted.hash, found->hash) << state << " " << node;
      EXPECT_EQ(minted.payload, payload) << state << " " << node;
      ASSERT_EQ(replica.accepted_heads().size(), 1u) << state << " " << node;
      EXPECT_EQ(replica.history_links(minted.hash)->merge_parents, found->merge_parents) << state << " " << node;
      std::cout << "BENCH name=heads.fixture.merge_minted state=" << state << " node=" << node << " generation=" << minted.generation
                << " delta=" << !delta.empty() << '\n';
      ++proved;
    }
  }
  std::cout << "BENCH name=heads.fixture.merges_minted ops=" << proved << '\n';
  EXPECT_GT(proved, 0u) << "no fixture held a merge over two whole frames";
}

// A space not the unaddressed one adds exactly its id, on every request, and reaches that space.
TEST(HeadsWire, AnotherSpacesMessagesCarryItsIdAndReachIt) {
  CapturedRequests captured;
  Cluster cluster;
  ClusterNodeOptions options;
  auto first = test::founded_spec("test/first");
  first.unaddressed = true;
  auto second = test::founded_spec("test/second");
  options.spaces = {first, second};
  options.exchange.write_copies = 2;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  b.interpose(captured.hook());
  const auto numbers = a.options().exchange.numbers;
  const auto first_id = a.api().heads().id_of("test/first");
  const auto second_id = a.api().heads().id_of("test/second");

  (void)a.dagda_node().spaces()->find(first_id)->converged();
  const auto first_gets = captured.of(numbers.get_heads).size();
  const auto first_puts = captured.of(numbers.put_commit);
  ASSERT_FALSE(first_puts.empty());
  for (const auto& put : first_puts) EXPECT_EQ(encode_history_entry(decode_history_entry(put)), put) << "no id on the unaddressed space";
  EXPECT_EQ(b.dagda_node().spaces()->find(second_id)->replica().committed_generation(), 1u) << "the first's commit reached only the first";
  EXPECT_EQ(b.dagda_node().spaces()->find(first_id)->replica().committed_generation(), 2u);

  (void)a.dagda_node().spaces()->find(second_id)->converged();
  const auto gets = captured.of(numbers.get_heads);
  ASSERT_GT(gets.size(), first_gets);
  EXPECT_EQ(gets.back(), Bytes(second_id.bytes.begin(), second_id.bytes.end()));
  const auto puts = captured.of(numbers.put_commit);
  ASSERT_GT(puts.size(), first_puts.size());
  const auto last = message::addressed(puts.back(), [](std::span<const uint8_t> bytes) { return decode_history_entry(bytes); });
  EXPECT_EQ(last.space, second_id);
  EXPECT_EQ(b.dagda_node().spaces()->find(second_id)->replica().committed_generation(), 2u);
}

// ---- Ordered work, on real nodes --------------------------------------------

Bytes a_full_entry(uint64_t generation) {
  HistoryEntry entry;
  entry.generation = generation;
  entry.hash = filled(static_cast<uint8_t>(generation));
  entry.body = HistoryEntry::Body::full;
  entry.payload = Bytes{1};
  return encode_history_entry(entry);
}

// C14. H26 on two real nodes: one peer's imports run in arrival order, one at a time, and a control
// commit call overtakes the queue. The scheduler's own proofs are Dannan's; this is that the types
// reach them. Answered in front of the handler, so the bytes need mean nothing.
TEST(HeadsExchange, ACommitOvertakesImportsAndOnePeersOrderHolds) {
  dannan::test::Gate first_running;
  std::mutex mutex;
  std::vector<std::string> ran;
  std::atomic_int active{0};
  std::atomic_int peak{0};
  Cluster cluster;
  dannan::test::GateOpener open_on_exit(first_running);
  ClusterNodeOptions options;
  options.exchange.write_copies = 1;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  const auto numbers = a.options().exchange.numbers;
  b.interpose(
      [&](dannan::net::MessageType type, const dannan::membership::NodeRecord&, const dannan::net::Message& request) -> std::optional<dannan::net::Handled> {
        if (type != numbers.put_history_entry && type != numbers.accept_commit) return std::nullopt;
        const auto now = active.fetch_add(1) + 1;
        auto seen = peak.load();
        while (now > seen && !peak.compare_exchange_weak(seen, now)) {
        }
        std::string name;
        if (type == numbers.accept_commit) {
          name = "accept";
        } else {
          name = std::to_string(decode_history_entry(request.payload).generation);
        }
        {
          std::lock_guard lock(mutex);
          ran.push_back(name);
        }
        if (name == "1") first_running.enter_and_wait();
        active.fetch_sub(1);
        return dannan::net::Handled::processed(dannan::net::Message{store::message::bool_reply, store::message::encode_bool(true), {}});
      });

  dannan::membership::NodeRecord peer;
  for (const auto& record : a.context().nodes().active())
    if (record.id == b.id()) peer = record;
  auto& routes = a.context().routes();
  const auto executor = a.context().time().executor();
  std::vector<std::shared_ptr<std::promise<dannan::net::CallOutcome>>> pending;
  const auto send = [&](dannan::net::MessageType type, Bytes payload, FrameType frame) {
    auto done = std::make_shared<std::promise<dannan::net::CallOutcome>>();
    pending.push_back(done);
    (void)routes.call(peer, type, std::move(payload), frame, 10s, executor, [done](dannan::net::CallOutcome outcome) { done->set_value(std::move(outcome)); });
  };
  send(numbers.put_history_entry, a_full_entry(1), FrameType::speculative);
  ASSERT_TRUE(first_running.wait_for_entries(1));
  for (uint64_t generation = 2; generation <= 5; ++generation) send(numbers.put_history_entry, a_full_entry(generation), FrameType::speculative);
  // Give the queue its four: they wait on the first, and the accept is sent after them all.
  ASSERT_TRUE(wait_until([&] { return b.context().diagnostics().work.ordered_pending_jobs == 4; }, 5s));
  send(numbers.accept_commit, encode_acceptance({9, filled(9), 1, {a.id()}}), FrameType::control);
  ASSERT_TRUE(wait_until([&] { return b.context().diagnostics().work.ordered_pending_jobs == 5; }, 5s));
  first_running.open();
  for (auto& done : pending) EXPECT_TRUE(done->get_future().get().reply.has_value());

  std::lock_guard lock(mutex);
  EXPECT_EQ(ran, (std::vector<std::string>{"1", "accept", "2", "3", "4", "5"}));
  EXPECT_EQ(peak.load(), 1);
}

// C27, H26. An ordered handler must not wait on an ordered call: a commit's replay on b fetches a's
// tree node with get_control_object, unordered, and it completes with a's ordered worker held.
TEST(HeadsExchange, ACommitWhoseReplayNeedsANodeFromTheSamePeerCompletes) {
  dannan::test::Gate a_held;
  Cluster cluster;
  dannan::test::GateOpener open_on_exit(a_held);
  ClusterNodeOptions options;
  // Past one leaf, so a commit's path can run through a node the peer has not read.
  test::NeutralEntries entries;
  for (int i = 0; i < 2000; ++i) entries["k" + std::to_string(1000 + i)] = test::make_group(1);
  auto spec = test::founded_spec("test/neutral");
  spec.founding = test::neutral_founding(std::move(entries));
  options.spaces = {spec};
  options.exchange.joiner = true;
  options.exchange.write_copies = 1;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(wait_until([&] { return a.context().nodes().active().size() >= 2 && b.context().nodes().active().size() >= 2; }));
  const auto id = a.api().heads().id_of("test/neutral");
  auto& space_a = *a.dagda_node().spaces()->find(id);
  auto& space_b = *b.dagda_node().spaces()->find(id);
  auto& exchange_a = *a.dagda_node().spaces()->exchange(id);
  const auto write = [&](const std::string& key) {
    (void)space_a.mutate_delta(test::neutral_mutation([&](test::NeutralEditing& editing) { editing.delta.upsert_keys[key] = test::make_group(1); }));
  };
  ASSERT_TRUE(wait_until(
      [&] {
        try {
          write("k1000x");
          return true;
        } catch (const HeadsNotReady&) {
          return false;
        }
      },
      10s));
  ASSERT_TRUE(exchange_a.wait_replicated(10s));
  ASSERT_EQ(space_b.replica().committed().hash, space_a.replica().committed().hash);

  // a's ordered worker is held by a request from b.
  const auto numbers = a.options().exchange.numbers;
  a.interpose([&](dannan::net::MessageType type, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<dannan::net::Handled> {
    if (type == numbers.put_history_entry) a_held.enter_and_wait();
    return std::nullopt;
  });
  dannan::membership::NodeRecord peer_a;
  for (const auto& record : b.context().nodes().active())
    if (record.id == a.id()) peer_a = record;
  auto held = std::make_shared<std::promise<dannan::net::CallOutcome>>();
  (void)b.context().routes().call(peer_a, numbers.put_history_entry, a_full_entry(1), FrameType::control, 10s, b.context().time().executor(),
                                  [held](dannan::net::CallOutcome outcome) { held->set_value(std::move(outcome)); });
  ASSERT_TRUE(a_held.wait_for_entries(1));

  const auto fetches_before = a.calls_of(store::message::get_control_object);
  write("k2999x");
  const auto head = space_a.replica().committed();
  // A delta, with its tree's new nodes on a alone until b's replay asks for them.
  const auto links = space_a.replica().history_links(head.hash);
  ASSERT_TRUE(links.has_value());
  EXPECT_EQ(links->body, HistoryEntry::Body::delta);
  ASSERT_TRUE(wait_until([&] { return space_b.replica().committed().hash == head.hash; }, 15s));
  EXPECT_GT(a.calls_of(store::message::get_control_object), fetches_before);
  EXPECT_EQ(a_held.entered(), 1u);
}

}  // namespace
}  // namespace dagda::heads
