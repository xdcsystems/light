#!/bin/bash
# tidy_run.sh: the clang-tidy gate (M6; #42, #43). One clang-tidy per TU, the TUs in parallel, a per-TU
# timeout, an exact per-TU result cache (scripts/tidy_cache.py makes the key). Two passes, one after the other
# (the same job count: RAM is the limit), over the same TUs and the same stripped compile DB:
#   main       the system clang-tidy 11 with the .clang-tidy files of the tree: every check of the project;
#   cognitive  the pinned PyPI clang-tidy in light_ai/venv (COG_VERSION) with scripts/tidy_cognitive.yaml only:
#              readability-function-cognitive-complexity, which LLVM 11 does not have (#43). The venv belongs to
#              the user: the gate only reads it; a missing binary or another version fails the gate at once,
#              with the install command (README, Build).
# Usage: bash scripts/tidy_run.sh [timeout_sec]
# Env:   TIDY_JOBS=N      parallel jobs (default min(nproc, MemAvailable / 400 MB): one clang-tidy 11 peaks
#                         at ~330 MB on pipeline.cpp; RAM, not the cores, is the limit on this board)
#        TIDY_NO_CACHE=1  no cache lookup, no store
# Logs:  main: build/tidy/<path of the TU without .cpp>.log (src/net/h264_encoder.log: a tree like src/,
#        never a flat name -- src/foo.cpp and src/net/foo.cpp must not share a log); cognitive: the same tree
#        under build/tidy/cognitive/; HANGS.txt in build/tidy (a cognitive entry starts with "cognitive: ");
#        cache: build/tidy-cache (one store for both passes: the key holds the binary, its version and the
#        --config-file bytes). All inside the project (#48: nothing of the project is written outside it,
#        not even to $TMPDIR or /tmp), git-ignored with build/. One run at a time (flock on $OUT/.lock; a second
#        run waits). The output filesystem is printed first, with a warning when it is the eMMC (storage_guard.sh).
TIDY=clang-tidy
CLANGXX=clang++-11   # the preprocessor of the cache key, both passes: the LLVM driver of clang-tidy 11 (the wheel has none)
DB=build
TMO=${1:-600}
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd -P)"
PROJ_ROOT="$(cd "$HERE/.." && pwd -P)"
cd "$PROJ_ROOT"       # the TU paths (src/...) and DB=build are relative to the project root
COG_TIDY=$PROJ_ROOT/venv/bin/clang-tidy
COG_VERSION=23.1.4
COG_CFG=$PROJ_ROOT/scripts/tidy_cognitive.yaml
OUT=$PROJ_ROOT/build/tidy
TDB=$OUT/cc
CACHE=$PROJ_ROOT/build/tidy-cache
CACHE_DAYS=14        # cache entries not used for this many days are pruned after the run
mkdir -p "$OUT"
# shellcheck source=storage_guard.sh
. "$HERE/storage_guard.sh"
storage_guard tidy "$OUT"
# The cognitive pass's tool first: a wrong or missing one fails before any work.
COG_FOUND=$("$COG_TIDY" --version 2>/dev/null | awk '/LLVM version/ {print $3}')
if [ "$COG_FOUND" != "$COG_VERSION" ]; then
  echo "tidy: FAIL -- the cognitive pass needs clang-tidy $COG_VERSION at $COG_TIDY (found: ${COG_FOUND:-none})"
  echo "tidy: install it (README, Build): cd $PROJ_ROOT && python3 -m venv venv &&" \
       "venv/bin/pip install --no-cache-dir clang-tidy==$COG_VERSION"
  exit 1
fi
# Two gates at once (board-check and a manual run) would clear and write the same $OUT: the second one waits.
exec 9> "$OUT/.lock"
if ! flock -n 9; then
  echo "tidy: another tidy_run.sh holds $OUT/.lock -- waiting for it to end"
  flock 9
