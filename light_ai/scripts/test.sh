#!/bin/bash
# test.sh: build and run the host-side unit tests (tests/, target light_ai_tests).
# Needs only cmake + a C++17 compiler: no MPP/RGA/RKNN/ffmpeg/EGL,
# so it runs on the board and on any PC.
# Usage: bash scripts/test.sh
# Build dir: build/tests (separate CMake project, the main build/ cache is untouched).
set -e

PROJ_ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd -P)"
TEST_BUILD_DIR="$PROJ_ROOT/build/tests"

cmake -S "$PROJ_ROOT/tests" -B "$TEST_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$TEST_BUILD_DIR" -j"$(nproc)"

# The config tests write a temporary INI into the working directory.
cd "$TEST_BUILD_DIR"
./light_ai_tests
