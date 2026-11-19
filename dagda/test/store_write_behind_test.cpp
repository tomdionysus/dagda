//
// Dagda - The Distributed Object Store
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

#include <algorithm>
#include <filesystem>
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

// Macha (tests/test_foundations.cpp): test_write_behind_acts_once_per_window.
// One large write fills one window, whatever it overfills by: nothing carries.
TEST(StoreWriteBehind, ActsOncePerWindowAndAnOverfillLeavesNoCarry) {
  WriteBehind behind(8);
  EXPECT_FALSE(behind.window_filled(3));
  EXPECT_FALSE(behind.window_filled(4));
  EXPECT_TRUE(behind.window_filled(1));
  EXPECT_FALSE(behind.window_filled(7));
  EXPECT_TRUE(behind.window_filled(20));
  EXPECT_FALSE(behind.window_filled(0));
}

// Macha (tests/test_foundations.cpp):
// test_write_behind_leaves_what_was_written_intact, the bytes read back.
TEST(StoreWriteBehind, LeavesWhatWasWrittenIntact) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "staged";
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  ASSERT_GE(fd, 0);
  WriteBehind behind(64 * 1024);
  std::vector<uint8_t> chunk(10000);
  uint64_t written = 0;
  for (int round = 0; round < 40; ++round) {
    std::fill(chunk.begin(), chunk.end(), static_cast<uint8_t>(round));
    ASSERT_EQ(::pwrite(fd, chunk.data(), chunk.size(), static_cast<off_t>(written)), static_cast<ssize_t>(chunk.size()));
    written += chunk.size();
    behind.wrote(fd, chunk.size());
  }
  start_writeback(fd);
  std::vector<uint8_t> back(written);
  ASSERT_EQ(::pread(fd, back.data(), back.size(), 0), static_cast<ssize_t>(back.size()));
  ASSERT_EQ(::close(fd), 0);
  for (uint64_t i = 0; i < written; ++i) ASSERT_EQ(back[i], static_cast<uint8_t>(i / chunk.size())) << "byte " << i;
}

}  // namespace
}  // namespace dagda::store
