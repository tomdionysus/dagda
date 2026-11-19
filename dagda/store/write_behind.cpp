//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/write_behind.h"

#include <fcntl.h>

namespace dagda::store {

bool WriteBehind::window_filled(uint64_t bytes) {
  _pending += bytes;
  if (_pending < _window) return false;
  _pending = 0;
  return true;
}

void WriteBehind::wrote(int fd, uint64_t bytes) {
  if (!window_filled(bytes)) return;
#if defined(__linux__)
  // Wait for the writeback started last window, then start this one.
  const unsigned flags = _started ? SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE : SYNC_FILE_RANGE_WRITE;
  (void)::sync_file_range(fd, 0, 0, flags);
#else
  (void)fd;
#endif
  _started = true;
}

void start_writeback(int fd) {
#if defined(__linux__)
  (void)::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
#else
  (void)fd;
#endif
}

}  // namespace dagda::store
