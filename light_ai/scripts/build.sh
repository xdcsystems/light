#!/bin/bash
set -e
cd "$(dirname "$0")/.."

echo "==> Проверка pkg-config модулей:"
pkg-config --modversion rockchip_mpp 2>/dev/null || echo "  rockchip_mpp НЕ найден"
pkg-config --modversion librga     2>/dev/null || echo "  librga НЕ найден"

echo "==> Поиск rknn_api.h и librknnrt.so:"
find /usr /opt -name rknn_api.h    2>/dev/null | head -3 || true
find /usr /opt -name librknnrt.so  2>/dev/null | head -3 || true

echo "==> CMake + сборка:"
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
