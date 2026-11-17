#!/bin/bash
# collect_state.sh — collect the current light_ai state into a single file for a new chat.
# Usage: bash scripts/collect_state.sh && cat build/light_ai_state.txt
# The output stays inside the project (build/, git-ignored; #48), not in /tmp.
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR" || { echo "project directory not found"; exit 1; }
mkdir -p build
OUT=build/light_ai_state.txt

{
echo "==== date / uptime ===="
date
uptime

echo; echo "==== git: status, branch, ahead/behind ===="
git status
git log --oneline -8
echo -n "branch: "; git rev-parse --abbrev-ref HEAD

echo; echo "==== git: uncommitted changes (should be empty) ===="
git status --short -- .

echo; echo "==== docs/roadmap.md: roadmap checkboxes ===="
grep -n '^- \[' docs/roadmap.md

echo; echo "==== README: CLI options table ===="
grep -n '^| `--' README.md 2>/dev/null | head -30

echo; echo "==== README: numeric lines (timings, FPS) ===="
grep -nE "[0-9]+\.[0-9]+ ?ms|FPS" README.md | head -40

echo; echo "==== project files ===="
ls -la
ls -la include src scripts 2>/dev/null

echo; echo "==== build and models ===="
ls -la build/light_ai 2>/dev/null || echo "build/light_ai: NOT BUILT"
ls -la misc/*.rknn 2>/dev/null || echo "no .rknn in misc/"

echo; echo "==== leftovers: .bak and logs ===="
ls *.bak include/*.bak src/*.bak 2>/dev/null || echo ".bak: none"
ls -la /tmp/*.log /tmp/cal* 2>/dev/null || echo "/tmp logs: none"
ls dataset/ 2>/dev/null | head -5 || true

echo; echo "==== SDK ===="
ls /mnt/nvme_data/work/rknn_model_zoo/3rdparty/rknpu2/include/ 2>/dev/null || echo "SDK path unavailable"
ls /usr/local/lib/librknn* 2>/dev/null

echo; echo "==== camera / NPU ===="
ls /dev/video* 2>/dev/null | head
cat /sys/kernel/debug/rknpu/version 2>/dev/null
cat /sys/kernel/debug/rknpu/load 2>/dev/null

echo; echo "==== system ===="
uname -a
grep PRETTY_NAME /etc/os-release 2>/dev/null
} > "$OUT"

echo "State collected: $OUT"
echo "Attach this file to the new chat along with the handoff summary."
