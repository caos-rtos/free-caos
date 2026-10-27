#!/bin/bash
#
# prog.sh - Flash STM32 microcontroller
#
# Usage:
#   ./prog.sh -j -f elfs/f407.elf     # J-Link + explicit file
#   ./prog.sh -s -f elfs/h753.elf     # ST-Link + explicit file
#   ./prog.sh -s -e 58                # ST-Link + example 58 for current MCU
#   ./prog.sh -s -a 2                 # ST-Link + workspace app_2
#
# Options:
#   -j        Use J-Link (Segger)
#   -s        Use ST-Link
#   -f FILE   ELF file to flash
#   -e N      Flash elfs/<MCU>/<mcu>_example_<N>.elf (MCU from cpu.cmake,
#             or from $CAOS_MCU when set - the bench exports it)
#   -a N      Flash elfs/<MCU>/<mcu>_app_<N>.elf     (workspace/app_N)
#   -n        Quiet: no output, the exit code is the answer. On FAILURE the
#             programmer log is printed anyway - a silent failure would be
#             worse than a noisy one. For scripts and test benches.
#   -h        Show this help
#
# Exit code: 0 only if the device was actually programmed.
#

JLINK=0
STLINK=0
QUIET=0
ELF_FILE=""

# say: normal chatter, silenced by -n. Errors never go through here.
say() {
    [ "$QUIET" -eq 0 ] && echo "$@"
    return 0
}

# Timeout (s) per evitare programmer appesi sul probe (process in stato D che
# tiene il lock ST-Link). Override: PROG_TIMEOUT=60 ./prog.sh ...
PROG_TIMEOUT="${PROG_TIMEOUT:-40}"

# run_prog: lancia il comando con timeout; se scade, avvisa e propaga errore.
# Con -n l'output del programmatore va in un log temporaneo, che viene
# riversato su stderr SOLO se qualcosa e' andato storto: silenziare un
# fallimento renderebbe il flag peggio che inutile.
run_prog() {
    local rc
    if [ "$QUIET" -eq 1 ]; then
        local log
        log=$(mktemp) || return 1
        timeout --kill-after=5 "$PROG_TIMEOUT" "$@" > "$log" 2>&1
        rc=$?
        [ $rc -ne 0 ] && cat "$log" >&2
        rm -f "$log"
    else
        timeout --kill-after=5 "$PROG_TIMEOUT" "$@"
        rc=$?
    fi
    if [ $rc -eq 124 ]; then
        echo "ERROR: programmer hung (>${PROG_TIMEOUT}s) - ST-Link probe stuck." >&2
        echo "       Unplug/replug the ST-Link USB cable and retry." >&2
        echo "       (leftover processes: pkill -9 -f STM32_Programmer_CLI)" >&2
    fi
    PROG_RC=$rc
    return $rc
}

# Parse arguments
EXAMPLE=""
APP=""
while getopts "jsnf:e:a:h" opt; do
    case $opt in
        j) JLINK=1 ;;
        s) STLINK=1 ;;
        n) QUIET=1 ;;
        f) ELF_FILE="$OPTARG" ;;
        e) EXAMPLE="$OPTARG" ;;
        a) APP="$OPTARG" ;;
        h)
            # Il blocco di commento in testa E' l'aiuto: si ferma alla prima
            # riga che non comincia per '#', cosi' non va inseguito a mano
            # ogni volta che si aggiunge un'opzione.
            awk 'NR<3 {next} /^#/ {sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0
            ;;
        *)
            echo "Usage: $0 [-n] -j|-s -f <elf_file> | -e <N> | -a <N>" >&2
            exit 1
            ;;
    esac
done

# -e N / -a N: build the ELF path from CURR_MCU in cpu.cmake. The two trees
# are separate: examples/example_N -> <mcu>_example_N.elf, workspace/app_N ->
# <mcu>_app_N.elf, so the same N never picks the wrong binary.
if [ -n "$EXAMPLE" ] || [ -n "$APP" ]; then
    if [ -n "$EXAMPLE" ] && [ -n "$APP" ]; then
        echo "Error: use either -e or -a, not both" >&2
        exit 1
    fi
    if [ -n "$ELF_FILE" ]; then
        echo "Error: use either -f or -e/-a, not both" >&2
        exit 1
    fi
    if [ -n "${CAOS_MCU:-}" ]; then
        CURR_MCU="${CAOS_MCU^^}"; MCU_SRC="CAOS_MCU"
    else
        CURR_MCU=$(grep -oP 'set\(CURR_MCU\s+"\K[^"]+' "$(dirname "$0")/cpu.cmake")
        MCU_SRC="cpu.cmake"
    fi
    if [ -z "$CURR_MCU" ]; then
        echo "Error: cannot read CURR_MCU from cpu.cmake" >&2
        exit 1
    fi
    MCU_LC=$(echo "$CURR_MCU" | tr '[:upper:]' '[:lower:]')
    if [ -n "$APP" ]; then
        if ! echo "$APP" | grep -qE '^[0-9]+$'; then
            echo "Error: -a takes a number: workspace projects are named app_<N>" >&2
            exit 1
        fi
        ELF_FILE="$(dirname "$0")/elfs/${CURR_MCU}/${MCU_LC}_app_${APP}.elf"
    else
        if ! echo "$EXAMPLE" | grep -qE '^[0-9]+$'; then
            echo "Error: -e takes a number: examples are named example_<N>" >&2
            exit 1
        fi
        ELF_FILE="$(dirname "$0")/elfs/${CURR_MCU}/${MCU_LC}_example_${EXAMPLE}.elf"
    fi
    say "MCU from $MCU_SRC: $CURR_MCU  ->  $ELF_FILE"
