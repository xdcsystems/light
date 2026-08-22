#!/usr/bin/env bash
set -euo pipefail

START_DIR="$(pwd)"
BUILD_DIR="build_test"

if [ -d "$BUILD_DIR" ]; then
    echo "🧹 Очищаем папку сборки: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "🔧 Сборка под Linux (PLATFORM_TEST)..."
cmake -G Ninja -S .. -B . -DPLATFORM_TEST=ON
ninja

cd "$START_DIR"

BIN_PATH="$(realpath "./$BUILD_DIR/light_sensor")"

echo ""
echo "✅ Сборка завершена!"
echo "📦 Бинарник: $BIN_PATH"
