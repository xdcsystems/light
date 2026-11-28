#!/bin/bash
# tidy_run.sh: the clang-tidy gate (M6; #42, #43). One clang-tidy per TU, the TUs in parallel, a per-TU
# timeout, an exact per-TU result cache (scripts/tidy_cache.py makes the key). The TUs: src/**/*.cpp (compile DB
# build/) and tests/*.cpp (the host tests' own DB, build/tests: configured here, not built, so SKIP_TESTS=1 works;
# tests/.clang-tidy relaxes only the function size). First the rules self-test, --verify-config of the rules config
# and scripts/rules_lint.sh (#93, #94, below), then two passes, one after the other (the same job count: RAM is
# the limit), over the same TUs and the same stripped compile DBs:
#   main   the system clang-tidy 11 with the .clang-tidy files of the tree: every check of the project;
#   rules  the pinned PyPI clang-tidy in light_ai/venv (RULES_VERSION) with scripts/tidy_rules.yaml only
#          (--experimental-custom-checks): readability-function-cognitive-complexity and misc-const-correctness,
#          which LLVM 11 does not have (#43, #94), and the custom-* project rules (#93: no statics, globals,
#          virtuals, inheritance, manual heap, smart pointers, const_cast, printf, std::cout outside main.cpp,
#          catch (...); #95: else-if, empty or endless loops). The venv belongs to the
#          user: the gate only reads it; a missing binary or another version fails the gate at once, with the
#          install command (README, Build). Named "cognitive" before #93.
# Usage: bash scripts/tidy_run.sh [timeout_sec]
# Env:   TIDY_JOBS=N      parallel jobs (default min(nproc, MemAvailable / 400 MB): one clang-tidy 11 peaks
#                         at ~330 MB on pipeline.cpp; RAM, not the cores, is the limit on this board)
#        TIDY_NO_CACHE=1  no cache lookup, no store
#        TIDY_FILES="src/a.cpp tests/b.cpp"
#                         a subset run (#93): the self-test, rules_lint.sh (the whole tree: well under 1 s) and
#                         both passes on only these TUs (src/**/*.cpp, tests/*.cpp; a header or a missing file ->
#                         exit 2), the same cache, DBs and log paths; only their logs and status are replaced, the
#                         summaries cover only them. Not the gate: scripts/build.sh unsets it.
#                         Unset or empty: every TU (the gate).
# Logs:  main: build/tidy/<path of the TU without .cpp>.log (src/net/h264_encoder.log, tests/test_log.log: a tree
#        like the sources, never a flat name -- src/foo.cpp and src/net/foo.cpp must not share a log); rules: the same
#        tree under build/tidy/rules/; the self-test and rules_lint.sh: build/tidy/selftest/; HANGS.txt in build/tidy
#        (a rules entry starts with "rules: ");
#        cache: build/tidy-cache (one store for both passes: the key holds the binary, its version and the
#        --config-file bytes). All inside the project (#48: nothing of the project is written outside it,
#        not even to $TMPDIR or /tmp), git-ignored with build/. One run at a time (flock on $OUT/.lock; a second
#        run waits). The output filesystem is printed first, with a warning when it is the eMMC (storage_guard.sh).
TIDY=clang-tidy-11   # by name, not "clang-tidy": venv/bin on PATH (its clang-tidy 23, clangd) must not replace the main pass
CLANGXX=clang++-11   # the preprocessor of the cache key, both passes: the LLVM driver of clang-tidy 11 (the wheel has none)
DB=build
TMO=${1:-600}
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd -P)"
PROJ_ROOT="$(cd "$HERE/.." && pwd -P)"
cd "$PROJ_ROOT"       # the TU paths (src/...) and DB=build are relative to the project root
RULES_TIDY=$PROJ_ROOT/venv/bin/clang-tidy
RULES_VERSION=23.1.4
RULES_CFG=$PROJ_ROOT/scripts/tidy_rules.yaml
OUT=$PROJ_ROOT/build/tidy
TDB=$OUT/cc
TDB_TESTS=$OUT/cc-tests
TESTS_BUILD=$PROJ_ROOT/build/tests
CACHE=$PROJ_ROOT/build/tidy-cache
CACHE_DAYS=14        # cache entries not used for this many days are pruned after the run
# TIDY_FILES first: a bad list fails before any work. SEL: the checked TU paths, relative to the project root.
# The entries are split on blanks, never globbed (set -f); each one is resolved by realpath (below).
SEL=""
set -f
for arg in ${TIDY_FILES:-}; do
  # one spelling per TU (./, //, ., .., an absolute path, ~/Projects/light: a symlink to the physical PROJ_ROOT):
  # resolved, then made relative; anything outside the project is left absolute and rejected below
  f=$(realpath -m -- "$arg")
  f=${f#"$PROJ_ROOT"/}
  case $arg in */) f="" ;; esac   # realpath drops a trailing /, but "x.cpp/" names no file
  case $f in
    src/*.cpp) ;;
    tests/*/*) f="" ;;
    tests/*.cpp) ;;
    *) f="" ;;
  esac
  if [ -z "$f" ] || [ ! -f "$f" ]; then
    echo "tidy: bad TIDY_FILES entry '$arg' -- a TU is src/**/*.cpp or tests/*.cpp and must exist" \
         "(a header: pass a .cpp that includes it)"
    exit 2
  fi
  case " $SEL " in *" $f "*) ;; *) SEL="$SEL $f" ;; esac
done
set +f
SEL=${SEL# }
mkdir -p "$OUT"
# shellcheck source=storage_guard.sh
. "$HERE/storage_guard.sh"
storage_guard tidy "$OUT"
# The rules pass's tool first: a wrong or missing one fails before any work.
RULES_FOUND=$("$RULES_TIDY" --version 2>/dev/null | awk '/LLVM version/ {print $3}')
if [ "$RULES_FOUND" != "$RULES_VERSION" ]; then
  echo "tidy: FAIL -- the rules pass needs clang-tidy $RULES_VERSION at $RULES_TIDY (found: ${RULES_FOUND:-none})"
  echo "tidy: install it (README, Build): cd $PROJ_ROOT && python3 -m venv venv &&" \
       "venv/bin/pip install --no-cache-dir clang-tidy==$RULES_VERSION"
  exit 1
fi
# Two gates at once (board-check and a manual run) would clear and write the same $OUT: the second one waits.
exec 9> "$OUT/.lock"
if ! flock -n 9; then
  echo "tidy: another tidy_run.sh holds $OUT/.lock -- waiting for it to end"
  flock 9
fi
mkdir -p "$TDB" "$TDB_TESTS" "$OUT/st" "$OUT/rules/st" "$CACHE/times" "$CACHE/times-rules"
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
# The tests' DB: configure only (the same arguments as scripts/test.sh, so the two never fight over the cache).
# Its copy keeps one entry per tests/*.cpp: the plain light_ai_tests one, the TSan one only for a file that has
# no other (tsan_main.cpp); the src/ files the tests compile are checked from the main DB, not twice.
if ! cmake -S "$PROJ_ROOT/tests" -B "$TESTS_BUILD" -DCMAKE_BUILD_TYPE=Release > "$OUT/cmake-tests.log" 2>&1; then
  echo "tidy: FAIL -- configuring the tests' compile DB failed: $OUT/cmake-tests.log"
  exit 1
fi
python3 - "$TESTS_BUILD/compile_commands.json" "$TDB_TESTS/compile_commands.json" "$PROJ_ROOT/tests" <<'PYEOF'
import json, os, sys
src, dst, tests = sys.argv[1], sys.argv[2], os.path.realpath(sys.argv[3])
keep = {}
for e in json.load(open(src)):
    f = os.path.realpath(os.path.join(e["directory"], e["file"]))
    if os.path.dirname(f) != tests or not f.endswith(".cpp"):
        continue
    tsan = "_tsan.dir/" in e.get("command", "") or "_tsan.dir/" in e.get("output", "")
    if f not in keep or (keep[f][1] and not tsan):
        keep[f] = (e, tsan)
json.dump([v[0] for v in keep.values()], open(dst, "w"))
PYEOF
if [ -z "$SEL" ]; then
  rm -rf "$OUT"/src "$OUT"/tests "$OUT"/st/* "$OUT"/rules/src "$OUT"/rules/tests "$OUT"/rules/st/*
  rm -rf "$OUT"/cognitive "$CACHE"/times-cognitive   # the second pass before its rename to rules (#93)
  rm -f "$OUT"/*.log   # flat logs of the script before #46
else
  # a subset run: only its own TUs; the logs of the others (the last full gate) stay as they were
  for f in $SEL; do
    rm -f "$OUT/${f%.cpp}.log" "$OUT/st/${f%.cpp}" "$OUT/rules/${f%.cpp}.log" "$OUT/rules/st/${f%.cpp}"
  done
fi
rm -f "$OUT"/HANGS.txt   # the hangs of this run only

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
# shellcheck disable=SC2086  # SEL: paths without blanks
[ -n "$SEL" ] && echo "tidy: TIDY_FILES subset, $(echo $SEL | wc -w) TUs -- not the gate: $SEL"

# Self-test of the rules (#93), in both modes, before any TU: a matcher with a typo matches nothing and a broken
# lint rule finds nothing, and the gate would stay green with the rule off. The rules pass runs on the fixtures of
# tests/tidy_probe/ (no compile DB: -std=c++17, -I include/ for the allowed bases) and must give exactly the hit
# sites of expected_rules.txt;
# rules_lint.sh runs on the fixture tree tests/tidy_probe/lint/ and must print exactly expected_lint.txt.
# Then rules_lint.sh on the project (every file, also in a subset run: it takes well under a second).
SELF=$OUT/selftest
PROBE=tests/tidy_probe
PROBE_TUS="$PROBE/violations.cpp $PROBE/src/main.cpp $PROBE/tests/harness.cpp"
rm -rf "$SELF"
mkdir -p "$SELF"
for f in $PROBE_TUS; do
  timeout "$TMO" "$RULES_TIDY" --experimental-custom-checks --config-file="$RULES_CFG" -quiet "$f" -- -std=c++17 \
    -I"$PROJ_ROOT/include" \
    > "$SELF/$(echo "$f" | tr / _).log" 2>&1 &
done
wait
# "<fixture>:<line> <check>", one line per hit: the site of the diagnostic (from tests/tidy_probe/ on: clang prints
# the absolute path, through a symlink when $PWD is one) and the first name in its trailing [...]; a check that
# drifts to another construct moves its hit off the expected line. A line without both (a processing error, a hit
# outside the fixtures) stays whole, so it can never match the expected list.
for f in $PROBE_TUS; do
  grep -E ': (fatal error|error|warning): |^Error' "$SELF/$(echo "$f" | tr / _).log" |
    sed -E 's#^/[^:]*/(tests/tidy_probe/[^: ]+:[0-9]+):[0-9]+: (error|warning): .*\[([^],]+)[],][^[]*$#\1 \3#'
done | LC_ALL=C sort > "$SELF/rules.txt"
grep -v '^#' "$PROBE/expected_rules.txt" | LC_ALL=C sort > "$SELF/rules.expected"
SELF_FAIL=0
if diff -u "$SELF/rules.expected" "$SELF/rules.txt" > "$SELF/rules.diff"; then
  echo "tidy self-test: rules PASS ($(wc -l < "$SELF/rules.txt") hits at the sites of expected_rules.txt)"
else
  echo "tidy self-test: rules FAIL -- the hit sites differ from $PROBE/expected_rules.txt (logs: $SELF):"
  tail -n +3 "$SELF/rules.diff"
  SELF_FAIL=1
fi
# --verify-config (#94): clang-tidy ignores a mistyped option key without a word, and the probe above cannot see a
# typo whose default equals the intended value. LLVM 23 cannot resolve the custom-* glob there (the custom checks
# exist only at run time), so that one line is expected and dropped; any other line fails. The step proves itself
# on $PROBE/verify_typo.yaml (one mistyped key): no "unknown check option" there means it no longer reports one.
VERIFY_NOISE="check glob 'custom-\*' doesn't match any known check"
timeout "$TMO" "$RULES_TIDY" --experimental-custom-checks --config-file="$PROJ_ROOT/$PROBE/verify_typo.yaml" \
  --verify-config > "$SELF/verify-typo.log" 2>&1
timeout "$TMO" "$RULES_TIDY" --experimental-custom-checks --config-file="$RULES_CFG" --verify-config \
  > "$SELF/verify.log" 2>&1
VERIFY_RC=$?   # 124: killed by the timeout -- an empty log then proves nothing
grep -v "$VERIFY_NOISE" "$SELF/verify.log" > "$SELF/verify.txt"
[ "$VERIFY_RC" -eq 124 ] && echo "tidy: --verify-config timed out after ${TMO}s" >> "$SELF/verify.txt"
if ! grep -q "unknown check option 'misc-const-correctness.WarnPointersAsValue'" "$SELF/verify-typo.log"; then
  echo "tidy self-test: verify-config FAIL -- no report of the mistyped key of $PROBE/verify_typo.yaml:"
  cat "$SELF/verify-typo.log"
  SELF_FAIL=1
elif [ -s "$SELF/verify.txt" ]; then
  echo "tidy self-test: verify-config FAIL -- $RULES_CFG:"
  cat "$SELF/verify.txt"
  SELF_FAIL=1
else
  echo "tidy self-test: verify-config PASS (every check and option key of $(basename "$RULES_CFG") is known)"
fi
bash "$HERE/rules_lint.sh" "$PROJ_ROOT/$PROBE/lint" | grep -v '^rules-lint:' > "$SELF/lint.txt"
if diff -u <(grep -v '^#' "$PROBE/expected_lint.txt") "$SELF/lint.txt" > "$SELF/lint.diff"; then
  echo "tidy self-test: lint PASS ($(wc -l < "$SELF/lint.txt") findings as expected_lint.txt)"
else
  echo "tidy self-test: lint FAIL -- rules_lint.sh output differs from $PROBE/expected_lint.txt:"
  tail -n +3 "$SELF/lint.diff"
  SELF_FAIL=1
fi
if [ "$SELF_FAIL" -ne 0 ]; then
  # a rule that does not fire on its fixture cannot vouch for the tree: no point in the passes
  echo "tidy: FAIL -- the rules self-test failed (logs: $SELF); fix the rule or its fixture first"
  exit 1
fi
bash "$HERE/rules_lint.sh" > "$SELF/project-lint.log"
LINT_RC=$?
cat "$SELF/project-lint.log"

# One TU of the current pass: key -> hit (copy the stored log) or run (store a clean / findings result only).
# The pass: P_TIDY (binary), P_ARGS (extra clang-tidy arguments, no blanks inside one), P_OUT (log root),
# P_ST (status root), P_TIMES (sort hints), P_TAG (progress prefix).
# Writes $P_OUT/<rel>.log and $P_ST/<rel> ("rc seconds hit|miss|nokey"), rel = the TU path without .cpp;
# prints one progress line.
# Always exits 0: the summaries read the status files, a missing one counts as an error.
tidy_one() {
  local f=$1 rel log key start rc secs n tag="" how=miss db=$TDB
  case $f in tests/*) db=$TDB_TESTS ;; esac
  rel=${f%.cpp}
  log=$P_OUT/$rel.log
  mkdir -p "$(dirname "$log")" "$(dirname "$P_ST/$rel")" "$(dirname "$P_TIMES/$f")"
  start=$(date +%s%N)
  key=""
  # shellcheck disable=SC2086  # P_ARGS: separate words, none with blanks
  if [ "${TIDY_NO_CACHE:-0}" != 1 ]; then
    key=$(timeout "$TMO" python3 "$HERE/tidy_cache.py" "$db" "$f" "$TMO" "$CLANGXX" -- \
          "$P_TIDY" $P_ARGS -quiet -p "$db" "$f" 2>/dev/null)
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
    timeout "$TMO" "$P_TIDY" $P_ARGS -quiet -p "$db" "$f" > "$log" 2>&1
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
export CLANGXX TMO TDB TDB_TESTS CACHE HERE TIDY_NO_CACHE

# find covers src/config/ too: a flat glob would silently drop
# the config/ TU from the gate.
# The TU list is the same for both passes; each pass orders it slowest first by its own times, so the tail is
# short: a TU never timed yet first (it may be slow), then by the last measured time, descending; the line count
# breaks ties and orders the TUs of a cold cache.
# A subset run takes SEL instead (checked above).
if [ -n "$SEL" ]; then
  # shellcheck disable=SC2086  # SEL: paths without blanks
  TUS=$(printf '%s\n' $SEL | xargs -r wc -l | awk '$2!="total" {print $2, $1}')
else
  TUS=$( { find src -name '*.cpp'; find tests -maxdepth 1 -name '*.cpp'; } | xargs -r wc -l | awk '$2!="total" {print $2, $1}')
  if ! echo "$TUS" | grep -q '^src/' || ! echo "$TUS" | grep -q '^tests/'; then
    echo "tidy: FAIL -- no TU found under src/ or tests/"
    exit 1
  fi
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
  # the logs and status files of this run's TUs only (a subset run leaves the others' in place)
  logs=$(for f in $files; do [ -f "$P_OUT/${f%.cpp}.log" ] && echo "$P_OUT/${f%.cpp}.log"; done | sort)
  stats=$(for f in $files; do [ -f "$P_ST/${f%.cpp}" ] && echo "$P_ST/${f%.cpp}"; done | sort)
  # /dev/null after $logs / $stats, -r on xargs: an empty list must not make grep / cat / wc read stdin (a hang)
  # shellcheck disable=SC2086  # paths without blanks
  grep -h ': error: ' $logs /dev/null | awk '{print $NF}' | sort | uniq -c | sort -rn
  if [ -n "$SEL" ]; then
    # a subset run prints the findings themselves (a few TUs): `grep ': error: '` on its output is enough
    echo "---- $name: findings ----"
    # shellcheck disable=SC2086
    grep -h ': error: ' $logs /dev/null | sort -u
  fi
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
run_pass rules "$RULES_TIDY" "--experimental-custom-checks --config-file=$RULES_CFG" "$OUT/rules" "$OUT/rules/st" \
         "$CACHE/times-rules" "rules: "
RULES_FINDINGS=$PASS_FINDINGS RULES_WALL=$PASS_WALL

echo "---- hangs / errors ----"
cat "$OUT/HANGS.txt" 2>/dev/null || echo "(none)"
STATS=$(echo "$TUS" | while read -r f _; do
          for s in "$OUT/st/${f%.cpp}" "$OUT/rules/st/${f%.cpp}"; do [ -f "$s" ] && echo "$s"; done
        done)
# shellcheck disable=SC2086
HITS=$(cat $STATS /dev/null | awk '$3 == "hit"' | wc -l)
# shellcheck disable=SC2086
RUNS=$(cat $STATS /dev/null | awk '$3 != "hit"' | wc -l)
if [ "${TIDY_NO_CACHE:-0}" != 1 ]; then
  # a hit touches its entry, so the mtime is the last use
  find "$CACHE" -maxdepth 1 \( -name '*.log' -o -name '*.rc' \) -mmin +$(( CACHE_DAYS * 1440 )) -delete
  find "$CACHE" -maxdepth 1 \( -name '*.log.*' -o -name '*.rc.*' \) -mmin +60 -delete   # temp files of a killed run
  # a hit touches its TU's time too
  find "$CACHE/times" "$CACHE/times-rules" -type f -mmin +$(( CACHE_DAYS * 1440 )) -delete
fi
echo "tidy: wall $(( MAIN_WALL + RULES_WALL )) s (main ${MAIN_WALL} s, rules ${RULES_WALL} s), $JOBS jobs;" \
     "cache: $HITS hits / $RUNS runs; $(find "$CACHE" -maxdepth 1 -name '*.rc' | wc -l) entries, $(du -sh "$CACHE" | cut -f1)"
TOTAL=$(( MAIN_FINDINGS + RULES_FINDINGS ))
HANGS=0
if [ -s "$OUT/HANGS.txt" ]; then
  HANGS=$(wc -l < "$OUT/HANGS.txt")
fi
# (a failed self-test exited above, before the passes)
if [ "$TOTAL" -gt 0 ] || [ "$HANGS" -gt 0 ] || [ "$LINT_RC" -ne 0 ]; then
  echo "tidy: FAIL${SEL:+ (subset: $(echo "$TUS" | wc -l) TUs)} -- findings: $TOTAL" \
       "(main $MAIN_FINDINGS, rules $RULES_FINDINGS), hangs/errors: $HANGS, rules-lint rc: $LINT_RC"
  exit 1
fi
if [ -n "$SEL" ]; then
  echo "tidy: PASS (subset: $(echo "$TUS" | wc -l) TUs, 0 findings: main + rules, self-test and rules-lint clean) -- not the gate"
else
  echo "tidy: PASS (0 findings: main + rules, self-test and rules-lint clean)"
fi
