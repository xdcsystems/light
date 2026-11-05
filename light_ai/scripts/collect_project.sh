#!/bin/bash
# scripts/collect_limited.sh — дамп проекта в части по бюджету размера.
# Файлы не рвутся: файл целиком попадает в одну часть; если в текущую
# часть не влезает — часть закрывается и файл открывает следующую.
# Одиночный файл больше бюджета занимает свою часть целиком (со warning).
#
# Бюджет: PART_BYTES (байт на часть, по умолчанию 12000).
# Ориентир: ~3-4 байта на токен кода, т.е. 12000 байт ≈ 3-4k токенов.
# Переопределение: PART_BYTES=8000 ./scripts/collect_limited.sh
set -euo pipefail
shopt -s nullglob

PART_BYTES=${PART_BYTES:-12000}

files=(CMakeLists.txt include/*.h include/*.hpp src/*.cpp scripts/*)

# сам скрипт из дампа выкидываем, каталоги — тоже
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

# объёмы в байтах
sizes=(); total=0
for f in "${files[@]}"; do
  n=$(wc -c < "$f"); sizes+=("$n"); total=$((total + n))
done

# жадная упаковка с сохранением порядка
parts=()   # parts[i]: строка индексов файлов части i
bytes=()   # bytes[i]: суммарный размер части i
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

# запись частей
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

