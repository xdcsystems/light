#!/bin/bash
set -euo pipefail

WHEEL_NAME="rknn_toolkit_lite2-2.5.0-cp39-cp39-manylinux_2_17_aarch64.manylinux2014_aarch64.whl"
WHEEL_URL="https://github.com/xdcsystems/light/releases/download/rknn-toolkit-lite2-v2.5.0/${WHEEL_NAME}"
WHEEL_SHA256="c834f41dc48456ef5eb0107355abe9abb167d7b47f7375d581f2975767451068"

cd "$(dirname "$0")/../misc"

if [ ! -f "$WHEEL_NAME" ]; then
    echo "Downloading wheel from releases..."
    curl -fL -o "$WHEEL_NAME" "$WHEEL_URL"
fi

echo "$WHEEL_SHA256  $WHEEL_NAME" | sha256sum -c -
pip install --no-index --find-links . rknn_toolkit_lite2==2.5.0

