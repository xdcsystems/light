#!/bin/bash
set -e

# Canonical physical path to the project root — symlinks resolved
PROJ_ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd -P)"
BUILD_DIR="$PROJ_ROOT/build"

# Pin the working directory to its physical path: CMake inherits the
# path spelling from cwd, and without this SSH and VS Code would see
# different paths in "Build files have been written to"
cd "$PROJ_ROOT"

echo "PROJ_ROOT=$PROJ_ROOT"
echo "BUILD_DIR=$BUILD_DIR"

echo "==> Prebuild checks:"
[ -n "$RKNN_ROOT" ] && echo "  RKNN_ROOT=$RKNN_ROOT (explicit override)"
for m in rockchip_mpp librga libavformat libavcodec libavutil egl glesv2; do
    v=$(pkg-config --modversion $m 2>/dev/null)
    if [ -n "$v" ]; then echo "  $m: $v"; else echo "  $m: NOT FOUND"; fi
done

echo "==> CMake + build:"
# BUILD_TYPE=RelWithDebInfo scripts/build.sh  -- for gdb sessions
BUILD_TYPE="${BUILD_TYPE:-Release}"
cmake -S "$PROJ_ROOT" -B "$BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j"$(nproc)"

if [ "${SKIP_TIDY:-0}" != "1" ]; then
    echo "==> clang-tidy gate (SKIP_TIDY=1 to skip):"
    bash "$PROJ_ROOT/scripts/tidy_run.sh" 600 || {
        echo "tidy gate FAILED -- build rejected"
        exit 1
    }
fi
