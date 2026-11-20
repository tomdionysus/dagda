//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement H25: the cache seed, kept to recover a head set, never authority. Each
// case names Macha's it moved from (tests/test_storage_metadata.cpp at 9a08846, "SM"),
// or the plan's C25, C26.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "dagda/heads/cache_seed.h"
#include "dagda/heads/replica.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/encoding/base.h"
#include "dannan/identity/durable_file.h"

namespace dagda::heads {
namespace {

using store::CacheOptions;
using store::PersistentBlockCache;
using test::commit_record;
using test::flip_byte_at;
using test::head_with;
using test::neutral_domain;
using test::neutral_payload;
using test::neutral_space;
using test::quiet_logger;
using test::record_after;
using test::record_of;
using test::ReplicaBed;
using test::StoreBed;

constexpr Magic seed_magic{'N', 'T', 'R', 'L', 'S', 'E', 'D', '1'};

struct SeedBed {
  StoreBed bed;
  std::filesystem::path root = bed.dir.path() / "cache";
  std::shared_ptr<const domain::StorageKey> key = test::key_of(51);
  PersistentBlockCache cache{{root, 4, true}, bed.key, bed.services()};
  CacheSeed seed{cache, key, neutral_domain(), seed_magic, quiet_logger()};

  std::filesystem::path file(const SpaceId& space = neutral_space()) const { return root / "metadata" / (dannan::encoding::hex(space.bytes) + ".meta"); }
};

Record some_record(uint64_t generation = 5) { return record_of(generation, Hash256{{9}}, head_with({"seeded"})); }

// test_metadata_recovery_cache_seed_is_not_accepted_authority (SM:2024).
TEST(HeadsSeed, ASeedIsNotAcceptedAuthority) {
  ReplicaBed bed;
  const auto base = bed.genesis();
  const auto cached = record_after(base, neutral_payload(head_with({"cached-only"})));

  // Force recovery of the primary state with a valid seed. The seed may be read, but
  // it must not acquire authority without a certificate from another replica.
  std::filesystem::create_directories(bed.path());
  {
    std::ofstream corrupt(bed.path() / "checkpoint.meta", std::ios::binary);
    corrupt << "not encrypted metadata";
  }
  {
    auto recovered = bed.open("space", cached);
    EXPECT_TRUE(recovered->recovery_required());
    EXPECT_EQ(recovered->committed().hash, cached.hash);
    EXPECT_TRUE(recovered->accepted_heads().empty());
    EXPECT_FALSE(recovered->acceptance(cached.hash).has_value());
  }

  // The marker keeps the same fail-closed state on another restart; being able to
  // decrypt the fallback checkpoint does not make it evidence.
  auto reopened = bed.open("space", cached);
  EXPECT_TRUE(reopened->recovery_required());
  EXPECT_EQ(reopened->committed().hash, cached.hash);
  EXPECT_TRUE(reopened->accepted_heads().empty());
  EXPECT_FALSE(reopened->acceptance(cached.hash).has_value());
}

// test_metadata_codec_and_replica's recovery lines (SM:3848, :4098-4133).
TEST(HeadsSeed, ADamagedCheckpointRecoversFromTheSeedAndIsPreserved) {
  ReplicaBed bed;
  Record recovery_seed;
  {
    auto replica = bed.open();
    recovery_seed = replica->committed();
  }
  flip_byte_at(bed.path() / "checkpoint.meta", 40);
  {
    auto recovered = bed.open("space", recovery_seed);
    EXPECT_TRUE(recovered->recovery_required());
    EXPECT_EQ(recovered->committed().hash, recovery_seed.hash);
    bool preserved = false;
    for (const auto& entry : std::filesystem::directory_iterator(bed.path()))
      preserved |= entry.path().filename().string().rfind("checkpoint.meta.corrupt.", 0) == 0;
    EXPECT_TRUE(preserved);
    // The seed may predate this node's commits: the author chain is broken.
    EXPECT_TRUE(std::filesystem::exists(bed.path() / "author-chain-broken"));
    recovered->mark_recovered();
    EXPECT_FALSE(recovered->recovery_required());
  }
  EXPECT_FALSE(std::filesystem::exists(bed.path() / "recovery.required"));
  auto reopened = bed.open();
  EXPECT_EQ(reopened->committed().hash, recovery_seed.hash);

  // The next mutation takes a new author id (md.cpp:2040-2044, :2215-2217).
  const auto node = test::author(7);
  const auto dot = reopened->reserve_mutation_dot(node, {});
  EXPECT_NE(dot.author, node);
  EXPECT_EQ(dot.sequence, 1U);
  EXPECT_FALSE(std::filesystem::exists(bed.path() / "author-chain-broken"));
}

// The same case's next lines: with no seed a primary failure is fail-closed, naming
// the file and the failure.
TEST(HeadsSeed, WithoutASeedAPrimaryFailureNamesTheFile) {
  ReplicaBed bed;
  {
    auto replica = bed.open();
  }
  flip_byte_at(bed.path() / "checkpoint.meta", 40);
  try {
    auto replica = bed.open();
    FAIL() << "a damaged checkpoint with no seed opened";
  } catch (const std::exception& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("checkpoint.meta"), std::string::npos);
    EXPECT_NE(message.find("AES-GCM authentication failed"), std::string::npos);
  }

