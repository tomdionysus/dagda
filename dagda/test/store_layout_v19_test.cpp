//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Layout v19 (step 6; the maintainer, 2026-11-04 evening): a backend made for any
// domain but v1 is layout v19, its marker naming the domain, every object
// sealed with the domain in the associated data; v18 disks (Macha's) open
// and are written as v18, byte for byte, for ever; a v19 disk is refused by
// a node that lacks its domain's key unless opened blind; the binding test
// (stack document, security requirement a; decision 12); the state layout
// marker; the control store and the cache under v19.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "dagda/domain/domain_id.h"
#include "dagda/store/control_store.h"
#include "dagda/store/local_format.h"
#include "dagda/store/local_store.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/store/sealing.h"
#include "dagda/store/state_layout.h"
#include "dagda/store/storage_pool.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/identity/crypto.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using dannan::identity::hex;
using dannan::identity::NodeId;
using test::object_path;
using test::pattern;
using test::StoreBed;

constexpr uint64_t mib = 1024 * 1024;
constexpr char domain_hex[] = "00112233445566778899aabbccddeeff";

NodeId filled(uint8_t byte) {
  NodeId id;
  id.bytes.fill(byte);
  return id;
}

domain::DomainId the_domain() { return domain::DomainId::parse(domain_hex).value(); }

domain::DomainId other_domain() {
  auto id = the_domain();
  id.bytes[15] ^= 1;
  return id;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), {});
}

std::shared_ptr<const domain::StorageKey> key_filled(uint8_t fill) {
  std::array<uint8_t, 32> key{};
  key.fill(fill);
  return std::make_shared<const domain::StorageKey>(dannan::identity::Secret(key));
}

// A pool of loose objects under `sealing` and `role`, on `node`.
std::unique_ptr<StoragePool> pool_of(StoreBed& bed, const std::filesystem::path& state, const std::filesystem::path& backend, const SealingScheme& sealing,
                                     std::shared_ptr<const domain::StorageKey> key, NodeId node = filled(0xab), domain::Role role = domain::Role::member,
                                     std::shared_ptr<const domain::StorageKey> ledger_key = {}) {
  std::filesystem::create_directories(backend);
  StoragePoolOptions options;
  options.backends = {{backend, 64 * mib, 0}};
  options.durability_batch_window = 0ms;
  options.pack_threshold = 0;
  options.pack_target_size = 0;
  options.sealing = sealing;
  options.role = role;
  options.ledger_key = std::move(ledger_key);
  return std::make_unique<StoragePool>(state, node, NodeId::random(), options, std::move(key), bed.pool_services());
}

// The marker, byte for byte: layout, node, token, domain, and the key check
// (the first sixteen bytes of HMAC-SHA256 under the domain key over
// "dagda/backend-key-check/19" || domain || token). The check was computed
// outside Dagda (Python's hmac) for the known-answer key and token.
TEST(StoreLayoutV19, TheBackendMarkerNamesTheDomainAndChecksTheKey) {
  StoreBed bed;
  const auto state = bed.dir.path() / "state";
  const auto known = bed.dir.path() / "known";
  const auto fresh = bed.dir.path() / "fresh";
  std::filesystem::create_directories(known);
  const std::string known_marker =
      "dagda-backend-v19 abababababababababababababababab cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd 00112233445566778899aabbccddeeff d18db9d0e20fa855ec83a0d5abea46b4\n";
  std::ofstream(known / ".macha.backend") << known_marker;
  {
    auto pool = pool_of(bed, state, known, SealingScheme::v19(the_domain()), bed.key);
    ASSERT_EQ(pool->online_backends(), 1u);
    EXPECT_EQ(read_file(known / ".macha.backend"), known_marker);
    EXPECT_TRUE(std::filesystem::exists(state / "ledger" / ("data-" + dannan::identity::to_string(filled(0xcd)))));
  }
  // A fresh directory is adopted under the same form, its check over its
  // own token.
  auto pool = pool_of(bed, bed.dir.path() / "fresh-state", fresh, SealingScheme::v19(the_domain()), bed.key);
  ASSERT_EQ(pool->online_backends(), 1u);
  const auto marker = read_file(fresh / ".macha.backend");
  ASSERT_EQ(marker.size(), 17u + 1 + 32 + 1 + 32 + 1 + 32 + 1 + 32 + 1);
  EXPECT_EQ(marker.substr(0, 51), "dagda-backend-v19 abababababababababababababababab ");
  const auto token = NodeId::parse(marker.substr(51, 32));
  ASSERT_TRUE(token.has_value());
  EXPECT_EQ(marker.substr(84, 32), domain_hex);
  Bytes message;
  for (const char c : std::string("dagda/backend-key-check/19")) message.push_back(static_cast<uint8_t>(c));
  const auto domain = the_domain();
  message.insert(message.end(), domain.bytes.begin(), domain.bytes.end());
  message.insert(message.end(), token->bytes.begin(), token->bytes.end());
  const auto mac = dannan::identity::hmac_sha256(bed.key->bytes(), message);
  EXPECT_EQ(marker.substr(117, 32), hex(std::span<const uint8_t>(mac.data(), 16)));
  EXPECT_EQ(marker.back(), '\n');
  const auto text = fresh.lexically_normal().string();
  const auto mirror = bed.dir.path() / "fresh-state" / "backend-identities" /
                      (hex(dannan::identity::sha256({reinterpret_cast<const uint8_t*>(text.data()), text.size()})) + ".id");
  EXPECT_EQ(read_file(mirror), marker);
}

