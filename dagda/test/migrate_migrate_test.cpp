//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The migration of Macha's layout v18 disks to the one layout, in place
// (the maintainer, 2026-11-04 night; migrate/migrate.h). First the mechanism: under
// the same key and nonce, v18's associated data (the id) and v19's (the
// label, the domain, the id) give the same ciphertext and another tag, so
// only the tag moves. Then the files: a loose object's 16 bytes, a pack
// record's tag and checksum, the ledgers re-sealed under the node-local key,
// the markers last. Then a whole v18 state built from the known answers,
// migrated, opened by the v19 store and read back; the second run finding
// nothing to do; a wrong key refused before any write; a crash between
// records left for a second run to finish; and the tool itself.
//
// The known answers are step 1a's v18 bytes and step 6b's v19 ones
// (build/kat/step6b/kat.py: GCM by hand over `openssl enc` blocks, checked
// against the GCM specification's test case 16 and the pinned v18 tag). The
// cases that opened Macha's files through the store
// (store_local_format_test's OpensALooseObjectMachaWrote and
// OpensAPackMachaWrote) are here, every assertion kept, after a migration.

#include "dagda/migrate/migrate.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/ledger/held_ledger.h"
#include "dagda/ledger/reference_counts.h"
#include "dagda/ledger/retention.h"
#include "dagda/migrate/legacy_v18.h"
#include "dagda/store/control_store.h"
#include "dagda/store/local_format.h"
#include "dagda/store/local_store.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/store/sealing.h"
#include "dagda/store/state_layout.h"
#include "dagda/store/storage_pool.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/legacy_v18.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/identity/argon2.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/key_custody.h"
#include "dannan/test/fixtures/child_process.h"

