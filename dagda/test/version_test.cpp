//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/version.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "dannan/test/fixtures/temp_dir.h"
#include "dannan/version.h"

namespace dagda {
namespace {

// The library and the headers it was built with agree, so a mismatch can be
// refused by comparing the two.
TEST(Version, LibraryMatchesItsHeaders) { EXPECT_STREQ(library_version(), kVersion); }

// Dagda is built against the Dannan it links: the minor version it was
// compiled against is the one loaded (0.x: a minor is a new API).
TEST(Version, BuiltAgainstTheDannanItLinks) {
  const std::string built = dannan_version_built_against();
  const std::string loaded = dannan::library_version();
  EXPECT_EQ(built, loaded);
  EXPECT_EQ(dannan::kVersionMajor, 0);
  EXPECT_EQ(dannan::kVersionMinor, 9);
}

// Dannan's shared fixtures are usable from here and write inside the build
// tree only.
TEST(Version, DannanFixturesWriteInsideTheBuildTree) {
  dannan::test::TempDir dir;
  EXPECT_TRUE(std::filesystem::is_directory(dir.path()));
  EXPECT_NE(dir.path().string().find("/test-tmp/"), std::string::npos);
}

}  // namespace
}  // namespace dagda