// A disk is its layout's for ever: a v18 disk opened by a pool of another
// domain stays offline and unchanged, and a v19 disk opens only in its own
// domain, never as v18.
TEST(StoreLayoutV19, AV18DiskStaysV18AndAV19DiskStaysItsDomains) {
  StoreBed bed;
  const auto state = bed.dir.path() / "state";
  const auto v18 = bed.dir.path() / "v18";
  const auto v19 = bed.dir.path() / "v19";
  std::string v18_marker;
  std::string v19_marker;
  {
    auto pool = pool_of(bed, state, v18, SealingScheme::v18(), bed.key);
    ASSERT_EQ(pool->online_backends(), 1u);
    v18_marker = read_file(v18 / ".macha.backend");
    EXPECT_EQ(v18_marker.substr(0, 18), "macha-backend-v18 ");
  }
  {
    auto pool = pool_of(bed, bed.dir.path() / "state-x", v19, SealingScheme::v19(the_domain()), bed.key);
    ASSERT_EQ(pool->online_backends(), 1u);
    v19_marker = read_file(v19 / ".macha.backend");
  }
  // The v18 disk in a pool of another domain: refused, and not rewritten.
  {
    auto pool = pool_of(bed, bed.dir.path() / "state-y", v18, SealingScheme::v19(the_domain()), bed.key);
    EXPECT_EQ(pool->online_backends(), 0u);
    EXPECT_TRUE(pool->degraded());
    EXPECT_TRUE(bed.sink->saw("layout v18 backend is domain v1's"));
    EXPECT_EQ(read_file(v18 / ".macha.backend"), v18_marker);
  }
  // The v19 disk in a v18 pool, and in a pool of another domain: refused.
  {
    auto pool = pool_of(bed, bed.dir.path() / "state-z", v19, SealingScheme::v18(), bed.key);
    EXPECT_EQ(pool->online_backends(), 0u);
    EXPECT_TRUE(bed.sink->saw("this pool is domain v1 (layout v18)"));
  }
  {
    auto pool = pool_of(bed, bed.dir.path() / "state-w", v19, SealingScheme::v19(other_domain()), bed.key);
    EXPECT_EQ(pool->online_backends(), 0u);
    EXPECT_TRUE(bed.sink->saw(std::string("backend belongs to domain ") + domain_hex));
  }
  EXPECT_EQ(read_file(v19 / ".macha.backend"), v19_marker);
  // Each in its own pool again: online, unchanged.
  {
    auto pool = pool_of(bed, state, v18, SealingScheme::v18(), bed.key);
    EXPECT_EQ(pool->online_backends(), 1u);
    EXPECT_EQ(read_file(v18 / ".macha.backend"), v18_marker);
  }
  {
    auto pool = pool_of(bed, bed.dir.path() / "state-x", v19, SealingScheme::v19(the_domain()), bed.key);
    EXPECT_EQ(pool->online_backends(), 1u);
    EXPECT_EQ(read_file(v19 / ".macha.backend"), v19_marker);
  }
  // A pool's options must hold together.
  StoragePoolOptions bad;
  bad.sealing = SealingScheme::v18();
  bad.role = domain::Role::blind;
  bad.ledger_key = key_filled(2);
  EXPECT_THROW(StoragePool(bed.dir.path() / "bad", filled(1), NodeId::random(), bad, nullptr, bed.pool_services()), std::invalid_argument);
  bad.sealing = SealingScheme::v19(the_domain());
  bad.ledger_key.reset();
  EXPECT_THROW(StoragePool(bed.dir.path() / "bad", filled(1), NodeId::random(), bad, nullptr, bed.pool_services()), std::invalid_argument);
  StoragePoolOptions member;
  member.sealing = SealingScheme::v19(the_domain());
  EXPECT_THROW(StoragePool(bed.dir.path() / "bad", filled(1), NodeId::random(), member, nullptr, bed.pool_services()), std::invalid_argument);
}

