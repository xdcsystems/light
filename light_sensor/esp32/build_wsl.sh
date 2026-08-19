#!/usr/bin/env bash
# Сборка прошивки ESP32 из WSL. IDF и toolchain лежат на диске Linux, не на C:.
set -euo pipefail

export MAMBA_ROOT_PREFIX="${MAMBA_ROOT_PREFIX:-$HOME/micromamba}"
eval "$("$HOME/bin/micromamba" shell hook -s bash)"
micromamba activate espidf

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh"

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD_DIR:-$HOME/esp/light_sensor_build}"
DEST="${ROOT}/firmware"

idf.py -C "$ROOT" -B "$BUILD" "$@"
if [ $# -eq 0 ]; then
    mkdir -p "$DEST"
    cp -f "$BUILD/light_sensor.bin" "$DEST/"
    cp -f "$BUILD/bootloader/bootloader.bin" "$DEST/"
    cp -f "$BUILD/partition_table/partition-table.bin" "$DEST/"
    cat > "$DEST/flash_args" <<'EOF'
--flash_mode dio --flash_size 4MB --flash_freq 40m
0x1000 bootloader.bin
0x8000 partition-table.bin
0x10000 light_sensor.bin
EOF
    echo "Firmware copied to $DEST"
fi