  // A recovery marker with no seed and no readable checkpoint also refuses.
  ReplicaBed marked;
  {
    auto replica = marked.open();
  }
  flip_byte_at(marked.path() / "checkpoint.meta", 40);
  dannan::identity::durable_replace_file(marked.path() / "recovery.required", "test");
  EXPECT_THROW(marked.open(), std::runtime_error);
  // An invalid seed is no seed.
  auto forged = marked.genesis();
  forged.generation = 4;
  EXPECT_THROW(marked.open("space", forged), std::runtime_error);
}

// test_storage_pool_and_persistent_cache's seed lines (SM:3616, :3829-3844).
TEST(HeadsSeed, TheSeedSurvivesReopenAndFollowsTheCache) {
  StoreBed bed;
  const auto cache_root = bed.dir.path() / "cache";
  const auto key = test::key_of(51);
  const auto record = heads::genesis_record(test::NeutralCodec().genesis());
  {
    PersistentBlockCache cache({cache_root, 2, true}, bed.key, bed.services());
    CacheSeed seed(cache, key, neutral_domain(), seed_magic, quiet_logger());
    seed.remember(neutral_space(), record);
    ASSERT_TRUE(seed.load(neutral_space()).has_value());
    EXPECT_EQ(seed.load(neutral_space())->hash, record.hash);
  }
  {
    PersistentBlockCache reopened({cache_root, 2, true}, bed.key, bed.services());
    CacheSeed seed(reopened, key, neutral_domain(), seed_magic, quiet_logger());
    ASSERT_TRUE(seed.load(neutral_space()).has_value());
    EXPECT_EQ(seed.load(neutral_space())->hash, record.hash);
    reopened.reconfigure({cache_root, 0, true});
    EXPECT_FALSE(reopened.enabled());
    EXPECT_FALSE(seed.load(neutral_space()).has_value());
    reopened.reconfigure({cache_root, 2, true});
    EXPECT_TRUE(reopened.enabled());
    ASSERT_TRUE(seed.load(neutral_space()).has_value());
  }
}

// C25.
TEST(HeadsSeed, AnotherDomainsOrSpacesSeedDoesNotOpen) {
  SeedBed bed;
  const auto record = some_record();
  bed.seed.remember(neutral_space(), record);
  ASSERT_TRUE(bed.seed.load(neutral_space()).has_value());

  // The other space's file, moved under this name.
  const auto other_space = space_id(neutral_domain(), "other");
  std::filesystem::copy_file(bed.file(), bed.file(other_space));
  EXPECT_FALSE(bed.seed.load(other_space).has_value());

  // Another domain's seed, same key and space.
  CacheSeed elsewhere(bed.cache, bed.key, domain::DomainId{{7}}, seed_magic, quiet_logger());
  EXPECT_FALSE(elsewhere.load(neutral_space()).has_value());

  // Another key, or another magic.
  CacheSeed other_key(bed.cache, test::key_of(52), neutral_domain(), seed_magic, quiet_logger());
  EXPECT_FALSE(other_key.load(neutral_space()).has_value());
  CacheSeed other_magic(bed.cache, bed.key, neutral_domain(), Magic{'O', 'T', 'H', 'E', 'R', 'S', 'E', 'D'}, quiet_logger());
  EXPECT_FALSE(other_magic.load(neutral_space()).has_value());

  // The space's own still opens: nothing above disturbed it.
  EXPECT_EQ(bed.seed.load(neutral_space())->hash, record.hash);
}

// C25.
TEST(HeadsSeed, OffMeansNeitherReadNorWritten) {
  StoreBed bed;
  const auto cache_root = bed.dir.path() / "cache";
  const auto key = test::key_of(51);
  const auto record = some_record();
  const auto file = cache_root / "metadata" / (dannan::encoding::hex(neutral_space().bytes) + ".meta");

  // Macha's prefer_metadata false: `seed` unset.
  CacheOptions options{cache_root, 4, false};
  PersistentBlockCache cache(options, bed.key, bed.services());
  CacheSeed seed(cache, key, neutral_domain(), seed_magic, quiet_logger());
  seed.remember(neutral_space(), record);
  EXPECT_FALSE(std::filesystem::exists(file));

  // Written while on, then off: not read.
  cache.reconfigure({cache_root, 4, true});
  seed.remember(neutral_space(), record);
  EXPECT_TRUE(std::filesystem::exists(file));
  ASSERT_TRUE(seed.load(neutral_space()).has_value());
  cache.reconfigure({cache_root, 4, false});
  EXPECT_FALSE(seed.load(neutral_space()).has_value());

  // No blocks is no cache, and so no seed (the live rule the fixture states).
  cache.reconfigure({cache_root, 0, true});
  EXPECT_FALSE(seed.load(neutral_space()).has_value());
  std::filesystem::remove(file);
  seed.remember(neutral_space(), record);
  EXPECT_FALSE(std::filesystem::exists(file));
}

// C25.
TEST(HeadsSeed, ACorruptSeedIsAbsent) {
  SeedBed bed;
  const auto record = some_record();
  bed.seed.remember(neutral_space(), record);
  ASSERT_TRUE(bed.seed.load(neutral_space()).has_value());
  const uint64_t size = std::filesystem::file_size(bed.file());

  for (uint64_t at : {uint64_t{0}, uint64_t{10}, uint64_t{30}, size - 1}) {
    flip_byte_at(bed.file(), at);
    EXPECT_FALSE(bed.seed.load(neutral_space()).has_value()) << "byte " << at;
    flip_byte_at(bed.file(), at);
  }
  ASSERT_TRUE(bed.seed.load(neutral_space()).has_value());

  // Bytes that open but are not a record whose hash holds.
  auto forged = record;
  forged.hash.bytes[0] ^= 1;
  const auto sealed = seal_file(*bed.key, seed_magic, seed_aad(neutral_domain(), neutral_space(), seed_magic), encode_record(forged));
  dannan::identity::durable_replace_file(bed.file(), {reinterpret_cast<const char*>(sealed.data()), sealed.size()});
  EXPECT_FALSE(bed.seed.load(neutral_space()).has_value());

  // Truncated, and empty.
  dannan::identity::durable_replace_file(bed.file(), "short");
  EXPECT_FALSE(bed.seed.load(neutral_space()).has_value());
  dannan::identity::durable_replace_file(bed.file(), "");
  EXPECT_FALSE(bed.seed.load(neutral_space()).has_value());
}

// C26, and the dedupe beside it: a record already written is not written again until
// the place moves, which resets it (persistent_cache.cpp:87).
TEST(HeadsSeed, AMovedPlaceIsWrittenAgain) {
  SeedBed bed;
  const auto record = some_record();
  bed.seed.remember(neutral_space(), record);
  ASSERT_TRUE(std::filesystem::exists(bed.file()));

  // Deduplicated by hash: the file's removal goes unnoticed, as in Macha.
  std::filesystem::remove(bed.file());
  bed.seed.remember(neutral_space(), record);
  EXPECT_FALSE(std::filesystem::exists(bed.file()));

  // A new record is written.
  const auto newer = some_record(6);
  bed.seed.remember(neutral_space(), newer);
  ASSERT_TRUE(std::filesystem::exists(bed.file()));
  EXPECT_EQ(bed.seed.load(neutral_space())->hash, newer.hash);

  // The same record at a moved place, a new path.
  const auto moved = bed.bed.dir.path() / "elsewhere";
  bed.cache.reconfigure({moved, 4, true});
  bed.seed.remember(neutral_space(), newer);
  const auto moved_file = moved / "metadata" / (dannan::encoding::hex(neutral_space().bytes) + ".meta");
  ASSERT_TRUE(std::filesystem::exists(moved_file));

  // And when `opening` moves over the same path.
  std::filesystem::remove(moved_file);
  bed.cache.reconfigure({moved, 4, true});
  bed.seed.remember(neutral_space(), newer);
  EXPECT_TRUE(std::filesystem::exists(moved_file));
  EXPECT_EQ(bed.seed.load(neutral_space())->hash, newer.hash);

  // Spaces do not share a dedupe.
  const auto other = space_id(neutral_domain(), "other");
  bed.seed.remember(other, newer);
  EXPECT_TRUE(bed.seed.load(other).has_value());
}

// The seed through a replica: remembered by one run, recovered by the next, the
// bytes the same.
TEST(HeadsSeed, ARememberedSeedRecoversAReplica) {
  SeedBed seeds;
  ReplicaBed bed;
  Record head;
  {
    auto replica = bed.open();
    commit_record(*replica, record_after(replica->current(), neutral_payload(head_with({"first"}))));
    head = replica->committed();
    seeds.seed.remember(neutral_space(), head);
  }
  flip_byte_at(bed.path() / "checkpoint.meta", 40);
  auto recovered = bed.open("space", seeds.seed.load(neutral_space()));
  EXPECT_TRUE(recovered->recovery_required());
  EXPECT_EQ(recovered->committed().hash, head.hash);
  EXPECT_EQ(recovered->committed().payload, head.payload);
  EXPECT_TRUE(recovered->accepted_heads().empty());
}

}  // namespace
}  // namespace dagda::heads
