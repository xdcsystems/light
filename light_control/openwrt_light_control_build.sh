#!/usr/bin/env bash
set -euo pipefail

# Run from WSL. Host tools live in $HOME/openwrt/host-tools (make/xargs/patch from micromamba).
START_DIR="$(pwd)"

export PATH="${HOME}/openwrt/host-tools/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PYTHONPATH="${HOME}/openwrt/pyshim${PYTHONPATH:+:$PYTHONPATH}"

BASE_DIR="${OPENWRT_SDK:-$HOME/openwrt/sdk}"
LIGHT_CONTROL_SRC="${LIGHT_CONTROL_SRC:-/mnt/c/Dev/light_control/light/light_control}"
IPK_COPY="${IPK_COPY:-/mnt/c/Dev/light_control/light/ipk}"

if [[ ! -d "$BASE_DIR" ]]; then
  echo "Error: OpenWrt SDK not found: $BASE_DIR" >&2
  exit 1
fi

cd "$BASE_DIR"

echo "=== SDK: $BASE_DIR ==="
echo "=== Sources: $LIGHT_CONTROL_SRC ==="

echo "=== Update custom feed index ==="
./scripts/feeds update light
./scripts/feeds install -p light light_control luci-app-light-control

echo "=== Build light_control ==="
make package/light_control/clean
rm -rf build_dir/target-mips_24kc_musl/light_control-*
make package/light_control/compile V=s LIGHT_CONTROL_SRC="$LIGHT_CONTROL_SRC"

echo "=== luci-app: copy files into noarch ipk (skip luci-base host lemon/gcc-16) ==="
SRC="$LIGHT_CONTROL_SRC/openwrt_pkg/luci-app-light-control"
PKGDIR="$HOME/openwrt/tmp-luci-app"
OUT="$BASE_DIR/bin/packages/mips_24kc/light"
rm -rf "$PKGDIR"
mkdir -p "$PKGDIR/CONTROL"
mkdir -p "$PKGDIR/www/luci-static/resources/view/light_control"
mkdir -p "$PKGDIR/usr/share/luci/menu.d"
mkdir -p "$PKGDIR/usr/share/rpcd/acl.d"
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
"$BASE_DIR/staging_dir/host/bin/bash" "$BASE_DIR/scripts/ipkg-build" -m "" "$PKGDIR" "$OUT"

cd "$START_DIR"

mkdir -p "$IPK_COPY"
cp -v "$OUT"/light_control_*.ipk "$OUT"/luci-app-light-control_*.ipk "$IPK_COPY/"

echo "=== Packages ==="
ls -lh "$IPK_COPY"
