#!/bin/bash
# scripts/collect_limited.sh — dump the project into size-budgeted parts.
# Files are never split: each file goes into a single part in full; if it does
# not fit into the current part, the part is closed and the file starts the next.
# A single file larger than the budget occupies its own part (with a warning).
#
# Budget: PART_BYTES (bytes per part, default 12000).
# Rule of thumb: ~3-4 bytes per code token, i.e. 12000 bytes ~= 3-4k tokens.
# Override: PART_BYTES=8000 ./scripts/collect_limited.sh
set -euo pipefail
shopt -s nullglob

PART_BYTES=${PART_BYTES:-12000}

files=(CMakeLists.txt include/*.h include/*.hpp src/*.cpp scripts/*)

# Exclude this script itself and any directories from the dump
filtered=()
for f in "${files[@]}"; do
  [[ -f "$f" && "$f" != scripts/collect_limited.sh && "$f" != scripts/collect_project.sh ]] || continue
  filtered+=("$f")
done
files=("${filtered[@]}")

if (( ${#files[@]} == 0 )); then
  echo "collect_limited.sh: files not found" >&2
  exit 1
fi

# File sizes in bytes
sizes=(); total=0
for f in "${files[@]}"; do
  n=$(wc -c < "$f"); sizes+=("$n"); total=$((total + n))
done

# Order-preserving greedy packing
parts=()   # parts[i]: space-separated file indices for part i
bytes=()   # bytes[i]: total byte size of part i
for i in "${!files[@]}"; do
  n="${sizes[i]}"
  if (( n > PART_BYTES )); then
    echo "collect_limited.sh: WARNING: ${files[i]} ($n bytes) exceeds PART_BYTES=$PART_BYTES, goes into its own part" >&2
  fi
  last=$(( ${#parts[@]} - 1 ))
  if (( last >= 0 && bytes[last] + n <= PART_BYTES )); then
    parts[last]+=" $i"
    bytes[last]=$(( bytes[last] + n ))
  else
    parts+=( "$i" )
    bytes+=( "$n" )
  fi
done

# Write parts
np=${#parts[@]}
for p in "${!parts[@]}"; do
  out="/tmp/project_part$(( p + 1 )).txt"; : > "$out"
  for i in ${parts[p]}; do
    { echo "===== ${files[i]} ====="; cat "${files[i]}"; echo; } >> "$out"
  done
done

echo "== total: $total bytes, $np parts (budget $PART_BYTES bytes/part)"
for p in "${!parts[@]}"; do
  nfiles=$(wc -w <<< "${parts[p]}")
  echo "part$(( p + 1 )): /tmp/project_part$(( p + 1 )).txt  ${bytes[p]} bytes, $nfiles files"
done
