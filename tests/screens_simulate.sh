#!/usr/bin/env bash
# screens_simulate.sh - the screens of one simulated test run, in the order
# run.sh shows them, from the real sd/screens.sh + apps.conf (make screens-sim).
#
# usage: screens_simulate.sh <pngondemand> <out_dir> <pre_s> <post_s> <final_s> <app:verdict:rc:secs:fails>...
#
# Each app is given as app:verdict:exit code:run time:FAIL lines, e.g.
# hello4:CRASH:139:23:0. Writes <out_dir>/NN_<screen>.png (+ .layout).
set -euo pipefail

if [[ $# -lt 6 ]]; then
  echo "usage: $0 <pngondemand> <out_dir> <pre_s> <post_s> <final_s> <app:verdict:rc:secs:fails>..." >&2
  exit 1
fi
PNGOD=$1
OUT=$2
PRE_S=$3
POST_S=$4
FINAL_S=$5
shift 5
RUNS=("$@")
SDPATH=sd
APPS=""
for r in "${RUNS[@]}"; do APPS="$APPS ${r%%:*}"; done
RESULTS=""
RC=0
SECS=0
FAILS=0

# shellcheck source=sd/screens.sh
. sd/screens.sh

rm -rf "$OUT"
mkdir -p "$OUT"
n=0
render() { # <name> <screen_* function> [args]
  local name
  name=$(printf '%02d_%s' "$n" "$1")
  shift
  "$@" >"$OUT/$name.layout"
  "$PNGOD" -s "$OUT/$name.layout" "$OUT/$name.png"
  echo "  $name.png"
  n=$((n + 1))
}

render start screen_start
i=0
nok=0
for r in "${RUNS[@]}"; do
  IFS=: read -r app verdict RC SECS FAILS <<<"$r"
  render "next_$app" screen_next "$i" "$app"
  RESULTS="$RESULTS $verdict"
  [[ $verdict == OK ]] && nok=$((nok + 1))
  render "${app}_$verdict" screen_result "$i" "$app" "$verdict"
  i=$((i + 1))
done
render "done_$nok" screen_done "$nok"
echo "$n screens -> $OUT/"
