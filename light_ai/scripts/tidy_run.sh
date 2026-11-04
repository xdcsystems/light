#!/bin/bash
# tidy_run.sh: per-file clang-tidy with per-file timeouts (M6).
# Logs under /tmp/tidy/. Usage: bash scripts/tidy_run.sh [timeout_sec]
TIDY=clang-tidy
DB=build
TDB=/tmp/tidy/cc
TMO=${1:-600}
mkdir -p /tmp/tidy "$TDB"
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
rm -f /tmp/tidy/*.log /tmp/tidy/HANGS.txt
FILES=$(wc -l src/*.cpp | sort -n | awk '$2!="total" {print $2}')
for f in $FILES; do
  name=$(basename "$f" .cpp)
  echo "tidy: $f (timeout ${TMO}s)"
  timeout "$TMO" "$TIDY" -quiet -p "$TDB" "$f" > "/tmp/tidy/$name.log" 2>&1
  rc=$?
  if [ "$rc" -eq 124 ]; then
    echo "$f" >> /tmp/tidy/HANGS.txt
    echo "  TIMEOUT -> logged in HANGS.txt"
  elif [ "$rc" -ne 0 ]; then
    echo "$f" >> /tmp/tidy/HANGS.txt
    echo "  rc=$rc (errors: see the log)"
  fi
done
echo "---- findings by check ----"
grep -h ': error: ' /tmp/tidy/*.log | awk '{print $NF}' | sort | uniq -c | sort -rn
echo "---- per-file finding counts ----"
for l in /tmp/tidy/*.log; do
  n=$(grep -c ': error: ' "$l")
  echo "$(basename "$l" .log): $n"
done
echo "---- hangs / errors ----"
cat /tmp/tidy/HANGS.txt 2>/dev/null || echo "(none)"
TOTAL=$(grep -h ': error: ' /tmp/tidy/*.log 2>/dev/null | wc -l)
HANGS=0
if [ -s /tmp/tidy/HANGS.txt ]; then
  HANGS=$(wc -l < /tmp/tidy/HANGS.txt)
fi
if [ "$TOTAL" -gt 0 ] || [ "$HANGS" -gt 0 ]; then
  echo "tidy: FAIL -- findings: $TOTAL, hangs/errors: $HANGS"
  exit 1
fi
echo "tidy: PASS (0 findings)"
