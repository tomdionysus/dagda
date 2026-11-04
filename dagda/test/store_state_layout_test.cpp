//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// "Refusing unversioned storage", its state half: the state directory's
// layout marker, Dagda's, naming the one layout and the domain.
//
// Moved from Macha's tests/test_storage_v18.cpp (develop 5a06d3c), every
// assertion kept: the state half of test_unversioned_state_is_refused (its
// backend half is ANonEmptyUnversionedBackendIsRefused). Macha drove it
// through a whole node's start; here the marker is opened as the node's start
// opened it. Macha's own v18 marker is refused since step 6b (the maintainer,
// 2026-11-04 night): the migration is manual, and the refusal names it.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "dagda/store/state_layout.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::store {
namespace {

const SealingScheme v1 = SealingScheme::v19(domain::domain_v1);

std::string read_all(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string refusal(const std::filesystem::path& state) {
  try {
    open_state_layout(state, v1);
  } catch (const std::exception& error) {
    return error.what();
  }
  return {};
}

bool refused_for_a_fresh_namespace(const std::filesystem::path& state) { return refusal(state).find("fresh namespace") != std::string::npos; }

// Macha: test_unversioned_state_is_refused, its state half. A state
// directory holding an older layout's files and no marker is refused, and
// left as it was.
TEST(StoreStateLayout, ANonEmptyUnversionedStateIsRefused) {
  dannan::test::TempDir dir;
  const auto state = dir.path() / "old-state";
  std::filesystem::create_directories(state);
  std::ofstream(state / "metadata.bin") << "old namespace";
  EXPECT_TRUE(refused_for_a_fresh_namespace(state));
  EXPECT_FALSE(std::filesystem::exists(state / "storage-layout"));
}

// The one layout's marker, byte for byte: an empty directory (the storage
// lock's file aside) is marked with the layout and the domain, and opens
// again marked.
TEST(StoreStateLayout, AFreshStateIsMarkedWithLayoutV19sBytes) {
  dannan::test::TempDir dir;
  const auto state = dir.path() / "state";
  std::filesystem::create_directories(state);
  std::ofstream(state / ".macha.lock") << "";
  open_state_layout(state, v1);
  EXPECT_EQ(read_all(state / "storage-layout"), "dagda-state-layout-v19 827b07101d79d9d84a433d8e9cb262ca\n");
  // Marked, it accepts whatever else the state holds.
  std::ofstream(state / "node-id") << "0123";
  open_state_layout(state, v1);
  // A missing directory is an empty one.
  open_state_layout(dir.path() / "new", v1);
  EXPECT_EQ(read_all(dir.path() / "new" / "storage-layout"), "dagda-state-layout-v19 827b07101d79d9d84a433d8e9cb262ca\n");
}

// A marker naming another layout is refused.
TEST(StoreStateLayout, AnotherLayoutIsRefused) {
  dannan::test::TempDir dir;
  const auto state = dir.path() / "state";
  std::filesystem::create_directories(state);
  std::ofstream(state / "storage-layout", std::ios::binary) << "dagda-state-layout-v19 827b07101d79d9d84a433d8e9cb262ca\n";
  open_state_layout(state, v1);
  std::ofstream(state / "storage-layout", std::ios::binary | std::ios::trunc) << "macha-state-layout-v17\n";
  EXPECT_TRUE(refused_for_a_fresh_namespace(state));
}

// Macha's v18 marker is refused with a reason that names the migration and
// the state path, and the marker is left as Macha wrote it: the store never
// migrates on its own.
TEST(StoreStateLayout, AV18StateIsRefusedNamingTheMigration) {
  dannan::test::TempDir dir;
  const auto state = dir.path() / "state";
  std::filesystem::create_directories(state);
  std::ofstream(state / "storage-layout", std::ios::binary) << "macha-state-layout-v18\n";
  const auto why = refusal(state);
  EXPECT_NE(why.find("layout v18"), std::string::npos) << why;
  EXPECT_NE(why.find("dagda-migrate --state " + state.string()), std::string::npos) << why;
  EXPECT_EQ(read_all(state / "storage-layout"), "macha-state-layout-v18\n");
  EXPECT_EQ(migrate_first(state).find("layout v18 (Macha's) is not opened by the store"), 0u);
}

}  // namespace
}  // namespace dagda::store
