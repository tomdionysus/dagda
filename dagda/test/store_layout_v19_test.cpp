//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The one layout, v19 (step 6; the maintainer, 2026-11-04 evening and night): every
// backend's marker names its domain, every object is sealed with the domain
// in the associated data, a disk is refused by a node that lacks its
// domain's key unless opened blind, and a member probes its key on a disk a
// blind node adopted before trusting it (the key probe at activate). Macha's
// v18 disks are refused with a reason naming dagda-migrate and nothing on
// them is changed: the migration is manual. The binding test (stack
// document, security requirement a; decision 12); the state layout marker;
// the control store and the cache. Every node's ledgers are under its
// node-local key, and a store with a ledger refuses to open without one.

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
#include "dannan/encoding/base.h"
#include "dannan/identity/crypto.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using dannan::encoding::hex;
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

// A pool of loose objects under `sealing` and `role`, on `node`, its
// ledgers under the bed's node-local key unless another is given.
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
  options.ledger_key = ledger_key ? std::move(ledger_key) : bed.node_local;
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
  EXPECT_EQ(backend_key_check(*bed.key, domain, *token), marker.substr(117, 32));
  EXPECT_EQ(marker.back(), '\n');
  EXPECT_EQ(backend_marker_text(SealingScheme::v19(domain), filled(0xab), *token, marker.substr(117, 32)) + "\n", marker);
  EXPECT_EQ(read_file(backend_identity_path(bed.dir.path() / "fresh-state", fresh)), marker);
}

