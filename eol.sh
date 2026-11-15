#!/bin/bash
# eol.sh - end of line: the whole test bench on every board, one board at a time.
#
#   ./eol.sh                  the seven boards, in order
#   ./eol.sh H753 U575        only these
#
# For each board it builds the firmware from this package (./bootstrap -m <MCU>
# and ./do), asks you to connect the Nucleo, waits for its ST-LINK and runs the
# full bench (test/run_test.sh). The report of each board goes to
#   end_of_line/<mcu>_self_test.html   (+ .xml, + .log with the whole output)
# Then it asks for the next board.
#
# Run it as your user, not with sudo: it asks for the sudo password once at
# the start, because the network classes set up the bench link
# (tools/bench_net.sh) as root. The tests that need a human (jumpers) are not
# run. On the NUCLEO-H7S3L8 it flashes the trampoline (workspace/app_5) first:
# without it no firmware starts from the XSPI2 flash.
#
# Environment: CAOS_TTY (console, if autodetection cannot pick it) and
# STM32_PRG_PATH (STM32CubeProgrammer), as for run_test.sh and prog.sh.

set -u
cd "$(dirname "$0")" || exit 1
ROOT=$(pwd)
OUT="$ROOT/end_of_line"

ALL="F439 F446 F767 H563 H753 H773 U575"
board_name() {
    case "$1" in
        F439) echo "NUCLEO-F439ZI" ;;   F446) echo "NUCLEO-F446ZE" ;;
        F767) echo "NUCLEO-F767ZI" ;;   H563) echo "NUCLEO-H563ZI" ;;
        H753) echo "NUCLEO-H753ZI" ;;   H773) echo "NUCLEO-H7S3L8" ;;
        U575) echo "NUCLEO-U575ZI-Q" ;; *)    echo "" ;;
    esac
}

MCUS="${*:-$ALL}"
MCUS="${MCUS^^}"
for m in $MCUS; do
    [ -n "$(board_name "$m")" ] || { echo "eol: unknown board '$m' (use: $ALL)"; exit 1; }
done

if [ "$(id -u)" -eq 0 ]; then
    echo "eol: run it as your user, not with sudo (the build must not be owned by root)."
    exit 1
fi
[ -x test/run_test.sh ] && [ -x test/hw_test ] || { echo "eol: test/run_test.sh or test/hw_test missing"; exit 1; }

echo "eol: the network classes need root - sudo asks for the password once."
sudo -v || exit 1
# keep the sudo ticket alive for the whole run (one board can take an hour)
( while true; do sudo -n -v 2>/dev/null; sleep 60; done ) &
KEEP=$!
trap 'kill $KEEP 2>/dev/null' EXIT

mkdir -p "$OUT"

stlink_seen() {
    ls /dev/serial/by-id/*STMicroelectronics_STLINK* >/dev/null 2>&1 \
        || lsusb 2>/dev/null | grep -qi '0483:37'          # ST-LINK/V2-1, V3
}

# Enter = go on, s = skip this board, q = stop here
ask() {
    local a
    read -r -p "$1 [Enter = go, s = skip, q = quit] " a </dev/tty
    case "$a" in s|S) return 1 ;; q|Q) echo "eol: stopped"; exit 0 ;; esac
    return 0
}

declare -A RESULT
n=0; tot=$(echo $MCUS | wc -w)
for m in $MCUS; do
    n=$((n + 1)); name=$(board_name "$m"); lc=${m,,}
    echo
    echo "================================================================"
    echo " Board $n/$tot: $name ($m)"
    echo "================================================================"

    echo "eol: building the firmware for $m (log: end_of_line/${lc}_build.log)"
    if ! { ./bootstrap -m "$m" && ./do; } > "$OUT/${lc}_build.log" 2>&1; then
        echo "eol: BUILD FAILED for $m - see end_of_line/${lc}_build.log"
        RESULT[$m]="build failed"
        continue
    fi
    echo "eol: $(ls elfs/"$m"/*.elf 2>/dev/null | wc -l) firmware in elfs/$m"

    ask "Connect the $name (only this board) and press Enter." || { RESULT[$m]="skipped"; continue; }
    while ! stlink_seen; do
        echo "eol: no ST-LINK seen. Check the USB cable (the ST-LINK connector, CN1) and that the board is powered."
        ask "Press Enter to look again." || { RESULT[$m]="skipped"; continue 2; }
    done

    if [ "$m" = H773 ]; then
        echo "eol: flashing the XSPI2 trampoline (workspace/app_5)"
        if ! ./prog.sh -s -a 5 > "$OUT/${lc}_trampoline.log" 2>&1; then
            echo "eol: trampoline not flashed - see end_of_line/${lc}_trampoline.log"
            RESULT[$m]="trampoline failed"
            continue
        fi
    fi

    while true; do
        echo "eol: test bench on $name - report end_of_line/${lc}_self_test.html"
        sudo --preserve-env=STM32_PRG_PATH,CAOS_TTY ./test/run_test.sh -m "$m" -r "$OUT/${lc}_self_test" 2>&1 | tee "$OUT/${lc}_self_test.log"
        rc=${PIPESTATUS[0]}
        # written by root: give them back, they go into the published repo
        sudo chown "$(id -u):$(id -g)" "$OUT/${lc}_self_test".* 2>/dev/null
        # run_test.sh refuses a board that is not $m before running anything
        if grep -q -E 'ERROR: (wrong board|no board answers|the board reports)' "$OUT/${lc}_self_test.log"; then
            echo "eol: the connected board is not the $name."
            ask "Connect the $name and press Enter." && continue
            RESULT[$m]="skipped"; break
        fi
        [ -f "$OUT/${lc}_self_test.html" ] && RESULT[$m]="done (exit $rc)" || RESULT[$m]="no report (exit $rc)"
        break
    done
done

echo
echo "================================================================"
echo " End of line"
echo "================================================================"
for m in $MCUS; do
    printf "  %-5s %-16s %s\n" "$m" "$(board_name "$m")" "${RESULT[$m]:-not run}"
done
echo "Reports: end_of_line/<mcu>_self_test.html"
