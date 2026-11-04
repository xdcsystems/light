#!/bin/bash
# tidy_run.sh: per-file clang-tidy with per-file timeouts (M6).
# Logs under /tmp/tidy/. Usage: bash scripts/tidy_run.sh [timeout_sec]
TIDY=clang-tidy
DB=build-tidy
TMO=${1:-600}
mkdir -p /tmp/tidy
rm -f /tmp/tidy/*.log /tmp/tidy/HANGS.txt
FILES=$(wc -l src/*.cpp | sort -n | awk '$2!="total" {print $2}')
for f in $FILES; do
  name=$(basename "$f" .cpp)
  echo "tidy: $f (timeout ${TMO}s)"
  timeout "$TMO" "$TIDY" -quiet -p "$DB" "$f" > "/tmp/tidy/$name.log" 2>&1
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
