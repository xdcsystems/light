#!/bin/bash
# scripts/collect_project.sh — дамп проекта в 3 части, файлы не рвутся
set -euo pipefail
shopt -s nullglob

files=(CMakeLists.txt include/*.h include/*.hpp src/*.cpp scripts/* README.md)

# сам скрипт из дампа выкидываем, каталоги — тоже
filtered=()
for f in "${files[@]}"; do
  [[ -f "$f" && "$f" != scripts/collect_project.sh ]] || continue
  filtered+=("$f")
done
files=("${filtered[@]}")

# объёмы и общая цель на часть
total=0; sizes=()
for f in "${files[@]}"; do
  n=$(wc -l < "$f"); sizes+=("$n"); total=$((total + n))
done
target=$(( (total + 2) / 3 ))

part=1; count=0
out="/tmp/audit_part1.txt"; : > "$out"
for i in "${!files[@]}"; do
  f="${files[i]}"; n="${sizes[i]}"
  if (( count >= target && part < 3 )); then
    part=$((part + 1)); out="/tmp/audit_part${part}.txt"; : > "$out"; count=0
  fi
  { echo "===== $f ====="; cat "$f"; echo; } >> "$out"
  count=$((count + n))
done

wc -l /tmp/audit_part*.txt

