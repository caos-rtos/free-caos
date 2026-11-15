#!/bin/bash
# run.sh - run a caOS firmware (.elf) on a simulated NUCLEO board in Renode.
#
# usage: simu/run.sh [-i] [-e] [-w] <board> <elf> [ms] ["cmd1;cmd2"]
#
#   <board>  h753 f767 f439 f446 u575 h563 h773
#   <elf>    e.g. elfs/H753/h753_example_2.elf, built with ./do
#
# Two ways to run it:
#   (default)  headless, for a fixed time: simulates <ms> milliseconds of
#              simulated time (default 500), then prints the console output
#              (about 20 s of real time for 5000 ms). "cmd1;cmd2" are typed on
#              the console after the first <ms>, each followed by Enter and by
#              SIMU_CMD_MS (default 1000) more ms. Output and Renode log stay in
#              simu/out/<elf>.uart and simu/out/<elf>.log.
#   -i         interactive: the console UART becomes a virtual serial port,
#              /tmp/caos_<board>, and a terminal opens on it right here (picocom
#              if installed, else simu/term.py): you see the console and type CLI
#              commands, as on the ST-LINK port of a real board. Leaving the
#              terminal (picocom: Ctrl-A Ctrl-X, term.py: Ctrl-]) stops the
#              simulation. Other programs (test/caos_shell, minicom with hardware
#              flow control OFF) can use the same port while it runs.
#              The Renode monitor listens on localhost:1234 (or the next free
#              port; SIMU_MONITOR_PORT): `telnet localhost 1234`, then pause, start, cpu PC, registers,
#              memory and peripherals of the running board.
#   -e         Ethernet: the board's MAC gets a LAN8742 PHY (address 0, link up,
#              autonegotiation done, 100 Mbit full duplex) and is wired through
#              a virtual switch to the host TAP caos-tap0, where the PC is
#              192.168.10.1 with a DHCP server (leases .50-.99), as on the test
#              bench. Boards: f439 f767 h753. The TAP is created on first use
#              with `sudo tools/bench_net.sh tap up` (owned by you, so Renode
#              needs no root); `sudo tools/bench_net.sh tap down` removes it.
#              The subnet is the bench's: real board and simulation, one at a
#              time.
#   -w         jumpers: loads simu/wires/<board>_ex<NN>.repl, the wires the
#              example wants between its pins (as on the bench), written as
#              GPIO connections: `gpioPortA: { 5 -> gpioPortC@10 }` is a wire
#              driven from PA5 and read on PC10. Plain GPIO only: what the
#              example's `wire` command checks, not the peripheral traffic.
#
# examples:
#   simu/run.sh h753 elfs/H753/h753_example_2.elf 5000
#   simu/run.sh u575 elfs/U575/u575_example_68.elf 4000 "help;fault stack"
#   simu/run.sh -i h563 elfs/H563/h563_example_68.elf
#   simu/run.sh -i -e f439 elfs/F439/f439_example_33.elf
#   simu/run.sh -w f767 elfs/F767/f767_example_29.elf 3000 "help;wire"
#   (ex64: "e64 wire")
#
# Accesses to addresses with no Renode model (a peripheral the platform lacks,
# or a firmware bug) are logged and summarised at the end (simu/unmapped.py).
#
# environment: SIMU_OUT (output folder), SIMU_MONITOR_PORT (-i, default 1234),
# SIMU_LOGLEVEL (Renode log level for everything; default: errors, plus warnings
# of the system bus = the unmapped accesses; 2 or lower slows the simulation a
# lot), SIMU_CMD_MS, RENODE_ROOT (see gen_platforms.py).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
usage() { sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; }

INTERACTIVE=0
ETH=0
WIRES=0
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        -i) INTERACTIVE=1; shift ;;
        -e) ETH=1; shift ;;
        -w) WIRES=1; shift ;;
        -*) echo "unknown option $1 (see --help)"; exit 2 ;;
        *) break ;;
    esac
done
[ $# -ge 2 ] || { usage; exit 2; }

BOARD=$1; ELF=$(readlink -f "$2"); MS=${3:-500}
OUT=${SIMU_OUT:-$ROOT/simu/out}; mkdir -p "$OUT"
case $BOARD in
  h753) CON=usart3 ;; f767) CON=usart3 ;; f439) CON=usart3 ;; f446) CON=usart3 ;;
  u575) CON=usart1 ;; h563) CON=usart3 ;; h773) CON=usart3 ;;
  *) echo "unknown board $BOARD (see --help)"; exit 2 ;;
