#
# Dagda - The Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# Dannan's shared fixtures are installed with it, header-only, included as
# "dannan/test/fixtures/x.h". Dagda's own, when it has any, live in
# fixtures/ here.
add_library(dagda_test_fixtures INTERFACE)
target_link_libraries(dagda_test_fixtures INTERFACE dannan::dannan GTest::gtest Threads::Threads)
# Tests write only inside the build tree.
target_compile_definitions(dagda_test_fixtures INTERFACE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp")

add_executable(dagda_tests
  version_test.cpp
  context_test.cpp
  contract_gates_test.cpp
  contract_include_direction_test.cpp
  domain_cluster_keys_test.cpp
  domain_domain_id_test.cpp
  domain_storage_key_test.cpp
  domain_wrap_test.cpp
  heads_census_test.cpp
  heads_control_node_store_test.cpp
  heads_cost_test.cpp
  heads_record_test.cpp
  heads_replica_bytes_test.cpp
  heads_replica_test.cpp
  heads_retirement_test.cpp
  heads_node_test.cpp
  heads_space_test.cpp
  heads_seed_test.cpp
  heads_tree_test.cpp
  heads_tree_vectors_test.cpp
  lease_claimed_collection_test.cpp
  lease_fence_test.cpp
  lease_keeper_test.cpp
  lease_lease_test.cpp
  lease_membership_test.cpp
  ledger_availability_test.cpp
  ledger_claim_walk_test.cpp
  ledger_collector_test.cpp
  ledger_held_ledger_test.cpp
  ledger_horizon_builder_test.cpp
  ledger_object_trie_test.cpp
  ledger_reference_counts_test.cpp
  ledger_retention_test.cpp
  ledger_sealed_journal_test.cpp
  ledger_trie_diff_test.cpp
  ledger_trie_union_test.cpp
  ledger_unreferenced_since_test.cpp
  migrate_compare_test.cpp
  migrate_crash_test.cpp
  migrate_legacy_v18_test.cpp
  migrate_migrate_test.cpp
  migrate_retag_test.cpp
  node_local_maintenance_test.cpp
  maintenance_steps_test.cpp
  node_maintenance_test.cpp
  node_seams_test.cpp
  node_test.cpp
  store_control_fetch_test.cpp
  store_control_store_test.cpp
  store_data_work_test.cpp
  store_distributed_store_test.cpp
  store_durability_barrier_test.cpp
  store_durability_domain_test.cpp
  store_durability_ticket_test.cpp
  store_io_pressure_test.cpp
  store_layout_v19_test.cpp
  store_local_format_test.cpp
  store_local_store_durability_test.cpp
  store_local_store_ledger_test.cpp
  store_local_store_presence_test.cpp
  scale_test.cpp
  store_local_store_test.cpp
  store_placement_vectors_test.cpp
  store_messages_test.cpp
  store_object_id_test.cpp
  store_persistent_cache_test.cpp
  store_presence_index_test.cpp
  store_remove_test.cpp
  store_repair_test.cpp
  store_replica_selector_test.cpp
  store_scope_test.cpp
  store_sealing_test.cpp
  store_state_layout_test.cpp
  store_storage_pool_test.cpp
  store_write_behind_test.cpp
)
target_link_libraries(dagda_tests PRIVATE dagda dagda_test_fixtures GTest::gtest_main dagda_warnings)
# The count of calls for stable storage interposes on libdagda's: dyld takes an
# interposing table only from a library, ELF from the executable first.
if(APPLE)
  add_library(dagda_test_sync_count SHARED fixtures/sync_count.cpp)
  target_include_directories(dagda_test_sync_count PRIVATE ${PROJECT_SOURCE_DIR})
  target_link_libraries(dagda_tests PRIVATE dagda_test_sync_count)
else()
  target_sources(dagda_tests PRIVATE fixtures/sync_count.cpp)
endif()
# The include-direction case reads the sources it checks; the migration's
# runs the tool, which is built beside the tests.
target_compile_definitions(dagda_tests PRIVATE DAGDA_SOURCE_DIR="${PROJECT_SOURCE_DIR}" DAGDA_BINARY_DIR="${PROJECT_BINARY_DIR}")
add_dependencies(dagda_tests dagda-migrate)
# Dannan's ChildProcess fixture opens a pseudo-terminal through libutil on
# Linux.
if(NOT APPLE)
  target_link_libraries(dagda_tests PRIVATE util)
endif()
