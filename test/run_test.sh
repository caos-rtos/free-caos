#!/bin/bash
# run_test.sh - run the caOS FREE test bench against the prebuilt firmware.
#
# The bench is a HOST program (test/hw_test) that drives the board over the
# ST-Link: it flashes an example from elfs/<MCU>/, talks to its console and
# checks the result. It does NOT build anything - build the firmware first
# with ./do, then run it here. CAOS_FREE=1 is set automatically.
#
# Run it with sudo: the network classes set up the bench link (address, DHCP,
# PC peers) through ./tools/bench_net.sh, which needs root.
#
#   sudo ./test/run_test.sh -m U575 -n ex68   # the ex68 class on a U575
#   sudo ./test/run_test.sh -m F767 -n ex84   # one class, F767
#   sudo ./test/run_test.sh -m H563 -l        # list the classes, run nothing
#   sudo ./test/run_test.sh -m U575 -n ex68 -t 3   # a single test of the class
#   sudo ./test/run_test.sh -m U575 -n ex68 -r     # + report <date>_U575.xml/.html
#
# Options:
#   -m MCU    MCU of the run (F446 F767 H563 H753 U575). The elfs must already
#             exist in elfs/<MCU>/ (build them with ./do). Exported as CAOS_MCU.
#   -r [NAME] write the report NAME.xml + NAME.html; without NAME (or when an
#             option follows) the name is <YYYY-MM-DD_HHMM>_<MCU>
#   -h        this help
# Everything else is passed to the bench as is (-n <class>, -t <n>, -l, ...).
#
# Environment:
#   CAOS_TTY   console device, if autodetection cannot pick it
#   CAOS_MCU   MCU of the run (same as -m)
#   CAOS_NO_PROBE_CHECK=1   skip the "right board?" check
#   CAOS_KEEP_TERM=1        do not close a serial terminal holding the console

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
EXE="$HERE/hw_test"

# FREE firmware: one config for every example. Set here so the bench never has
# to be told, and so it skips the checks that compare against a per-example
# config (there is none in the FREE package).
export CAOS_FREE=1
# The bench and prog.sh resolve paths (elfs/, prog.sh, cpu.cmake) from here.
export CAOS_ROOT="$ROOT"

PROG_CLI=""      # from STM32_PRG_PATH, resolved by check_board()

usage() {
    awk 'NR<3 {next} /^#/ {sub(/^# ?/,""); print; next} {exit}' "${BASH_SOURCE[0]}"
    exit 0
}

mcu_device_ids() {
    case "$1" in
        F103)      echo "0x410 0x412 0x414 0x418 0x420 0x428 0x430" ;;
        F407|F417) echo "0x413" ;;
        F429)      echo "0x419" ;;
        F446)      echo "0x421" ;;
        F746)      echo "0x449" ;;
        F767)      echo "0x451" ;;
        H563)      echo "0x484" ;;
        H747|H753) echo "0x450" ;;   # same ID: told apart by the Board line
        U575)      echo "0x482" ;;
        *)         echo "" ;;
    esac
}

ARGS=()
REPORT=""
REPORT_DEFAULT=0
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage ;;
        -m)        [ $# -ge 2 ] || { echo "-m wants an MCU, e.g. -m U575" >&2; exit 1; }
                   CAOS_MCU="${2^^}"; export CAOS_MCU; shift 2 ;;
        -r)        if [ $# -ge 2 ] && [ "${2#-}" = "$2" ]; then
                       REPORT="$2"; shift 2
                   else
                       REPORT_DEFAULT=1; shift      # named below, once the MCU is known
                   fi ;;
        *)         ARGS+=("$1"); shift ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "run_test: WARNING: not root - the network classes cannot set up the" >&2
    echo "          bench link (./tools/bench_net.sh). Run: sudo $0 ..." >&2
fi

if [ ! -x "$EXE" ]; then
    echo "ERROR: bench executable not found: $EXE" >&2
    exit 1
fi

# The MCU: -m/CAOS_MCU, or cpu.cmake. It decides which elfs/ dir is used.
if [ -n "${CAOS_MCU:-}" ]; then
    MCU="${CAOS_MCU^^}"; MCU_SRC="-m/CAOS_MCU"
else
    MCU="$(grep -oP 'set\(CURR_MCU "\K[A-Z0-9]+' "$ROOT/cpu.cmake" 2>/dev/null)"
    MCU_SRC="cpu.cmake"
fi
[ -n "$MCU" ] && export CAOS_MCU="$MCU"
# -r without a name: <date>_<MCU>
[ "$REPORT_DEFAULT" = 1 ] && REPORT="$(date +%Y-%m-%d_%H%M)_${MCU:-MCU}"

PROBE_SN=""
BOARD_SEEN=""