esac
[ -f "$ELF" ] || { echo "no such elf: $2 (build it with ./do)"; exit 2; }
"$ROOT/simu/gen_platforms.py" || exit 1   # Renode models patched for caOS -> simu/out/platforms
[ -f "$ROOT/simu/out/platforms/nucleo_$BOARD.repl" ] || { echo "no simulated platform for $BOARD here (see --help)"; exit 2; }

# --- Ethernet (-e): PHY on the MAC, switch, host TAP --------------------------
ETH_LOAD=""
if [ "$ETH" -eq 1 ]; then
    case $BOARD in
      f439|f767|h753) ;;
      *) echo "-e: no simulated Ethernet MAC on $BOARD (f439 f767 h753)"; exit 2 ;;
    esac
    TAP=caos-tap0
    # LAN8742A as caOS sees it: ID 0x0007/0xC131, BMSR link up + AN complete,
    # partner 10/100 HD/FD, reg 31 (special control/status) AN done + 100FD.
    PHY='phy: Network.EthernetPhysicalLayer @ ethernet 0 { Id1: 0x0007; Id2: 0xC131; BasicControl: 0x3100; BasicStatus: 0x782D; AutoNegotiationAdvertisement: 0x01E1; AutoNegotiationLinkPartnerBasePageAbility: 0x45E1; VendorSpecific15: 0x1058 }'
    # The PC side: TAP present AND holding .1 (bench_net.sh `up` on the wire
    # takes the address back). bench_net.sh tap up is idempotent.
    if ! ip -o -4 addr show dev "$TAP" 2>/dev/null | grep -q ' 192\.168\.10\.1/24 '; then
        if [ -x "$ROOT/tools/bench_net.sh" ]; then
            echo "-e: preparing $TAP (sudo tools/bench_net.sh tap up)"
            sudo "$ROOT/tools/bench_net.sh" tap up || { echo "-e: $TAP not ready"; exit 1; }
        else
            echo "-e: $TAP is missing. Create it once, as root:"
            echo "    ip tuntap add dev $TAP mode tap user $USER && ip link set $TAP up"
            echo "    ip addr add 192.168.10.1/24 dev $TAP"
            echo "  and a DHCP server on it, e.g.:"
            echo "    dnsmasq --interface=$TAP --bind-interfaces --dhcp-range=192.168.10.50,192.168.10.99,1h"
            exit 1
        fi
    fi
    ETH_LOAD="machine LoadPlatformDescriptionFromString \"$PHY\"
emulation CreateSwitch \"caos_sw\"
connector Connect sysbus.ethernet caos_sw
emulation CreateTap \"$TAP\" \"caos_tap\" true
connector Connect host.caos_tap caos_sw"
fi

# --- jumpers (-w): GPIO connections of this example on this board ------------
WIRE_LOAD=""
if [ "$WIRES" -eq 1 ]; then
    NUM=$(basename "$ELF" .elf | sed -n 's/.*_example_\([0-9]*\)$/\1/p')
    WF="$ROOT/simu/wires/${BOARD}_ex${NUM}.repl"
    [ -n "$NUM" ] && [ -f "$WF" ] || { echo "-w: no jumper file ${WF#$ROOT/} (see simu/wires/)"; exit 2; }
    WIRE_LOAD="machine LoadPlatformDescription @$WF"
fi

RESC="$ROOT/simu/platforms/nucleo_$BOARD.resc"
LOAD="mach create \"$BOARD\"
machine LoadPlatformDescription @$ROOT/simu/out/platforms/nucleo_$BOARD.repl
$ETH_LOAD
$WIRE_LOAD
sysbus LoadELF @$ELF
$( [ -f "$RESC" ] && echo "include @$RESC" )
$( [ -n "${SIMU_LOGLEVEL:-}" ] && echo "logLevel $SIMU_LOGLEVEL" || printf 'logLevel 3\nlogLevel 2 sysbus' )"
TAG=$(basename "$ELF" .elf)
LOG=$OUT/$TAG.log

