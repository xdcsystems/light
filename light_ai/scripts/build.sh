#!/bin/bash
set -e

# канонический физический путь к проекту — без симлинков
PROJ_ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd -P)"
BUILD_DIR="$PROJ_ROOT/build"

# фиксируем рабочую директорию в физическом написании: CMake наследует
# написание пути от cwd, и без этого SSH и VS Code получали бы разные
# значения в логе «Build files have been written to»
cd "$PROJ_ROOT"

echo "PROJ_ROOT=$PROJ_ROOT"
echo "BUILD_DIR=$BUILD_DIR"

echo "==> Prebuild checks:"
pkg-config --modversion rockchip_mpp 2>/dev/null || true
pkg-config --modversion librga     2>/dev/null || true
pkg-config --modversion libavformat 2>/dev/null || true
if [ -n "$RKNN_ROOT" ]; then
    echo "  RKNN_ROOT=$RKNN_ROOT (explicit override)"
fi

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
