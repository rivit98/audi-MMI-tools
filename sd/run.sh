#!/bin/ksh
# run.sh - executed by copie_scr.sh (core/copie_scr_plain.sh) on SD insert:
#     cd $SDPATH; exec ksh ./run.sh $SDPATH
#
# Runs every test app in APPS one after another (filled in by `make sd`,
# settings per app in apps.conf). First the start screen for PRE_S
# seconds (what this is, don't pull the card), then for each app:
#   1. its "Next" screen for PRE_S seconds (the wait before it)
#   2. the app, with stdout + stderr captured in var/<app>_out.log and its
#      exit code, run time and verdict appended there as "=== " lines.
#      It is stopped (TERM, then KILL) if it runs longer than its timeout.
#   3. its result screen for POST_S seconds
# then the "all done" screen for FINAL_S seconds. The verdicts are also in
# var/summary.txt.
#
# The screens are rendered when they are needed: screens.sh prints the
# layout, bin/pngondemand turns it into a PNG in /tmp, and bin/showScreen -t
# (core/bin/showScreen, DrGER's tool, proven on the car) shows it. If any
# of that fails, or showScreen returns early, the wait is made up with
# sleep, so the timing stays the same.
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
BIN=/tmp/rivit_app
SCREEN="${BIN}/screen"     # screen.layout -> screen.png, rendered by show
LOG="${VAR}/rivit_app.log"
SUMMARY="${VAR}/summary.txt"
mkdir -p "${VAR}"

# app_info (ARGS, TIMEOUT from apps.conf) and the screen_* layouts. Without
# it the apps still run (defaults), only the screens are missing.
[ -f "${SDPATH}/screens.sh" ] && . "${SDPATH}/screens.sh"

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

# show <seconds> <screen_* function> [args]: render that screen and show it
# for that long (blocking). The layout is made first, while RC, SECS and
# FAILS are still the app's (run_limited sets them again for showScreen).
show() {
    _s0=${SECONDS}; _secs=$1; shift
    rm -f "${SCREEN}.png"
    "$@" > "${SCREEN}.layout"
    "${BIN}/pngondemand" "${SCREEN}.layout" "${SCREEN}.png" || echo "pngondemand $*: exit $?"
    if [ -x "${BIN}/showScreen" ] && [ -f "${SCREEN}.png" ]; then
        run_limited $((_secs + 10)) "${VAR}/showscreen.log" \
            "${BIN}/showScreen" -t "${_secs}" "${SCREEN}.png"
        echo "showScreen $*: exit ${RC}"
    else
        echo "no showScreen or no screen for $*"
    fi
    _left=$((_secs - (SECONDS - _s0)))
    [ ${_left} -gt 0 ] && sleep ${_left}
}

run_app() {   # run_app <i> <app>
    I=$1; APP=$2
    app_info "${APP}" || { ARGS=""; TIMEOUT=120; }    # (no screens.sh)
    OUT="${VAR}/${APP}_out.log"

    show ${PRE_S} screen_next ${I} "${APP}"

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
    RESULTS="${RESULTS} ${VERDICT}"

    sloginfo > "${VAR}/${APP}_slog.txt" 2>&1
    for f in /HBpersistence/core/"${APP}".* /mnt/persistence/"${APP}".* /tmp/"${APP}".*; do
        [ -f "$f" ] || continue
        echo "core file: $f"
        cp "$f" "${VAR}/" && rm -f "$f"
    done
    sync

    show ${POST_S} screen_result ${I} "${APP}" ${VERDICT}
}

{
    echo "=== rivit_app run.sh  $(date)  SDPATH=${SDPATH}  APPS=${APPS}"
    uname -a
    rm -rf "${BIN}" && mkdir -p "${BIN}"
    cp "${SDPATH}"/bin/* "${BIN}/" && chmod 755 "${BIN}"/*
    rm -f "${SUMMARY}" "${VAR}/showscreen.log"

    NOK=0; RESULTS=""; N=0
    show ${PRE_S} screen_start
    for A in ${APPS}; do
        run_app ${N} "${A}"
        N=$((N + 1))
    done
    show ${FINAL_S} screen_done ${NOK}

    rm -rf "${BIN}"
    sync
} > "${LOG}" 2>&1
