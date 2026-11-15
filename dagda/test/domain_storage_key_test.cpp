//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Domain v1's storage key is Macha's, byte for byte (requirement D1). The
// expected values were computed outside both codebases from Macha's
// derivation (crypto.cpp:103-119 at develop 5a06d3c): SHA-256 and HMAC from
// a reference library, HKDF by RFC 5869 by hand and cross-checked with
// `openssl kdf HKDF`.

#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>
#include <string>

#include "dagda/domain/storage_key.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::domain {
namespace {

using dannan::identity::hex;

// A key file as an operator writes one: the line end is not part of it.
constexpr char key_file[] = "dagda known-answer cluster key\r\n";

dannan::identity::ClusterKey load(const dannan::test::TempDir& dir, const std::string& contents) {
  const auto path = dir.path() / "cluster.key";
  std::ofstream(path, std::ios::binary) << contents;
  return dannan::identity::load_cluster_key(path);
}

TEST(DomainStorageKey, DerivesMachasStorageKeyFromTheClusterKeyFile) {
  dannan::test::TempDir dir;
  const auto cluster = load(dir, key_file);
  // Macha's master: the SHA-256 of the file less its trailing CR and LF.
  EXPECT_EQ(hex(cluster.master), "fc8b31b8d26898b022c16586fde3c06bfe6e46e3b1dfbf31211ba5485ebc1dbb");
  // Macha's ClusterKeys::storage: HKDF-SHA256(master, no salt, "macha/storage/v1").
  const auto key = StorageKey::derive_v1(cluster);
  EXPECT_EQ(hex(key.bytes()), "879277d5274170fabe955dd8b14c14edbd54533790e24617b7cff11a9679bbd8");
}

TEST(DomainStorageKey, IsDannansDerivationUnderMachasLabel) {
  dannan::test::TempDir dir;
  const auto cluster = load(dir, key_file);
  const auto key = StorageKey::derive_v1(cluster);
  const auto expected = cluster.derive(storage_key_label_v1);
  EXPECT_EQ(hex(key.bytes()), hex(expected));
}

// The labels Dannan derives its own keys under are not Macha's: its auth key
// and cluster id differ from Macha's, so neither may stand in for anything
// Macha wrote. The storage key is the only one the store's bytes depend on.
TEST(DomainStorageKey, DannansOwnLabelsAreNotMachas) {
  dannan::test::TempDir dir;
  const auto cluster = load(dir, key_file);
  // Macha's cluster id: the first 16 bytes of HKDF(master, "macha/cluster-id/v1").
  EXPECT_NE(hex(cluster.cluster_id), "f8119b5593004911566ebfdcb5951b0d");
  EXPECT_EQ(hex(cluster.cluster_id), "8adff876bbc85ad6d8e7abc4db1d6dc7");
  // Macha's auth key: HKDF(master, "macha/auth/v1").
  EXPECT_NE(hex(cluster.auth), "363d7ce7101df259c698ffb6ec64c03d01c0c1cf43b0db982c0bc31163ffece3");
  EXPECT_EQ(hex(cluster.derive("macha/auth/v1")), "363d7ce7101df259c698ffb6ec64c03d01c0c1cf43b0db982c0bc31163ffece3");
}

TEST(DomainStorageKey, TrailingLineEndsAreNotPartOfTheKey) {
  dannan::test::TempDir dir;
  const auto bare = StorageKey::derive_v1(load(dir, "dagda known-answer cluster key"));
  const auto crlf = StorageKey::derive_v1(load(dir, "dagda known-answer cluster key\n\r\n"));
  EXPECT_EQ(hex(bare.bytes()), hex(crlf.bytes()));
}

TEST(DomainStorageKey, IsThirtyTwoBytesOrRefused) {
  EXPECT_THROW(StorageKey(dannan::identity::Secret::random(16)), std::invalid_argument);
  const auto random = dannan::identity::Secret::random(32);
  const std::string expected = hex(random.reveal());
  const StorageKey key(dannan::identity::Secret(random.reveal()));
  EXPECT_EQ(hex(key.bytes()), expected);
}

}  // namespace
}  // namespace dagda::domain
