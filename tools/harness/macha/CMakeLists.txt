#
# Dagda - The Distributed Object Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The side-by-side harness's Macha half (TODO/2026-11-15-side-by-side-harness.md): Macha's
# store at f0597a4 from Dagda's read-only extract, its macha_core library alone (no
# server, tests, books or FUSE: it links no FUSE, libtorrent or FFmpeg), on Dannan
# 0.5.3, and harness-macha linking it. Built on a Pi, out of every tree:
#
#   cmake -S tools/harness/macha -B <build> -DMACHA_SOURCE=<extract> \
#     -DCMAKE_PREFIX_PATH=<dannan 0.5.3 prefix> -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build <build> --target harness-macha -j2

cmake_minimum_required(VERSION 3.24)
project(harness_macha LANGUAGES CXX)

set(MACHA_SOURCE "" CACHE PATH "Macha's source at f0597a4 (Dagda's read-only extract)")
if(NOT EXISTS "${MACHA_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "MACHA_SOURCE must name the extract of Macha at f0597a4")
endif()

# Macha's core alone, as its own build makes it.
set(MACHA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(MACHA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MACHA_ENABLE_BOOKS OFF CACHE BOOL "" FORCE)
set(MACHA_ENABLE_FUSE OFF CACHE BOOL "" FORCE)
set(MACHA_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(${MACHA_SOURCE} macha EXCLUDE_FROM_ALL)

add_executable(harness-macha ${CMAKE_CURRENT_SOURCE_DIR}/../harness_macha.cpp)
target_include_directories(harness-macha PRIVATE ${MACHA_SOURCE}/src ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(harness-macha PRIVATE macha_core)
set_target_properties(harness-macha PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
