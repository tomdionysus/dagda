//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "dagda/domain/storage_key.h"
#include "dagda/store/local_store.h"

// The CONTROL class's store (requirement S11): one LocalStore of its own,
// beside the DATA pool and never inside it, with its own quota, so DATA
// exhaustion cannot block a CONTROL write. It is not observed by the pool's
// I/O pressure monitor and never enters the DATA credit arbiter.
namespace dagda::store {

// Macha's metadata_store settings (config.hpp:40-46), with its defaults.
struct ControlStoreOptions {
  // Empty means <state>/metadata-objects.
  std::filesystem::path path{};
  // Ceiling for content-addressed control objects (namespace nodes,
  // catalogue shards, tombstone batches).
  uint64_t limit = 4ULL * 1024 * 1024 * 1024;
  // Free space kept on the filesystem, so a CONTROL put stops before the
  // device is full and CONTROL stays writable after DATA's reserve has
  // stopped DATA on a shared filesystem. Macha keeps none
  // (local_state.cpp:134), which section 7 of the requirements lists as not
  // to be reproduced; 256 MiB by default (the maintainer, 2026-11-04), configurable.
  uint64_t reserve_free = 256ULL * 1024 * 1024;
  size_t pack_threshold = 1024 * 1024;
  size_t pack_target_size = 64 * 1024 * 1024;
  // The domain its objects are sealed for; v1 by default. The control store
  // is a member's: it opens what it holds.
  SealingScheme sealing{};
};

// Where the control store lives under `state`.
std::filesystem::path control_store_path(const std::filesystem::path& state, const ControlStoreOptions& options);

// Opens the control store as Macha's local state does
// (cluster/local_state.cpp:134-142): authoritative, a durability domain of
// its own over its root, and its held ledger at <state>/ledger/control,
// sealed under `ledger_key` (the node-local key), with `ledger_cache_bytes`
// of cache (ledger_cache_share(total, LedgerShare::control_held) for a
// node's one cache). Throws as LocalStore's constructor does. Waits on the
// device.
std::unique_ptr<LocalStore> open_control_store(const std::filesystem::path& state, const ControlStoreOptions& options,
                                               std::shared_ptr<const domain::StorageKey> key, std::shared_ptr<const domain::StorageKey> ledger_key,
                                               LocalStore::Services services,
                                               uint64_t ledger_cache_bytes = ledger_cache_share(default_ledger_cache_bytes, LedgerShare::control_held));

}  // namespace dagda::store
