//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/test/fixtures/sync_count.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>

namespace {

std::atomic<bool> counting{false};
std::atomic<uint64_t> calls{0};

void note_call() {
  if (counting.load(std::memory_order_relaxed)) calls.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

extern "C" {

void dagda_test_sync_count_begin() {
  calls = 0;
  counting = true;
}
void dagda_test_sync_count_end() { counting = false; }
uint64_t dagda_test_sync_count() { return calls.load(std::memory_order_relaxed); }

}  // extern "C"

#if defined(__APPLE__)

extern "C" int dagda_test_counted_fsync(int fd) {
  note_call();
  return ::fsync(fd);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
  const void* replacement;
  const void* replacee;
} interpose_fsync{reinterpret_cast<const void*>(dagda_test_counted_fsync), reinterpret_cast<const void*>(::fsync)};

#else

extern "C" int fsync(int fd) {
  note_call();
  return static_cast<int>(::syscall(SYS_fsync, fd));
}

extern "C" int fdatasync(int fd) {
  note_call();
  return static_cast<int>(::syscall(SYS_fdatasync, fd));
}

extern "C" int syncfs(int fd) __THROW {
  note_call();
  return static_cast<int>(::syscall(SYS_syncfs, fd));
}

#endif
