#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Путь к ESP‑IDF (ожидаем симлинк или реальную папку в externals)
IDF_PATH="$SCRIPT_DIR/externals/esp-idf"

if [ ! -d "$IDF_PATH" ]; then
    echo "❌ Ошибка: ESP‑IDF не найден по пути $IDF_PATH" >&2
    exit 1
fi

export IDF_PATH

 # set environment
. "$IDF_PATH/export.sh"

cd "$SCRIPT_DIR/esp32"

BUILD_DIR="$SCRIPT_DIR/esp32/build"
if [ -d "$BUILD_DIR" ]; then
    (cd "$BUILD_DIR"; ninja clean 2>/dev/null || true)
fi

#idf.py set-target esp32
idf.py build

echo ""
echo "✅ Сборка ESP32 завершена"
echo "📦 Bootloader: $SCRIPT_DIR/esp32/build/bootloader/bootloader.bin"
echo "📦 App image:   $SCRIPT_DIR/esp32/build/*.bin"

