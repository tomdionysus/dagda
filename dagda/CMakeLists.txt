#
# Dagda - The Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The store: a hosted system on Dannan, never a floor under it. It links
# Dannan; Dannan never includes anything from here.
add_library(dagda SHARED
  version.cpp
  context.cpp
  node.cpp
  contract/gates.cpp
  contract/horizon.cpp
  domain/domain.cpp
  domain/domain_id.cpp
  domain/storage_key.cpp
  domain/wrap.cpp
  heads/cache_seed.cpp
  heads/control_node_store.cpp
  heads/record.cpp
  heads/replica.cpp
  heads/sealed_file.cpp
  heads/tree.cpp
  lease/fence.cpp
  lease/keeper.cpp
  lease/lease.cpp
  lease/lease_table.cpp
  lease/local_records.cpp
  lease/membership.cpp
  ledger/availability.cpp
  ledger/claim_walk.cpp
  ledger/collector.cpp
  ledger/held_ledger.cpp
  ledger/node_horizon_builder.cpp
  ledger/object_trie.cpp
  ledger/reference_counts.cpp
  ledger/retention.cpp
  ledger/retention_ledger.cpp
  ledger/sealed_journal.cpp
  ledger/trie_diff.cpp
  ledger/trie_union.cpp
  ledger/unreferenced_since.cpp
  migrate/compare.cpp
  migrate/legacy_v18.cpp
  migrate/manifest.cpp
  migrate/manifest_format.cpp
  migrate/migrate.cpp
  migrate/retag.cpp
  store/availability.cpp
  store/control_objects.cpp
  store/control_store.cpp
  store/cpu_pacing.cpp
  store/data_work.cpp
  store/distributed_store.cpp
  store/durability_domain.cpp
  store/durability_ticket.cpp
  store/io_pressure.cpp
  store/local_format.cpp
  store/local_store.cpp
  store/local_store_files.cpp
  store/messages.cpp
  store/object_id.cpp
  store/persistent_cache.cpp
  store/placement.cpp
  store/presence_index.cpp
  store/remove.cpp
  store/repair.cpp
  store/replica_selector.cpp
  store/sealing.cpp
  store/state_layout.cpp
  store/storage_pool.cpp
  store/storage_server.cpp
  store/write_behind.cpp
)
add_library(dagda::dagda ALIAS dagda)
target_include_directories(dagda
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
)
target_link_libraries(dagda
  PUBLIC dannan::dannan
  PRIVATE dagda_warnings
)
set_target_properties(dagda PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
