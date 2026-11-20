#
# Dagda - The Distributed Object Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

# The tag, this number and --version are the same.
project(dagda VERSION 0.4.0 LANGUAGES CXX)

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

# The network layer by minor, at least the patch Dagda was built and tested on: from 0.6.0 Dannan keeps its ABI across a
# minor's patches (SONAME libdannan.so.0.10 from 0.10.0, SameMinorVersion), and only a new minor changes it: a request
# for 0.9.2 would never find 0.10.0, nor the reverse. 0.10.0: the stack's first minor (CHANGELOG, "Adopting 0.10.0").
# One number for the build and the installed package's config, which a consumer meets (test/package).
set(DAGDA_DANNAN_MINIMUM 0.10.0)
find_package(dannan ${DAGDA_DANNAN_MINIMUM} CONFIG REQUIRED)

# Dannan's one sanitizer switch, honoured by name for Dagda's own targets (Dannan's docs/building.md): `address` (ASan
# with UBSan) or `thread` (TSan). An installed Dannan never carries the flags, so the Dannan this links must itself be
# built with the same DANNAN_SANITIZE; tools/ci.sh does both and checks each library with nm.
set(DANNAN_SANITIZE "" CACHE STRING "Sanitizer for Dagda's targets: empty, address or thread")
if(DANNAN_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
elseif(DANNAN_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT DANNAN_SANITIZE STREQUAL "")
  message(FATAL_ERROR "DANNAN_SANITIZE is address, thread or empty, not '${DANNAN_SANITIZE}'")
endif()
# Where an install puts things; before any target names its install paths.
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

# Fuzz targets on Dannan's harness (dannan::testing): compiled into dagda_tests's smoke test, and with
# DAGDA_BUILD_FUZZING each subsystem's as its own binary.
if(DAGDA_BUILD_TESTING OR DAGDA_BUILD_FUZZING)
  add_subdirectory(dagda/fuzz)
endif()

if(DAGDA_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(dagda/test)
endif()