// A disk is its domain's for ever: a v19 disk opens only in its own
// domain. Macha's v18 disk is refused with a reason naming the migration and
// the state, and left as it was: the store never migrates on its own.
TEST(StoreLayoutV19, AV18DiskIsRefusedNamingTheMigrationAndAV19DiskStaysItsDomains) {
  StoreBed bed;
  const auto state = bed.dir.path() / "state";
  const auto v18 = bed.dir.path() / "v18";
  const auto v19 = bed.dir.path() / "v19";
  std::filesystem::create_directories(v18);
  const std::string v18_marker = "macha-backend-v18 abababababababababababababababab cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd\n";
  std::ofstream(v18 / ".macha.backend") << v18_marker;
  std::string v19_marker;
  {
    auto pool = pool_of(bed, bed.dir.path() / "state-x", v19, SealingScheme::v19(the_domain()), bed.key);
    ASSERT_EQ(pool->online_backends(), 1u);
    v19_marker = read_file(v19 / ".macha.backend");
  }
  // The v18 disk, in a pool of its own domain (v1) and in another's:
  // refused, naming dagda-migrate, and not rewritten.
  for (const auto& sealing : {SealingScheme::v19(domain::domain_v1), SealingScheme::v19(the_domain())}) {
    auto pool = pool_of(bed, state, v18, sealing, bed.key);
    EXPECT_EQ(pool->online_backends(), 0u);
    EXPECT_TRUE(pool->degraded());
    EXPECT_TRUE(test::saw(*bed.sink, "layout v18 (Macha's) is not opened by the store"));
    EXPECT_TRUE(test::saw(*bed.sink, "dagda-migrate --state " + state.string()));
    EXPECT_EQ(read_file(v18 / ".macha.backend"), v18_marker);
    EXPECT_FALSE(std::filesystem::exists(backend_identity_path(state, v18)));
  }
  // The v19 disk in a pool of another domain: refused.
  {
    auto pool = pool_of(bed, bed.dir.path() / "state-w", v19, SealingScheme::v19(other_domain()), bed.key);
    EXPECT_EQ(pool->online_backends(), 0u);
    EXPECT_TRUE(test::saw(*bed.sink, std::string("backend belongs to domain ") + domain_hex));
  }
  EXPECT_EQ(read_file(v19 / ".macha.backend"), v19_marker);
  // In its own pool again: online, unchanged.
  {
    auto pool = pool_of(bed, bed.dir.path() / "state-x", v19, SealingScheme::v19(the_domain()), bed.key);
    EXPECT_EQ(pool->online_backends(), 1u);
    EXPECT_EQ(read_file(v19 / ".macha.backend"), v19_marker);
  }
  // A pool's options must hold together: a member needs the domain key, and
  // every pool the node-local key for its ledgers.
  StoragePoolOptions member;
  member.sealing = SealingScheme::v19(the_domain());
  member.ledger_key = key_filled(2);
  EXPECT_THROW(StoragePool(bed.dir.path() / "bad", filled(1), NodeId::random(), member, nullptr, bed.pool_services()), std::invalid_argument);
  member.ledger_key.reset();
  EXPECT_THROW(StoragePool(bed.dir.path() / "bad", filled(1), NodeId::random(), member, bed.key, bed.pool_services()), std::invalid_argument);
  StoragePoolOptions blind;
  blind.sealing = SealingScheme::v19(the_domain());
  blind.role = domain::Role::blind;
  EXPECT_THROW(StoragePool(bed.dir.path() / "bad", filled(1), NodeId::random(), blind, nullptr, bed.pool_services()), std::invalid_argument);
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
    EXPECT_TRUE(test::saw(*bed.sink, "backend key check failed"));
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
    // Flipped, not set: the byte might already be any given value.
    altered.seekg(-1, std::ios::end);
    const char byte = static_cast<char>(altered.get() ^ 0x01);
    altered.seekp(-1, std::ios::end);
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
// empty disk, with no key to check. The same node given the key proves it
// by the probe, and the marker carries the check from then on.
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
  // record opens, and the probe has put the check into the marker.
  {
    auto enrolled = pool_of(bed, bed.dir.path() / "blind-state", bed.dir.path() / "blind", sealing, bed.key, filled(2), domain::Role::member, key_filled(3));
    ASSERT_EQ(enrolled->online_backends(), 1u);
    EXPECT_EQ(enrolled->get(id), std::optional<Bytes>(data));
    EXPECT_TRUE(enrolled->valid(id));
    const auto marker = read_file(bed.dir.path() / "blind" / ".macha.backend");
    EXPECT_EQ(marker.find(" blind\n"), std::string::npos);
    EXPECT_EQ(read_file(backend_identity_path(bed.dir.path() / "blind-state", bed.dir.path() / "blind")), marker);
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

// The key probe at activate (the maintainer, 2026-11-04 night): a member opening a
// disk a blind node adopted has no check to prove its key against, so it
// opens a record. The wrong key fails the probe: the backend stays offline
// with the reason, and the record is still there. The right key passes, and
// the marker is upgraded with the check so the wrong key is refused at the
// marker from then on.
TEST(StoreLayoutV19, AMemberProbesItsKeyOnABlindAdoptedBackend) {
  StoreBed bed;
  const auto sealing = SealingScheme::v19(the_domain());
  const auto data = pattern(2500, 6);
  const auto id = object_id(data);
  const auto disk = bed.dir.path() / "disk";
  const auto state = bed.dir.path() / "state";
  auto member = pool_of(bed, bed.dir.path() / "member-state", bed.dir.path() / "member", sealing, bed.key, filled(1));
  ASSERT_TRUE(member->put(id, data));
  const auto sealed = member->get_sealed(id).value();
  {
    auto blind = pool_of(bed, state, disk, sealing, nullptr, filled(2), domain::Role::blind);
    ASSERT_EQ(blind->online_backends(), 1u);
    ASSERT_TRUE(blind->put_sealed(id, sealed));
  }
  const auto blind_marker = read_file(disk / ".macha.backend");
  ASSERT_NE(blind_marker.find(" blind\n"), std::string::npos);
  {
    auto wrong = pool_of(bed, state, disk, sealing, key_filled(7), filled(2));
    EXPECT_EQ(wrong->online_backends(), 0u);
    EXPECT_TRUE(wrong->degraded());
    EXPECT_TRUE(test::saw(*bed.sink, "backend key probe failed"));
    EXPECT_TRUE(test::saw(*bed.sink, "record " + to_string(id) + " does not open under this node's key"));
  }
  // Nothing discarded, nothing rewritten.
  EXPECT_TRUE(std::filesystem::exists(object_path(disk, id)));
  EXPECT_EQ(read_file(disk / ".macha.backend"), blind_marker);
  {
    auto right = pool_of(bed, state, disk, sealing, bed.key, filled(2));
    ASSERT_EQ(right->online_backends(), 1u);
    EXPECT_EQ(right->get(id), std::optional<Bytes>(data));
  }
  const auto checked = read_file(disk / ".macha.backend");
  EXPECT_EQ(checked.find(" blind\n"), std::string::npos);
  EXPECT_EQ(checked.substr(0, blind_marker.size() - 6), blind_marker.substr(0, blind_marker.size() - 6));
  {
    // The marker refuses the wrong key now, before any probe.
    auto wrong = pool_of(bed, state, disk, sealing, key_filled(7), filled(2));
    EXPECT_EQ(wrong->online_backends(), 0u);
    EXPECT_TRUE(test::saw(*bed.sink, "backend key check failed"));
  }
}

// Step 6's finding: a copy that does not open is discarded by get and by
// the scrub only where the backend's key check has passed. On a
// blind-adopted disk holding nothing to probe, the member's key is
// unchecked, so a record that arrives sealed under another key is kept,
// not thrown away as corrupt.
TEST(StoreLayoutV19, ACopyThatDoesNotOpenIsKeptUntilTheKeyIsChecked) {
  StoreBed bed;
  const auto sealing = SealingScheme::v19(the_domain());
  const auto data = pattern(1800, 8);
  const auto id = object_id(data);
  auto member = pool_of(bed, bed.dir.path() / "member-state", bed.dir.path() / "member", sealing, bed.key, filled(1));
  ASSERT_TRUE(member->put(id, data));
  const auto sealed = member->get_sealed(id).value();
  const auto disk = bed.dir.path() / "disk";
  const auto state = bed.dir.path() / "state";
  {
    auto blind = pool_of(bed, state, disk, sealing, nullptr, filled(2), domain::Role::blind);
    ASSERT_EQ(blind->online_backends(), 1u);
  }
  {
    // Nothing to probe: online, the key unchecked.
    auto unchecked = pool_of(bed, state, disk, sealing, key_filled(7), filled(2));
    ASSERT_EQ(unchecked->online_backends(), 1u);
    ASSERT_TRUE(unchecked->put_sealed(id, sealed));
    EXPECT_FALSE(unchecked->get(id).has_value());
    EXPECT_TRUE(unchecked->has(id));
    EXPECT_TRUE(test::saw(*bed.sink, "kept (key unchecked)"));
    const auto scrubbed = unchecked->scrub_step(0, 0);
    EXPECT_TRUE(scrubbed.complete);
    EXPECT_TRUE(unchecked->has(id));
    EXPECT_TRUE(std::filesystem::exists(object_path(disk, id)));
  }
  // Where the key has been checked, a corrupt copy goes as before.
  const auto checked_disk = bed.dir.path() / "checked";
  auto checked = pool_of(bed, bed.dir.path() / "checked-state", checked_disk, sealing, bed.key, filled(3));
  ASSERT_TRUE(checked->put(id, data));
  test::corrupt_object(checked_disk, id);
  EXPECT_FALSE(checked->get(id).has_value());
  EXPECT_TRUE(test::saw(*bed.sink, "discarding corrupt local object " + to_string(id)));
  EXPECT_FALSE(checked->has(id));
}

// Every ledger a node keeps is sealed under its node-local key: a store with
// a ledger directory and no such key refuses to open, saying so, as a pool
// does (above). Nothing falls back to the domain key.
TEST(StoreLayoutV19, AStoreWithALedgerNeedsTheNodeLocalKey) {
  StoreBed bed;
  auto options = LocalStoreOptions::loose_only(64 * mib);
  options.ledger_dir = bed.dir.path() / "ledger";
  std::string why;
  try {
    LocalStore store(bed.dir.path() / "store", options, bed.key, bed.services());
  } catch (const std::invalid_argument& error) {
    why = error.what();
  }
  EXPECT_NE(why.find("node-local key"), std::string::npos) << why;
  EXPECT_FALSE(std::filesystem::exists(options.ledger_dir));
  options.ledger_key = bed.node_local;
  LocalStore store(bed.dir.path() / "store", options, bed.key, bed.services());
  EXPECT_TRUE(std::filesystem::is_directory(options.ledger_dir));
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
  EXPECT_THROW((void)open_object(*bed.key, SealingScheme::v19(domain::domain_v1), id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size),
               std::runtime_error);
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
  // And domain v1's store of the same key reads it as nothing it can open.
  move_to(bed.dir.path() / "v1", id);
  auto v1 = bed.open(bed.dir.path() / "v1", LocalStoreOptions::loose_only(64 * mib));
  EXPECT_TRUE(v1->has(id));
  EXPECT_THROW((void)v1->get(id), std::runtime_error);
  // A blind local store's options must hold together too: a ledger needs
  // the node's key.
  auto blind = options;
  blind.role = domain::Role::blind;
  blind.ledger_dir = bed.dir.path() / "blind-ledger";
  EXPECT_THROW(std::make_unique<LocalStore>(bed.dir.path() / "blind", blind, nullptr, bed.services()), std::invalid_argument);
}

// The state directory's marker names the layout and the domain, refusing
// another domain and Macha's v18 (store_state_layout_test has its bytes).
TEST(StoreLayoutV19, TheStateLayoutMarkerNamesTheDomain) {
  dannan::test::TempDir dir;
  const auto v19 = dir.path() / "v19";
  open_state_layout(v19, SealingScheme::v19(the_domain()));
  EXPECT_EQ(read_file(v19 / "storage-layout"), std::string("dagda-state-layout-v19 ") + domain_hex + "\n");
  open_state_layout(v19, SealingScheme::v19(the_domain()));
  EXPECT_THROW(open_state_layout(v19, SealingScheme::v19(domain::domain_v1)), std::runtime_error);
  EXPECT_THROW(open_state_layout(v19, SealingScheme::v19(other_domain())), std::runtime_error);
  EXPECT_EQ(read_file(v19 / "storage-layout"), std::string("dagda-state-layout-v19 ") + domain_hex + "\n");
  const auto v18 = dir.path() / "v18";
  std::filesystem::create_directories(v18);
  std::ofstream(v18 / "storage-layout") << "macha-state-layout-v18\n";
  EXPECT_THROW(open_state_layout(v18, SealingScheme::v19(domain::domain_v1)), std::runtime_error);
  EXPECT_EQ(read_file(v18 / "storage-layout"), "macha-state-layout-v18\n");
}

// The control store and the cache seal under their domain.
TEST(StoreLayoutV19, TheControlStoreAndTheCacheSealUnderTheirDomain) {
  StoreBed bed;
  const auto data = pattern(900, 5);
  const auto id = object_id(data);
  ControlStoreOptions control_options;
  control_options.limit = 16 * mib;
  control_options.reserve_free = 0;
  control_options.pack_threshold = 0;
  control_options.pack_target_size = 0;
  control_options.sealing = SealingScheme::v19(the_domain());
  auto control = open_control_store(bed.dir.path() / "state", control_options, bed.key, bed.node_local, bed.services());
  ASSERT_TRUE(control->put(id, data));
  EXPECT_EQ(control->get(id), std::optional<Bytes>(data));
  const auto file = read_file(object_path(control_store_path(bed.dir.path() / "state", control_options), id));
  const auto loose = local_format::decode_loose({reinterpret_cast<const uint8_t*>(file.data()), file.size()});
  EXPECT_THROW((void)open_object(*bed.key, SealingScheme::v19(domain::domain_v1), id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size),
               std::runtime_error);
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
  EXPECT_THROW((void)open_object(*bed.key, SealingScheme::v19(domain::domain_v1), id, cached_loose.nonce, cached_loose.tag, cached_loose.ciphertext,
                                 cached_loose.plain_size),
               std::runtime_error);
}

}  // namespace
}  // namespace dagda::store
