#!/bin/ksh
# run.sh - executed by copie_scr.sh (core/copie_scr_plain.sh) on SD insert:
#     cd $SDPATH; exec ksh ./run.sh $SDPATH
#
# Runs every test app in APPS one after another (filled in by `make sd`,
# settings per app in apps.conf). First screens/start.png for PRE_S
# seconds (what this is, don't pull the card), then for each app:
#   1. screens/next_<app>.png for PRE_S seconds (the wait before it)
#   2. the app, with stdout + stderr captured in var/<app>_out.log and its
#      exit code, run time and verdict appended there as "=== " lines.
#      It is stopped (TERM, then KILL) if it runs longer than its timeout.
#   3. screens/<app>_<verdict>.png for POST_S seconds
# then screens/done_<number of OK apps>.png for FINAL_S seconds. The
# verdicts are also in var/summary.txt.
#
# The screens are PNGs pre-rendered by sdk/mkscreens.py and shown with
# bin/showScreen -t (core/bin/showScreen, DrGER's tool, proven on the car).
# If showScreen or a PNG is missing, or showScreen returns early, the wait
# is made up with sleep, so the timing stays the same.
#
# Binaries are copied to /tmp before running: the FAT32 card may not carry
# an exec bit, and the card can be pulled while a program runs.
#
# After each app it collects crash evidence onto the card:
#  - the system log (sloginfo) -> var/<app>_slog.txt: the Audi dumper writes
#    a "run fault ... ip" line and a backtrace there when a process crashes
#  - core files: the dumper (-d /HBpersistence/core -d /mnt/persistence,
#    last resort /tmp) writes <name>.core*, gzip-compressed. They are moved
#    to the card so they don't use up the unit's flash (efs-persist).
APPS="@APPS@"
PRE_S=@PRE_S@          # screen before each app
POST_S=@POST_S@        # result screen after each app
FINAL_S=@FINAL_S@      # final screen
KILL_GRACE=15          # after TERM on timeout: the apps' own teardown limit is 10 s
SDPATH="${1:-${0%/*}}"
VAR="${SDPATH}/var"
SCREENS="${SDPATH}/screens"
BIN=/tmp/rivit_app
LOG="${VAR}/rivit_app.log"
SUMMARY="${VAR}/summary.txt"
mkdir -p "${VAR}"

# app_info <app>: ARGS and TIMEOUT from apps.conf (its description is
# only used for the screens)
app_info() {
    ARGS=""; TIMEOUT=120
    [ -f "${SDPATH}/apps.conf" ] || return
    while IFS='|' read -r n t a d; do
        [ "$n" = "$1" ] || continue
        ARGS=$a
        [ -n "$t" ] && TIMEOUT=$t
    done < "${SDPATH}/apps.conf"
}

# run_limited <timeout> <out_file> <cmd> [args...]
# Runs cmd in the background with stdout + stderr appended to out_file. A
# watcher sends TERM after <timeout> s (or when the card is pulled) and
# KILL KILL_GRACE s later. Sets RC, SECS (wall time) and TIMED_OUT (0/1).
run_limited() {
    _limit=$1; _out=$2; shift 2
    rm -f "${BIN}/timed_out"
    _t0=${SECONDS}
    "$@" >> "${_out}" 2>&1 &
    _pid=$!
    (
        t=0
        while [ $t -lt ${_limit} ] && [ -d "${SDPATH}" ]; do
            sleep 1
            t=$((t + 1))
        done
        kill -TERM ${_pid} 2>/dev/null || exit 0
        : > "${BIN}/timed_out"                 # (QNX has no touch)
        echo "$1: stopped after ${_limit} s (TERM)"
        sleep ${KILL_GRACE}
        kill -KILL ${_pid} 2>/dev/null && echo "$1: still running, sent KILL"
    ) &
    _watch=$!
    wait ${_pid}
    RC=$?
    kill ${_watch} 2>/dev/null
    SECS=$((SECONDS - _t0))
    TIMED_OUT=0
    [ -f "${BIN}/timed_out" ] && TIMED_OUT=1
}

