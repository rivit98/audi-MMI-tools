# screens.sh - run.sh's status screens, as layouts for bin/pngondemand
# (the layout language: src/pngondemand/README.md). Sourced by run.sh on
# the car (ksh), so echo without backslashes (ksh's echo expands them).
#
# Each screen_* function prints one layout on stdout; run.sh renders it
# right before showing it, so the screens can say what actually happened:
#   screen_start                  first: what the run is, don't pull the card
#   screen_result <i> <app> <verdict>
#                                 after the i-th app (0-based); uses RC,
#                                 SECS and FAILS
# They read APPS, PRE_S, POST_S and SDPATH (for apps.conf). There is no
# "Next" screen before an app, no final "all done" screen and no run order
# on the screens: the apps show their own instructions.

# app_info <app>: ARGS, TIMEOUT, TITLE and TEXT from apps.conf
app_info() {
    ARGS=""; TIMEOUT=120; TITLE="(not in apps.conf)"; TEXT=""
    [ -f "${SDPATH}/apps.conf" ] || return 0
    while IFS='|' read -r _n _t _a _ti _tx; do
        [ "${_n}" = "$1" ] || continue
        ARGS=${_a}; TITLE=${_ti:-${_n}}; TEXT=${_tx}
        [ -n "${_t}" ] && TIMEOUT=${_t}
    done < "${SDPATH}/apps.conf"
    return 0
}

# verdict_info <verdict> <app>: VCOL (colour name) and VWHY (what it means)
verdict_info() {
    case $1 in
    OK)      VCOL=g; VWHY="exit code 0, no FAIL lines in its output" ;;
    WARN)    VCOL=y; VWHY="exit code 0, but its output has FAIL lines" ;;
    FAIL)    VCOL=r; VWHY="it exited with a non-zero exit code" ;;
    CRASH)   VCOL=r; VWHY="killed by a signal: it crashed" ;;
    TIMEOUT) VCOL=y; VWHY="it ran over its ${TIMEOUT} s timeout and was stopped" ;;
    MISSING) VCOL=d; VWHY="the program is not on the card" ;;
    *)       VCOL=d; VWHY="" ;;
    esac
}

# The frame every screen shares, with <footer> in the bar at the bottom.
_frame() {
    echo "colour bg 101820"
    echo "colour w E8E8E8"
    echo "colour d 8A929E"
    echo "colour c 5AC8FA"
    echo "colour g 4CD964"
    echo "colour r FF5A50"
    echo "colour y FFB020"
    echo "bg bg"
    echo "rect 0 0 800 6 c"
    echo "footer 44 small 1C2836 w $1"
    echo "at 20"
}
_big() { echo "line big $1 $2"; echo "gap 6"; }   # <colour> <text>
_ln()  { echo "line small $1 ${2:-}"; }             # <colour> [<text>]

_napps() { _N=0; for _a in ${APPS}; do _N=$((_N + 1)); done; }

screen_start() {
    _napps
    _frame "first test in ${PRE_S} s"
    _big c "MMI 3G+ native app tests"
    echo "para small w ${_N} test programs run one after another, each followed by a screen with its result. This takes a few minutes."
    _ln w
    _ln y "Do not remove the SD card until the last result screen has gone."
}

screen_result() {   # <i> <app> <verdict>
    _napps
    app_info "$2"
    verdict_info "$3" "$2"
    if [ $(($1 + 1)) -lt "${_N}" ]; then
        _frame "next test in ${POST_S} s"
    else
        _frame "back to the MMI in ${POST_S} s"
    fi
    _big "${VCOL}" "$2: $3"
    _ln "${VCOL}" "${VWHY}"
    _ln w "exit code ${RC}, ran ${SECS} s, ${FAILS} FAIL lines"
    _ln d "${TITLE}"
}
