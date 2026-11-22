#!/bin/bash
set -euo pipefail

# Phase 4 (#126): cyclictest jitter gating & RT telemetry acceptance check under load.
PROJ_ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd -P)"
cd "$PROJ_ROOT"

LOOPS="${CYCLICTEST_LOOPS:-15000}"
MAX_JITTER_US="${MAX_JITTER_US:-150}"
PORT="${BENCH_PORT:-1234}"
DEVICE="${BENCH_DEVICE:-/dev/video20}"
LOG_AI="build/bench_light_ai.log"
LOG_CYC="build/bench_cyclictest.log"

if ! command -v cyclictest >/dev/null 2>&1; then
    echo "run_bench.sh: cyclictest not installed" >&2
    exit 1
fi

echo "run_bench.sh: starting light_ai load on $DEVICE (port $PORT)..."
( sleep 2; exec ffmpeg -hide_banner -loglevel error -i "tcp://127.0.0.1:$PORT" -f null - ) >/dev/null 2>&1 &
FF_PID=$!

./scripts/run.sh --device "$DEVICE" --net --net-h264 --net-port "$PORT" > "$LOG_AI" 2>&1 &
AI_PID=$!

cleanup() {
    sudo -n pkill -INT -x light_ai 2>/dev/null || true
    kill "$AI_PID" "$FF_PID" 2>/dev/null || true
    wait "$AI_PID" "$FF_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for warmup (first [perf] rt window)
sleep 7
if ! pgrep -x light_ai >/dev/null 2>&1; then
    echo "FAIL: light_ai not running" >&2
    exit 1
fi

echo "run_bench.sh: running cyclictest -t4 -a 4-7 -p 80 -m -i 1000 -l $LOOPS..."
sudo -n cyclictest -t4 -a 4-7 -p 80 -m -i 1000 -l "$LOOPS" -q | tee "$LOG_CYC"

# Stop load cleanly (SIGINT -> Drain & summary)
sudo -n pkill -INT -x light_ai 2>/dev/null || true
wait "$AI_PID" 2>/dev/null || true
kill "$FF_PID" 2>/dev/null || true
wait "$FF_PID" 2>/dev/null || true
trap - EXIT

echo "=== cyclictest max jitter check (limit: ${MAX_JITTER_US} us) ==="
lines=$(grep -c '^T:' "$LOG_CYC" || true)
if [ "$lines" -eq 0 ]; then echo "FAIL: no cyclictest output" >&2; exit 1; fi
max_obs=$(awk '/^T:/ { for (i=1; i<=NF; i++) if ($i=="Max:") { v=$(i+1)+0; if (v>m) m=v } } END { print m+0 }' "$LOG_CYC")
echo "cyclictest_max_us=$max_obs"
if [ "$max_obs" -ge "$MAX_JITTER_US" ]; then
    echo "FAIL: max jitter $max_obs us >= $MAX_JITTER_US us" >&2
    exit 1
fi

echo "=== RT telemetry check (post-warmup windows) ==="
grep -E "\[perf\] rt|\[perf\] enc|\[perf\] mux" "$LOG_AI" | tail -n 12
echo "PASS: cyclictest max=$max_obs us (< $MAX_JITTER_US us)"
