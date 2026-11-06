//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// A whole layout v18 state, as the migration's tests build it: every category
// the migration moves, under domain v1's key, from the known answers
// (build/kat/step6b/kat.py), and what it must read back as once migrated.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dagda/ledger/held_ledger.h"
#include "dagda/ledger/reference_counts.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/migrate/legacy_v18.h"
#include "dagda/migrate/manifest.h"
#include "dagda/migrate/migrate.h"
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

namespace dagda::test::v18 {

using namespace std::chrono_literals;
using namespace store::local_format;
using dannan::identity::hex;
using dannan::identity::NodeId;
using migrate::MigrationOptions;
using migrate::MigrationReport;
using store::object_id;
using store::SealingScheme;

inline constexpr uint64_t mib = 1024 * 1024;
inline constexpr std::string_view plaintext = "An object Macha sealed under its own id.";
inline constexpr char object_hex[] = "3e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8";
inline constexpr char nonce_hex[] = "000102030405060708090a0b";
inline constexpr char ciphertext_hex[] = "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
inline constexpr char v18_tag_hex[] = "6f6aa83135d5e163f1b04d0c9ec0758c";
inline constexpr char v1_tag_hex[] = "b05d61c20d49091b6d3ee9e145ef880c";
inline constexpr uint64_t put_ms = 1793700000000ull;
// The loose file as Macha wrote it, and as the migration leaves it.
inline constexpr char loose_v18_hex[] =
    "4448544f424a30310000000000000028000102030405060708090a0b6f6aa83135d5e163f1b04d0c9ec0758c"
    "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
inline constexpr char loose_v19_hex[] =
    "4448544f424a30310000000000000028000102030405060708090a0bb05d61c20d49091b6d3ee9e145ef880c"
    "43550b1cd4a892751f602e1a3ec39f8c1ce80674082cf57c62457d06688a4bc417718343251f2723";
// A pack's put record header at put_ms, before and after; a touch at
// put_ms + 500 and a tombstone at put_ms + 1000, which carry nothing sealed.
inline constexpr char put_header_v18_hex[] =
    "4d414348504b303101000001a1a0d9f1003e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "00000000000000280000000000000028000102030405060708090a0b6f6aa83135d5e163f1b04d0c9ec0758c"
    "adcbbd1ad98f932c95b0af893dce6de8efa1d701f299bb7a0e25358c929fc5a2";
inline constexpr char put_header_v19_hex[] =
    "4d414348504b303101000001a1a0d9f1003e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "00000000000000280000000000000028000102030405060708090a0bb05d61c20d49091b6d3ee9e145ef880c"
    "fcb468f0b824611dae607ea77ae1708bbcc74730bcb8dfa08af2731aef36ee76";
inline constexpr char touch_hex[] =
    "4d414348504b303103000001a1a0d9f2f43e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "de2a9d5429d594851c0975a73134b1cb11c2f5a0d13ed3015333df6bb545b580";
inline constexpr char tombstone_hex[] =
    "4d414348504b303102000001a1a0d9f4e83e2d0f89b0b998fb97c9c3972a6f50791847d4cd70101c3910f971c77d43a4f8"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "dde293c4c0f89a1ec8e5b3ec8656f0cbf163c2ebbc563e5e0c159e9eea53718a";
// Domain v1's marker for node ab.. and token cd.. after the migration.
inline constexpr char migrated_marker[] =
    "dagda-backend-v19 abababababababababababababababab cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd 827b07101d79d9d84a433d8e9cb262ca 96fe387af04a9cdb77b3cd0086f4b356\n";
inline constexpr char cluster_key_text[] = "dagda known-answer cluster key\r\n";
inline const SealingScheme v1 = SealingScheme::v19(domain::domain_v1);

inline Bytes unhex(std::string_view text) { return dannan::identity::unhex(std::string(text)).value(); }

inline Bytes bytes_of(std::string_view text) { return Bytes(text.begin(), text.end()); }

inline NodeId filled(uint8_t byte) {
  NodeId id;
  id.bytes.fill(byte);
  return id;
}

inline std::array<uint8_t, 12> the_nonce() {
  std::array<uint8_t, 12> out{};
  const auto bytes = unhex(nonce_hex);
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return out;
}

inline ObjectId the_id() { return object_id(bytes_of(plaintext)); }

inline std::string read_text(const std::filesystem::path& path) {
  const auto bytes = legacy::read_file(path);
  return std::string(bytes.begin(), bytes.end());
}

// Every node's custody source: a passphrase, light costs.
inline dannan::identity::KeySource custody_source() {
  return dannan::identity::KeySource::typed_passphrase(
      [] {
        const std::string phrase = "dagda";
        return dannan::identity::Secret(std::span(reinterpret_cast<const uint8_t*>(phrase.data()), phrase.size()));
      },
      dannan::identity::Argon2Costs{1024, 1, 1});
}

inline dannan::identity::ClusterKey cluster_key_from(const std::filesystem::path& file, std::string_view contents = cluster_key_text) {
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
    // The id the disks carry, in Dannan's file, as the operator arranges it.
    std::ofstream(node_state / "node-id") << dannan::identity::to_string(node) << "\n";
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
      ledger::RetentionStore claims(state, bed.key, quiet_logger());
      claims.retain(contract::RetentionClass::data, loose[1].first, {origin, 1});
      claims.retain(contract::RetentionClass::control, control_objects[0].first, {origin, 2});
      EXPECT_TRUE(claims.compact_if_needed(0));
      claims.retain(contract::RetentionClass::data, packed[1].first, {origin, 3});
    }
    // Reference counts at a root.
    {
      ledger::ReferenceCounts counts(state / "ledger" / "referenced", bed.key, 1 << 20, quiet_logger());
      references_root = control_objects[0].first;
      counts.reset(references_root, {loose[1].first, loose[1].first, packed[1].first}, {control_objects[1].first}, {7, 3});
    }
  }

  static void seed_ledger(const std::filesystem::path& dir, std::shared_ptr<const domain::StorageKey> key, std::vector<ObjectId> ids) {
    ledger::ObjectTrie::Options options;
    options.cache_bytes = 1 << 20;
    ledger::HeldLedger ledger(dir, std::move(key), options, quiet_logger());
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
    return migrate::migrate_state(state, {backend}, cluster(), custody(), options);
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
inline void expect_v19_opens(StoreBed& bed, const V18State& v18) {
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
    ledger::RetentionStore claims(v18.state, node_local, quiet_logger());
    EXPECT_EQ(claims.claims(contract::RetentionClass::data, v18.loose[1].first).adds, (std::map<NodeId, uint64_t>{{v18.origin, 1}}));
    EXPECT_EQ(claims.claims(contract::RetentionClass::control, v18.control_objects[0].first).adds, (std::map<NodeId, uint64_t>{{v18.origin, 2}}));
    EXPECT_EQ(claims.claims(contract::RetentionClass::data, v18.packed[1].first).adds, (std::map<NodeId, uint64_t>{{v18.origin, 3}}));
    EXPECT_EQ(claims.claim_objects(contract::RetentionClass::data), 2u);
    EXPECT_EQ(claims.claim_objects(contract::RetentionClass::control), 1u);
  }
  {
    ledger::ReferenceCounts counts(v18.state / "ledger" / "referenced", node_local, 1 << 20, quiet_logger());
    EXPECT_EQ(counts.root(), std::optional<ObjectId>(v18.references_root));
    EXPECT_EQ(counts.totals().entries, 7u);
    EXPECT_EQ(counts.totals().extent_count, 3u);
    EXPECT_EQ(hex(counts.views().extents.get(v18.loose[1].first).value()), "0000000000000002");
    EXPECT_EQ(hex(counts.views().nodes.get(v18.control_objects[1].first).value()), "0000000000000001");
  }
}

// What S7 brings, standing in: the metadata seam filled, so the comparison's other
// categories can be shown to pass, and only with it there.
class FilledMetadata final : public migrate::MetadataManifestReader {
 public:
  explicit FilledMetadata(std::string head = "same") : _head(std::move(head)) {}
  void read(const migrate::MetadataManifestInput&, const std::function<void(const migrate::MetadataFact&)>& emit) override {
    emit({"certificates", "certificate 1", bytes_of("certificate")});
    emit({"heads", "head 1", bytes_of(_head)});
    emit({"history", "frame 1", bytes_of("frame")});
  }
  bool covers(const std::filesystem::path&, const std::filesystem::path&) const override { return false; }

 private:
  std::string _head;
};

}  // namespace dagda::test::v18
