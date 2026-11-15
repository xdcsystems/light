#!/bin/bash
# tidy_run.sh: the clang-tidy gate (M6; #42). One clang-tidy per TU, the TUs in parallel, a per-TU
# timeout, an exact per-TU result cache (scripts/tidy_cache.py makes the key).
# Usage: bash scripts/tidy_run.sh [timeout_sec]
# Env:   TIDY_JOBS=N      parallel jobs (default min(nproc, MemAvailable / 400 MB): one clang-tidy peaks
#                         at ~330 MB on pipeline.cpp; RAM, not the cores, is the limit on this board)
#        TIDY_NO_CACHE=1  no cache lookup, no store
# Logs:  ${TMPDIR:-/tmp}/tidy/<name>.log, HANGS.txt there; cache: ${TMPDIR:-/tmp}/tidy-cache.
TIDY=clang-tidy
CLANGXX=clang++-11   # the preprocessor of the cache key: the same LLVM driver as clang-tidy 11
DB=build
TMO=${1:-600}
OUT=${TMPDIR:-/tmp}/tidy
TDB=$OUT/cc
CACHE=${TMPDIR:-/tmp}/tidy-cache
CACHE_DAYS=14        # cache entries not used for this many days are pruned after the run
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd -P)"
mkdir -p "$OUT" "$TDB" "$OUT/st" "$CACHE/times"
# clang rejects GCC-specific big.LITTLE flags carried by the cross DB
# (see the M6-tidy log: "the clang compiler does not support
# '-mtune=cortex-a76.cortex-a55'") -- strip them into a per-run copy
# of the compile DB. If clang chokes on more flags, add them to BAD.
python3 - "$DB/compile_commands.json" "$TDB/compile_commands.json" <<'PYEOF'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
db = json.load(open(src))
BAD = ( " -mtune=cortex-a76.cortex-a55", )
for e in db:
    c = e["command"]
    for b in BAD:
        c = c.replace(b, "")
    e["command"] = c
