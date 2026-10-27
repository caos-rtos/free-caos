# stlink_pick.sh - pick the ST-LINK whose board carries a given MCU.
# Sourced by prog.sh and hw_test.sh, not run on its own.
#
# With ONE probe connected nothing changes: the caller gets an empty answer
# and talks to "the" probe as it always did. With TWO or more (a bridge board
# next to the one under test, the two halves of ex65) the first probe found
# could be either: STM32_Programmer_CLI -l st lists every probe with its serial
# and the board name the ST-LINK firmware declares (NUCLEO-H563ZI ...), without
# connecting to any MCU, and the board whose name holds the MCU number wins.
#
#   stlink_pick <mcu> [cli]   mcu: H563, h563, h563_example_62 ...
#     stdout: the serial to pass as sn=, empty if one probe or none listed
#     return: 0 = decided, 1 = ambiguous or no match (message on stderr)
#
# CAOS_STLINK_SN set by the user always wins: two boards with the same MCU
# cannot be told apart by name.

# STM32_PRG_PATH -> stm32_prg_cli, the default [cli] of the functions below
. "$(dirname "${BASH_SOURCE[0]}")/stm32_prg.sh"

stlink_pick() {
    local mcu="$1"
    local cli="${2:-$(stm32_prg_cli 2>/dev/null)}"

    if [ -n "${CAOS_STLINK_SN:-}" ]; then
        echo "$CAOS_STLINK_SN"
        return 0
    fi
    [ -x "$cli" ] || return 0

    # "SN|board" for each probe, from the probe list section only: the UART
    # section that follows repeats the board names. A probe busy with another
    # program is listed WITHOUT its board name: it still counts as a probe
    # (board "?"), or two probes would look like one and the old "first
    # found" behaviour would come back exactly when it is most dangerous.
    local pairs
    pairs="$(timeout 20 "$cli" -l st 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '
        /Connected ST-LINK Probes List/ { f = 1; next }
        f && /^=====/                   { exit }
        f && /ST-LINK SN/  { if (sn != "") print sn "|?"; sn = $NF }
        f && /Board Name/  { b = $0; sub(/.*: */, "", b); print sn "|" b; sn = "" }
        END                { if (sn != "") print sn "|?" }')"

    local n
    n=$(printf '%s\n' "$pairs" | grep -c '|')
    [ "$n" -eq 0 ] && return 0

    local num
    num=$(printf '%s' "$mcu" | grep -oiP '^[a-z]\K[0-9]{3}' | head -1)

    # One probe: still the board must be the one asked for. Without this an
    # H753 elf went into the only board connected, a U575 (25/10): the
    # programmer takes any image at 0x08000000, the board then stays silent
    # and the run reads as "no banner". A name that cannot be read ("?",
    # probe busy) or a board whose name holds no MCU number keeps the old
    # behaviour.
    if [ "$n" -eq 1 ]; then
        local board="${pairs#*|}"
        if [ -n "$num" ] && [ "$board" != "?" ] && \
           printf '%s' "$board" | grep -qE '[0-9]{3}' && \
           ! printf '%s' "$board" | grep -q -- "$num"; then
            echo "ERROR: the only ST-LINK connected is on ${board}, not on ${mcu}." >&2
            echo "       Connect the ${mcu} board, or set CAOS_STLINK_SN=<serial> to force it." >&2
            return 1
        fi
        return 0
    fi
    local hits h
    hits="$(printf '%s\n' "$pairs" | grep -- "$num")"
    h=$(printf '%s\n' "$hits" | grep -c '|')
    if [ -n "$num" ] && [ "$h" -eq 1 ]; then
        echo "${hits%%|*}"
        return 0
    fi

    echo "ERROR: $n ST-LINK connected and ${h} carry ${mcu}: which one to use?" >&2
    printf '%s\n' "$pairs" | sed 's/|/   /; s/^/         /' >&2
    echo "       Disconnect the others, or set CAOS_STLINK_SN=<serial>." >&2
    return 1
}

# The OTHER board's serial when exactly two ST-LINK are connected: the
# companion of a two-board test (ex65's slave, ex62's bridge). Its console is
# /dev/serial/by-id/*<serial>-if02. Empty with one probe, or three and more
# (no way to tell which one is the companion).
#
#   stlink_peer <picked serial> [cli]
stlink_peer() {
    local mine="$1"
    local cli="${2:-$(stm32_prg_cli 2>/dev/null)}"
    [ -n "$mine" ] && [ -x "$cli" ] || return 0
    local sns
    sns="$(timeout 20 "$cli" -l st 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '
        /Connected ST-LINK Probes List/ { f = 1; next }
        f && /^=====/                   { exit }
        f && /ST-LINK SN/               { print $NF }')"
    [ "$(printf '%s\n' "$sns" | grep -c .)" -eq 2 ] || return 0
    printf '%s\n' "$sns" | grep -v -- "$mine" | head -1
}