// Requirement D3: a node without the domain's key is refused the disk
// unless it opens blind; blind it holds ciphertext, answers presence,
// verifies by the ciphertext digest, and opens nothing. The node's held
// ledgers are under a key of its own whatever its role.
TEST(StoreLayoutV19, AV19DiskIsRefusedWithoutItsKeyUnlessOpenedBlind) {
  StoreBed bed;
  const auto state = bed.dir.path() / "state";
  const auto disk = bed.dir.path() / "disk";
  const auto sealing = SealingScheme::v19(the_domain());
  const auto own = key_filled(9);
  const auto data = pattern(3000, 1);
  const auto id = object_id(data);
  dannan::identity::Hash256 digest;
  {
    auto pool = pool_of(bed, state, disk, sealing, bed.key, filled(0xab), domain::Role::member, own);
    ASSERT_EQ(pool->online_backends(), 1u);
    ASSERT_TRUE(pool->put(id, data));
    const auto sealed = pool->get_sealed(id);
    ASSERT_TRUE(sealed.has_value());
    EXPECT_EQ(sealed->plain_size, data.size());
    EXPECT_EQ(sealed->ciphertext.size(), data.size());
    digest = blind_digest(sealed->nonce, sealed->tag, sealed->ciphertext);
    EXPECT_EQ(pool->held_digest(id), digest);
    EXPECT_EQ(pool->get(id), std::optional<Bytes>(data));
  }
  // Another key: refused at open, nothing read.
  {
    auto pool = pool_of(bed, state, disk, sealing, key_filled(7), filled(0xab), domain::Role::member, own);
    EXPECT_EQ(pool->online_backends(), 0u);
    EXPECT_TRUE(pool->degraded());
    EXPECT_TRUE(bed.sink->saw("backend key check failed"));
  }
  // Blind: no domain key, the held ledger under the node's own.
  {
    auto pool = pool_of(bed, state, disk, sealing, nullptr, filled(0xab), domain::Role::blind, own);
    ASSERT_EQ(pool->online_backends(), 1u);
    EXPECT_EQ(pool->role(), domain::Role::blind);
    EXPECT_TRUE(pool->has(id));
    EXPECT_TRUE(pool->valid(id));
    EXPECT_EQ(pool->held_digest(id), digest);
    EXPECT_THROW((void)pool->get(id), std::logic_error);
    EXPECT_THROW((void)pool->put(id, data), std::logic_error);
    EXPECT_THROW((void)pool->put_deferred(id, data), std::logic_error);
    // The object is still there: a blind get discards nothing.
    EXPECT_TRUE(pool->has(id));
    // A record altered on the disk no longer carries the digest a member
    // bound to the id: that is what a blind node can tell. Its structure is
    // still whole, which is all valid() can say with no key.
    const auto file = object_path(disk, id);
    std::fstream altered(file, std::ios::in | std::ios::out | std::ios::binary);
    altered.seekp(-1, std::ios::end);
    const char byte = 0x5a;
    altered.write(&byte, 1);
    altered.close();
    EXPECT_NE(pool->held_digest(id), digest);
    EXPECT_TRUE(pool->valid(id));
  }
  // The member sees it as corrupt.
  {
    auto pool = pool_of(bed, state, disk, sealing, bed.key, filled(0xab), domain::Role::member, own);
    ASSERT_EQ(pool->online_backends(), 1u);
    EXPECT_FALSE(pool->valid(id));
  }
}