namespace dagda::migrate {
namespace {

using namespace std::chrono_literals;
using dannan::identity::hex;
using dannan::identity::NodeId;
using store::object_id;
using store::SealingScheme;
using test::pattern;
using test::StoreBed;
using namespace store::local_format;
namespace legacy = test::legacy;

constexpr uint64_t mib = 1024 * 1024;
constexpr std::string_view plaintext = "An object Macha sealed under its own id.";
constexpr char object_hex[] = "3e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8";
constexpr char nonce_hex[] = "000102030405060708090a0b";
constexpr char ciphertext_hex[] = "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
constexpr char v18_tag_hex[] = "6f6aa83135d5e163f1b04d0c9ec0758c";
constexpr char v1_tag_hex[] = "b05d61c20d49091b6d3ee9e145ef880c";
constexpr uint64_t put_ms = 1793700000000ull;
// The loose file as Macha wrote it, and as the migration leaves it.
constexpr char loose_v18_hex[] =
    "4448544f424a30310000000000000028000102030405060708090a0b6f6aa83135d5e163f1b04d0c9ec0758c"
    "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
constexpr char loose_v19_hex[] =
    "4448544f424a30310000000000000028000102030405060708090a0bb05d61c20d49091b6d3ee9e145ef880c"
    "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
// A pack's put record header at put_ms, before and after; a touch at
// put_ms + 500 and a tombstone at put_ms + 1000, which carry nothing sealed.
constexpr char put_header_v18_hex[] =
    "4d414348504b303101000001a1a0d9f1003e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "00000000000000280000000000000028000102030405060708090a0b6f6aa83135d5e163f1b04d0c9ec0758c"
    "adcbbd1ad98f932c95b0af893dce6de8efa1d701f299bb7a0e25358c929fc5a2";
constexpr char put_header_v19_hex[] =
    "4d414348504b303101000001a1a0d9f1003e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "00000000000000280000000000000028000102030405060708090a0bb05d61c20d49091b6d3ee9e145ef880c"
    "fcb468f0b824611dae607ea77ae1708bbcc74730bcb8dfa08af2731aef36ee76";
constexpr char touch_hex[] =
    "4d414348504b303103000001a1a0d9f2f43e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "de2a9d5429d594851c0975a73134b1cb11c2f5a0d13ed3015333df6bb545b580";
constexpr char tombstone_hex[] =
    "4d414348504b303102000001a1a0d9f4e83e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "dde293c4c0f89a1ec8e5b3ec8656f0cbf163c2ebbc563e5e0c159e9eea53718a";
// Domain v1's marker for node ab.. and token cd.. after the migration.
constexpr char migrated_marker[] =
    "dagda-backend-v19 abababababababababababababababab cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd 827b07101d79d9d84a433d8e9cb262ca 96fe387af04a9cdb77b3cd0086f4b356\n";
constexpr char cluster_key_text[] = "dagda known-answer cluster key\r\n";
const SealingScheme v1 = SealingScheme::v19(domain::domain_v1);

Bytes unhex(std::string_view text) { return dannan::identity::unhex(std::string(text)).value(); }

Bytes bytes_of(std::string_view text) { return Bytes(text.begin(), text.end()); }

NodeId filled(uint8_t byte) {
  NodeId id;
  id.bytes.fill(byte);
  return id;
}

std::array<uint8_t, 12> the_nonce() {
  std::array<uint8_t, 12> out{};
  const auto bytes = unhex(nonce_hex);
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return out;
}

ObjectId the_id() { return object_id(bytes_of(plaintext)); }

std::string read_text(const std::filesystem::path& path) {
  const auto bytes = legacy::read_file(path);
  return std::string(bytes.begin(), bytes.end());
}

// Every node's custody source: a passphrase, light costs.
dannan::identity::KeySource custody_source() {
  return dannan::identity::KeySource::typed_passphrase(
      [] {
        const std::string phrase = "dagda";
        return dannan::identity::Secret(std::span(reinterpret_cast<const uint8_t*>(phrase.data()), phrase.size()));
      },
      dannan::identity::Argon2Costs{1024, 1, 1});
}

dannan::identity::ClusterKey cluster_key_from(const std::filesystem::path& file, std::string_view contents = cluster_key_text) {
  std::ofstream(file, std::ios::binary) << contents;
  return dannan::identity::load_cluster_key(file);
}

// A v18 state built from the known answers and more: a backend with loose
// objects and packs (the known-answer object loose and packed, a touched
// one, a tombstoned one), its seeded held ledger, the control store and its
// ledger, a cache, claims in both classes (checkpointed and journaled) and
// reference counts, all as Macha left them, under domain v1's key; and the
// node's custody beside it.
struct V18State {
  explicit V18State(StoreBed& bed, std::string name = "node")
      : root(bed.dir.path() / name),
        state(root / "state"),
        node_state(root / "node"),
        backend(root / "data"),
        control(state / "metadata-objects"),
        cache(root / "cache"),
        cluster_key_file(root / "cluster.key") {
    std::filesystem::create_directories(root);
    (void)cluster_key_from(cluster_key_file);
    (void)dannan::identity::KeyCustody::create(node_state, custody_source());
    const auto& key = *bed.key;
    legacy::write_state_layout_v18(state);
    legacy::write_backend_marker_v18(state, backend, node, token);

    // The known-answer object, loose and packed (pack 1 with its touch).
    known = the_id();
    legacy::write_file(loose_path(backend / "objects", known), unhex(loose_v18_hex));
    loose.emplace_back(known, bytes_of(plaintext));
    Bytes pack = unhex(put_header_v18_hex);
    const auto ciphertext = unhex(ciphertext_hex);
    pack.insert(pack.end(), ciphertext.begin(), ciphertext.end());
    const auto touch = unhex(touch_hex);
    pack.insert(pack.end(), touch.begin(), touch.end());
    legacy::write_file(legacy::pack_path(backend, 1), pack);
    packed.emplace_back(known, bytes_of(plaintext));
    for (uint8_t i = 1; i <= 4; ++i) {
      const auto data = pattern(4096 + i, i);
      legacy::write_loose_v18(backend, key, object_id(data), data);
      loose.emplace_back(object_id(data), data);
    }
    // Pack 2: three puts, then one put and its tombstone.
    for (uint8_t i = 10; i < 13; ++i) {
      const auto data = pattern(900 + i, i);
      legacy::append_pack_put_v18(backend, 2, key, object_id(data), data, put_ms + i);
      packed.emplace_back(object_id(data), data);
    }
    {
      const auto data = pattern(700, 13);
      tombstoned = object_id(data);
      legacy::append_pack_put_v18(backend, 2, key, tombstoned, data, put_ms + 13);
      legacy::append_pack_record_v18(backend, 2, pack_remove, tombstoned, put_ms + 14);
    }
    std::vector<ObjectId> held;
    for (const auto& [id, data] : loose) held.push_back(id);
    for (const auto& [id, data] : packed) held.push_back(id);
    seed_ledger(state / "ledger" / ("data-" + dannan::identity::to_string(token)), bed.key, held);

    // The control store, two loose objects, its ledger seeded.
    std::vector<ObjectId> control_held;
    for (uint8_t i = 20; i < 22; ++i) {
      const auto data = pattern(3000 + i, i);
      legacy::write_loose_v18(control, key, object_id(data), data);
      control_objects.emplace_back(object_id(data), data);
      control_held.push_back(object_id(data));
    }
    seed_ledger(state / "ledger" / "control", bed.key, control_held);
    {
      const auto data = pattern(2000, 30);
      legacy::write_loose_v18(cache, key, object_id(data), data);
      cache_objects.emplace_back(object_id(data), data);
    }

    // Claims: two checkpointed into the tries, one left in the journal.
    {
      ledger::RetentionStore claims(state, bed.key, test::quiet_logger());
      claims.retain(contract::RetentionClass::data, loose[1].first, {origin, 1});
      claims.retain(contract::RetentionClass::control, control_objects[0].first, {origin, 2});
      EXPECT_TRUE(claims.compact_if_needed(0));
      claims.retain(contract::RetentionClass::data, packed[1].first, {origin, 3});
    }
    // Reference counts at a root.
    {
      ledger::ReferenceCounts counts(state / "references", bed.key, 1 << 20, test::quiet_logger());
      references_root = control_objects[0].first;
      counts.reset(references_root, {loose[1].first, loose[1].first, packed[1].first}, {control_objects[1].first}, {7, 3});
    }
  }