fi

# Validate arguments
if [ -z "$ELF_FILE" ]; then
    echo "Error: ELF file required (-f), example (-e N) or app (-a N)" >&2
    echo "Usage: $0 -j|-s -f <elf_file> | -e <N> | -a <N>"
    exit 1
fi

if [ ! -f "$ELF_FILE" ]; then
    echo "Error: File not found: $ELF_FILE" >&2
    exit 1
fi

if [ $JLINK -eq 0 ] && [ $STLINK -eq 0 ]; then
    echo "Error: Select probe type (-j for J-Link, -s for ST-Link)" >&2
    exit 1
fi

if [ $JLINK -eq 1 ] && [ $STLINK -eq 1 ]; then
    echo "Error: Select only one probe type" >&2
    exit 1
fi

# STM32_PRG_PATH -> STM32_Programmer_CLI (not needed by openocd)
. "$(dirname "$0")/tools/stm32_prg.sh"

# Detect MCU from filename
MCU=$(basename "$ELF_FILE" .elf)

say "=== Programming $MCU with $ELF_FILE ==="

if [ $JLINK -eq 1 ]; then
    say "Probe: J-Link (Segger)"

    case $MCU in
        f407*|f429*|f446*)
            # Use openocd for F4xx with J-Link.
            # connect_assert_srst = connect-under-reset: keep the core in reset
            # until the debug port is up. Without it, firmware already on the
            # chip (MPU/clock reconfig, fault, WFI) can block "Could not
            # initialize the debug port" on flaky J-Link/target combos.
            run_prog openocd -f interface/jlink.cfg \
                    -c "transport select swd" \
                    -c "adapter_khz 2000" \
                    -f target/stm32f4x.cfg \
                    -c "reset_config srst_only srst_nogate connect_assert_srst" \
                    -c "program $ELF_FILE verify reset exit"
            ;;
        h563*|h753*|h747*|f103*|u575*)
            # Use STM32CubeProgrammer for H5/H7/U5 (openocd may not support)
            PRG_CLI="$(stm32_prg_cli)" || exit 1
            run_prog "$PRG_CLI" \
                -c port=SWD mode=UR freq=4000 index=0 \
                -d "$ELF_FILE" -v -rst
            ;;
        *)
            echo "Error: Unknown MCU: $MCU" >&2
            exit 1
            ;;
    esac

elif [ $STLINK -eq 1 ]; then
    say "Probe: ST-Link"
    PRG_CLI="$(stm32_prg_cli)" || exit 1

    # Two or more ST-LINK connected (a bridge board, the two halves of ex65):
    # pick the one whose board carries this MCU, never "the first found".
    . "$(dirname "$0")/tools/stlink_pick.sh"
    STLINK_SN="$(stlink_pick "$MCU")" || exit 1
    SN_OPT=""
    if [ -n "$STLINK_SN" ]; then
        SN_OPT="sn=$STLINK_SN"
        say "ST-LINK SN $STLINK_SN (picked for $MCU)"
    fi

    # H5xx/U5xx: erase first then program (TrustZone workaround)
    case $MCU in
        h563*|u575*)
            run_prog "$PRG_CLI" \
                -c port=SWD mode=UR freq=4000 $SN_OPT \
                -e all \
                -d "$ELF_FILE" -v -rst
            ;;
        *)
            run_prog "$PRG_CLI" \
                -c port=SWD mode=UR freq=4000 $SN_OPT \
                -d "$ELF_FILE" -v -rst
            ;;
    esac
fi
# Il codice del programmatore e' l'unica cosa che dice se il device e' stato
# davvero scritto, e va propagato: finora l'ultimo comando dello script era un
# echo, quindi prog.sh usciva 0 ANCHE dopo un fallimento e chi lo chiamava da
# script non poteva accorgersene.
if [ "${PROG_RC:-1}" -ne 0 ]; then
    echo "ERROR: programming FAILED ($MCU, rc=${PROG_RC:-1})" >&2
    exit "${PROG_RC:-1}"
fi

# The reset goes to the probe just programmed, not to the first one found
if [ "$QUIET" -eq 1 ]; then
    CAOS_STLINK_SN="${STLINK_SN:-${CAOS_STLINK_SN:-}}" ./reset_mcu.sh > /dev/null 2>&1
else
    CAOS_STLINK_SN="${STLINK_SN:-${CAOS_STLINK_SN:-}}" ./reset_mcu.sh
fi

say "=== Done ==="
exit 0
