#
# Dagda - The Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# A consumer of the installed package, as the Macha port is: it must find
# dagda, and through it Dannan and its dependencies, from the prefix alone.
cmake_minimum_required(VERSION 3.24)
project(dagda_package_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(dagda ${DAGDA_EXPECTED_VERSION} CONFIG REQUIRED)
add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE dagda::dagda)

# The installed cluster fixture, as Lugh's and Lir's tests use it: run
# fixture_consumer; it exits 0 when its one case passes.
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
add_executable(fixture_consumer fixture_consumer.cpp)
target_link_libraries(fixture_consumer PRIVATE dagda::test_fixtures GTest::gtest_main Threads::Threads)
target_compile_definitions(fixture_consumer PRIVATE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp")
