//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/test/fixtures/child_exit.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>

#if defined(__SANITIZE_THREAD__)
#define DAGDA_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DAGDA_TEST_TSAN 1
#endif
#endif

namespace {

// Reports raised in this process since it began, or since the fork that made it.
std::atomic<unsigned> reports{0};

void forked() { reports.store(0, std::memory_order_relaxed); }

[[maybe_unused]] const int registered = pthread_atfork(nullptr, nullptr, forked);

}  // namespace

// The sanitizers call this once for each report's summary line, in place of
// printing it; it prints it as they would, and counts it.
extern "C" void __sanitizer_report_error_summary(const char* summary) {
  reports.fetch_add(1, std::memory_order_relaxed);
  std::fprintf(stderr, "%s\n", summary);
}

namespace dagda::test {

void child_exit(int code) {
#if defined(DAGDA_TEST_TSAN)
  // None of its own: past the sanitizer's _exit, which would answer the
  // inherited count. The raw call ends the process as _exit does.
  if (reports.load(std::memory_order_relaxed) == 0) {
#if defined(__linux__)
    ::syscall(SYS_exit_group, code);
#elif defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    ::syscall(SYS_exit, code);
#pragma clang diagnostic pop
#endif
  }
#endif
  ::_exit(code);
}

}  // namespace dagda::test
