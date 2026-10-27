#!/bin/bash
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
#
# A DHCP server for the bench, for as long as a test needs one — and not a
# minute longer.
#
# WHY THIS EXISTS
#
# Every ETH example from ex35 on takes its address from DHCP and has no static
# fallback: with nobody answering, the board's IP stays 0.0.0.0 and thirteen
# examples (35-45, 48, 49) plus their NCM clones cannot be tested at all.
#
# WHY IT IS NOT A SYSTEM SERVICE
#
# Because a bench must not leave a DHCP server running on a machine that also
# has real networks on it. This starts a dnsmasq of its OWN — its own pid file,
# its own lease file, --conf-file=/dev/null so it reads none of the system
# configuration, and --bind-interfaces so it touches exactly one interface and
# leaves the LXC instance on lxcbr0 alone. It is started before the examples
# that need it and stopped after.
#
# WHY IT NEEDS ROOT
#
# Port 67 is privileged; there is no way around it. That is the whole reason
# for the sudoers entry, and it is why this script is a script and not a few
# lines inlined in the test: the rule grants exactly this command and nothing
# else.
#
#   /etc/sudoers.d/caos-bench:
#     <user> ALL=(root) NOPASSWD: <package>/tools/bench_dhcp.sh
#
# Guard the interface name: it is the one argument that reaches a root command,
# so it is checked against the kernel's own list rather than trusted.
#
# Usage:
#   bench_dhcp.sh start <iface> [subnet]   subnet defaults to 192.168.10
#   bench_dhcp.sh stop
#   bench_dhcp.sh status                   exit 0 when ours is running

set -u

RUN_DIR=/run/caos-bench
PIDFILE="$RUN_DIR/dnsmasq.pid"
LEASES="$RUN_DIR/dnsmasq.leases"

# How long a lease lasts.
#
# This was 2m, on the reasoning that a bench should not hold addresses after
# the boards are gone. That reasoning was wrong about the timescale: a single
# example stays powered for minutes at a time - flashing, the CLI phases, a
# client loop that runs every 31 s - so a two-minute lease means the board
# spends the run RENEWING rather than working. It showed up as ex40's
# board_binds_an_address taking 37 s in a full run against 10.9 s on its own,
# which pushed every later phase out of step with the client's cycle: the
# PC-side server logged two complete sessions during the very phase that
# reported "did not establish a connection".
#
# An hour outlasts any single class and most full runs. Nothing is leaked by
# it: the lease file lives in /run and the server is stopped with the bench,
# so the addresses go away with it either way.
LEASE_TIME=1h

die() { echo "bench_dhcp: $*" >&2; exit 1; }

# The interface must exist. Rejecting anything else keeps a typo — or worse —
# from reaching dnsmasq with root behind it.
check_iface() {
    local n="$1"
    [ -n "$n" ] || die "no interface given"
    case "$n" in
        *[!a-zA-Z0-9_.-]*) die "interface name '$n' has characters an interface cannot have" ;;
    esac
    [ -e "/sys/class/net/$n" ] || die "no such interface: $n"
}

running() {
    [ -f "$PIDFILE" ] || return 1
    local p; p=$(cat "$PIDFILE" 2>/dev/null)
    [ -n "$p" ] || return 1
    # Ours, and still dnsmasq: a recycled pid must not look like a live server.
    grep -qa dnsmasq "/proc/$p/cmdline" 2>/dev/null
}

case "${1:-}" in
start)
    iface="${2:-}"
    subnet="${3:-192.168.10}"
    check_iface "$iface"
    case "$subnet" in
        *[!0-9.]*) die "bad subnet '$subnet' (expected three octets, e.g. 192.168.10)" ;;
    esac

    running && { echo "already running (pid $(cat "$PIDFILE"))"; exit 0; }

    [ "$(id -u)" = 0 ] || die "must run as root (port 67 is privileged) - use sudo"

    mkdir -p "$RUN_DIR" || die "cannot create $RUN_DIR"
    : > "$LEASES"

    # The PC's own address on that interface. The bench never configures the
    # network, but a DHCP server cannot serve a subnet it has no address in,
    # so this is checked and reported rather than silently assumed.
    if ! ip -o -4 addr show dev "$iface" 2>/dev/null | grep -q " $subnet\."; then
        die "$iface holds no address in $subnet.0/24 - give it one first:
    sudo ip addr add $subnet.1/24 dev $iface"
    fi

    # --conf-file=/dev/null   : ignore every system config file
    # --bind-interfaces       : this interface only; the LXC dnsmasq keeps its own
    # --no-hosts / --port=0   : DHCP only, no DNS at all - nothing to hijack
    # --dhcp-authoritative    : answer at once instead of waiting for another server
    #
    # option:dns-server is ANNOUNCED even though this dnsmasq serves no DNS
    # itself (--port=0). With --port=0 it advertises no resolver at all, so the
    # board's lease carried DNS=0.0.0.0 and every lookup in ex37/ex72 failed
    # before leaving the part. The resolver those two need is a separate
    # process on 53, started by `bench_net.sh peer 37|72` - it lives at the
    # bench address, which is what this option names. Harmless for every other
    # example: they are told where a resolver would be and never ask.
    dnsmasq \
        --conf-file=/dev/null \
        --pid-file="$PIDFILE" \
        --dhcp-leasefile="$LEASES" \
        --interface="$iface" \
        --bind-interfaces \
        --except-interface=lo \
        --no-hosts \
        --port=0 \
        --dhcp-range="$subnet.50,$subnet.99,$LEASE_TIME" \
        --dhcp-option=option:dns-server,"$subnet.1" \
        --dhcp-authoritative \
        || die "dnsmasq failed to start on $iface"

    sleep 0.3
    running || die "dnsmasq exited right after starting - is another DHCP server on $iface?"
    echo "dhcp up on $iface serving $subnet.50-$subnet.99 (pid $(cat "$PIDFILE"))"
    ;;

stop)
    if running; then
        p=$(cat "$PIDFILE")
        kill "$p" 2>/dev/null
        for _ in $(seq 20); do running || break; sleep 0.1; done
        running && kill -9 "$p" 2>/dev/null
        echo "dhcp stopped"
    else
        echo "not running"
    fi
    rm -f "$PIDFILE"
    ;;

status)
    if running; then echo "running (pid $(cat "$PIDFILE"))"; exit 0
    else echo "not running"; exit 1; fi
    ;;

*)
    echo "usage: $(basename "$0") start <iface> [subnet] | stop | status" >&2
    exit 2
    ;;
esac
