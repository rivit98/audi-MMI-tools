#!/bin/ksh
# run.sh - executed by copie_scr.sh (core/copie_scr_plain.sh) on SD insert:
#     cd $SDPATH; exec ksh ./run.sh $SDPATH
#
# Copies bin/ (the runner and the unit tools the apps use) to /tmp/mmi_apps
# and starts the runner, the knob-driven menu of the apps in apps/
# (src/runner/README.md). The runner runs each app, with its timeout,
# verdict, var/summary.txt line, slog and core files. It ends with "Leave
# the menu" or when the card is pulled.
#
# Binaries run from /tmp: the FAT32 card may not carry an exec bit, and the
# card can be pulled while a program runs. Everything a run writes goes to
# var/ on the card (the apps run with var/ as cwd).
SDPATH="${1:-${0%/*}}"
case "${SDPATH}" in /*) ;; *) SDPATH=$(cd "${SDPATH}" && pwd) ;; esac   # we cd away
VAR="${SDPATH}/var"
BIN=/tmp/mmi_apps
mkdir -p "${VAR}"
cd "${VAR}" || exit 1

{
    echo "=== run.sh  $(date)  SDPATH=${SDPATH}"
    uname -a
    rm -rf "${BIN}" && mkdir -p "${BIN}"
    cp "${SDPATH}"/bin/* "${BIN}/" && chmod 755 "${BIN}"/*
    rm -f "${VAR}/summary.txt"

    "${BIN}/runner" "${SDPATH}" > "${VAR}/runner_out.log" 2>&1
    RC=$?
    echo "runner exit code: ${RC}"
    # a crashed runner: its slog line and core file, if the card is still in
    if [ ${RC} -ne 0 ] && [ -d "${SDPATH}" ]; then
        sloginfo > "${VAR}/runner_slog.txt" 2>&1
        for f in /HBpersistence/core/runner.* /mnt/persistence/runner.* /tmp/runner.*; do
            [ -f "$f" ] || continue
            echo "core file: $f"
            cp "$f" "${VAR}/" && rm -f "$f"
        done
    fi

    rm -rf "${BIN}"
} > "${VAR}/run.log" 2>&1
