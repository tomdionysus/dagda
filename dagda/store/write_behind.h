//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>

namespace dagda::store {

// Keeps a bulk writer's unwritten data small. On a shared ext4 filesystem a
// small fsync (a state commit) commits the journal, which can wait for every
// other file's dirty data; a writer that leaves gigabytes dirty makes control
// wait (law 1). A writer told of each write starts writeback every window
// and, before starting the next, waits for the last to reach the disk, so it
// holds at most about two windows unwritten. Not durability: nothing here is
// an fsync. Linux only; elsewhere it does nothing. Single owner: the writer.
class WriteBehind {
 public:
  static constexpr uint64_t default_window = 8ull * 1024 * 1024;
  explicit WriteBehind(uint64_t window = default_window) : _window(window) {}

  // `bytes` were just written to `fd`. Waits on the device for the last
  // window's writeback, once a window fills.
  void wrote(int fd, uint64_t bytes);

  // Whether the bytes written so far fill a window, which then starts again:
  // the decision wrote() acts on. Waits on nothing.
  bool window_filled(uint64_t bytes);

 private:
  uint64_t _window;
  uint64_t _pending = 0;
  bool _started = false;
};

// A whole file was just written to `fd`: starts its writeback without
// waiting for it. Thread-safe.
void start_writeback(int fd);

}  // namespace dagda::store