json.dump(db, open(dst, "w"))
PYEOF
rm -f "$OUT"/*.log "$OUT"/HANGS.txt "$OUT"/st/*

# MemAvailable is printed either way: a FAIL from an OOM kill (rc=137 in HANGS.txt) is recognizable
MEM_MB=$(awk '/^MemAvailable:/ {print int($2 / 1024)}' /proc/meminfo)
if [ -n "$TIDY_JOBS" ]; then
  JOBS=$TIDY_JOBS
  JOBS_WHY="TIDY_JOBS"
else
  JOBS=$(( MEM_MB / 400 ))
  [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
  [ "$JOBS" -lt 1 ] && JOBS=1
  JOBS_WHY="min(nproc $(nproc), MemAvailable / 400 MB)"
fi
echo "tidy: $JOBS jobs ($JOBS_WHY), MemAvailable ${MEM_MB} MB, timeout ${TMO}s," \
     "cache $([ "${TIDY_NO_CACHE:-0}" = 1 ] && echo off || echo "$CACHE")"

# One TU: key -> hit (copy the stored log) or run (store a clean / findings result only).
# Writes $OUT/<name>.log and $OUT/st/<name> ("rc seconds hit|miss|nokey"), prints one progress line.
# Always exits 0: the summaries read the status files, a missing one counts as an error.
tidy_one() {
  local f=$1 name key start rc secs n tag="" how=miss
  name=$(basename "$f" .cpp)
  start=$(date +%s%N)
  key=""
  if [ "${TIDY_NO_CACHE:-0}" != 1 ]; then
    key=$(timeout "$TMO" python3 "$HERE/tidy_cache.py" "$TDB" "$f" "$TMO" "$CLANGXX" -- "$TIDY" -quiet -p "$TDB" "$f" 2>/dev/null)
    [ -z "$key" ] && how=nokey
  fi
  if [ -n "$key" ] && [ -f "$CACHE/$key.rc" ] && [ -f "$CACHE/$key.log" ]; then
    cp "$CACHE/$key.log" "$OUT/$name.log"
    touch "$CACHE/$key.log" "$CACHE/$key.rc"
    read -r rc _ < "$CACHE/$key.rc"
    how=hit
  else
    timeout "$TMO" "$TIDY" -quiet -p "$TDB" "$f" > "$OUT/$name.log" 2>&1
    rc=$?
  fi
  secs=$(( ($(date +%s%N) - start) / 1000000 ))
  secs=$(printf '%d.%01d' $(( secs / 1000 )) $(( secs % 1000 / 100 )))
  n=$(grep -c ': error: ' "$OUT/$name.log")
  if [ "$how" != hit ]; then
    # the sort hint: a completed run only (a timeout or a kill says nothing about the TU's cost)
    { [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ]; } && echo "$secs" > "$CACHE/times/${f//\//_}"
    # Store only a clean run (rc 0) or a findings run: rc 1 with >= 1 check finding, no compile error,
    # no clang-tidy processing error. Never a timeout (124), an OOM kill (137), a crash.
    if [ -n "$key" ] && { [ "$rc" -eq 0 ] || { [ "$rc" -eq 1 ] && [ "$n" -gt 0 ] &&
         ! grep -q -e 'clang-diagnostic-error' -e '^Error while processing' "$OUT/$name.log"; }; }; then
      cp "$OUT/$name.log" "$CACHE/$key.log.$$" && mv "$CACHE/$key.log.$$" "$CACHE/$key.log"
      echo "$rc $secs" > "$CACHE/$key.rc.$$" && mv "$CACHE/$key.rc.$$" "$CACHE/$key.rc"
    fi
  fi
  echo "$rc $secs $how" > "$OUT/st/$name"
  [ "$how" = hit ] && tag=" (cached)"
  [ "$how" = nokey ] && tag=" (no key)"
  [ "$rc" -eq 124 ] && tag="$tag TIMEOUT"
  [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && tag="$tag rc=$rc"
  printf 'tidy: %s %s s %s%s\n' "$f" "$secs" "$n" "$tag"
  return 0
}
export -f tidy_one
export TIDY CLANGXX TMO OUT TDB CACHE HERE TIDY_NO_CACHE

# find covers src/config/ too: a flat glob would silently drop
# the config/ TU from the gate.
# Slowest first, so the tail is short: a TU never timed yet first (it may be slow), then by the last
# measured time, descending; the line count breaks ties and orders the TUs of a cold cache.
FILES=$(find src -name '*.cpp' | xargs wc -l | awk '$2!="total" {print $2, $1}' | while read -r f lines; do
  t=$(cat "$CACHE/times/${f//\//_}" 2>/dev/null || echo inf)
  echo "$t $lines $f"
done | sort -k1,1gr -k2,2nr | awk '{print $3}')
T0=$(date +%s)
printf '%s\n' $FILES | xargs -P "$JOBS" -n 1 bash -c 'tidy_one "$1"' _
WALL=$(( $(date +%s) - T0 ))

for f in $FILES; do
  name=$(basename "$f" .cpp)
  if [ ! -f "$OUT/st/$name" ]; then
    echo "$f" >> "$OUT/HANGS.txt"
    echo "  $f: no status (the worker died) -> logged in HANGS.txt"
    continue
  fi
  read -r rc _ < "$OUT/st/$name"
  if [ "$rc" -eq 124 ]; then
    echo "$f" >> "$OUT/HANGS.txt"
    echo "  $f: TIMEOUT -> logged in HANGS.txt"
  elif [ "$rc" -ne 0 ]; then
    echo "$f" >> "$OUT/HANGS.txt"
    echo "  $f: rc=$rc (errors: see the log)"
  fi
done
echo "---- findings by check ----"
grep -h ': error: ' "$OUT"/*.log | awk '{print $NF}' | sort | uniq -c | sort -rn
echo "---- per-file finding counts ----"
for l in "$OUT"/*.log; do
  n=$(grep -c ': error: ' "$l")
  echo "$(basename "$l" .log): $n"
done
echo "---- hangs / errors ----"
cat "$OUT/HANGS.txt" 2>/dev/null || echo "(none)"
echo "---- slowest TUs (s) ----"
for s in "$OUT"/st/*; do echo "$(awk '{print $2, $3}' "$s") $(basename "$s")"; done | sort -k1,1gr | head -n 10
HITS=$(cat "$OUT"/st/* 2>/dev/null | awk '$3 == "hit"' | wc -l)
RUNS=$(cat "$OUT"/st/* 2>/dev/null | awk '$3 != "hit"' | wc -l)
if [ "${TIDY_NO_CACHE:-0}" != 1 ]; then
  # a hit touches its entry, so the mtime is the last use
  find "$CACHE" -maxdepth 1 \( -name '*.log' -o -name '*.rc' \) -mmin +$(( CACHE_DAYS * 1440 )) -delete
  find "$CACHE" -maxdepth 1 \( -name '*.log.*' -o -name '*.rc.*' \) -mmin +60 -delete   # temp files of a killed run
fi
echo "tidy: wall ${WALL} s, $JOBS jobs; cache: $HITS hits / $RUNS runs;" \
     "$(find "$CACHE" -maxdepth 1 -name '*.rc' | wc -l) entries, $(du -sh "$CACHE" | cut -f1)"
TOTAL=$(grep -h ': error: ' "$OUT"/*.log 2>/dev/null | wc -l)
HANGS=0
if [ -s "$OUT/HANGS.txt" ]; then
  HANGS=$(wc -l < "$OUT/HANGS.txt")
fi
if [ "$TOTAL" -gt 0 ] || [ "$HANGS" -gt 0 ]; then
  echo "tidy: FAIL -- findings: $TOTAL, hangs/errors: $HANGS"
  exit 1
fi
echo "tidy: PASS (0 findings)"