# Is the connected board the one $MCU asks for? Also finds the probe serial,
# which names the console stably (ttyACM numbering is not fixed across boots).
check_board() {
    [ -n "$MCU" ] || return 0
    [ "${CAOS_NO_PROBE_CHECK:-0}" = "1" ] && return 0
    [ "${CAOS_PROBE:--s}" = "-j" ] && return 0
    . "$ROOT/tools/stm32_prg.sh"
    PROG_CLI="$(stm32_prg_cli)" || return 1

    local want out id board
    want="$(mcu_device_ids "$MCU")"
    [ -n "$want" ] || return 0

    # One or more ST-Links: pick the one whose board carries $MCU.
    if [ -f "$ROOT/tools/stlink_pick.sh" ]; then
        . "$ROOT/tools/stlink_pick.sh"
        local sn
        sn="$(stlink_pick "$MCU" "$PROG_CLI")" || return 1
        [ -n "$sn" ] && export CAOS_STLINK_SN="$sn"
    fi

    out="$(timeout 40 "$PROG_CLI" -c port=SWD mode=UR freq=4000 ${CAOS_STLINK_SN:+sn=$CAOS_STLINK_SN} 2>&1 \
           | sed 's/\x1b\[[0-9;]*m//g')"
    id="$(printf '%s' "$out"    | grep -oP 'Device ID\s*:\s*\K0x[0-9A-Fa-f]+' | head -1)"
    board="$(printf '%s' "$out" | grep -oP 'Board\s*:\s*\K.*'                 | head -1 | tr -d '\r')"
    PROBE_SN="$(printf '%s' "$out" | grep -oP 'ST-LINK SN\s*:\s*\K\S+'        | head -1 | tr -d '\r')"

    if [ -z "$id" ]; then
        echo "ERROR: no board answers on the ST-Link probe." >&2
        echo "       $MCU_SRC asks for $MCU. Connect the board, or skip this" >&2
        echo "       check with CAOS_NO_PROBE_CHECK=1." >&2
        return 1
    fi
    if ! printf '%s' "$want" | grep -qw -- "$id"; then
        echo "ERROR: wrong board." >&2
        echo "       $MCU_SRC says $MCU (expected Device ID: $want)" >&2
        echo "       connected    : Device ID $id${board:+  Board $board}" >&2
        return 1
    fi
    local num="${MCU#?}"
    if [ -n "$board" ] && ! printf '%s' "$board" | grep -q -- "$num"; then
        echo "ERROR: the board reports '$board', which is not $MCU." >&2
        echo "       Device ID $id is compatible but ambiguous (H747/H753):" >&2
        echo "       the Board name decides." >&2
        return 1
    fi
    BOARD_SEEN="$board"
    return 0
}

# The console: an explicit CAOS_TTY wins; otherwise the stable by-id symlink of
# the probe (-if02 = the ST-Link VCP); otherwise the only ttyACM, if there is
# just one.
resolve_tty() {
    [ -n "${CAOS_TTY:-}" ] && return 0
    if [ -n "${PROBE_SN:-}" ]; then
        local by_id
        by_id="$(echo /dev/serial/by-id/*"${PROBE_SN}"*-if02)"
        if [ -e "$by_id" ]; then export CAOS_TTY="$by_id"; return 0; fi
    fi
    local acm=(/dev/ttyACM*)
    if [ "${#acm[@]}" -eq 1 ] && [ -e "${acm[0]}" ]; then
        export CAOS_TTY="${acm[0]}"; return 0
    fi
    if [ "${#acm[@]}" -gt 1 ]; then
        echo "ERROR: more than one serial console and no /dev/serial/by-id to" >&2
        echo "       tell them apart: pick one with CAOS_TTY=/dev/ttyACMn." >&2
        return 1
    fi
    return 0
}

# A serial terminal (minicom/picocom/...) left open steals bytes from the bench
# and makes a healthy run look like a firmware fault. Close it (CAOS_KEEP_TERM=1
# to keep it).
check_tty_free() {
    local tty="${CAOS_TTY:-/dev/ttyACM0}"
    [ -e "$tty" ] || return 0
    command -v fuser >/dev/null 2>&1 || return 0
    [ -n "${CAOS_KEEP_TERM:-}" ] && return 0
    fuser -s "$tty" 2>/dev/null || return 0
    local pid comm killed=0
    for pid in $(fuser "$tty" 2>/dev/null); do
        [ "$pid" = "$$" ] && continue
        comm="$(cat /proc/$pid/comm 2>/dev/null)" || continue
        case "$comm" in minicom|picocom|screen|cu|tio) ;; *) continue ;; esac
        echo "run_test: closing $comm (pid $pid) holding $tty" >&2
        kill -TERM "$pid" 2>/dev/null; killed=1
        local i=0
        while [ $i -lt 20 ] && kill -0 "$pid" 2>/dev/null; do sleep 0.05; i=$((i+1)); done
        kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
    done
    [ "$killed" = "1" ] && { sleep 0.2; [ -t 0 ] && stty sane 2>/dev/null; }
    return 0
}

check_board || exit 1
resolve_tty || exit 1
check_tty_free || exit 1

echo "run_test: MCU=${MCU:-?} [$MCU_SRC]${BOARD_SEEN:+ (${BOARD_SEEN})}  console=${CAOS_TTY:-/dev/ttyACM0}  FREE"

# The firmware about to be flashed, read from the elfs (BUILD_VER).
if [ ! -d "$ROOT/elfs/$MCU" ] || ! ls "$ROOT"/elfs/"$MCU"/*.elf >/dev/null 2>&1; then
    echo "ERROR: no firmware in elfs/$MCU: build it first with ./do" >&2
    exit 1
fi
fw_vers="$(strings -a "$ROOT"/elfs/"$MCU"/*.elf 2>/dev/null \
           | grep -E '^[0-9a-f]{7,40}(\+dev)? - [0-9]{4}-' | sort -u)"
[ -n "$fw_vers" ] && echo "$fw_vers" | sed 's/^/firmware: /'

[ -n "$REPORT" ] && echo "report: $REPORT.xml / $REPORT.html"
if [ -n "$REPORT" ]; then
    exec "$EXE" "$REPORT" -f 12 ${ARGS[@]+"${ARGS[@]}"}
fi
exec "$EXE" ${ARGS[@]+"${ARGS[@]}"}
