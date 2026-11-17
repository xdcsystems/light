#!/bin/bash
# usb_replug.sh -- software unplug / replug of the USB device behind a V4L2 node,
# for hot-attach tests without pulling the connector (repeated physical
# re-plugging wears the board's USB port).
#
# Mechanism: /sys/bus/usb/devices/<busid>/authorized. Writing 0 disconnects the
# device from every driver -- uvcvideo sees a disconnect, /dev/videoN disappears,
# exactly as on a physical unplug; writing 1 re-enumerates it (the node number
# may change, as after a real re-plug). The camera stays powered: this is a
# logical unplug, not a power cut.
#
# Usage (as root; installed root-owned, see CLAUDE.md "USB camera tests"):
#   usb_replug.sh cycle <dev|busid> [off_seconds]  -- off, wait (default 3 s), on
#   usb_replug.sh off   <dev|busid>                -- prints the busid to re-enable
#   usb_replug.sh on    <busid>
#   usb_replug.sh list                             -- V4L2 nodes and their busids
#
# Input is validated strictly: the script only ever writes 0/1 into the
# `authorized` file of a non-root-hub USB device.

set -euo pipefail

die() { echo "usb_replug: $*" >&2; exit 1; }

# /dev/videoN or videoN -> busid of the USB device (not the interface) behind it.
busid_of_node() {
    local node d
    node=$(basename "$1")
    [[ $node =~ ^video[0-9]+$ ]] || die "not a V4L2 node: $1"
    [ -e "/sys/class/video4linux/$node/device" ] || die "no such node: $1"
    d=$(readlink -f "/sys/class/video4linux/$node/device")
    while [ "$d" != "/" ] && [ ! -f "$d/busnum" ]; do d=$(dirname "$d"); done
    [ "$d" != "/" ] || die "$1 is not a USB device"
    basename "$d"
}

resolve() {
    if [[ $1 =~ ^[0-9]+-[0-9]+(\.[0-9]+)*$ ]]; then
        echo "$1"                      # already a busid (e.g. 3-1, 1-1.2); never a root hub
    else
        busid_of_node "$1"
    fi
}

set_auth() {
    local busid=$1 val=$2 f
    [[ $busid =~ ^[0-9]+-[0-9]+(\.[0-9]+)*$ ]] || die "bad busid: $busid"
    f="/sys/bus/usb/devices/$busid/authorized"
    [ -f "$f" ] || die "no USB device $busid"
    echo "$val" > "$f"
}

cmd=${1:-}
case "$cmd" in
    cycle)
        [ $# -ge 2 ] || die "usage: cycle <dev|busid> [off_seconds]"
        busid=$(resolve "$2"); secs=${3:-3}
        [[ $secs =~ ^[0-9]{1,3}$ ]] || die "bad off_seconds: $secs"
        set_auth "$busid" 0; echo "usb_replug: $busid off for ${secs}s"
        sleep "$secs"
        set_auth "$busid" 1; echo "usb_replug: $busid on" ;;
    off)
        [ $# -eq 2 ] || die "usage: off <dev|busid>"
        busid=$(resolve "$2"); set_auth "$busid" 0
        echo "usb_replug: $busid off (re-enable: usb_replug.sh on $busid)" ;;
    on)
        [ $# -eq 2 ] || die "usage: on <busid>"
        set_auth "$2" 1; echo "usb_replug: $2 on" ;;
    list)
        for n in /sys/class/video4linux/video*; do
            [ -e "$n" ] || continue
            b=$(busid_of_node "$(basename "$n")" 2>/dev/null) || continue
            echo "/dev/$(basename "$n") $b $(cat "$n/name")"
        done ;;
    *)
        sed -n '2,20p' "$0"; exit 2 ;;
esac
