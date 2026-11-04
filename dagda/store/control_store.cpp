//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/control_store.h"

#include <utility>

namespace dagda::store {

std::filesystem::path control_store_path(const std::filesystem::path& state, const ControlStoreOptions& options) {
  return options.path.empty() ? state / "metadata-objects" : options.path;
}

std::unique_ptr<LocalStore> open_control_store(const std::filesystem::path& state, const ControlStoreOptions& options,
                                               std::shared_ptr<const domain::StorageKey> key, LocalStore::Services services, uint64_t ledger_cache_bytes) {
  LocalStoreOptions local;
  local.limit = options.limit;
  local.reserve_free = options.reserve_free;
  local.pack_threshold = options.pack_threshold;
  local.pack_target_size = options.pack_target_size;
  local.ledger_dir = state / "ledger" / "control";
  local.ledger_cache_bytes = static_cast<size_t>(ledger_cache_bytes);
  local.sealing = options.sealing;
  return std::make_unique<LocalStore>(control_store_path(state, options), local, std::move(key), std::move(services), LocalStoreMode::authoritative);
}

}  // namespace dagda::store