# --- interactive: virtual serial port + terminal, until the terminal exits ----
if [ "$INTERACTIVE" -eq 1 ]; then
    PTY=/tmp/caos_$BOARD
    PIDF=/tmp/caos_$BOARD.pid                  # the dotnet process, not the launcher
    alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }
    halt() {                                   # dotnet may ignore SIGTERM: 3 s, then -9
        alive "$1" || return 0
        kill "$1" 2>/dev/null || true
        for _ in $(seq 15); do alive "$1" || return 0; sleep 0.2; done
        kill -9 "$1" 2>/dev/null || true
        for _ in $(seq 10); do alive "$1" || return 0; sleep 0.2; done
    }
    # a simulation of this board left running (terminal closed, crash): its
    # PTY and monitor port would make this one fail
    OLD=$(cat "$PIDF" 2>/dev/null || true)
    if alive "$OLD" && grep -qa Renode.dll "/proc/$OLD/cmdline" 2>/dev/null; then
        echo "stopping a previous $BOARD simulation still running (pid $OLD)"
        halt "$OLD"
    fi
    rm -f "$PTY" "$PIDF"
    # the monitor listens on a TCP port (never on this terminal, which is the
    # console): the first free one from SIMU_MONITOR_PORT (default 1234)
    MPORT=$(python3 - "${SIMU_MONITOR_PORT:-1234}" <<'PY'
import socket, sys
p = int(sys.argv[1])
while True:
    s = socket.socket()
    try:
        s.bind(('0.0.0.0', p)); break
    except OSError:
        p += 1
    finally:
        s.close()
print(p)
PY
)
    # /usr/bin/renode is a script that starts dotnet as a CHILD: killing $! would
    # leave the simulation orphaned. --pid-file gives the real process.
    renode --disable-xwt -P "$MPORT" --plain --pid-file "$PIDF" -e "$LOAD
emulation CreateUartPtyTerminal \"caos_console\" \"$PTY\" true
connector Connect sysbus.$CON caos_console
start" < /dev/null > "$LOG" 2>&1 &
    RPID=$!
    stop() {
        trap - EXIT HUP INT TERM
        halt "$(cat "$PIDF" 2>/dev/null || true)"
        halt "$RPID"
        wait "$RPID" 2>/dev/null || true
        rm -f "$PTY" "$PIDF"
    }
    trap 'stop' EXIT
    trap 'stop; exit 0' HUP INT TERM
    for _ in $(seq 50); do [ -e "$PTY" ] && break; alive "$RPID" || break; sleep 0.2; done
    [ -e "$PTY" ] || { stop; echo "Renode did not open $PTY: see $LOG"; exit 1; }
    echo "$TAG on a simulated $BOARD, console on $PTY (Renode log: $LOG)"
    echo ">>> Renode monitor on localhost:$MPORT  -> telnet localhost $MPORT <<<"
    echo "    pause | start | cpu PC | cpu GetRegistersValues | sysbus ReadDoubleWord 0x20000000 | help"
    if command -v picocom >/dev/null; then
        echo ">>> CTRL+A and CTRL+X to end the simulation <<<"
        picocom -q -b 115200 "$PTY" || true
    else
        echo ">>> CTRL+] to end the simulation <<<"
        python3 "$ROOT/simu/term.py" "$PTY" || true
    fi
    stop
    echo "simulation stopped"
    python3 "$ROOT/simu/unmapped.py" "$LOG" "$ELF"
    exit 0
fi

# --- headless, fixed time ----------------------------------------------------
CMDS=$(CMDS="${4:-}" CON=$CON STEP=${SIMU_CMD_MS:-1000} python3 -c '
import os
for c in filter(None, (x.strip() for x in os.environ["CMDS"].split(";"))):
    for ch in c + "\r":
        print("%s WriteChar %d" % (os.environ["CON"], ord(ch)))
    print("emulation RunFor \"%.3f\"" % (int(os.environ["STEP"]) / 1000))
')
UART=$OUT/$TAG.uart; rm -f "$UART"
echo "simulating $MS ms of $TAG (headless, output at the end; -i for an interactive console)..."
timeout $((MS / 10 + 60)) renode --disable-xwt --console --plain -e "$LOAD
$CON CreateFileBackend @$UART true
emulation RunFor \"$(LC_ALL=C awk "BEGIN{printf \"%.3f\", $MS/1000}")\"
$CMDS
cpu PC
sysbus FindSymbolAt \`cpu PC\`
cpu ExecutedInstructions
quit" < /dev/null > "$LOG" 2>&1 || true   # no TTY: Renode console mode hangs on a terminal
echo "== $TAG ($MS ms virtual) uart=$UART"
cat "$UART" 2>/dev/null
echo
python3 "$ROOT/simu/unmapped.py" "$LOG" "$ELF"
