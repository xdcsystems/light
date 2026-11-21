#!/bin/bash
set -euo pipefail

MODE="${1:-on}"

pin_irq() {
    local pattern="$1" cpus="$2"
    awk -v pat="$pattern" '$0 ~ pat {gsub(/:/, "", $1); print $1}' /proc/interrupts | while read -r irq; do
        if [ -w "/proc/irq/$irq/smp_affinity_list" ]; then
            echo "$cpus" > "/proc/irq/$irq/smp_affinity_list" 2>/dev/null || true
        fi
    done
}

case "$MODE" in
    on)
        systemctl stop irqbalance 2>/dev/null || true

        for u in init.scope system.slice user.slice; do
            systemctl set-property --runtime "$u" AllowedCPUs=0-3
        done

        # Системные и сетевые прерывания -> A55 (0-3)
        pin_irq 'eth0-'        '0-3'
        pin_irq 'dw-mci'       '0-1'
        pin_irq 'mmc0'         '0-1'
        pin_irq 'saradc'       '0-1'
        pin_irq 'i2c|spi'      '0-1'
        pin_irq 'xhci-hcd'     '2'

        # Ускорители light_ai -> A76 (4-7)
        pin_irq 'fb000000\.gpu' '4'
        pin_irq 'rga3'          '4'
        pin_irq 'jpegd'         '4'
        pin_irq 'rkvenc-core'   '4'

        # Три ядра NPU -> отдельные ядра 5, 6, 7
        npu_cores=(5 6 7)
        idx=0
        awk '/fdab0000\.npu/ {gsub(/:/, "", $1); print $1}' /proc/interrupts | while read -r irq; do
            echo "${npu_cores[$idx]}" > "/proc/irq/$irq/smp_affinity_list" 2>/dev/null || true
            idx=$(( (idx + 1) % 3 ))
        done
        ;;

    off)
        # Возвращаем все 8 ядер для ОС и сборок (разводку IRQ оставляем — она полезна всегда)
        for u in init.scope system.slice user.slice; do
            systemctl set-property --runtime "$u" AllowedCPUs=0-7
        done
        ;;

    status)
        echo "system.slice: $(cat /sys/fs/cgroup/system.slice/cpuset.cpus.effective 2>/dev/null || echo n/a)"
        echo "user.slice:   $(cat /sys/fs/cgroup/user.slice/cpuset.cpus.effective 2>/dev/null || echo n/a)"
        echo "irqbalance:   $(systemctl is-active irqbalance 2>/dev/null || true)"
        for irq in $(awk '/fdab0000\.npu|rga3|eth0-0 / {gsub(/:/, "", $1); print $1}' /proc/interrupts); do
            printf "IRQ %-4s -> CPU %s\n" "$irq" "$(cat "/proc/irq/$irq/smp_affinity_list" 2>/dev/null || echo n/a)"
        done
        ;;

    *)
        echo "Usage: $0 {on|off|status}" >&2
        exit 2
        ;;
esac
