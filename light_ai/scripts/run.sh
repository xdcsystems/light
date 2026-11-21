#!/bin/bash
set -e

PROJ_ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd -P)"
BIN="$PROJ_ROOT/build/light_ai"
DEFAULT_CFG="$PROJ_ROOT/build/light_ai.ini"
ISOLATE_SCRIPT="$PROJ_ROOT/scripts/isolate_rt.sh"

cd "$PROJ_ROOT"

if [ ! -x "$BIN" ]; then
    echo "run.sh: binary not found or not executable: $BIN" >&2
    echo "run.sh: build first with: ./scripts/build.sh" >&2
    exit 1
fi

has_help=0
has_config=0
for arg in "$@"; do
    case "$arg" in
        -h|--help) has_help=1 ;;
        --config)  has_config=1 ;;
    esac
done

if [ "$has_help" -eq 1 ]; then
    exec "$BIN" "$@"
fi

if [ "${SKIP_GOVERNORS:-0}" != "1" ]; then
    need_gov=0
    for f in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor \
             /sys/class/devfreq/fdab0000.npu/governor \
             /sys/class/devfreq/fb000000.gpu/governor \
             /sys/class/devfreq/dmc/governor; do
        if [ -r "$f" ] && [ "$(cat "$f")" != "performance" ]; then
            need_gov=1
            break
        fi
    done
    if [ "$need_gov" -eq 1 ]; then
        if [ "$(id -u)" -eq 0 ]; then
            bash "$PROJ_ROOT/scripts/set_governors.sh" >/dev/null || \
                echo "run.sh: warning: failed to set performance governors (continuing)" >&2
        elif command -v sudo >/dev/null 2>&1; then
            sudo -n bash "$PROJ_ROOT/scripts/set_governors.sh" >/dev/null || \
                echo "run.sh: warning: failed to set performance governors (continuing)" >&2
        fi
    fi
fi

CFG_ARGS=()
if [ "$has_config" -eq 0 ]; then
    CFG="${LIGHT_AI_CONFIG:-$DEFAULT_CFG}"
    if [ ! -f "$CFG" ]; then
        echo "run.sh: config not found: $CFG" >&2
        if [ "$CFG" = "$DEFAULT_CFG" ]; then
            echo "run.sh: copy template first: cp scripts/light_ai.ini.example build/light_ai.ini" >&2
        fi
        exit 2
    fi
    CFG_ARGS=(--config "$CFG")
fi

# Изоляция A55 (ОС + фоновые потоки) / A76 (горячие потоки light_ai)
if [ "${SKIP_ISOLATE:-0}" != "1" ] && [ -r "$ISOLATE_SCRIPT" ] && command -v systemd-run >/dev/null 2>&1; then
    SUDO=()
    RUN_AS=()
    can_isolate=0
    if [ "$(id -u)" -eq 0 ]; then
        can_isolate=1
    elif command -v sudo >/dev/null 2>&1; then
        SUDO=(sudo -n)
        RUN_AS=(-p "User=$(id -un)")
        can_isolate=1
    fi

    if [ "$can_isolate" -eq 1 ] && \
       "${SUDO[@]}" env "SKIP_DMA_LATENCY=${SKIP_DMA_LATENCY:-0}" bash "$ISOLATE_SCRIPT" on >/dev/null 2>&1; then
        cleanup_isolate() {
            "${SUDO[@]}" bash "$ISOLATE_SCRIPT" off >/dev/null 2>&1 || true
        }
        trap cleanup_isolate EXIT

        IO_MODE="--pipe"
        if [ -t 0 ] && [ -t 1 ]; then
            IO_MODE="--pty"
        fi

        ENV_ARGS=()
        while IFS= read -r kv; do
            [ -n "$kv" ] && ENV_ARGS+=(--setenv="$kv")
        done < <(env | grep '^LIGHT_AI_' || true)

        rc=0
        "${SUDO[@]}" systemd-run "$IO_MODE" --wait --collect \
            --slice=light_ai.slice \
            -p AllowedCPUs=0-7 \
            -p LimitRTPRIO=90 \
            -p LimitMEMLOCK=infinity \
            -p WorkingDirectory="$PROJ_ROOT" \
            "${RUN_AS[@]}" \
            "${ENV_ARGS[@]}" \
            /usr/bin/taskset -c 0-3 \
            "$BIN" "${CFG_ARGS[@]}" "$@" || rc=$?
        exit "$rc"
    else
        echo "run.sh: warning: failed to enable RT isolation (continuing without isolation)" >&2
    fi
fi

exec "$BIN" "${CFG_ARGS[@]}" "$@"