// A sealed record moves between roles unopened: a blind node holds what a
// member sealed and the member reads it back; a blind node adopts its own
// empty disk, with no key to check.
TEST(StoreLayoutV19, ASealedRecordMovesToABlindNodeAndBackUnopened) {
  StoreBed bed;
  const auto sealing = SealingScheme::v19(the_domain());
  const auto data = pattern(2000, 2);
  const auto id = object_id(data);
  auto member = pool_of(bed, bed.dir.path() / "member-state", bed.dir.path() / "member", sealing, bed.key, filled(1));
  ASSERT_TRUE(member->put(id, data));
  const auto sealed = member->get_sealed(id).value();
  const auto digest = member->held_digest(id).value();
  {
    auto blind = pool_of(bed, bed.dir.path() / "blind-state", bed.dir.path() / "blind", sealing, nullptr, filled(2), domain::Role::blind, key_filled(3));
    ASSERT_EQ(blind->online_backends(), 1u);
    EXPECT_NE(read_file(bed.dir.path() / "blind" / ".macha.backend").find(" blind\n"), std::string::npos);
    EXPECT_FALSE(blind->has(id));
    EXPECT_TRUE(blind->put_sealed(id, sealed));
    EXPECT_TRUE(blind->has(id));
    EXPECT_EQ(blind->held_digest(id), digest);
    // A second copy of a record held is taken as held.
    EXPECT_TRUE(blind->put_sealed(id, sealed));
    EXPECT_EQ(blind->get_sealed(id), std::optional<LocalStore::SealedObject>(sealed));
  }
  // The same node, given the key (its ledgers still under its own): the
  // record opens.
  {
    auto enrolled = pool_of(bed, bed.dir.path() / "blind-state", bed.dir.path() / "blind", sealing, bed.key, filled(2), domain::Role::member, key_filled(3));
    ASSERT_EQ(enrolled->online_backends(), 1u);
    EXPECT_EQ(enrolled->get(id), std::optional<Bytes>(data));
    EXPECT_TRUE(enrolled->valid(id));
  }
  // A member takes a sealed record too, without opening it.
  auto second = pool_of(bed, bed.dir.path() / "second-state", bed.dir.path() / "second", sealing, bed.key, filled(4));
  EXPECT_TRUE(second->put_sealed(id, sealed));
  EXPECT_EQ(second->get(id), std::optional<Bytes>(data));
  // A record whose size lies is refused before anything is written, and
  // the backend stays online: it is the record's fault.
  auto lying = sealed;
  lying.plain_size += 1;
  EXPECT_THROW((void)second->put_sealed(object_id(pattern(10)), lying), std::invalid_argument);
  EXPECT_EQ(second->online_backends(), 1u);
}

// The binding test (stack document, security requirement a; decision 12) on
// disk: a v19 object moved to another domain's store, or to another id,
// does not open, under the same key.
TEST(StoreLayoutV19, AV19ObjectMovedToAnotherDomainOrIdDoesNotOpen) {
  StoreBed bed;
  const auto data = pattern(1500, 3);
  const auto id = object_id(data);
  auto options = LocalStoreOptions::loose_only(64 * mib);
  options.sealing = SealingScheme::v19(the_domain());
  auto mine = bed.open(bed.dir.path() / "mine", options);
  ASSERT_TRUE(mine->put(id, data));
  EXPECT_EQ(mine->sealing(), SealingScheme::v19(the_domain()));
  EXPECT_EQ(mine->get(id), std::optional<Bytes>(data));
  // The file as written opens only under its domain and id.
  const auto file = read_file(object_path(bed.dir.path() / "mine", id));
  const auto loose = local_format::decode_loose({reinterpret_cast<const uint8_t*>(file.data()), file.size()});
  EXPECT_EQ(open_object(*bed.key, SealingScheme::v19(the_domain()), id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size), data);
  EXPECT_THROW((void)open_object(*bed.key, SealingScheme::v19(other_domain()), id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size),
               std::runtime_error);
  EXPECT_THROW((void)open_object(*bed.key, id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size), std::runtime_error);
  // Moved to another domain's store (placed before the store opens, as a
  // disk's contents are).
  const auto move_to = [&](const std::filesystem::path& root, const ObjectId& as) {
    std::filesystem::create_directories(object_path(root, as).parent_path());
    std::filesystem::copy_file(object_path(bed.dir.path() / "mine", id), object_path(root, as));
  };
  auto other_options = options;
  other_options.sealing = SealingScheme::v19(other_domain());
  move_to(bed.dir.path() / "other", id);
  auto other = bed.open(bed.dir.path() / "other", other_options);
  EXPECT_TRUE(other->has(id));
  EXPECT_THROW((void)other->get(id), std::runtime_error);
  EXPECT_FALSE(other->valid(id));
  // Moved to another id in its own store.
  auto other_id = object_id(pattern(1500, 4));
  move_to(bed.dir.path() / "mine", other_id);
  EXPECT_THROW((void)mine->get(other_id), std::runtime_error);
  EXPECT_FALSE(mine->valid(other_id));
  // And a v18 store of the same key reads it as nothing it can open.
  move_to(bed.dir.path() / "v18", id);
  auto v18 = bed.open(bed.dir.path() / "v18", LocalStoreOptions::loose_only(64 * mib));
  EXPECT_TRUE(v18->has(id));
  EXPECT_THROW((void)v18->get(id), std::runtime_error);
  // A blind local store's options must hold together too.
  auto blind = options;
  blind.role = domain::Role::blind;
  blind.sealing = SealingScheme::v18();
  EXPECT_THROW(bed.open(bed.dir.path() / "blind", blind), std::invalid_argument);
  blind.sealing = SealingScheme::v19(the_domain());
  blind.ledger_dir = bed.dir.path() / "blind-ledger";
  EXPECT_THROW(std::make_unique<LocalStore>(bed.dir.path() / "blind", blind, nullptr, bed.services()), std::invalid_argument);
}

