#!/bin/bash
# sync_sd.sh - copy build/sd/ (make sd) to a mounted SD card, changes only.
#
#   tools/sync_sd.sh /media/$USER/CARD
#
# rsync -rt: FAT has no owners or modes, so only files and times;
# --modify-window=1: FAT keeps times to 2 s; --delete: files no longer in
# build/sd/ go from the card too, except var/ (the car's logs: copy it off
# first). make sd copies with cp -p, so unchanged files keep their times and
# are skipped. Runs on the host: the card is mounted here, not in Docker.
set -euo pipefail

if [[ $# -ne 1 || -z "$1" ]]; then
  echo "usage: $0 <sd card mount point>" >&2
  exit 2
fi
card="${1%/}"
card="${card:-/}"
root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/build/sd"

if [[ ! -f "$src/run.sh" ]]; then
  echo "$src: no card layout, run make sd first" >&2
  exit 1
fi
if [[ ! -d "$card" ]]; then
  echo "$card: not a directory" >&2
  exit 1
fi

# Only the root of a mounted FAT file system: --delete must never run on a
# wrong path (a typo, the home directory, a card that isn't mounted).
card_real="$(cd "$card" && pwd -P)"
target="$(findmnt -n -o TARGET --target "$card_real" || true)"
fstype="$(findmnt -n -o FSTYPE --target "$card_real" || true)"
if [[ -z "$target" || "$target" != "$card_real" ]]; then
  echo "$card: not a mount point (it is inside '$target'): give the card's root" >&2
  exit 1
fi
case "$fstype" in
vfat | msdos | exfat) ;;
*)
  echo "$card: file system '$fstype', not FAT: is this the SD card?" >&2
  exit 1
  ;;
esac

rsync -rt --modify-window=1 --delete --exclude=/var/ --itemize-changes --stats \
  "$src/" "$card_real/"
echo "syncing ..."
sync
echo "done: $card is up to date; unmount it before pulling the card"
