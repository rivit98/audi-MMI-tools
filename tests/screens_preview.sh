#!/usr/bin/env bash
# screens_preview.sh - every status screen run.sh can show, rendered on the
# host from the real sd/screens.sh + sd/apps.conf (make screens-preview).
#
# usage: screens_preview.sh <pngondemand> <out_dir> "<apps>" <pre_s> <post_s> <final_s>
#
# pngondemand runs with -s, so a screen whose text does not fit fails this.
set -euo pipefail

if [[ $# -ne 6 ]]; then
  echo "usage: $0 <pngondemand> <out_dir> \"<apps>\" <pre_s> <post_s> <final_s>" >&2
  exit 1
fi
PNGOD=$1
OUT=$2
APPS=$3
PRE_S=$4
POST_S=$5
FINAL_S=$6
SDPATH=sd
RESULTS=""
# sample values for the result screens
RC=0
SECS=42
FAILS=0
VERDICTS="OK WARN FAIL CRASH TIMEOUT MISSING"

# shellcheck source=sd/screens.sh
. sd/screens.sh

mkdir -p "$OUT"
n=0
render() { # <name> <screen_* function> [args]
  local name=$1
  shift
  "$@" >"$OUT/$name.layout"
  "$PNGOD" -s "$OUT/$name.layout" "$OUT/$name.png"
  n=$((n + 1))
}

render start screen_start
napps=0
for app in $APPS; do
  RESULTS="$(for _ in $(seq 1 "$napps"); do echo OK; done)"
  render "next_$app" screen_next "$napps" "$app"
  for v in $VERDICTS; do
    case $v in
    WARN) RC=0 FAILS=2 ;;
    FAIL) RC=1 FAILS=0 ;;
    CRASH) RC=139 FAILS=0 ;;
    TIMEOUT) RC=143 FAILS=0 ;;
    MISSING) RC=127 FAILS=0 ;;
    *) RC=0 FAILS=0 ;;
    esac
    RESULTS="$RESULTS $v"
    render "${app}_$v" screen_result "$napps" "$app" "$v"
    RESULTS=${RESULTS% *}
  done
  napps=$((napps + 1))
done
for k in $(seq 0 "$napps"); do
  RESULTS="$(for i in $(seq 1 "$napps"); do if [[ $i -le $k ]]; then echo OK; else echo FAIL; fi; done)"
  render "done_$k" screen_done "$k"
done
echo "$n screens -> $OUT/ (layouts: *.layout)"
