#!/bin/bash
set -euo pipefail

MODE="${1:-on}"
STATE_FILE="/run/light_ai_rt.state"
DMA_UNIT="light_ai-dma-latency.service"
THP_FILE="/sys/kernel/mm/transparent_hugepage/enabled"

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

        # Сохраняем исходные значения VM/THP один раз (повторный on их не затрёт)
        if [ ! -f "$STATE_FILE" ]; then
            {
                [ -r /proc/sys/vm/swappiness ]    && echo "SWAPPINESS=$(cat /proc/sys/vm/swappiness)"
                [ -r /proc/sys/vm/stat_interval ] && echo "STAT_INTERVAL=$(cat /proc/sys/vm/stat_interval)"
                if [ -r /proc/sys/kernel/sched_rt_runtime_us ]; then
                    rt_cur="$(cat /proc/sys/kernel/sched_rt_runtime_us)"
                    [ "${rt_cur:-0}" -le 0 ] 2>/dev/null && rt_cur=950000
                    echo "SCHED_RT_RUNTIME_US=$rt_cur"
                fi
                if [ -r "$THP_FILE" ]; then
                    thp_cur="$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$THP_FILE")"
                    [ -n "$thp_cur" ] && echo "THP=$thp_cur"
                fi
            } > "$STATE_FILE"
        fi

        # Снижаем фоновый шум ядра по памяти (swap остаётся страховкой под 4 ГБ ОЗУ)
        [ -w /proc/sys/vm/swappiness ]    && echo 1  > /proc/sys/vm/swappiness
        [ -w /proc/sys/vm/stat_interval ] && echo 60 > /proc/sys/vm/stat_interval
        [ -w "$THP_FILE" ]                && echo never > "$THP_FILE" 2>/dev/null || true

        # Disable RT throttling strictly when watchdog is active
        if [ "${ENABLE_RT_UNLIMITED:-0}" = "1" ] && [ -w /proc/sys/kernel/sched_rt_runtime_us ]; then
            echo -1 > /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null || true
        fi

        # Запрещаем глубокие C-states на время RT-прогона (fd держится открытым в system.slice)
        if [ "${SKIP_DMA_LATENCY:-0}" != "1" ] && [ -w /dev/cpu_dma_latency ]; then
            if ! systemctl is-active --quiet "$DMA_UNIT"; then
                systemctl reset-failed "$DMA_UNIT" 2>/dev/null || true
                systemd-run --unit="$DMA_UNIT" --collect \
                    --slice=system.slice \
                    --description="Hold /dev/cpu_dma_latency=0 for light_ai RT isolation" \
                    /bin/bash -c 'exec 3>/dev/cpu_dma_latency && printf "\000\000\000\000" >&3 && exec sleep infinity' \
                    >/dev/null 2>&1 || true
            fi
        fi

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
        pin_irq 'rga2'          '4'
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
        # Отпускаем /dev/cpu_dma_latency (возвращаем C-states)
        systemctl stop "$DMA_UNIT" 2>/dev/null || true

        # Восстанавливаем исходные VM/THP
        if [ -f "$STATE_FILE" ]; then
            while IFS='=' read -r k v; do
                [ -z "$v" ] && continue
                case "$k" in
                    SWAPPINESS)    [ -w /proc/sys/vm/swappiness ]    && echo "$v" > /proc/sys/vm/swappiness 2>/dev/null || true ;;
                    STAT_INTERVAL) [ -w /proc/sys/vm/stat_interval ] && echo "$v" > /proc/sys/vm/stat_interval 2>/dev/null || true ;;
                    THP)                 [ -w "$THP_FILE" ]                && echo "$v" > "$THP_FILE" 2>/dev/null || true ;;
                    SCHED_RT_RUNTIME_US) [ -w /proc/sys/kernel/sched_rt_runtime_us ] && echo "$v" > /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null || true ;;
                esac
            done < "$STATE_FILE"
            rm -f "$STATE_FILE"
        fi

        # Возвращаем все 8 ядер для ОС и сборок (разводку IRQ оставляем — она полезна всегда)
        for u in init.scope system.slice user.slice; do
            systemctl set-property --runtime "$u" AllowedCPUs=0-7
        done
        ;;

    status)
        echo "system.slice:     $(cat /sys/fs/cgroup/system.slice/cpuset.cpus.effective 2>/dev/null || echo n/a)"
        echo "user.slice:       $(cat /sys/fs/cgroup/user.slice/cpuset.cpus.effective 2>/dev/null || echo n/a)"
        echo "irqbalance:       $(systemctl is-active irqbalance 2>/dev/null || true)"
        dma_state="$(systemctl is-active "$DMA_UNIT" 2>/dev/null || true)"
        if [ -r /dev/cpu_dma_latency ]; then
            dma_val="$(od -An -td4 -N4 /dev/cpu_dma_latency 2>/dev/null | tr -d ' ')"
            echo "cpu_dma_latency:  ${dma_state} (${dma_val} us)"
        else
            echo "cpu_dma_latency:  ${dma_state}"
        fi
        echo "vm.swappiness:    $(cat /proc/sys/vm/swappiness 2>/dev/null || echo n/a)"
        echo "vm.stat_interval: $(cat /proc/sys/vm/stat_interval 2>/dev/null || echo n/a)"
        echo "sched_rt_runtime: $(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null || echo n/a)"
        [ -r "$THP_FILE" ] && echo "thp:              $(cat "$THP_FILE")"
        for irq in $(awk '/fdab0000\.npu|rga3|eth0-0 / {gsub(/:/, "", $1); print $1}' /proc/interrupts); do
            printf "IRQ %-4s -> CPU %s\n" "$irq" "$(cat "/proc/irq/$irq/smp_affinity_list" 2>/dev/null || echo n/a)"
        done
        ;;

    *)
        echo "Usage: $0 {on|off|status}" >&2
        exit 2
        ;;
esac