  static void seed_ledger(const std::filesystem::path& dir, std::shared_ptr<const domain::StorageKey> key, std::vector<ObjectId> ids) {
    ledger::ObjectTrie::Options options;
    options.cache_bytes = 1 << 20;
    ledger::HeldLedger ledger(dir, std::move(key), options, test::quiet_logger());
    // The known-answer object is both loose and packed: held once.
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    ledger.seed([ids] { return ids; });
  }

  dannan::identity::ClusterKey cluster() const { return dannan::identity::load_cluster_key(cluster_key_file); }
  dannan::identity::KeyCustody custody() const { return dannan::identity::KeyCustody::open(node_state, custody_source()); }
  std::shared_ptr<const domain::StorageKey> node_local() const {
    return std::make_shared<const domain::StorageKey>(domain::StorageKey::derive_node_local(custody()));
  }
  MigrationReport migrate(MigrationOptions options = {}) const {
    options.cache = cache;
    return migrate_state(state, {backend}, cluster(), custody(), options);
  }
  // Every file under the node, digested.
  std::map<std::filesystem::path, std::string> digest() const { return legacy::tree_digest(root); }

  const std::filesystem::path root;
  const std::filesystem::path state;
  const std::filesystem::path node_state;
  const std::filesystem::path backend;
  const std::filesystem::path control;
  const std::filesystem::path cache;
  const std::filesystem::path cluster_key_file;
  const NodeId node = filled(0xab);
  const NodeId token = filled(0xcd);
  const NodeId origin = filled(0x0a);
  ObjectId known;
  ObjectId tombstoned;
  ObjectId references_root;
  std::vector<std::pair<ObjectId, Bytes>> loose;
  std::vector<std::pair<ObjectId, Bytes>> packed;
  std::vector<std::pair<ObjectId, Bytes>> control_objects;
  std::vector<std::pair<ObjectId, Bytes>> cache_objects;
};

// What a migrated state must read back as, through the v19 store: every
// object by its bytes, every claim, the counts, the ledgers under the
// node-local key. Nothing walks: the held ledgers are seeded.
void expect_v19_opens(StoreBed& bed, const V18State& v18) {
  const auto node_local = v18.node_local();
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "dagda-state-layout-v19 827b07101d79d9d84a433d8e9cb262ca\n");
  EXPECT_EQ(read_text(v18.backend / ".macha.backend"), migrated_marker);
  EXPECT_EQ(read_text(store::backend_identity_path(v18.state, v18.backend)), migrated_marker);
  EXPECT_EQ(hex(legacy::read_file(loose_path(v18.backend / "objects", v18.known))), loose_v19_hex);
  store::open_state_layout(v18.state, v1);
  {
    store::StoragePoolOptions options;
    options.backends = {{v18.backend, 64 * mib, 0}};
    options.durability_batch_window = 0ms;
    options.ledger_key = node_local;
    store::StoragePool pool(v18.state, v18.node, NodeId::random(), options, bed.key, bed.pool_services());
    ASSERT_EQ(pool.online_backends(), 1u);
    EXPECT_TRUE(pool.held_view().identity.complete);
    for (const auto& [id, data] : v18.loose) {
      EXPECT_EQ(pool.get(id), std::optional<Bytes>(data)) << store::to_string(id);
      EXPECT_TRUE(pool.held_view().held(id));
    }
    for (const auto& [id, data] : v18.packed) {
      EXPECT_EQ(pool.get(id), std::optional<Bytes>(data)) << store::to_string(id);
      EXPECT_TRUE(pool.held_view().held(id));
    }
    EXPECT_FALSE(pool.has(v18.tombstoned));
    EXPECT_TRUE(pool.valid(v18.known));
  }
  {
    auto control = store::open_control_store(v18.state, {}, bed.key, node_local, bed.services());
    EXPECT_TRUE(control->held_view().identity.complete);
    for (const auto& [id, data] : v18.control_objects) {
      EXPECT_EQ(control->get(id), std::optional<Bytes>(data)) << store::to_string(id);
      EXPECT_TRUE(control->held_view().held(id));
    }
  }
  {
    store::CacheOptions options;
    options.path = v18.cache;
    options.max_blocks = 8;
    store::PersistentBlockCache cache(options, bed.key, bed.services());
    ASSERT_TRUE(cache.enabled());
    for (const auto& [id, data] : v18.cache_objects) EXPECT_EQ(cache.get(id), std::optional<Bytes>(data));
  }
  {
    ledger::RetentionStore claims(v18.state, node_local, test::quiet_logger());
    EXPECT_EQ(claims.claims(contract::RetentionClass::data, v18.loose[1].first).adds, (std::map<NodeId, uint64_t>{{v18.origin, 1}}));
    EXPECT_EQ(claims.claims(contract::RetentionClass::control, v18.control_objects[0].first).adds, (std::map<NodeId, uint64_t>{{v18.origin, 2}}));
    EXPECT_EQ(claims.claims(contract::RetentionClass::data, v18.packed[1].first).adds, (std::map<NodeId, uint64_t>{{v18.origin, 3}}));
    EXPECT_EQ(claims.claim_objects(contract::RetentionClass::data), 2u);
    EXPECT_EQ(claims.claim_objects(contract::RetentionClass::control), 1u);
  }
  {
    ledger::ReferenceCounts counts(v18.state / "references", node_local, 1 << 20, test::quiet_logger());
    EXPECT_EQ(counts.root(), std::optional<ObjectId>(v18.references_root));
    EXPECT_EQ(counts.totals().entries, 7u);
    EXPECT_EQ(counts.totals().extent_count, 3u);
    EXPECT_EQ(hex(counts.views().extents.get(v18.loose[1].first).value()), "0000000000000002");
    EXPECT_EQ(hex(counts.views().nodes.get(v18.control_objects[1].first).value()), "0000000000000001");
  }
}

// ---- The mechanism ----------------------------------------------------------

// Under the same key and nonce, the id alone and the label, domain and id
// as associated data give the same ciphertext (the keystream does not see
// the associated data) and different tags (GHASH does). The migrated tag of
// Macha's record is step 6b's known answer; the layout a record carries is
// told by which associated data authenticates it.
TEST(MigrateSealing, TheSameKeyAndNonceGiveTheSameCiphertextAndAnotherTag) {
  const auto key = test::known_answer_key();
  const auto id = the_id();
  ASSERT_EQ(store::to_string(id), object_hex);
  const auto nonce = the_nonce();
  const auto plain = bytes_of(plaintext);
  const auto v18 = dannan::identity::aes_gcm_seal(key->bytes(), nonce, plain, associated_data_v18(id));
  const auto v19 = dannan::identity::aes_gcm_seal(key->bytes(), nonce, plain, v1.associated_data(id));
  EXPECT_EQ(hex(v18.ciphertext), ciphertext_hex);
  EXPECT_EQ(hex(v19.ciphertext), ciphertext_hex);
  EXPECT_EQ(hex(v18.tag), v18_tag_hex);
  EXPECT_EQ(hex(v19.tag), v1_tag_hex);
  EXPECT_NE(v18.tag, v19.tag);
  // The rewrite: opened under v18, sealed again under v19 with the same
  // nonce, the ciphertext checked identical, only the tag returned.
  EXPECT_EQ(hex(migrated_tag(*key, v1, id, nonce, v18.tag, v18.ciphertext, plain.size())), v1_tag_hex);
  EXPECT_EQ(record_layout(*key, v1, id, nonce, v18.tag, v18.ciphertext, plain.size()), RecordLayout::v18);
  EXPECT_EQ(record_layout(*key, v1, id, nonce, v19.tag, v19.ciphertext, plain.size()), RecordLayout::v19);
  auto flipped = v18.tag;
  flipped[0] ^= 1;
  EXPECT_EQ(record_layout(*key, v1, id, nonce, flipped, v18.ciphertext, plain.size()), RecordLayout::neither);
  EXPECT_EQ(record_layout(*test::key_of(7), v1, id, nonce, v18.tag, v18.ciphertext, plain.size()), RecordLayout::neither);
  EXPECT_THROW((void)migrated_tag(*key, v1, id, nonce, v19.tag, v19.ciphertext, plain.size()), std::runtime_error);
  // The migrated record opens as v19 and not as v18 any more.
  EXPECT_EQ(store::open_object(*key, v1, id, nonce, v19.tag, v18.ciphertext, plain.size()), plain);
  EXPECT_THROW((void)open_object_v18(*key, id, nonce, v19.tag, v18.ciphertext, plain.size()), std::runtime_error);
}

// A loose file: sixteen bytes at offset 28 change and nothing else. A second
// pass finds it v19; a dry run writes nothing.
TEST(MigrateLoose, OnlyTheTagMoves) {
  StoreBed bed;
  const auto file = loose_path(bed.dir.path() / "objects", the_id());
  legacy::write_file(file, unhex(loose_v18_hex));
  MigrationReport report;
  bool proved = false;
  MigrationOptions dry;
  dry.dry_run = true;
  migrate_loose_file(file, the_id(), *bed.key, v1, report, proved, dry);
  EXPECT_EQ(hex(legacy::read_file(file)), loose_v18_hex);
  EXPECT_EQ(report.loose_migrated, 1u);
  EXPECT_EQ(report.bytes_written, 0u);
  EXPECT_TRUE(proved);
  report = {};
  migrate_loose_file(file, the_id(), *bed.key, v1, report, proved, {});
  const auto after = legacy::read_file(file);
  EXPECT_EQ(hex(after), loose_v19_hex);
  const auto before = unhex(loose_v18_hex);
  ASSERT_EQ(after.size(), before.size());
  for (size_t i = 0; i < after.size(); ++i)
    if (i < loose_tag_offset || i >= loose_tag_offset + 16) EXPECT_EQ(after[i], before[i]) << i;
  EXPECT_EQ(report.loose_migrated, 1u);
  EXPECT_EQ(report.bytes_written, 16u);
  report = {};
  migrate_loose_file(file, the_id(), *bed.key, v1, report, proved, {});
  EXPECT_EQ(report.loose_already, 1u);
  EXPECT_EQ(report.loose_migrated, 0u);
  EXPECT_EQ(hex(legacy::read_file(file)), loose_v19_hex);
}

// Macha: OpensALooseObjectMachaWrote. A loose object file Macha wrote opens
// in the store once migrated, and not before.
TEST(MigrateLoose, ALooseObjectMachaWroteOpensOnceMigrated) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  const auto file = loose_path(root / "objects", the_id());
  legacy::write_file(file, unhex(loose_v18_hex));
  store::LocalStoreOptions packing;
  packing.limit = 64 * mib;
  {
    auto store = bed.open(root, packing);
    EXPECT_TRUE(store->has(the_id()));
    EXPECT_THROW((void)store->get(the_id()), std::runtime_error);
  }
  MigrationReport report;
  bool proved = false;
  migrate_loose_file(file, the_id(), *bed.key, v1, report, proved, {});
  auto store = bed.open(root, packing);
  EXPECT_TRUE(store->has(the_id()));
  EXPECT_EQ(store->get(the_id()), std::optional<Bytes>(bytes_of(plaintext)));
  EXPECT_FALSE(store->is_packed(the_id()));
}