fi
mkdir -p "$TDB" "$OUT/st" "$OUT/cognitive/st" "$CACHE/times" "$CACHE/times-cognitive"
# clang rejects GCC-specific big.LITTLE flags carried by the cross DB
# (see the M6-tidy log: "the clang compiler does not support
# '-mtune=cortex-a76.cortex-a55'"; LLVM 23 too: "unknown target CPU") -- strip them into a per-run copy
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
rm -rf "$OUT"/src "$OUT"/st/* "$OUT"/cognitive/src "$OUT"/cognitive/st/* "$OUT"/HANGS.txt
rm -f "$OUT"/*.log   # flat logs of the script before #46

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

# One TU of the current pass: key -> hit (copy the stored log) or run (store a clean / findings result only).
# The pass: P_TIDY (binary), P_ARGS (extra clang-tidy arguments, no blanks inside one), P_OUT (log root),
# P_ST (status root), P_TIMES (sort hints), P_TAG (progress prefix).
# Writes $P_OUT/<rel>.log and $P_ST/<rel> ("rc seconds hit|miss|nokey"), rel = the TU path without .cpp;
# prints one progress line.
# Always exits 0: the summaries read the status files, a missing one counts as an error.
tidy_one() {
  local f=$1 rel log key start rc secs n tag="" how=miss
  rel=${f%.cpp}
  log=$P_OUT/$rel.log
  mkdir -p "$(dirname "$log")" "$(dirname "$P_ST/$rel")" "$(dirname "$P_TIMES/$f")"
  start=$(date +%s%N)
  key=""
  # shellcheck disable=SC2086  # P_ARGS: separate words, none with blanks
  if [ "${TIDY_NO_CACHE:-0}" != 1 ]; then
    key=$(timeout "$TMO" python3 "$HERE/tidy_cache.py" "$TDB" "$f" "$TMO" "$CLANGXX" -- \
          "$P_TIDY" $P_ARGS -quiet -p "$TDB" "$f" 2>/dev/null)
    [ -z "$key" ] && how=nokey
  fi
  if [ -n "$key" ] && [ -f "$CACHE/$key.rc" ] && [ -f "$CACHE/$key.log" ]; then
    cp "$CACHE/$key.log" "$log"
    touch "$CACHE/$key.log" "$CACHE/$key.rc"
    [ -f "$P_TIMES/$f" ] && touch "$P_TIMES/$f"
    read -r rc _ < "$CACHE/$key.rc"
    how=hit
  else
    # shellcheck disable=SC2086
    timeout "$TMO" "$P_TIDY" $P_ARGS -quiet -p "$TDB" "$f" > "$log" 2>&1
    rc=$?
  fi
  secs=$(( ($(date +%s%N) - start) / 1000000 ))
  secs=$(printf '%d.%01d' $(( secs / 1000 )) $(( secs % 1000 / 100 )))
  n=$(grep -c ': error: ' "$log")
  if [ "$how" != hit ]; then
    # the sort hint: a completed run only (a timeout or a kill says nothing about the TU's cost)
    { [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ]; } && echo "$secs" > "$P_TIMES/$f"
    # Store only a clean run (rc 0) or a findings run: rc 1 with >= 1 check finding, no compile error,
    # no clang-tidy processing error. Never a timeout (124), an OOM kill (137), a crash.
    if [ -n "$key" ] && { [ "$rc" -eq 0 ] || { [ "$rc" -eq 1 ] && [ "$n" -gt 0 ] &&
         ! grep -q -e 'clang-diagnostic-error' -e '^Error while processing' "$log"; }; }; then
      cp "$log" "$CACHE/$key.log.$$" && mv "$CACHE/$key.log.$$" "$CACHE/$key.log"
      echo "$rc $secs" > "$CACHE/$key.rc.$$" && mv "$CACHE/$key.rc.$$" "$CACHE/$key.rc"
    fi
  fi
  echo "$rc $secs $how" > "$P_ST/$rel"
  [ "$how" = hit ] && tag=" (cached)"
  [ "$how" = nokey ] && tag=" (no key)"
  [ "$rc" -eq 124 ] && tag="$tag TIMEOUT"
  [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && tag="$tag rc=$rc"
  printf '%s %s %s s %s%s\n' "$P_TAG" "$f" "$secs" "$n" "$tag"
  return 0
}
export -f tidy_one
export CLANGXX TMO TDB CACHE HERE TIDY_NO_CACHE

# find covers src/config/ too: a flat glob would silently drop
# the config/ TU from the gate.
# The TU list is the same for both passes; each pass orders it slowest first by its own times, so the tail is
# short: a TU never timed yet first (it may be slow), then by the last measured time, descending; the line count
# breaks ties and orders the TUs of a cold cache.
TUS=$(find src -name '*.cpp' | xargs -r wc -l | awk '$2!="total" {print $2, $1}')
if [ -z "$TUS" ]; then
  echo "tidy: FAIL -- no TU found under src/"
  exit 1
fi
order_tus() {  # $1: the times dir of the pass
  echo "$TUS" | while read -r f lines; do
    t=$(cat "$1/$f" 2>/dev/null || echo inf)
    echo "$t $lines $f"
  done | sort -k1,1gr -k2,2nr | awk '{print $3}'
}

# One pass: run every TU, then its summary. $1 name, $2 binary, $3 extra args, $4 log root, $5 status root,
# $6 times dir, $7 HANGS.txt prefix. Sets PASS_FINDINGS and PASS_WALL.
run_pass() {
  local name=$1 pre=$7 files t0 f rc l n s logs stats
  export P_TIDY=$2 P_ARGS=$3 P_OUT=$4 P_ST=$5 P_TIMES=$6
  if [ "$name" = main ]; then P_TAG="tidy:"; else P_TAG="tidy $name:"; fi
  export P_TAG
  echo "==== tidy pass: $name ($("$P_TIDY" --version | awk '/LLVM version/ {print "LLVM " $3}')${P_ARGS:+, $P_ARGS}) ===="
  files=$(order_tus "$P_TIMES")
  t0=$(date +%s)
  printf '%s\n' $files | xargs -r -P "$JOBS" -n 1 bash -c 'tidy_one "$1"' _
  PASS_WALL=$(( $(date +%s) - t0 ))

  for f in $files; do
    if [ ! -f "$P_ST/${f%.cpp}" ]; then
      echo "$pre$f" >> "$OUT/HANGS.txt"
      echo "  $pre$f: no status (the worker died) -> logged in HANGS.txt"
      continue
    fi
    read -r rc _ < "$P_ST/${f%.cpp}"
    if [ "$rc" -eq 124 ]; then
      echo "$pre$f" >> "$OUT/HANGS.txt"
      echo "  $pre$f: TIMEOUT -> logged in HANGS.txt"
    elif [ "$rc" -ne 0 ]; then
      echo "$pre$f" >> "$OUT/HANGS.txt"
      echo "  $pre$f: rc=$rc (errors: see the log)"
    fi
  done
  echo "---- $name: findings by check ----"
  logs=$(find "$P_OUT/src" -name '*.log' 2>/dev/null | sort)
  stats=$(find "$P_ST" -type f | sort)
  # /dev/null after $logs / $stats, -r on xargs: an empty list must not make grep / cat / wc read stdin (a hang)
  # shellcheck disable=SC2086  # paths without blanks
  grep -h ': error: ' $logs /dev/null | awk '{print $NF}' | sort | uniq -c | sort -rn
  echo "---- $name: per-file finding counts ----"
  for l in $logs; do
    n=$(grep -c ': error: ' "$l")
    echo "${l#"$P_OUT"/}: $n" | sed 's/\.log: /: /'
  done
  echo "---- $name: slowest TUs (s) ----"
  for s in $stats; do echo "$(awk '{print $2, $3}' "$s") ${s#"$P_ST"/}"; done | sort -k1,1gr | head -n 10
  # shellcheck disable=SC2086
  PASS_FINDINGS=$(cat $logs /dev/null | grep -c ': error: ')
  echo "tidy $name: wall ${PASS_WALL} s, findings: $PASS_FINDINGS"
}

run_pass main "$TIDY" "" "$OUT" "$OUT/st" "$CACHE/times" ""
MAIN_FINDINGS=$PASS_FINDINGS MAIN_WALL=$PASS_WALL
run_pass cognitive "$COG_TIDY" "--config-file=$COG_CFG" "$OUT/cognitive" "$OUT/cognitive/st" "$CACHE/times-cognitive" "cognitive: "
COG_FINDINGS=$PASS_FINDINGS COG_WALL=$PASS_WALL

echo "---- hangs / errors ----"
cat "$OUT/HANGS.txt" 2>/dev/null || echo "(none)"
STATS=$(find "$OUT/st" "$OUT/cognitive/st" -type f | sort)
# shellcheck disable=SC2086
HITS=$(cat $STATS /dev/null | awk '$3 == "hit"' | wc -l)
# shellcheck disable=SC2086
RUNS=$(cat $STATS /dev/null | awk '$3 != "hit"' | wc -l)
if [ "${TIDY_NO_CACHE:-0}" != 1 ]; then
  # a hit touches its entry, so the mtime is the last use
  find "$CACHE" -maxdepth 1 \( -name '*.log' -o -name '*.rc' \) -mmin +$(( CACHE_DAYS * 1440 )) -delete
  find "$CACHE" -maxdepth 1 \( -name '*.log.*' -o -name '*.rc.*' \) -mmin +60 -delete   # temp files of a killed run
  # a hit touches its TU's time too
  find "$CACHE/times" "$CACHE/times-cognitive" -type f -mmin +$(( CACHE_DAYS * 1440 )) -delete
fi
echo "tidy: wall $(( MAIN_WALL + COG_WALL )) s (main ${MAIN_WALL} s, cognitive ${COG_WALL} s), $JOBS jobs;" \
     "cache: $HITS hits / $RUNS runs; $(find "$CACHE" -maxdepth 1 -name '*.rc' | wc -l) entries, $(du -sh "$CACHE" | cut -f1)"
TOTAL=$(( MAIN_FINDINGS + COG_FINDINGS ))
HANGS=0
if [ -s "$OUT/HANGS.txt" ]; then
  HANGS=$(wc -l < "$OUT/HANGS.txt")
fi
if [ "$TOTAL" -gt 0 ] || [ "$HANGS" -gt 0 ]; then
  echo "tidy: FAIL -- findings: $TOTAL (main $MAIN_FINDINGS, cognitive $COG_FINDINGS), hangs/errors: $HANGS"
  exit 1
fi
echo "tidy: PASS (0 findings: main + cognitive)"
