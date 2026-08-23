#!/usr/bin/env bash
set -euo pipefail

# Запоминаем путь, откуда запустили скрипт
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
START_DIR="$(pwd)"

BASE_DIR="${OPENWRT_SDK:-$HOME/Projects/light/light_control/externals/openwrt}"
LIGHT_CONTROL_SRC="${LIGHT_CONTROL_SRC:-$SCRIPT_DIR}"

if [[ ! -d "$BASE_DIR" ]]; then
  echo "Ошибка: SDK не найден: $BASE_DIR" >&2
  echo "Задай OPENWRT_SDK, если SDK лежит не в \$HOME/Projects/light/light_control/externals/openwrt" >&2
  exit 1
fi

cd "$BASE_DIR"

echo "=== Шаг 1: Обновление индекса пакетов (чтобы OpenWrt увидел light_control) ==="
make package/index

echo "=== Шаг 2: Сборка пакета с подробным логом ==="
# 1. Очищаем пакет
make package/light_control/clean

# 2. Удаляем папку сборки полностью (чтобы исчез CMakeCache.txt и CMakeFiles)
rm -rf build_dir/target-mips_24kc_musl/light_control-*

# 3. (Надёжный вариант) Удаляем все следы CMake для этого пакета
find . -type d -path "*/light_control*" -name "CMakeFiles" -exec rm -rf {} + 2>/dev/null || true
find . -path "*/light_control*" -name "CMakeCache.txt" -delete 2>/dev/null || true

make package/light_control/compile V=s

echo "=== Шаг 3: luci-app-light-control (noarch ipk) ==="
# JS/menu/ACL — luci.mk тянет host-инструменты luci-base, этому приложению они не нужны.
SRC="$LIGHT_CONTROL_SRC/openwrt_pkg/luci-app-light-control"
PKGDIR="$BASE_DIR/tmp/luci-app-light-control-ipk"
OUT="$BASE_DIR/bin/packages/mips_24kc/light_control"
IPKG_BUILD="$BASE_DIR/scripts/ipkg-build"

if [[ ! -d "$SRC" ]]; then
  echo "Ошибка: исходники LuCI не найдены: $SRC" >&2
  exit 1
fi
if [[ ! -f "$IPKG_BUILD" ]]; then
  echo "Ошибка: нет $IPKG_BUILD" >&2
  exit 1
fi

rm -rf "$PKGDIR"
mkdir -p "$PKGDIR/CONTROL"
mkdir -p "$PKGDIR/www/luci-static/resources/view/light_control"
mkdir -p "$PKGDIR/usr/share/luci/menu.d"
mkdir -p "$PKGDIR/usr/share/rpcd/acl.d"
mkdir -p "$OUT"

cp "$SRC/htdocs/luci-static/resources/view/light_control/"*.js "$PKGDIR/www/luci-static/resources/view/light_control/"
cp "$SRC/root/usr/share/luci/menu.d/"*.json "$PKGDIR/usr/share/luci/menu.d/"
cp "$SRC/root/usr/share/rpcd/acl.d/"*.json "$PKGDIR/usr/share/rpcd/acl.d/"

cat > "$PKGDIR/CONTROL/control" << EOF
Package: luci-app-light-control
Version: 1.0.23-1
Depends: luci-base, light_control
License: MIT
Section: luci
Architecture: all
Maintainer: xdcsystems
Description: LuCI support for Light Control
EOF

if [[ -x "$BASE_DIR/staging_dir/host/bin/bash" ]]; then
  "$BASE_DIR/staging_dir/host/bin/bash" "$IPKG_BUILD" -m "" "$PKGDIR" "$OUT"
else
  bash "$IPKG_BUILD" -m "" "$PKGDIR" "$OUT"
fi

# Возвращаемся в исходную директорию
cd "$START_DIR"

echo "=== Готово. Вернулись в: $START_DIR ==="

echo "Сборка завершена."

# --- Путь к готовому .ipk (правильный, без дублирования externals/openwrt) ---
IPK_PATH="${BASE_DIR}/bin/packages/mips_24kc/light_control/light_control_1.0.23-1_mips_24kc.ipk"
LUCI_IPK="${BASE_DIR}/bin/packages/mips_24kc/light_control/luci-app-light-control_1.0.23-1_all.ipk"

if [[ ! -f "$IPK_PATH" ]]; then
  echo "Ошибка: .ipk не найден по пути: $IPK_PATH" >&2
  exit 1
fi
if [[ ! -f "$LUCI_IPK" ]]; then
  echo "Ошибка: .ipk не найден по пути: $LUCI_IPK" >&2
  exit 1
fi

echo "=== Пакет найден: ==="
ls -lh "$IPK_PATH" "$LUCI_IPK"

IPK_COPY="${IPK_COPY:-$SCRIPT_DIR/../ipk}"
if [[ -d "$(dirname "$IPK_COPY")" ]]; then
  mkdir -p "$IPK_COPY"
  cp -v "$IPK_PATH" "$LUCI_IPK" "$IPK_COPY/"
fi
