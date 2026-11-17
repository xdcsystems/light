#!/bin/bash
# test.sh: build and run the host-side unit tests (tests/, target light_ai_tests).
# Needs only cmake + a C++17 compiler: no MPP/RGA/RKNN/ffmpeg/EGL,
# so it runs on the board and on any PC.
# Usage: bash scripts/test.sh   (SKIP_TSAN=1: skip the ThreadSanitizer run of the threaded suites)
# Build dir: build/tests (separate CMake project, the main build/ cache is untouched).
set -e

PROJ_ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd -P)"
TEST_BUILD_DIR="$PROJ_ROOT/build/tests"

cmake -S "$PROJ_ROOT/tests" -B "$TEST_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$TEST_BUILD_DIR" --target light_ai_tests -j"$(nproc)"

# The config tests write a temporary INI into the working directory.
cd "$TEST_BUILD_DIR"
./light_ai_tests

# Threaded suites under ThreadSanitizer: a data race exits non-zero (set -e fails the gate).
if [ "${SKIP_TSAN:-0}" != "1" ]; then
    tsan_t0=$(date +%s)
    cmake --build "$TEST_BUILD_DIR" --target light_ai_tests_tsan -j"$(nproc)"
    ./light_ai_tests_tsan
    echo "tsan: build + run $(( $(date +%s) - tsan_t0 )) s"
else
    echo "tsan: skipped (SKIP_TSAN=1)"
fi
