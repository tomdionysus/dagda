//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// "Refusing unversioned storage", its state half: the state directory's
// layout marker, Dagda's, with Macha's bytes.
//
// Moved from Macha's tests/test_storage_v18.cpp (develop 5a06d3c), every
// assertion kept: the state half of test_unversioned_state_is_refused (its
// backend half is ANonEmptyUnversionedBackendIsRefused). Macha drove it
// through a whole node's start; here the marker is opened as the node's start
// opened it.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "dagda/store/state_layout.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::store {
namespace {

std::string read_all(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool refused_for_a_fresh_namespace(const std::filesystem::path& state) {
  try {
    open_state_layout(state);
  } catch (const std::exception& error) {
    return std::string(error.what()).find("fresh namespace") != std::string::npos;
  }
  return false;
}

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

// Layout v18's marker, byte for byte: an empty directory (the storage lock's
// file aside) is marked, and opens again marked.
TEST(StoreStateLayout, AFreshStateIsMarkedWithLayoutV18sBytes) {
  dannan::test::TempDir dir;
  const auto state = dir.path() / "state";
  std::filesystem::create_directories(state);
  std::ofstream(state / ".macha.lock") << "";
  open_state_layout(state);
  EXPECT_EQ(read_all(state / "storage-layout"), "macha-state-layout-v18\n");
  // Marked, it accepts whatever else the state holds.
  std::ofstream(state / "node-id") << "0123";
  open_state_layout(state);
  // A missing directory is an empty one.
  open_state_layout(dir.path() / "new");
  EXPECT_EQ(read_all(dir.path() / "new" / "storage-layout"), "macha-state-layout-v18\n");
}

// A marker Macha wrote opens; one naming another layout is refused.
TEST(StoreStateLayout, AnotherLayoutIsRefused) {
  dannan::test::TempDir dir;
  const auto state = dir.path() / "state";
  std::filesystem::create_directories(state);
  std::ofstream(state / "storage-layout", std::ios::binary) << "macha-state-layout-v18\n";
  open_state_layout(state);
  std::ofstream(state / "storage-layout", std::ios::binary | std::ios::trunc) << "macha-state-layout-v17\n";
  EXPECT_TRUE(refused_for_a_fresh_namespace(state));
}

}  // namespace
}  // namespace dagda::store
