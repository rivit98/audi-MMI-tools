# screens.sh - run.sh's status screens, as layouts for bin/pngondemand
# (the layout language: src/pngondemand/README.md). Sourced by run.sh on
# the car (ksh), so echo without backslashes (ksh's echo expands them).
#
# Each screen_* function prints one layout on stdout; run.sh renders it
# right before showing it, so the screens can say what actually happened:
#   screen_start                  first: what the run is, don't pull the card
#   screen_next <i> <app>         before the i-th app (0-based)
#   screen_result <i> <app> <verdict>
#                                 after it; uses RC, SECS and FAILS
#   screen_done <k>               at the end: k of the apps were OK
# They read APPS, PRE_S, POST_S, FINAL_S, SDPATH (for apps.conf) and
# RESULTS (the verdicts so far, in APPS order).

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

# _order <ndone> <cur>: the run order. The first ndone apps with their
# verdict (from RESULTS), app number cur (0-based) highlighted.
_order() {
    _ln d "Run order:"
    _i=0
    for _a in ${APPS}; do
        _p=${_a}
        while [ ${#_p} -lt 12 ]; do _p="${_p} "; done
        if [ "${_i}" -lt "$1" ]; then
            _v=""; _j=0
            for _r in ${RESULTS}; do
                [ "${_j}" = "${_i}" ] && _v=${_r}
                _j=$((_j + 1))
            done
            verdict_info "${_v}" "${_a}"
            _ln "${VCOL}" "    ${_p} ${_v:-done}"
        elif [ "${_i}" = "$2" ]; then
            _ln c "  > ${_p} now"
        else
            _ln w "    ${_a}"
        fi
        _i=$((_i + 1))
    done
}

screen_start() {
    _napps
    _frame "first test in ${PRE_S} s"
    _big c "MMI 3G+ native app tests"
    echo "para small w ${_N} test programs run one after another, each with a screen before it (what it does, what you should see) and one after it (the result). This takes a few minutes."
    _ln w
    _ln y "Do not remove the SD card until the \"all done\" screen has gone."
    _ln w
    _order 0 -1
}

screen_next() {   # <i> <app>
    _napps
    app_info "$2"
    _frame "$2 starts in ${PRE_S} s"
    _big c "Next: $2"
    _ln c "${TITLE}"
    echo "para small w ${TEXT}"
    _ln w
    echo "para small d runs: $2 <sd> ${ARGS}"
    _ln d "timeout: ${TIMEOUT} s"
    _ln w
    _order "$1" "$1"
}

screen_result() {   # <i> <app> <verdict>
    _napps
    app_info "$2"
    verdict_info "$3" "$2"
    if [ $(($1 + 1)) -lt "${_N}" ]; then
        _frame "next app in ${POST_S} s"
    else
        _frame "results in ${POST_S} s"
    fi
    _big "${VCOL}" "$2: $3"
    _ln "${VCOL}" "${VWHY}"
    _ln w "exit code ${RC}, ran ${SECS} s, ${FAILS} FAIL lines"
    _ln d "${TITLE}"
    _ln w
    _order $(($1 + 1)) -1
}

screen_done() {   # <k>
    _napps
    if [ "$1" = "${_N}" ]; then _c=g; elif [ "$1" -gt 0 ]; then _c=y; else _c=r; fi
    _frame "back to the MMI in ${FINAL_S} s"
    _big c "All done"
    _big "${_c}" "$1 of ${_N} apps OK"
    _ln w
    _order "${_N}" -1
}
