#
# Dagda - The Distributed Object Store
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
  heads/exchange.cpp
  heads/messages.cpp
  heads/record.cpp
  heads/register_map.cpp
  heads/replica.cpp
  heads/node_pack.cpp
  heads/retirement.cpp
  heads/sealed_file.cpp
  heads/space.cpp
  heads/spaces.cpp
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
  migrate/heads_metadata.cpp
  migrate/legacy_v18.cpp
  migrate/manifest.cpp
  migrate/manifest_format.cpp
  migrate/disk_order.cpp
  migrate/migrate.cpp
  migrate/retag.cpp
  store/availability.cpp
  store/control_objects.cpp
  store/control_store.cpp
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
  store/offer_book.cpp
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
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(dagda
  PUBLIC dannan::dannan
  PRIVATE dagda_warnings
)
# The SONAME is the whole version, as Dannan's: no release promises another's
# ABI yet, so a host built against one refuses to load another.
set_target_properties(dagda PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION} EXPORT_NAME dagda)

# The in-process cluster (Cluster, ClusterNode, stepped time, heads on) and the
# neutral owner, for Lugh's and Lir's multi-node tests, so they keep no fixture of
# their own (the three-systems design, "The fixture"; rule 3). Header-only, as
# Dannan's fixtures are: dagda::test_fixtures brings dagda::dagda, and the consumer
# links its own GoogleTest and names its DANNAN_TEST_TEMP_ROOT. Macha's fixtures and
# the cases' own helpers stay in the tree.
set(DAGDA_INSTALLED_FIXTURES cluster.h heads.h heads_merge.h heads_space.h ledger.h store.h tree.h)
add_library(dagda_test_fixtures_installed INTERFACE)
target_link_libraries(dagda_test_fixtures_installed INTERFACE dagda)
set_target_properties(dagda_test_fixtures_installed PROPERTIES EXPORT_NAME test_fixtures)
add_library(dagda::test_fixtures ALIAS dagda_test_fixtures_installed)

# Installed for Macha, Lugh and Lir: find_package(dagda 0.1.4 CONFIG), the
# exact version, then link dagda::dagda, which brings Dannan with it. Headers
# keep their tree, so "dagda/<subsystem>/x.h" includes the same installed as in
# the tree; the tests are not installed, and of their fixtures only the set above.
include(CMakePackageConfigHelpers)
install(TARGETS dagda dagda_test_fixtures_installed EXPORT dagdaTargets LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dagda FILES_MATCHING PATTERN "*.h"
        PATTERN "include" EXCLUDE PATTERN "test" EXCLUDE)
list(TRANSFORM DAGDA_INSTALLED_FIXTURES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/ OUTPUT_VARIABLE DAGDA_INSTALLED_FIXTURE_PATHS)
install(FILES ${DAGDA_INSTALLED_FIXTURE_PATHS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dagda/test/fixtures)
install(FILES ${PROJECT_BINARY_DIR}/generated/dagda/version.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dagda)
install(EXPORT dagdaTargets NAMESPACE dagda:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dagda)
configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/dagdaConfig.cmake.in ${PROJECT_BINARY_DIR}/dagdaConfig.cmake
                              INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dagda)
# As the SONAME: only the exact version is compatible until a release promises an ABI.
write_basic_package_version_file(${PROJECT_BINARY_DIR}/dagdaConfigVersion.cmake COMPATIBILITY ExactVersion)
install(FILES ${PROJECT_BINARY_DIR}/dagdaConfig.cmake ${PROJECT_BINARY_DIR}/dagdaConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dagda)
