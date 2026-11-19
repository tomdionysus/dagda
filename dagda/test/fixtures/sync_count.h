//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>

// Macha's fsync_calls and syncfs_calls (tests/test_invariants.cpp): every call
// in the process that asks for stable storage, the shared library's included.
extern "C" {
void dagda_test_sync_count_begin();
void dagda_test_sync_count_end();
uint64_t dagda_test_sync_count();
}

namespace dagda::test {

// Counts from its construction to its destruction.
class SyncCount {
 public:
  SyncCount() { dagda_test_sync_count_begin(); }
  ~SyncCount() { dagda_test_sync_count_end(); }
  SyncCount(const SyncCount&) = delete;
  SyncCount& operator=(const SyncCount&) = delete;
  uint64_t count() const { return dagda_test_sync_count(); }
};

}  // namespace dagda::test
