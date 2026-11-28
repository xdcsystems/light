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
# The build tree, the test builds, the tidy logs and cache all live under build/: warn when it is on the eMMC
# shellcheck source=storage_guard.sh
. "$PROJ_ROOT/scripts/storage_guard.sh"
storage_guard build "$BUILD_DIR"

# BUILD_ON_A76=1 (only with light_ai stopped): every compile and clang-tidy run takes one of the A76 cores 4-7
# (scripts/a76_slot.sh), 4 jobs at most (3.8 GB RAM). Exported: test.sh and tidy_run.sh follow it.
export BUILD_ON_A76="${BUILD_ON_A76:-0}"
if [ "$BUILD_ON_A76" = "1" ]; then
    "$PROJ_ROOT/scripts/a76_slot.sh" --check || exit 1
    LAUNCH="$PROJ_ROOT/scripts/a76_slot.sh"
    JOBS=4
else
    LAUNCH=""   # passed to cmake anyway: it clears the value cached by an earlier BUILD_ON_A76=1 run
    JOBS="$(nproc)"
fi
echo "BUILD_ON_A76=$BUILD_ON_A76 (-j$JOBS)"

# Pin the libav* checks to the self-built ffmpeg (same pin as CMakeLists):
# PKG_CONFIG_PATH prepends -- distro .pc files stay visible for
# MPP/RGA/EGL, self-built libav* win by search order. PKG_CONFIG_LIBDIR
# (LIBDIR-replace) would hide rockchip_mpp.pc from this loop.
if [ -f /usr/local/lib/pkgconfig/libavformat.pc ]; then
    export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi

echo "==> Prebuild checks:"
[ -n "$RKNN_ROOT" ] && echo "  RKNN_ROOT=$RKNN_ROOT (explicit override)"
for m in rockchip_mpp librga libavformat libavcodec libavutil egl glesv2; do
    v=$(pkg-config --modversion $m 2>/dev/null)
    p=$(pkg-config --variable=pcfiledir $m 2>/dev/null)
    if [ -n "$v" ]; then echo "  $m: $v (${p:-?})"; else echo "  $m: NOT FOUND"; fi
done

echo "==> CMake + build:"
# BUILD_TYPE=RelWithDebInfo scripts/build.sh  -- for gdb sessions
BUILD_TYPE="${BUILD_TYPE:-Release}"
cmake -S "$PROJ_ROOT" -B "$BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_RULE_LAUNCH_COMPILE="$LAUNCH"
cmake --build "$BUILD_DIR" -j"$JOBS"

if [ "${SKIP_TESTS:-0}" != "1" ]; then
    echo "==> unit tests gate (SKIP_TESTS=1 to skip):"
    bash "$PROJ_ROOT/scripts/test.sh" || {
        echo "unit tests FAILED -- build rejected"
        exit 1
    }
fi

if [ "${SKIP_TIDY:-0}" != "1" ]; then
    echo "==> clang-tidy gate (SKIP_TIDY=1 to skip):"
    # always every TU: a TIDY_FILES left in the environment (a subset run, #93) must not narrow the gate
    env -u TIDY_FILES bash "$PROJ_ROOT/scripts/tidy_run.sh" 600 || {
        echo "tidy gate FAILED -- build rejected"
        exit 1
    }
fi
