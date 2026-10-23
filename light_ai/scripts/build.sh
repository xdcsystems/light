#!/bin/bash
set -e
cd "$(dirname "$0")/.."

echo "==> Checking pkg-config modules:"
pkg-config --modversion rockchip_mpp 2>/dev/null || echo "  rockchip_mpp NOT found"
pkg-config --modversion librga     2>/dev/null || echo "  librga NOT found"

# Targeted checks instead of a full find over /usr and /opt (saves seconds).
echo "==> Locating rknn_api.h and librknnrt.so:"
RKNN_INC_DEFAULT="/mnt/nvme_data/work/rknn_model_zoo/3rdparty/rknpu2/include"
for d in "$RKNN_INC_DEFAULT" /usr/include /usr/local/include; do
    [ -f "$d/rknn_api.h" ] && echo "  header: $d/rknn_api.h"
done
[ -f /usr/lib/librknnrt.so ] && echo "  lib: /usr/lib/librknnrt.so"

echo "==> CMake + build:"
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"