// The state directory's marker: v18's text unchanged, v19's naming the
// domain, each refusing the other and another domain.
TEST(StoreLayoutV19, TheStateLayoutMarkerNamesTheDomain) {
  dannan::test::TempDir dir;
  const auto v18 = dir.path() / "v18";
  open_state_layout(v18);
  EXPECT_EQ(read_file(v18 / "storage-layout"), "macha-state-layout-v18\n");
  open_state_layout(v18, SealingScheme::v18());
  EXPECT_THROW(open_state_layout(v18, SealingScheme::v19(the_domain())), std::runtime_error);
  EXPECT_EQ(read_file(v18 / "storage-layout"), "macha-state-layout-v18\n");
  const auto v19 = dir.path() / "v19";
  open_state_layout(v19, SealingScheme::v19(the_domain()));
  EXPECT_EQ(read_file(v19 / "storage-layout"), std::string("dagda-state-layout-v19 ") + domain_hex + "\n");
  open_state_layout(v19, SealingScheme::v19(the_domain()));
  EXPECT_THROW(open_state_layout(v19), std::runtime_error);
  EXPECT_THROW(open_state_layout(v19, SealingScheme::v19(other_domain())), std::runtime_error);
  EXPECT_EQ(read_file(v19 / "storage-layout"), std::string("dagda-state-layout-v19 ") + domain_hex + "\n");
  EXPECT_THROW(open_state_layout(dir.path() / "x", SealingScheme{17, {}}), std::invalid_argument);
}

// The control store and the cache seal under v19 when told to.
TEST(StoreLayoutV19, TheControlStoreAndTheCacheSealUnderV19) {
  StoreBed bed;
  const auto data = pattern(900, 5);
  const auto id = object_id(data);
  ControlStoreOptions control_options;
  control_options.limit = 16 * mib;
  control_options.reserve_free = 0;
  control_options.pack_threshold = 0;
  control_options.pack_target_size = 0;
  control_options.sealing = SealingScheme::v19(the_domain());
  auto control = open_control_store(bed.dir.path() / "state", control_options, bed.key, bed.services());
  ASSERT_TRUE(control->put(id, data));
  EXPECT_EQ(control->get(id), std::optional<Bytes>(data));
  const auto file = read_file(object_path(control_store_path(bed.dir.path() / "state", control_options), id));
  const auto loose = local_format::decode_loose({reinterpret_cast<const uint8_t*>(file.data()), file.size()});
  EXPECT_THROW((void)open_object(*bed.key, id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size), std::runtime_error);
  EXPECT_EQ(open_object(*bed.key, SealingScheme::v19(the_domain()), id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size), data);

  CacheOptions cache_options;
  cache_options.path = bed.dir.path() / "cache";
  cache_options.max_blocks = 8;
  cache_options.sealing = SealingScheme::v19(the_domain());
  PersistentBlockCache cache(cache_options, bed.key, bed.services());
  ASSERT_TRUE(cache.enabled());
  ASSERT_TRUE(cache.put(id, data));
  EXPECT_EQ(cache.get(id), std::optional<Bytes>(data));
  const auto cached = read_file(object_path(cache_options.path, id));
  const auto cached_loose = local_format::decode_loose({reinterpret_cast<const uint8_t*>(cached.data()), cached.size()});
  EXPECT_THROW((void)open_object(*bed.key, id, cached_loose.nonce, cached_loose.tag, cached_loose.ciphertext, cached_loose.plain_size), std::runtime_error);
}

}  // namespace
}  // namespace dagda::store