// A pack: a put record's tag and the checksum after it change (48 bytes in
// one write) and nothing else; a touch and a tombstone, sealing nothing,
// are untouched. Macha: OpensAPackMachaWrote, after the migration: a put
// and then a touch keep the object and take the touch's time; a tombstone
// after them removes it.
TEST(MigratePack, OnlyTheTagAndChecksumMoveAndAPackMachaWroteOpens) {
  StoreBed bed(put_ms + 500);
  const auto root = bed.dir.path() / "store";
  Bytes pack = unhex(put_header_v18_hex);
  const auto ciphertext = unhex(ciphertext_hex);
  pack.insert(pack.end(), ciphertext.begin(), ciphertext.end());
  const auto touch = unhex(touch_hex);
  pack.insert(pack.end(), touch.begin(), touch.end());
  const auto file = legacy::pack_path(root, 1);
  legacy::write_file(file, pack);
  MigrationReport report;
  bool proved = false;
  migrate_pack_file(file, *bed.key, v1, report, proved, {});
  EXPECT_EQ(report.pack_records_migrated, 1u);
  EXPECT_EQ(report.pack_records_unsealed, 1u);
  EXPECT_EQ(report.bytes_written, 48u);
  auto expected = unhex(put_header_v19_hex);
  expected.insert(expected.end(), ciphertext.begin(), ciphertext.end());
  expected.insert(expected.end(), touch.begin(), touch.end());
  const auto after = legacy::read_file(file);
  EXPECT_EQ(hex(after), hex(expected));
  for (size_t i = 0; i < after.size(); ++i)
    if (i < pack_tag_offset || i >= pack_header_size) EXPECT_EQ(after[i], pack[i]) << i;
  ASSERT_TRUE(decode_pack_header(std::span<const uint8_t>(after).first(pack_header_size)).has_value());
  report = {};
  migrate_pack_file(file, *bed.key, v1, report, proved, {});
  EXPECT_EQ(report.pack_records_already, 1u);
  EXPECT_EQ(report.pack_records_migrated, 0u);
  EXPECT_EQ(hex(legacy::read_file(file)), hex(expected));

  store::LocalStoreOptions packing;
  packing.limit = 64 * mib;
  {
    auto store = bed.open(root, packing);
    EXPECT_TRUE(store->has(the_id()));
    EXPECT_TRUE(store->is_packed(the_id()));
    EXPECT_EQ(store->get(the_id()), std::optional<Bytes>(bytes_of(plaintext)));
    EXPECT_EQ(store->stored_size(the_id()), pack_header_size + plaintext.size());
    // Touched at put_ms + 500, which is now: not older than a millisecond.
    EXPECT_FALSE(store->older_than(the_id(), std::chrono::milliseconds(0)));
    bed.wall.advance(std::chrono::milliseconds(2));
    EXPECT_TRUE(store->older_than(the_id(), std::chrono::milliseconds(1)));
  }
  const auto tombstone = unhex(tombstone_hex);
  legacy::write_file(file, tombstone, true);
  auto store = bed.open(root, packing);
  EXPECT_FALSE(store->has(the_id()));
  EXPECT_FALSE(store->get(the_id()).has_value());
}

