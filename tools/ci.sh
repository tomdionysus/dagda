#!/bin/sh
#
# Dagda - The Distributed Object Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# One step of GitHub CI, on Debian 13 as the nodes have it. Dannan comes from its
# public repository at the tag Dagda pins (DANNAN_TAG), built and installed into
# a prefix of its own, library only, under the same compiler and sanitizer as the
# step. Macha's fixtures are not on GitHub, so the cases that read them skip.
#
#   tools/ci.sh check     the header block on every source file, and clang-format
#   tools/ci.sh release   GCC Release, warnings as errors, the whole suite
#   tools/ci.sh asan      GCC with ASan and UBSan, the whole suite
#   tools/ci.sh tsan      clang with TSan, Dannan under TSan too, the whole suite
#
# The gates of record still run on the Pi cluster (CLAUDE.md, "Tests"); this is
# the public build's check.
set -eu

STEP=${1:?usage: tools/ci.sh check|release|asan|tsan}
DANNAN_TAG=${DANNAN_TAG:-0.9.2}
JOBS=${JOBS:-$(nproc)}
ROOT=$(pwd)
WORK=$ROOT/build-ci

dannan() { # build type, then extra cmake arguments
  type=$1
  shift
  rm -rf "$WORK/dannan-src" "$WORK/dannan-build"
  git clone --quiet --depth 1 --branch "$DANNAN_TAG" https://github.com/tomdionysus/dannan.git "$WORK/dannan-src"
  cmake -S "$WORK/dannan-src" -B "$WORK/dannan-build" -G Ninja -DCMAKE_BUILD_TYPE="$type" \
    -DDANNAN_BUILD_TESTING=OFF -DDANNAN_BUILD_TOOLS=OFF -DCMAKE_INSTALL_PREFIX="$WORK/dannan" "$@"
  cmake --build "$WORK/dannan-build" -j "$JOBS"
  cmake --install "$WORK/dannan-build"
}

dagda() { # build type, then extra cmake arguments
  type=$1
  shift
  cmake -S "$ROOT" -B "$WORK/dagda" -G Ninja -DCMAKE_BUILD_TYPE="$type" -DDAGDA_BUILD_TESTING=ON \
    -DDAGDA_WARNINGS_AS_ERRORS=ON -DCMAKE_PREFIX_PATH="$WORK/dannan" "$@"
  cmake --build "$WORK/dagda" -j "$JOBS"
}

suite() {
  (cd "$WORK/dagda" && "$@" ./dagda_tests)
}

mkdir -p "$WORK"
case "$STEP" in
  check)
    status=0
    for file in $(git ls-files '*.h' '*.cpp' '*.h.in'); do
      if ! head -n 7 "$file" | grep -q 'Dagda - The Distributed Object Store'; then
        echo "no header block: $file"
        status=1
      fi
    done
    git ls-files '*.h' '*.cpp' | xargs clang-format --dry-run --Werror || status=1
    exit $status
    ;;
  release)
    dannan Release
    dagda Release
    suite
    ;;
  asan)
    flags="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g"
    dannan RelWithDebInfo
    dagda RelWithDebInfo "-DCMAKE_CXX_FLAGS=$flags" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" \
      "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined"
    suite env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
    ;;
  tsan)
    flags="-fsanitize=thread -fno-omit-frame-pointer -g"
    clang="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
    # shellcheck disable=SC2086
    dannan RelWithDebInfo $clang "-DCMAKE_CXX_FLAGS=$flags" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread" \
      "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread"
    # shellcheck disable=SC2086
    dagda RelWithDebInfo $clang "-DCMAKE_CXX_FLAGS=$flags" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread" \
      "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread"
    # The two cases that hold more locks than the deadlock detector tracks run
    # alone with it off, as the release gates do.
    two="DistributedStore.AControlGraphLargerThanTheConnectionBudgetStillPublishes:StoreDistributedStore.RetainDataBatchesALargePublicationWithinBoundedTime"
    suite env TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 sh -c "./dagda_tests --gtest_filter=-$two"
    suite env TSAN_OPTIONS=halt_on_error=1:detect_deadlocks=0 sh -c "./dagda_tests --gtest_filter=$two"
    ;;
  *)
    echo "unknown step: $STEP" >&2
    exit 2
    ;;
esac
