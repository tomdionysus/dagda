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