// ---- A whole state ------------------------------------------------------------

// A v18 state built from the known answers migrates in place, counts what
// it did, and opens as v19 with every object, claim and count read back;
// the second run finds it v19 and touches nothing.
TEST(MigrateState, AV18StateMigratesInPlaceAndOpensAsV19) {
  StoreBed bed;
  V18State v18(bed);
  const auto before = v18.digest();
  std::vector<std::string> said;
  MigrationOptions options;
  options.say = [&](std::string_view line) { said.emplace_back(line); };
  const auto report = v18.migrate(options);
  EXPECT_FALSE(report.already_v19);
  EXPECT_EQ(report.loose_migrated, 5u + 2 + 1);
  EXPECT_EQ(report.loose_already, 0u);
  EXPECT_EQ(report.loose_unreadable, 0u);
  EXPECT_EQ(report.packs, 2u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.pack_records_unsealed, 2u);
  EXPECT_EQ(report.pack_records_already, 0u);
  EXPECT_EQ(report.pack_regions_skipped, 0u);
  EXPECT_EQ(report.pack_tail_bytes, 0u);
  // The two held tries, the claims journal and its two tries, the two
  // reference tries.
  EXPECT_EQ(report.ledgers_migrated, 7u);
  EXPECT_EQ(report.ledgers_already, 0u);
  EXPECT_EQ(report.ledger_tail_bytes_dropped, 0u);
  // The backend's marker, its mirror, the state layout.
  EXPECT_EQ(report.markers_rewritten, 3u);
  EXPECT_GE(report.bytes_written, 8u * 16 + 5 * 48);
  EXPECT_TRUE(report.unreadable.empty());
  EXPECT_NE(report.summary().find("loose objects: 8 migrated"), std::string::npos) << report.summary();
  EXPECT_NE(report.summary().find("markers rewritten: 3"), std::string::npos);
  EXPECT_FALSE(said.empty());
  // Only tags, checksums, ledgers and markers changed: every loose file's
  // length is as it was, and the accounting file (none here) and the
  // custody are untouched.
  const auto after = v18.digest();
  EXPECT_EQ(after.size(), before.size() + 0u);
  EXPECT_EQ(after.at(v18.node_state / "identity" / "custody"), before.at(v18.node_state / "identity" / "custody"));

  // Idempotent: nothing to do, nothing written.
  const auto again = v18.migrate();
  EXPECT_TRUE(again.already_v19);
  EXPECT_EQ(again.loose_migrated + again.pack_records_migrated + again.ledgers_migrated + again.markers_rewritten, 0u);
  EXPECT_EQ(again.summary(), "state is layout v19 already: nothing to do\n");
  EXPECT_EQ(v18.digest(), after);

  expect_v19_opens(bed, v18);
}

