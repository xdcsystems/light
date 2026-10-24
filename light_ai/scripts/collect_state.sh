#!/bin/bash
# collect_state.sh — собирает текущее состояние light_ai в один файл для нового чата.
# Запуск: bash collect_state.sh && cat /tmp/light_ai_state.txt
cd ~/Projects/light/light_ai || { echo "нет каталога проекта"; exit 1; }
OUT=/tmp/light_ai_state.txt

{
echo "==== date / uptime ===="
date
uptime

echo; echo "==== git: статус, ветка, ahead/behind ===="
git status
git log --oneline -8
echo -n "ahead of origin: "; git rev-list --count origin/master..master 2>/dev/null

echo; echo "==== git: незакоммиченное (должно быть пусто) ===="
git status --short

echo; echo "==== README: записи Roadmap (все чекбоксы) ===="
grep -n "^- \[" README.md

echo; echo "==== README: блок опций CLI ===="
grep -n -A12 "^Options:" README.md 2>/dev/null | head -30

echo; echo "==== README: строки с числами (тайминги, FPS) ===="
grep -nE "[0-9]+\.[0-9]+ ?ms|FPS" README.md | head -40

echo; echo "==== файлы проекта ===="
ls -la
ls -la include src scripts 2>/dev/null

echo; echo "==== сборка и модели ===="
ls -la build/light_ai 2>/dev/null || echo "build/light_ai: НЕ СОБРАН"
ls -la *.rknn 2>/dev/null || echo "нет .rknn в корне"

echo; echo "==== хвосты: .bak и логи ===="
ls *.bak include/*.bak src/*.bak 2>/dev/null || echo ".bak: нет"
ls -la /tmp/*.log /tmp/cal* 2>/dev/null || echo "/tmp логов: нет"
ls dataset/ 2>/dev/null | head -5 || true

echo; echo "==== SDK ===="
ls /mnt/nvme_data/work/rknn_model_zoo/3rdparty/rknpu2/include/ 2>/dev/null || echo "путь SDK недоступен"
ls /usr/local/lib/librknn* 2>/dev/null

echo; echo "==== камера / NPU ===="
ls /dev/video* 2>/dev/null | head
cat /sys/kernel/debug/rknpu/version 2>/dev/null
cat /sys/kernel/debug/rknpu/load 2>/dev/null

echo; echo "==== система ===="
uname -a
grep PRETTY_NAME /etc/os-release 2>/dev/null
} > "$OUT"

echo "Состояние собрано: $OUT"
echo "Приложите этот файл к новому чату вместе с handoff-сводкой."