# show <png> <seconds>: the screen for that long (blocking)
show() {
    _s0=${SECONDS}
    if [ -x "${BIN}/showScreen" ] && [ -f "${SCREENS}/$1" ]; then
        run_limited $(($2 + 10)) "${VAR}/showscreen.log" \
            "${BIN}/showScreen" -t "$2" "${SCREENS}/$1"
        echo "showScreen $1: exit ${RC}"
    else
        echo "no showScreen or ${SCREENS}/$1"
    fi
    _left=$(($2 - (SECONDS - _s0)))
    [ ${_left} -gt 0 ] && sleep ${_left}
}

run_app() {   # run_app <app>
    APP=$1
    app_info "${APP}"
    OUT="${VAR}/${APP}_out.log"

    show "next_${APP}.png" ${PRE_S}

    echo "--- ${APP} ${ARGS}  (timeout ${TIMEOUT} s)"
    : > "${OUT}"
    if [ -f "${BIN}/${APP}" ]; then
        run_limited ${TIMEOUT} "${OUT}" "${BIN}/${APP}" "${SDPATH}" ${ARGS}
        [ -d "${SDPATH}" ] || exit 1               # card pulled: stop everything
        FAILS=$(grep -c FAIL "${OUT}" 2>/dev/null)
        [ -n "${FAILS}" ] || FAILS=0
        if [ ${TIMED_OUT} = 1 ]; then
            VERDICT=TIMEOUT; DETAIL="stopped after ${TIMEOUT} s"
        elif [ ${RC} -gt 128 ]; then
            VERDICT=CRASH; DETAIL="signal $((RC - 128))"
        elif [ ${RC} -ne 0 ]; then
            VERDICT=FAIL; DETAIL="exit ${RC}"
        elif [ ${FAILS} -gt 0 ]; then
            VERDICT=WARN; DETAIL="exit 0"
        else
            VERDICT=OK; DETAIL="exit 0"
        fi
    else
        RC=127; SECS=0; FAILS=0; VERDICT=MISSING; DETAIL="no binary"
        echo "no ${BIN}/${APP}" > "${OUT}"
    fi
    {
        echo "=== exit code: ${RC}$([ ${RC} -gt 128 ] && echo " (killed by signal $((RC - 128)))")"
        echo "=== run time: ${SECS} s"
        echo "=== verdict: ${VERDICT} (${DETAIL}, ${FAILS} FAIL lines)"
    } >> "${OUT}"
    echo "${APP} exit code: ${RC}  verdict: ${VERDICT} (${DETAIL})  ${SECS} s"
    echo "${APP} ${VERDICT} ${DETAIL}, ${SECS} s, ${FAILS} FAIL lines" >> "${SUMMARY}"
    [ ${VERDICT} = OK ] && NOK=$((NOK + 1))

    sloginfo > "${VAR}/${APP}_slog.txt" 2>&1
    for f in /HBpersistence/core/"${APP}".* /mnt/persistence/"${APP}".* /tmp/"${APP}".*; do
        [ -f "$f" ] || continue
        echo "core file: $f"
        cp "$f" "${VAR}/" && rm -f "$f"
    done
    sync

    show "${APP}_${VERDICT}.png" ${POST_S}
}

{
    echo "=== rivit_app run.sh  $(date)  SDPATH=${SDPATH}  APPS=${APPS}"
    uname -a
    rm -rf "${BIN}" && mkdir -p "${BIN}"
    cp "${SDPATH}"/bin/* "${BIN}/" && chmod 755 "${BIN}"/*
    rm -f "${SUMMARY}" "${VAR}/showscreen.log"

    NOK=0
    show start.png ${PRE_S}
    for A in ${APPS}; do
        run_app "${A}"
    done
    show "done_${NOK}.png" ${FINAL_S}

    rm -rf "${BIN}"
    sync
} > "${LOG}" 2>&1
