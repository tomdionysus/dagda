#
# Dagda - The Distributed Object Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# Fuzz targets, one per decoder of bytes from a peer or a disk, registered with
# Dannan's harness (dannan::testing). Every target is compiled into the test
# binary's smoke test, which runs on each gate; with DAGDA_BUILD_FUZZING, each
# file also builds as its own binary: a libFuzzer fuzzer where the compiler has
# libFuzzer, otherwise a replay driver over files.
set(DAGDA_FUZZ_TARGETS domain heads lease ledger migrate store)

add_library(dagda_fuzz_targets OBJECT)
foreach(target ${DAGDA_FUZZ_TARGETS})
  target_sources(dagda_fuzz_targets PRIVATE ${target}_fuzz.cpp)
endforeach()
target_link_libraries(dagda_fuzz_targets PUBLIC dagda dannan::testing PRIVATE dagda_warnings)

if(DAGDA_BUILD_FUZZING)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  check_cxx_source_compiles("#include <cstddef>
#include <cstdint>
extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }" DAGDA_HAVE_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  foreach(target ${DAGDA_FUZZ_TARGETS})
    add_executable(dagda_fuzz_${target} ${target}_fuzz.cpp driver.cpp)
    target_link_libraries(dagda_fuzz_${target} PRIVATE dagda dannan::testing dagda_warnings)
    if(DAGDA_HAVE_LIBFUZZER)
      target_compile_definitions(dagda_fuzz_${target} PRIVATE DAGDA_LIBFUZZER)
      target_compile_options(dagda_fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
      target_link_options(dagda_fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
  endforeach()
endif()
