# storage_guard.sh: sourced by build.sh and tidy_run.sh (#48), never run on its own.
# storage_guard <label> <dir>: prints the filesystem of <dir> and WARNS -- never fails, so a fresh machine
# still builds -- when <dir> is on the eMMC: the filesystem of / or any /dev/mmcblk* device (on the board
# /userdata, the old home of the checkout, is a second eMMC partition, so "the same as /" alone misses it).
# Every write of the gate wears the eMMC; the checkout belongs on the NVMe (README, Build, "Where the files live").
# findmnt, not df: df names the root device /dev/root, findmnt the real partition.
storage_guard() {
  local label=$1 dir=$2 src tgt root
  while [ ! -e "$dir" ] && [ "$dir" != / ]; do dir=$(dirname "$dir"); done   # not created yet: its parent
  read -r src tgt < <(findmnt -n -o SOURCE,TARGET -T "$dir" 2>/dev/null)
  root=$(findmnt -n -o SOURCE / 2>/dev/null)
  echo "$label: output $dir on ${src:-?} (${tgt:-?})"
  if [ -n "$src" ] && { [ "$src" = "$root" ] || [[ $src == /dev/mmcblk* ]]; }; then
    echo "$label: WARNING: $dir is on the eMMC ($src): every build and gate run wears it." \
         "Move the checkout to the NVMe: README, Build, \"Where the files live\"."
  fi
}
