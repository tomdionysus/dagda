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
  domain_storage_key_test.cpp
  ledger_held_ledger_test.cpp
  ledger_object_trie_test.cpp
  ledger_sealed_journal_test.cpp
  ledger_trie_diff_test.cpp
  ledger_trie_union_test.cpp
  store_durability_domain_test.cpp
  store_durability_ticket_test.cpp
  store_local_format_test.cpp
  store_local_store_durability_test.cpp
  store_local_store_ledger_test.cpp
  store_local_store_presence_test.cpp
  store_local_store_test.cpp
  store_object_id_test.cpp
  store_presence_index_test.cpp
  store_sealing_test.cpp
  store_write_behind_test.cpp
)
target_link_libraries(dagda_tests PRIVATE dagda dagda_test_fixtures GTest::gtest_main dagda_warnings)
