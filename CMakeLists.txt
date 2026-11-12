#
# Dagda - The Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

# The tag, this number and --version are the same.
project(dagda VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Executables at the top of the build tree: ./build/dagda_tests.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(DAGDA_BUILD_TESTING "Build dagda_tests (GoogleTest)" OFF)
option(DAGDA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DAGDA_BUILD_FUZZING "Build each fuzz target as its own binary (libFuzzer where the compiler has it)" OFF)

# The network layer, pinned by minor version (0.x: a minor is a new API), at least the patch that fixed what Dagda relies on.
find_package(dannan 0.5.3 CONFIG REQUIRED)
include(GNUInstallDirs)

add_library(dagda_warnings INTERFACE)
target_compile_options(dagda_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # The lock annotations are checked, not decorative.
  target_compile_options(dagda_warnings INTERFACE -Wthread-safety)
endif()
if(DAGDA_WARNINGS_AS_ERRORS)
  target_compile_options(dagda_warnings INTERFACE -Werror)
endif()

configure_file(dagda/include/dagda/version.h.in ${PROJECT_BINARY_DIR}/generated/dagda/version.h @ONLY)

add_subdirectory(dagda)
add_subdirectory(tools)

if(DAGDA_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(dagda/test)
endif()
