#!/bin/bash
# ref_replay.sh: one layer-A replay of the reference detections (reference/README.md, issue #32).
# Serves CLIP through tools/mjpeg_replay.py on loopback, runs light_ai on it with no viewer
# (recognition only) and --box-log OUT, and proves the run drop-free: every clip frame
# decoded and inferred exactly once, in order (pipeline frame N = clip frame N).
#
# Usage: bash tools/ref_replay.sh CLIP OUT.csv POOL [REF.csv]
#   CLIP   concatenated MJPEG (ffmpeg -c copy -f mjpeg), e.g. misc/ref_evening.mjpeg
#   OUT    the --box-log of this replay; the light_ai log goes to OUT minus .csv plus .log,
#          the server log to .srv.log next to it
#   POOL   --pool (1 or 3)
#   REF    optional: OUT must be cmp-identical to it
# Env: PORT (default 8090), FPS (server pacing, default 10), BIN (default ./build/light_ai),
#      EXTRA (more light_ai options, e.g. "--model misc/x.rknn").
# Fails (exit 1, "FAIL: <why>") unless: light_ai exits 0 (a dropped frame leaves it waiting
# for frame N: the 2 s watchdog, a refused reconnect, then the timer -> 124), the log has no
# source-trouble line (TROUBLE below: a dropped frame, a detach, a stall), the server sent every frame, and OUT holds
# exactly N frame blocks with ids 0..N-1 in order. Runs from the project root; needs sudo -n.
set -u

PROJ_ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd -P)"
cd "$PROJ_ROOT" || exit 1

if [ $# -lt 3 ]; then
    sed -n '2,18p' "$0"
    exit 2
fi
CLIP=$1
OUT=$2
POOL=$3
REF=${4:-}
PORT=${PORT:-8090}
FPS=${FPS:-10}
BIN=${BIN:-./build/light_ai}
EXTRA=${EXTRA:-}
LOG=${OUT%.csv}.log
SRV_LOG=${OUT%.csv}.srv.log

fail() { echo "FAIL: $*"; exit 1; }
# Source trouble in the light_ai log (a clean replay has none of these lines).
TROUBLE='net-src: (corrupt|all buffers)|detached|dequeue failed|no frame in'

N=$(python3 tools/mjpeg_replay.py "$CLIP" --count) || fail "clip check: $N"
rm -f "$OUT" "$LOG" "$SRV_LOG" 2>/dev/null || sudo -n rm -f "$OUT" "$LOG" "$SRV_LOG"

python3 tools/mjpeg_replay.py "$CLIP" --port "$PORT" --fps "$FPS" 2> "$SRV_LOG" &
SRV_PID=$!
for _ in $(seq 50); do
    grep -q '^listening' "$SRV_LOG" 2>/dev/null && break
    sleep 0.1
done
grep -q '^listening' "$SRV_LOG" || { kill "$SRV_PID" 2>/dev/null; fail "server did not start: $(cat "$SRV_LOG")"; }

# The replay lasts hold + N / FPS; 60 s of margin for the startup and the drain.
T=$(( N / ${FPS%.*} + 60 ))
t0=$(date +%s)
# shellcheck disable=SC2086  # EXTRA is a word list on purpose
sudo -n timeout -s INT "$T" "$BIN" --net-src "http://127.0.0.1:$PORT/" --pool "$POOL" --frames "$N" \
    --box-log "$OUT" $EXTRA > "$LOG" 2>&1
RC=$?
wall=$(( $(date +%s) - t0 ))
# light_ai closed the connection on exit: the server returns; a hung one is killed.
for _ in $(seq 50); do
    kill -0 "$SRV_PID" 2>/dev/null || break
    sleep 0.1
done
kill "$SRV_PID" 2>/dev/null
wait "$SRV_PID" 2>/dev/null

[ "$RC" -eq 0 ] ||
    fail "light_ai exit $RC (124 = stopped by the timer: a frame was lost); $(grep -m3 -E "$TROUBLE|error" "$LOG" | tr '\n' ' ')"
if grep -q -E "$TROUBLE" "$LOG"; then
    fail "source trouble in $LOG: $(grep -m3 -E "$TROUBLE" "$LOG" | tr '\n' ' ')"
fi
grep -q "^sent $N\$" "$SRV_LOG" || fail "server did not send all $N frames: $(tr '\n' ' ' < "$SRV_LOG")"
[ -s "$OUT" ] || fail "no box log $OUT"
# Frame block headers are the lines that start with a digit: frame,raw_n,sm_n.
BAD=$(awk -F, -v n="$N" '
    /^[0-9]/ { if ($1 != k) { print "frame id " $1 " where " k " was expected"; bad = 1; exit } k++ }
    END { if (!bad && k != n) print k " frame blocks, " n " frames in the clip" }' k=0 "$OUT")
[ -z "$BAD" ] || fail "$OUT: $BAD"
if [ -n "$REF" ]; then
    cmp "$OUT" "$REF" > /dev/null || fail "$OUT differs from $REF: $(cmp "$OUT" "$REF" 2>&1 | head -1)"
fi
echo "PASS: $N frames in, $N frame blocks out, pool $POOL, ${wall} s, $(sha256sum "$OUT" | cut -c1-16)${REF:+, cmp-identical to $REF}"
