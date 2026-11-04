//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A bulk writer starts writeback once a window of bytes has filled, then
// starts counting again (law 1: a writer's dirty data must not make control
// wait). Macha has no case of its own for it; this is the decision wrote()
// acts on.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <vector>

#include "dagda/store/write_behind.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::store {
namespace {

TEST(StoreWriteBehind, AWindowFillsThenStartsAgain) {
  WriteBehind behind(100);
  EXPECT_FALSE(behind.window_filled(40));
  EXPECT_FALSE(behind.window_filled(59));
  EXPECT_TRUE(behind.window_filled(1));
  EXPECT_FALSE(behind.window_filled(99));
  EXPECT_TRUE(behind.window_filled(250));
  EXPECT_TRUE(behind.window_filled(100));
  EXPECT_EQ(WriteBehind::default_window, 8ull * 1024 * 1024);
}

// Telling it of writes and starting a whole file's writeback leave the file
// as written: neither changes a byte nor fails on a regular file.
TEST(StoreWriteBehind, WritebackLeavesTheFileAsWritten) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "file";
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  ASSERT_GE(fd, 0);
  WriteBehind behind(4096);
  const std::vector<uint8_t> block(4096, 0x5a);
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(::write(fd, block.data(), block.size()), static_cast<ssize_t>(block.size()));
    behind.wrote(fd, block.size());
  }
  start_writeback(fd);
  ASSERT_EQ(::close(fd), 0);
  EXPECT_EQ(std::filesystem::file_size(path), 4u * 4096);
}

}  // namespace
}  // namespace dagda::store