// A dry run classifies and counts as a run would and writes nothing.
TEST(MigrateState, ADryRunWritesNothing) {
  StoreBed bed;
  V18State v18(bed);
  const auto before = v18.digest();
  MigrationOptions options;
  options.dry_run = true;
  const auto report = v18.migrate(options);
  EXPECT_TRUE(report.dry_run);
  EXPECT_EQ(report.loose_migrated, 8u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.ledgers_migrated, 7u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  EXPECT_EQ(report.bytes_written, 0u);
  EXPECT_EQ(v18.digest(), before);
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
  EXPECT_NE(report.summary().find("dry run"), std::string::npos);
}

// The wrong cluster key is refused before any write, and so is a disk the
// migration does not recognise: a state with no marker or another layout's,
// a v19 state of another domain, a backend without a marker, a store in
// use. Nothing on the disk changes by any refusal.
TEST(MigrateState, AWrongKeyAndAnUnrecognisedDiskAreRefusedBeforeAnyWrite) {
  StoreBed bed;
  V18State v18(bed);
  // The test's own files go beside the node, so the node's digest is the
  // disk's alone.
  const auto scratch = bed.dir.path() / "scratch";
  std::filesystem::create_directories(scratch);
  const auto before = v18.digest();
  const auto refused = [&](const std::function<void()>& run) -> std::string {
    try {
      run();
    } catch (const MigrationRefused& error) {
      EXPECT_EQ(v18.digest(), before);
      return error.what();
    }
    ADD_FAILURE() << "not refused";
    return {};
  };
  const auto wrong = cluster_key_from(scratch / "wrong.key", "another cluster\n");
  MigrationOptions options;
  options.cache = v18.cache;
  auto why = refused([&] { migrate_state(v18.state, {v18.backend}, wrong, v18.custody(), options); });
  EXPECT_NE(why.find("wrong cluster key"), std::string::npos) << why;
  EXPECT_NE(why.find("opens none of the first 8 records"), std::string::npos) << why;

  why = refused([&] { migrate_state(v18.state, {v18.backend, scratch / "nowhere"}, v18.cluster(), v18.custody(), options); });
  EXPECT_NE(why.find("nowhere"), std::string::npos) << why;
  const auto unmarked = scratch / "unmarked";
  std::filesystem::create_directories(unmarked);
  why = refused([&] { migrate_state(v18.state, {v18.backend, unmarked}, v18.cluster(), v18.custody(), options); });
  EXPECT_NE(why.find("has no .macha.backend marker"), std::string::npos) << why;
  why = refused([&] { migrate_state(scratch / "no-state", {v18.backend}, v18.cluster(), v18.custody(), options); });
  EXPECT_NE(why.find("not a directory"), std::string::npos) << why;
  {
    // In use: the backend's lock held, as a running node holds it.
    store::StorageLock held(v18.backend);
    why = refused([&] { migrate_state(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options); });
    EXPECT_NE(why.find("is in use"), std::string::npos) << why;
  }
  // Other states: no marker, another layout, another domain's v19.
  for (const auto& [text, expect] : std::vector<std::pair<std::string, std::string>>{
           {"macha-state-layout-v17\n", "neither layout v18"}, {"dagda-state-layout-v19 00112233445566778899aabbccddeeff\n", "neither layout v18"}}) {
    const auto other = scratch / "other-state";
    std::filesystem::create_directories(other);
    std::ofstream(other / "storage-layout", std::ios::binary) << text;
    why = refused([&] { migrate_state(other, {}, v18.cluster(), v18.custody(), {}); });
    EXPECT_NE(why.find(expect), std::string::npos) << why;
    std::filesystem::remove_all(other);
  }
  std::filesystem::create_directories(scratch / "bare");
  why = refused([&] { migrate_state(scratch / "bare", {}, v18.cluster(), v18.custody(), {}); });
  EXPECT_NE(why.find("no storage-layout marker"), std::string::npos) << why;
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
}

// A ledger that opens under neither key (another node's, or damaged) is
// refused, naming it; the objects before it are migrated, which a second
// run skips, and the markers are not touched, so the state is still v18.
TEST(MigrateState, ALedgerUnderNeitherKeyIsRefusedAndTheStateStaysV18) {
  StoreBed bed;
  V18State v18(bed);
  // A stranger's trie under the state.
  {
    ledger::ObjectTrie stranger(v18.state / "ledger" / "stranger", test::key_of(0x77), {}, test::quiet_logger());
    stranger.apply(std::vector<ledger::ObjectTrie::Change>{{v18.known, Bytes{1}}});
    stranger.checkpoint();
  }
  std::string why;
  try {
    (void)v18.migrate();
  } catch (const MigrationRefused& error) {
    why = error.what();
  }
  EXPECT_NE(why.find("ledger/stranger"), std::string::npos) << why;
  EXPECT_NE(why.find("neither the node-local key nor the domain key"), std::string::npos) << why;
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
  EXPECT_EQ(read_text(v18.backend / ".macha.backend"), legacy::backend_marker_v18(v18.node, v18.token));
  EXPECT_EQ(hex(legacy::read_file(loose_path(v18.backend / "objects", v18.known))), loose_v19_hex);
  std::filesystem::remove_all(v18.state / "ledger" / "stranger");
  const auto report = v18.migrate();
  EXPECT_EQ(report.loose_already, 8u);
  EXPECT_EQ(report.pack_records_already, 5u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  expect_v19_opens(bed, v18);
}

// A crash between records (a child that ends the process after three)
// leaves every record v18 or v19, the ledgers under one key or the other,
// and the markers v18, so the store still refuses the disk; a second run
// finishes, skipping what was done, and the store opens.
TEST(MigrateState, ACrashMidWayLeavesADiskASecondRunFinishes) {
  StoreBed bed;
  V18State v18(bed);
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      // Secure memory is not inherited across a fork: the child derives its
      // own keys from the files and never touches the parent's.
      MigrationOptions options;
      options.cache = v18.cache;
      options.after_step = [](const MigrationReport& so_far) {
        if (so_far.loose_migrated + so_far.pack_records_migrated >= 3) ::_exit(0);
      };
      (void)migrate_state(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
      ::_exit(21);
    } catch (...) {
      ::_exit(22);
    }
  }
  ASSERT_EQ(test::exit_code(child), std::optional<int>(0));

  // What it left: three records v19, the rest v18, the markers v18.
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
  EXPECT_EQ(read_text(v18.backend / ".macha.backend"), legacy::backend_marker_v18(v18.node, v18.token));
  size_t v19_records = 0;
  size_t v18_records = 0;
  for (const auto& [id, data] : v18.loose) {
    const auto file = legacy::read_file(loose_path(v18.backend / "objects", id));
    const auto loose = decode_loose(file);
    const auto layout = record_layout(*bed.key, v1, id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size);
    EXPECT_NE(layout, RecordLayout::neither);
    (layout == RecordLayout::v19 ? v19_records : v18_records) += 1;
  }
  EXPECT_EQ(v19_records, 3u);
  EXPECT_EQ(v18_records, v18.loose.size() - 3);
  // The store refuses the half-migrated disk, naming the migration.
  {
    store::StoragePoolOptions options;
    options.backends = {{v18.backend, 64 * mib, 0}};
    options.ledger_key = v18.node_local();
    store::StoragePool pool(v18.state, v18.node, NodeId::random(), options, bed.key, bed.pool_services());
    EXPECT_EQ(pool.online_backends(), 0u);
    EXPECT_TRUE(bed.sink->saw("dagda-migrate --state " + v18.state.string()));
  }
  EXPECT_THROW(store::open_state_layout(v18.state, v1), std::runtime_error);

  const auto report = v18.migrate();
  EXPECT_EQ(report.loose_already, 3u);
  EXPECT_EQ(report.loose_migrated, 5u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.ledgers_migrated, 7u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  expect_v19_opens(bed, v18);
}

// A crash between ledgers: after the objects and some ledgers, with the
// markers still v18. The second run finds every object v19, the ledgers
// done under the node-local key and the rest under the domain key.
TEST(MigrateState, ACrashBetweenLedgersIsResumed) {
  StoreBed bed;
  V18State v18(bed);
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      MigrationOptions options;
      options.cache = v18.cache;
      options.after_step = [](const MigrationReport& so_far) {
        if (so_far.ledgers_migrated >= 3) ::_exit(0);
      };
      (void)migrate_state(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
      ::_exit(21);
    } catch (...) {
      ::_exit(22);
    }
  }
  ASSERT_EQ(test::exit_code(child), std::optional<int>(0));
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
  const auto report = v18.migrate();
  EXPECT_EQ(report.loose_already, 8u);
  EXPECT_EQ(report.pack_records_already, 5u);
  EXPECT_EQ(report.ledgers_already, 3u);
  EXPECT_EQ(report.ledgers_migrated, 4u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  expect_v19_opens(bed, v18);
}

// A torn tail on a pack and on a journal, and a damaged record: the tail is
// left for the store to truncate, the journal's dropped as its replay would
// drop it, the record listed and left, and the rest migrates.
TEST(MigrateState, TornTailsAndADamagedRecordAreReportedAndTheRestMigrates) {
  StoreBed bed;
  V18State v18(bed);
  // A torn pack tail: half a header.
  legacy::write_file(legacy::pack_path(v18.backend, 2), Bytes(40, 0x5a), true);
  // A damaged loose record: its last byte flipped.
  test::corrupt_object(v18.backend, v18.loose[2].first);
  // A torn journal tail: bytes after the claims' last frame.
  legacy::write_file(v18.state / "retention" / "claims.log", Bytes(7, 0x33), true);
  const auto report = v18.migrate();
  EXPECT_EQ(report.pack_tail_bytes, 40u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.loose_migrated, 7u);
  EXPECT_EQ(report.loose_unreadable, 1u);
  ASSERT_EQ(report.unreadable.size(), 1u);
  EXPECT_NE(report.unreadable[0].find(store::to_string(v18.loose[2].first)), std::string::npos);
  EXPECT_EQ(report.ledger_tail_bytes_dropped, 7u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  EXPECT_NE(report.summary().find("left as it was"), std::string::npos);
  // The damaged one stays damaged, the rest open.
  const auto damaged = v18.loose[2];
  v18.loose.erase(v18.loose.begin() + 2);
  expect_v19_opens(bed, v18);
  store::StoragePoolOptions options;
  options.backends = {{v18.backend, 64 * mib, 0}};
  options.ledger_key = v18.node_local();
  store::StoragePool pool(v18.state, v18.node, NodeId::random(), options, bed.key, bed.pool_services());
  ASSERT_EQ(pool.online_backends(), 1u);
  EXPECT_FALSE(pool.valid(damaged.first));
}

// The tool: usage, --version, a run that migrates (its evidence on
// stdout), a second that finds nothing to do, and a refusal with its reason
// on stderr and exit code 1.
TEST(MigrateTool, DagdaMigrateRunsTheMigration) {
  StoreBed bed;
  V18State v18(bed);
  const std::string tool = std::string(DAGDA_BINARY_DIR) + "/dagda-migrate";
  ASSERT_TRUE(std::filesystem::exists(tool));
  ::setenv("DAGDA_TEST_PASSPHRASE", "dagda", 1);
  const auto deadline = [] { return std::chrono::steady_clock::now() + 60s; };
  const auto run = [&](std::vector<std::string> arguments, const std::string& log) {
    arguments.insert(arguments.begin(), tool);
    dannan::test::ChildProcess child(arguments, bed.dir.path() / log);
    const auto output = child.read_all(deadline());
    const auto code = child.wait(deadline());
    return std::make_pair(code, output);
  };
  const std::vector<std::string> common{
      "--state",          v18.state.string(),      "--node-state", v18.node_state.string(), "--cluster-key", v18.cluster_key_file.string(),
      "--passphrase-env", "DAGDA_TEST_PASSPHRASE", "--backend",    v18.backend.string(),    "--cache",       v18.cache.string()};
  {
    const auto [code, output] = run({"--version"}, "version.log");
    EXPECT_EQ(code, std::optional<int>(0));
    EXPECT_EQ(output.substr(0, 6), "0.1.0 ");
  }
  {
    const auto [code, output] = run({}, "usage.log");
    EXPECT_EQ(code, std::optional<int>(2));
    EXPECT_NE(read_text(bed.dir.path() / "usage.log").find("usage: dagda-migrate"), std::string::npos);
  }
  {
    auto wrong = common;
    wrong[5] = (v18.root / "wrong.key").string();
    (void)cluster_key_from(v18.root / "wrong.key", "another cluster\n");
    const auto [code, output] = run(wrong, "wrong.log");
    EXPECT_EQ(code, std::optional<int>(1));
    EXPECT_NE(read_text(bed.dir.path() / "wrong.log").find("wrong cluster key"), std::string::npos);
    EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
  }
  {
    const auto [code, output] = run(common, "run.log");
    EXPECT_EQ(code, std::optional<int>(0)) << read_text(bed.dir.path() / "run.log");
    EXPECT_NE(output.find("loose objects: 8 migrated"), std::string::npos) << output;
    EXPECT_NE(output.find("pack records: 5 migrated"), std::string::npos) << output;
    EXPECT_NE(output.find("ledgers: 7 re-sealed"), std::string::npos) << output;
    EXPECT_NE(output.find("markers rewritten: 3"), std::string::npos) << output;
  }
  {
    const auto [code, output] = run(common, "again.log");
    EXPECT_EQ(code, std::optional<int>(0));
    EXPECT_NE(output.find("nothing to do"), std::string::npos) << output;
  }
  expect_v19_opens(bed, v18);
}

}  // namespace
}  // namespace dagda::migrate
