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
)
target_link_libraries(dagda_tests PRIVATE dagda dagda_test_fixtures GTest::gtest_main dagda_warnings)
