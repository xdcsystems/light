#!/bin/bash
# set_governors.sh — «стандартный запуск» light_ai: performance на всех
# частотных доменах. Без CPU performance выигрыш cores-012 исчезает
# (21.2 -> 25.6 ms), NPU/DDR performance безвредны и добавляют запас.
# Запуск: sudo bash scripts/set_governors.sh
set -e

for p in /sys/devices/system/cpu/cpufreq/policy*; do
    echo performance > "$p/scaling_governor"
done
echo performance > /sys/class/devfreq/fdab0000.npu/governor
echo performance > /sys/class/devfreq/dmc/governor

echo "== governors =="
for p in /sys/devices/system/cpu/cpufreq/policy*; do
    echo "$p: $(cat "$p/scaling_governor") cur=$(cat "$p/scaling_cur_freq")"
done
echo "npu: $(cat /sys/class/devfreq/fdab0000.npu/governor) @ $(cat /sys/class/devfreq/fdab0000.npu/cur_freq)"
echo "dmc: $(cat /sys/class/devfreq/dmc/governor) @ $(cat /sys/class/devfreq/dmc/cur_freq)"

