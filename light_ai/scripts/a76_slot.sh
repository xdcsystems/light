#!/usr/bin/env bash
# a76_slot.sh: run one single-threaded build job on a free Cortex-A76 core (BUILD_ON_A76=1: build.sh, test.sh,
# tidy_run.sh). isolcpus=4-7 takes these cores out of the scheduler's load balancing: a mask of several of them does
# not spread jobs, so each job is pinned to exactly one core.
# Usage: a76_slot.sh --check           preflight, once per gate script: no running light_ai (process or service),
#                                      CPUs 4-7 allowed by the cpuset; exit 1 otherwise, or when it cannot tell
#        a76_slot.sh <cmd> [args...]   wait for a free slot (flock on build/.a76_slot_<cpu>), then exec <cmd> on that
#                                      core. The command keeps the lock descriptor: the kernel releases the lock when
#                                      the process (and any child that inherited the descriptor) ends, however it ends.
#                                      Without BUILD_ON_A76=1 in the environment: exec <cmd> as it is, no slot, no pin.
if [ $# -eq 0 ]; then
    echo "usage: a76_slot.sh --check | <cmd> [args...]" >&2
    exit 2
fi

if [ "$1" = "--check" ]; then
    pgrep -x light_ai > /dev/null 2>&1
    rc=$?
    if [ "$rc" -ne 1 ]; then   # 1: no such process; 0: found; anything else: pgrep itself failed
        echo "a76_slot: light_ai is running, or pgrep failed (rc=$rc): stop it first" >&2
        exit 1
    fi
    # list-units, not is-active: the systemd of Debian 11 does not expand a glob there
    if ! active_units=$(systemctl list-units --type=service --state=active,activating,reloading,deactivating --no-legend --no-pager \
                        'light_ai*.service' 2> /dev/null); then
        echo "a76_slot: systemctl list-units failed, cannot tell whether a light_ai service runs" >&2
        exit 1
    fi
    if [ -n "$active_units" ]; then
        echo "a76_slot: active service(s): $active_units" >&2
        exit 1
    fi
    for c in 4 5 6 7; do
        if ! taskset -c "$c" true 2> /dev/null; then
            echo "a76_slot: CPU $c blocked by cpuset (run: sudo -n ./scripts/isolate_rt.sh off)" >&2
            exit 1
        fi
    done
    exit 0
fi

# The CMake cache of a build tree keeps this launcher after a BUILD_ON_A76=1 run. A later manual cmake --build, make
# or IDE build runs no preflight, so it must not reach the A76 cores.
if [ "${BUILD_ON_A76:-0}" != "1" ]; then
    exec "$@"
fi

# the lock files stay inside the project (build/), never in /tmp
LOCK_DIR="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd -P)/build"
mkdir -p "$LOCK_DIR" || exit 1
while :; do
    for cpu in 4 5 6 7; do
        # a descriptor picked by bash: fd 9 is taken by the run lock of tidy_run.sh, which the jobs inherit
        exec {fd}> "$LOCK_DIR/.a76_slot_$cpu" || exit 1
        flock -n "$fd"
        rc=$?
        if [ "$rc" -eq 0 ]; then
            # "--" before the CPU list: taskset of util-linux 2.36 takes one after it as the command to run
            exec taskset -c -- "$cpu" "$@"
        fi
        exec {fd}>&-   # a busy slot: close at once, or the waiting loop runs out of descriptors
        if [ "$rc" -ne 1 ]; then   # 1: the lock is held; anything else will not get better by waiting
            echo "a76_slot: flock failed (rc=$rc) on $LOCK_DIR/.a76_slot_$cpu" >&2
            exit 1
        fi
    done
    sleep 0.05
done
