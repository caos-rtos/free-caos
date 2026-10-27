#!/bin/bash
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
#
# Bring up the bench side of the board's link: the PC's address on it, and a
# DHCP server for the boards that ask for one.
#
# WHY THIS EXISTS ALONGSIDE bench_dhcp.sh
#
# bench_dhcp.sh does one thing - it runs a dnsmasq of its own - and it REFUSES
# to start if the interface holds no address in the board's /24, because a DHCP
# server cannot serve a subnet it is not on. Giving the interface that address
# is the step it will not take for you, and it is the step that was missing
# when running the bench by hand.
#
# So this is the pair, in the order caosServices::dhcpStart() uses:
#   1. every other address on the board's subnet REMOVED from the interface
#   2. 192.168.10.1/24 put on it, as the only address on that subnet
#   3. the DHCP server on that interface
#
# WHY 192.168.10.1 AND NOT WHATEVER THE PC ALREADY HAS
#
# Six examples (ex36, 37, 39, 40, 42, 43) carry 192.168.10.1 as a COMPILED-IN
# constant: as far as that firmware is concerned the syslog collector, the HTTP
# server, the MQTT broker and the Modbus server all live there. An interface
# sitting on 192.168.10.150 is on the right subnet and is still the wrong host.
#
# WHY THE OTHERS ARE REMOVED AND NOT JUST JOINED
#
# Adding .1 alongside an address the interface already had on the same subnet
# leaves the old one PRIMARY and .1 SECONDARY. The kernel takes the source
# address from the route, which still names the primary, so everything the PC
# STARTS keeps leaving as .150 - while everything the board starts towards .1
# gets .1 back, because there the source is pinned by the request:
#
#   ip route get 192.168.10.60  ->  src 192.168.10.150     (not .1)
#
# The board would then see one peer address when it calls the PC and another
# when the PC calls it, in the same run: fine for ex36/37/39/40/42/49, where the
# board always calls out, and wrong for ex34 and ex41, where the PC reaches a
# server running ON the board. Taking the other address off the subnet leaves
# exactly one address the interface can speak from or answer on - nothing to
# reason about, and no second address for a test to accidentally match.
#
# WHAT DOES NOT COME BACK
#
# The addresses removed in step 1 are NOT restored by `down`. They belong to
# whatever configures this machine's networking - netplan, NetworkManager - and
# that is what should put them back: `netplan apply`, `nmcli device reapply`,
# or replugging the cable. Each removed address is printed as it goes, so it is
# in the terminal scrollback if it has to be typed back by hand.
#
# Note for a netplan/NM box: if netplan or NM owns that address it may put it
# straight back on its own, which defeats step 1. `up` fails loudly in that case
# rather than running on with the wrong source address, and `status` names
# whatever is still sharing the subnet.
#
# RELATION TO THE TEST BENCH
#
# The bench arranges the address and the server itself through caosServices
# (suiteSetUp), so this script is not required to run the tests. It is for the
# case where you want the link up BEFORE and BETWEEN runs: with both already up,
# the bench's service menu finds everything present and starts nothing, which
# means no sudo prompt in the middle of a run.
#
# THE USB SIDE
#
# The NCM examples (ex54-56, ex69-83) put the board on a USB network interface
# instead of the wired one, with a static address on the same bench subnet. They
# need the same thing - the PC at 192.168.10.1 - plus one more: NetworkManager
# has to be told to leave that interface alone, or it hands it a lease on a
# subnet of its own (172.21.10.100 has been seen) and the board is unreachable.
# `usb` does both; `usb-restore` gives the interface back.
#
# Usage:
#   bench_net.sh up   [iface] [subnet]   iface auto-detected; subnet 192.168.10
#   bench_net.sh down [iface] [subnet]
#   bench_net.sh usb  [iface] [subnet]   the NCM interface, for ex54-56/69-83
#   bench_net.sh usb-restore [iface]
#   bench_net.sh usb-link [iface] [subnet]   NCM up, no bench address, for ex94
#   bench_net.sh ns-usb up|down|status       NCM iface in netns caos_ncm as .200 (ex95)
#   bench_net.sh ns-usb ping <ip> [count]    ping from inside that namespace
#   bench_net.sh ns-usb iperf <ip> <secs> [up|down|dual] [streams]
#                                            iperf2 client from inside it (ex96)
#   bench_net.sh ns-router up|down|status    the two PCs of app_3 (caos_rA/rB)
#   bench_net.sh ns-router ping A|B <ip> [n] ping from inside one of them
#   bench_net.sh eth  up|down [iface]    only one interface on the subnet
#   bench_net.sh eth  bounce [seconds]   carrier away and back, bench untouched
#   bench_net.sh txq  one|restore|status [iface]   single TX queue (see ex85)
#   bench_net.sh status      [subnet]    exit 0 when the link is fully up
#
# Run it with sudo: both steps need root (the address, and port 67).

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DHCP="$HERE/bench_dhcp.sh"

SUBNET_DEFAULT=192.168.10
PEER_HOST=1                       # the PC is always .1 on the board's link
STATEDIR=/run/caos-bench
CAP_UNIT=caos-bench-capture    # transient systemd unit of `capture` (see there)
OURSFILE="$STATEDIR/peer_ip.ours" # remembers that WE added .1, and where

die() { echo "bench_net: $*" >&2; exit 1; }
say() { echo "bench_net: $*"; }

need_root() {
    [ "$(id -u)" = 0 ] || die "must run as root (address + port 67) - use sudo"
}

# The interface facing the board.
#
# The same discriminator caosServices::ifaceForBoard() uses, and for the same
# reason: the SUBNET, not the name. A physical interface (has a device), with a
# cable in it (carrier 1), already holding an address in the board's /24. The
# board itself has no address yet - that is what the DHCP server is for - so it
# is never asked to answer.
detect_iface() {
    local pfx="$1" i n first_wired="" first_no_default=""
    for i in /sys/class/net/*; do
        n=$(basename "$i")
        [ "$n" = lo ] && continue
        [ -e "$i/device" ] || continue          # skip lxcbr0, docker0, veth...
        # NEVER a USB NIC: that is the NCM board's own interface, and `up` is
        # the ETH side. Picking it was a real failure - after an NCM class the
        # wired interface comes back with whatever addresses NetworkManager's
        # generic profiles give it, none on the bench subnet, so the only
        # interface holding 192.168.10.x was the board's. `up` then configured
        # the NCM link while the ETH examples had no route at all.
        case "$(readlink -f "$i/device")" in
            */usb[0-9]*/*) continue ;;
        esac
        case "$n" in wl*) continue ;; esac      # nor wireless

        # The fallback candidate is remembered BEFORE the carrier test, and on
        # purpose. `up` is often run moments after `eth up`, while the PHY is
        # still autonegotiating: requiring a carrier there made it answer "no
        # interface found on 192.168.10.0/24" about an interface that was
        # sitting right in front of it, and the ETH class that followed an NCM
        # one aborted with no DHCP server at all.
        [ -n "$first_wired" ] || first_wired="$n"

        # On a machine with only one wired port this is the same interface as
        # above and changes nothing. On one with more than one - a second
        # onboard port, a dock - the plain "first one seen" guess is a coin
        # flip between the board's cable and the machine's normal uplink, and
        # alphabetical sysfs order has no reason to favor the board. An
        # interface carrying this machine's default route IS that uplink, by
        # definition: a point-to-point cable to a NUCLEO never is one. Seen
        # 2026-09-08: enp6s0 (the house LAN, default route) beat enp7s0 (the
        # board) this way, and 192.168.10.0/24 ended up routed into the wrong
        # room.
        if [ -z "$first_no_default" ] \
           && ! ip route show default 2>/dev/null | grep -q "dev $n\b"; then
            first_no_default="$n"
        fi

        [ "$(cat "$i/carrier" 2>/dev/null)" = 1 ] || continue
        if ip -o -4 addr show dev "$n" 2>/dev/null | grep -q " $pfx\."; then
            echo "$n"; return 0                 # already on the bench subnet
        fi
    done
    # None on the subnet: fall back to a wired interface. Being on the subnet
    # is how the bench USUALLY finds its interface, but it cannot be a
    # requirement here - `up` exists precisely to put the address back on an
    # interface that has lost it. Prefer one that is not the machine's default
    # route; only guess blindly if every wired candidate carries one.
    [ -n "$first_no_default" ] && { echo "$first_no_default"; return 0; }
    [ -n "$first_wired" ] && { echo "$first_wired"; return 0; }
    return 1
}

# Guard the interface name: it is the one argument that reaches a root command.
check_iface() {
    local n="$1"
    [ -n "$n" ] || die "no interface given"
    case "$n" in
        *[!a-zA-Z0-9_.-]*) die "interface name '$n' has characters an interface cannot have" ;;
    esac
    [ -e "/sys/class/net/$n" ] || die "no such interface: $n"
}

check_subnet() {
    case "$1" in
        *[!0-9.]*) die "bad subnet '$1' (expected three octets, e.g. 192.168.10)" ;;
    esac
}

peer_ip()    { echo "$1.$PEER_HOST/24"; }
# Is .1 present? With a second argument, present ON THAT INTERFACE.
#
# The interface matters more than it looks. During NCM work .1 lives on the USB
# NIC, and a bare "is .1 anywhere" check then reports the ETH side as ready
# while every packet for a wired board still leaves through USB - which is what
# happened: `up` said ".1 already present - left alone" and `ip route get
# 192.168.10.34` answered dev enx02ca050000c1.
peer_is_up() {
    if [ $# -ge 2 ] && [ -n "$2" ]; then
        ip -o -4 addr show dev "$2" 2>/dev/null | grep -q " $1\.$PEER_HOST/24 "
    else
        ip -o -4 addr show 2>/dev/null | grep -q " $1\.$PEER_HOST/24 "
    fi
}

# Every address on the board's subnet that is NOT ours. Only that subnet: an
# interface may legitimately carry addresses on other networks, and those are
# none of the bench's business.
others_on_subnet() {
    ip -o -4 addr show dev "$1" 2>/dev/null \
        | awk -v pfx="$2." -v keep="$2.$PEER_HOST/24" \
              '$4 ~ "^"pfx && $4 != keep {print $4}'
}

# Is 192.168.10.1 someone else's address on this link?
#
# THE REASON THIS CHECK EXISTS
#
# 192.168.10.1 is not a free address everywhere. On the office network it is the
# real GATEWAY. Running `up` there would hand this PC the router's address: the
# machine loses its own way out, and every other host on that LAN starts sending
# the router's traffic to this laptop as soon as its ARP caches turn over.
#
# TWO CHECKS, BECAUSE ONE IS NOT ENOUGH
#
# The obvious test - ARP for .1 and see if anything answers - is weak here, and
# weak in exactly the case that matters. When the bench is actually in use the
# cable is in the NUCLEO, so the office gateway is not on the link and cannot
# answer whatever the truth is. The dangerous moment is the other one: `up` run
# absent-mindedly with the office cable still plugged in. Only THEN does ARP see
# the gateway - so the check earns its keep precisely when it can fire, and is
# silent (harmlessly) the rest of the time.
#
# The stronger signal does not depend on what is on the wire at all: what this
# machine has been CONFIGURED to think. A link with a default gateway on the
# board's subnet is a real network by definition; the bench link has none
# (ipv4.gateway is empty on the bench connection, and there is no default route
# through that interface). That holds whether or not the cable is plugged in.
#
# Either one firing is enough to stop.

# Configured: does this machine route through the board's subnet?
subnet_has_gateway() {
    local pfx="$1" iface="$2"
    # A default route out of this interface, or via a gateway on that subnet.
    ip route show default 2>/dev/null \
        | grep -qE "(dev $iface\b|via $pfx\.)" && return 0
    return 1
}

# On the wire: does anything answer at that address right now?
#
# `arping -D` is duplicate-address-detection mode: it sends from 0.0.0.0, so it
# never claims the address itself, and its exit status is 0 when the address is
# FREE and non-zero when somebody replied.
#
# But non-zero ALSO means it could not run at all - without CAP_NET_RAW it dies
# with "UID/EUID 0 or capability CAP_NET_RAW required" and exits 1, exactly like
# a positive detection. Reading the status alone therefore reports "occupied" on
# an empty link, which is the wrong answer in the direction that blocks the
# bench. `up` runs under sudo so the permission case should not arise, but a
# status that means two opposite things is not something to build a safety check
# on: the output is inspected, and anything unexpected is reported rather than
# silently taken as one of the two answers.
#
# Returns: 0 = something answered, 1 = free, 2 = could not tell.
address_answers() {
    local ip="$1" iface="$2" out rc
    if command -v arping >/dev/null 2>&1; then
        out=$(arping -D -c 2 -w 3 -I "$iface" "$ip" 2>&1); rc=$?
        [ $rc -eq 0 ] && return 1                       # clean run, free

        # Could not run at all - no CAP_NET_RAW. Non-zero here means nothing
        # about the address, so it must not be read as a detection.
        case "$out" in
            *CAP_NET_RAW*|*"you may need to run as root"*|*"Operation not permitted"*)
                return 2 ;;
        esac

        # Somebody answered: a real reply names the MAC it came from.
        case "$out" in
            *"Unicast reply"*|*"reply from"*) return 0 ;;
        esac

        # Nobody answered - the address is free as far as the WIRE is concerned.
        #
        # This exits 1 with "100% packet loss", the same status as the failures
        # above, which is why the output is read instead of the status. And it
        # is the normal answer in a case worth naming: when the address is
        # ALREADY OURS. `-D` sends from 0.0.0.0 and the kernel does not answer
        # ARP for its own address arriving on the very interface that holds it,
        # so re-running `up` on a link that is already up reports "free". That
        # is correct for what this check is for - it asks whether some OTHER
        # host holds the address, and duplicate-address detection is exactly
        # that question - but it does mean the probe cannot confirm our own.
        case "$out" in
            *"100% packet loss"*|*"Received 0 repl"*) return 1 ;;
        esac

        return 2                                        # genuinely unexpected
    fi
    ping -c 2 -W 1 -I "$iface" "$ip" >/dev/null 2>&1 && return 0
    ip neigh show dev "$iface" 2>/dev/null | grep -q "^$ip .*lladdr" && return 0
    return 1
}

# What the kernel would actually put in the packet towards the board.
src_towards() {
    ip -o route get "$1.60" 2>/dev/null | sed -n 's/.*src \([0-9.]*\).*/\1/p'
}

resolve_iface() {
    local given="$1" pfx="$2" iface
    if [ -n "$given" ]; then
        iface="$given"
    else
        iface=$(detect_iface "$pfx") || die "no interface found on $pfx.0/24.
    Plug the board in, or give the interface an address on that subnet:
        sudo ip addr add $pfx.1/24 dev <iface>
    Interfaces seen: $(ls /sys/class/net | tr '\n' ' ')"
    fi
    check_iface "$iface"
    echo "$iface"
}

# ---------------------------------------------------------------------------
# ONE TRANSMIT QUEUE, so the PC stops deciding what the board's test measures.
#
# A modern NIC has several hardware transmit queues - four on this bench - with
# an `mq` root qdisc and one fq_codel per queue. With XPS unconfigured
# (xps_cpus=0, which is the default here) the kernel picks the queue from a HASH
# of the flow's four-tuple, and the source ports are ephemeral, so WHICH queue a
# connection lands on is drawn afresh on every run.
#
# Each active queue then gets an equal share of the link, and flows that landed
# together split their queue's share between them. For ex85, which opens four
# connections at once and asserts they are served evenly, that is fatal:
#
#   4 flows on 4 queues   25/25/25/25   even      - 9.4% of runs
#   two flows sharing     33/33/17/17   2:1       - 56% of runs
#   three flows sharing   50/17/17/17   3:1       - 19% of runs
#
# Measured on H563 over four consecutive runs, and the board's own figures were
# IDENTICAL in all of them: 95 Mbps total, low_water 54/64, zero refusals, zero
# retransmissions. The spread was the PC's hash, not the board's buffer quota -
# and a test that fails three runs in four while the firmware is blameless is
# worse than no test.
#
# With a single queue, fq_codel is left doing what it is good at: fair queuing
# BETWEEN FLOWS, on one queue, deterministically. What the board then receives
# is an even offer, and any imbalance left is the board's own - which is the
# thing ex85 exists to measure.
#
# The bench moves 100 Mbit/s, so one queue costs nothing here: the multiqueue
# machinery is for line rates this link does not reach.
#
# Not fatal when it does not work: a NIC that cannot change its channel count
# still runs every test on the bench, and only ex85's fairness phase is left
# measuring the hash. It says so and carries on.
# ---------------------------------------------------------------------------
txq_file() { echo "$STATEDIR/txq.$1"; }

# A NIC describes its queues in ONE of two ways, and which one is not a detail:
# `ethtool -L` has to be told the same word back.
#
#   Combined: 4   RX: n/a  TX: n/a      one set of queues doing both (this bench)
#   RX: 4  TX: 4  Combined: n/a         separate sets (common on laptop NICs and
#                                       USB-Ethernet adapters)
#
# Reading only Combined - which is what this did at first - means a machine of
# the second kind gets no reduction at all, silently, and ex85 goes on measuring
# the queue hash there. So: Combined first, TX as the fallback.
#
# It prints "KIND COUNT" as one line, and the kind is not decoration: it is the
# word `ethtool -L` has to be given back. Returning it through a global would
# not work - every caller reads this through $(...), which runs it in a subshell
# where an assignment dies with the subshell.
#
# ethtool -l prints maximums first, then "Current hardware settings:" - so the
# current figure is the one AFTER that line, and taking the first match would
# read the maximum.
txq_current() {
    local out v
    out=$(ethtool -l "$1" 2>/dev/null) || return 1
    out=$(echo "$out" | awk '/^Current hardware settings:/{f=1} f')

    v=$(echo "$out" | awk '/^Combined:/{print $2; exit}')
    if [ -n "$v" ] && [ "$v" -eq "$v" ] 2>/dev/null; then
        echo "combined $v"; return 0
    fi

    v=$(echo "$out" | awk '/^TX:/{print $2; exit}')
    if [ -n "$v" ] && [ "$v" -eq "$v" ] 2>/dev/null; then
        echo "tx $v"; return 0
    fi

    return 1
}

txq_hw_count() { ls -d /sys/class/net/"$1"/queues/tx-* 2>/dev/null | wc -l; }

txq_one() {
    local iface="$1" cur kind hw
    command -v ethtool >/dev/null 2>&1 || {
        say "WARNING: ethtool not installed - transmit queues left as they are."
        say "         ex85's fairness phase will be measuring the PC's queue hash."
        return 0
    }
    hw=$(txq_hw_count "$iface")
    if ! read -r kind cur < <(txq_current "$iface"); then
        # A NIC with one queue and no channel support is the common case on
        # USB-Ethernet adapters, and it is already what we want - so say which
        # of the two it is rather than warning about both.
        if [ "$hw" -le 1 ]; then
            say "$iface has $hw transmit queue and no channel control - nothing to do"
        else
            say "WARNING: $iface reports no channel count but has $hw transmit"
            say "         queues; they are left alone and ex85 may see an uneven"
            say "         share. Check 'ethtool -l $iface'."
        fi
        return 0
    fi
    if [ "$cur" -eq 1 ]; then
        say "$iface already has a single transmit queue"
        return 0
    fi

    mkdir -p "$STATEDIR" 2>/dev/null
    # Written BEFORE the change, and with the KIND: if ethtool half-succeeds,
    # `down` must still know both what to put back and how to say it.
    echo "$kind $cur" > "$(txq_file "$iface")"

    if ! ethtool -L "$iface" "$kind" 1 2>/dev/null; then
        rm -f "$(txq_file "$iface")"
        say "WARNING: $iface refused 'ethtool -L $iface $kind 1' (was $cur)."
        say "         Its queues are unchanged; ex85 may see an uneven share."
        return 0
    fi

    # Changing the channel count resets the link on most drivers, and the
    # address survives it but the carrier takes a moment. Waiting here rather
    # than in the caller keeps the DHCP server from being started onto a link
    # that is still coming back.
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ "$(cat "/sys/class/net/$iface/operstate" 2>/dev/null)" = up ] && break
        sleep 1
    done
    # What ethtool accepted and what the kernel ended up with are not the same
    # question: some drivers clamp the request. The queues under /sys are the
    # ones packets actually go through, so that is what gets reported.
    hw=$(txq_hw_count "$iface")
    say "$iface set to a single transmit queue ($kind was $cur, now $hw queue(s))"
    say "  - the share the board sees is fq_codel's, not the queue hash's"
}

txq_restore() {
    local iface="$1" f saved kind prev
    f="$(txq_file "$iface")"
    [ -f "$f" ] || return 0
    saved=$(cat "$f" 2>/dev/null)
    rm -f "$f"
    [ -n "$saved" ] || return 0
    # "kind count", or a bare count from a file written before the kind was
    # recorded - in which case combined is the only thing it can have been.
    kind=$(echo "$saved" | awk '{print ($2 == "") ? "combined" : $1}')
    prev=$(echo "$saved" | awk '{print ($2 == "") ? $1 : $2}')
    if ethtool -L "$iface" "$kind" "$prev" 2>/dev/null; then
        say "$iface transmit queues restored to $prev ($kind)"
    else
        say "WARNING: could not restore $iface to $prev transmit queues."
        say "         Put them back by hand: sudo ethtool -L $iface $kind $prev"
    fi
}

case "${1:-}" in
up)
    subnet="${3:-$SUBNET_DEFAULT}"
    check_subnet "$subnet"
    need_root
    iface=$(resolve_iface "${2:-}" "$subnet") || exit 1

    say "interface $iface, subnet $subnet.0/24"

    # BEFORE anything is changed: is this the bench, or a real network?
    if [ "${BENCH_NET_FORCE:-0}" != 1 ]; then
        if subnet_has_gateway "$subnet" "$iface"; then
            die "this machine routes through $iface or through $subnet.x.
    That makes it a real network, not the bench - and on the office LAN
    $subnet.$PEER_HOST is the GATEWAY. Taking it would cut this machine off
    and draw other hosts' traffic here. Nothing has been changed."
        fi
        say "checking that $subnet.$PEER_HOST is free on $iface..."
        address_answers "$subnet.$PEER_HOST" "$iface"
        case $? in
        0)  die "something already answers at $subnet.$PEER_HOST on $iface.
    On the bench nothing should: the board has no address until DHCP gives it
    one. A reply here usually means the office cable is still plugged in, and
    what answered is the gateway. Nothing has been changed.
    If you are certain this is the bench:
        sudo BENCH_NET_FORCE=1 $0 up $iface $subnet" ;;
        2)  # Not fatal on its own: the configured-gateway check above already
            # passed, and that one does not depend on the wire. Say plainly
            # that half the evidence is missing rather than imply a clean check.
            say "WARNING: could not probe $subnet.$PEER_HOST (arping gave no usable"
            say "         answer). Relying on the gateway check alone - make sure"
            say "         the cable is in the NUCLEO and not in the office LAN." ;;
        esac
        say "nothing routes through this link"
    else
        say "BENCH_NET_FORCE=1: skipping the bench-vs-real-network checks"
    fi

    mkdir -p "$STATEDIR" || die "cannot create $STATEDIR"

    # 0. Take the bench subnet off every OTHER interface first.
    #
    # One interface on the subnet at a time - the rule this whole script exists
    # to keep - applies between interfaces as much as within one. After an NCM
    # class the USB NIC still holds 192.168.10.1, and adding the same address
    # here left it on both: the kernel then picked the USB route for a wired
    # board, and `ip route get 192.168.10.34` answered dev enx02ca050000c1 while
    # everything looked configured. An ETH example would have found no peer.
    for other in /sys/class/net/*; do
        o=$(basename "$other")
        [ "$o" = "$iface" ] && continue
        [ "$o" = lo ] && continue
        for a in $(ip -o -4 addr show dev "$o" 2>/dev/null \
                   | awk -v pfx="$subnet." '$4 ~ "^"pfx {print $4}'); do
            ip addr del "$a" dev "$o" 2>/dev/null \
                && say "removed $a from $o (the subnet belongs to $iface now)"
        done
    done

    # 1. Clear every other address on the board's subnet.
    others=$(others_on_subnet "$iface" "$subnet")
    for a in $others; do
        ip addr del "$a" dev "$iface" 2>/dev/null \
            || die "cannot remove $a from $iface"
        say "removed $a from $iface (not restored - see the header)"
    done

    # 2. The PC's address on the link, now the only one on this subnet.
    if peer_is_up "$subnet" "$iface"; then
        say "$(peer_ip "$subnet") already on $iface - left alone"
    else
        ip addr add "$(peer_ip "$subnet")" brd "$subnet.255" dev "$iface" \
            || die "cannot add $(peer_ip "$subnet") to $iface"
        echo "$iface" > "$OURSFILE"     # ours to remove, and from where
        say "$(peer_ip "$subnet") added to $iface"
    fi

    # With nothing else on the subnet the kernel has one candidate left - but
    # ASK, rather than assume. This is the property the whole step exists for,
    # and netplan or NetworkManager may have restored an address behind our
    # back between the two commands above.
    #
    # ASK MORE THAN ONCE, THOUGH.
    #
    # `ip addr add` returns before the kernel has finished installing the
    # connected route for the new address. Asked in that window, `ip route get`
    # finds no route for the bench subnet and falls back to the DEFAULT route -
    # which on a laptop is the office WiFi. The answer is then the WiFi address
    # (192.168.1.161 here), and it is not a claim on the subnet at all: it is
    # the absence of one. Seen 2026-09-06, with the check firing on a link that
    # was correct a second later.
    src=""
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        src=$(src_towards "$subnet")
        [ "$src" = "$subnet.$PEER_HOST" ] && break
        sleep 0.2
    done
    if [ "$src" != "$subnet.$PEER_HOST" ]; then
        claimants=$(others_on_subnet "$iface" "$subnet" | tr '\n' ' ')
        if [ -n "$claimants" ]; then
            die "source towards $subnet.0/24 is ${src:-none}, not $subnet.$PEER_HOST.
    Something else still claims this subnet on $iface:
        $claimants
    If netplan or NetworkManager keeps putting it back, stop it for the run:
        sudo nmcli device disconnect $iface   (then re-run this)"
        fi
        die "source towards $subnet.0/24 is ${src:-none}, not $subnet.$PEER_HOST -
    and NOTHING else holds an address on this subnet, so this is not a
    competing claim. The address on $iface is not (or no longer) there:
        $(ip -o -4 addr show dev "$iface" | awk '{print $4}' | tr '\n' ' ')
    ${src:+That source is whatever the DEFAULT route points at, which is what
    the kernel falls back to when the bench subnet has no route of its own.
    }Most likely NetworkManager took the address back. Stop it for the run:
        sudo nmcli device disconnect $iface   (then re-run this)"
    fi
    say "source towards $subnet.0/24 is $src"

    # NOT the transmit queues. `up` used to call txq_one() here, and that was
    # wrong: `ethtool -L` RESETS THE LINK, and the bench runs `up` from
    # ensurePeerIp() in every test's setUp - so whenever the queues were not
    # already at one, a test had its carrier dropped underneath it. Measured on
    # 2026-09-16 while chasing an "intermittent link" that turned out to be the
    # bench resetting things on itself.
    #
    # The single queue is needed by ONE example (ex85, whose fairness phase
    # otherwise measures the NIC's queue hash), so it is arranged around that
    # example and by `txq one` on demand - not on the path every test walks.

    # 3. The DHCP server. Delegated, not reimplemented: one dnsmasq invocation
    #    for both the bench and this script means one thing to get right.
    [ -x "$DHCP" ] || die "missing or not executable: $DHCP"
    # A server bound to ANOTHER interface has to be moved, not left alone.
    # bench_dhcp.sh's own "already running" check only asks whether a dnsmasq of
    # ours exists, not where it is listening - so after an NCM class it stayed on
    # the USB NIC while `up` reported success, and a wired board would have
    # waited for offers that were being broadcast somewhere else.
    if "$DHCP" status >/dev/null 2>&1; then
        if ! ps -o cmd= -p "$(cat "$STATEDIR/dnsmasq.pid" 2>/dev/null)" 2>/dev/null \
             | grep -q -- "--interface=$iface"; then
            "$DHCP" stop >/dev/null 2>&1
            say "DHCP was serving another interface - moving it to $iface"
        fi
    fi
    "$DHCP" start "$iface" "$subnet" || die "DHCP did not start"

    echo
    say "link is up. The board should take a lease from $subnet.50-$subnet.99."
    say "Leases appear in $STATEDIR/dnsmasq.leases as boards bind."
    ;;

usb)
    # The USB side of the same job: give the board's NCM interface the PC's
    # address, and keep NetworkManager off it.
    #
    # WHY THIS IS A PROBLEM AT ALL
    #
    # An NCM example makes the board appear as a USB network interface, and the
    # examples carry a STATIC address on the bench subnet (192.168.10.69 for
    # ex69, and so on for the fifteen clones plus ex54-56). What they need on
    # the PC side is simply 192.168.10.1 on that interface.
    #
    # NetworkManager sees a new wired NIC and does what it does with any wired
    # NIC: it manages it, and hands it a lease on a subnet of its own - the
    # bench has seen 172.21.10.100. The board is then unreachable, and `ping -I`
    # makes it look reachable when it is not, because -I bypasses the routing
    # decision that would have failed. That has cost a whole class already.
    #
    # So two steps, in this order: tell NM to leave the device alone, THEN put
    # the address on it. Setting the address first just gives NM something to
    # overwrite.
    need_root
    subnet="${3:-$SUBNET_DEFAULT}"
    check_subnet "$subnet"

    # Find the USB network interface the same way caosNet::candidates() does -
    # by the BUS the device sits on, not by its name. udev names these from the
    # MAC (enx02ca050000a9), which starts with "en" exactly like the on-board
    # Ethernet and contains neither "usb" nor "ncm": matching names is how this
    # was got wrong the first time.
    # Waited for, not merely looked up. The caller reflashes the board and asks
    # for the link straight after, but a flash RESETS the part: the NCM
    # interface disappears and comes back under a new kernel index a few
    # seconds later. Looking once lost that race, this command died, the DHCP
    # server stayed on the wired interface, and the board - which only sends
    # DISCOVER_RETRY_N=3 discovers, 4 s apart - gave up before anything was
    # listening for it. It showed as ex82 failing at 0.0.0.0 in a full round
    # while passing on its own, because on its own the link was already up.
    usbif="${2:-}"
    if [ -z "$usbif" ]; then
        for _ in $(seq 30); do
            for i in /sys/class/net/*; do
                n=$(basename "$i")
                [ "$n" = lo ] && continue
                [ -e "$i/device" ] || continue
                case "$(readlink -f "$i/device")" in
                    */usb[0-9]*/*) usbif="$n"; break 2 ;;
                esac
            done
            sleep 0.5
        done
    fi
    [ -n "$usbif" ] || die "no USB network interface found after 15 s.
    Flash an NCM example first (ex54-56, ex69-83) and wait for it to enumerate:
        ip -o link show | tail -3"
    check_iface "$usbif"

    # Existing is not the same as usable. The interface appears in
    # /sys/class/net as soon as the device is enumerated, while the link comes
    # up a moment later - and a DHCP server bound in that gap serves nobody.
    # The board sends DISCOVER_RETRY_N=3 discovers 4 s apart and then stops,
    # so it either got an answer in that window or sits at 0.0.0.0 until
    # something makes it ask again. That is ex82 failing its address phase
    # while a later phase, arriving 15 s afterwards, worked perfectly.
    ip link set "$usbif" up 2>/dev/null || true
    for _ in $(seq 20); do
        [ "$(cat "/sys/class/net/$usbif/operstate" 2>/dev/null)" = "up" ] && break
        sleep 0.5
    done

    # Say so when it never came up, rather than carrying on quietly.
    # `ip link set up` returns 0 even when the interface stays down, so the
    # loop above can expire without anything noticing - and every phase that
    # follows then fails against a link that was never there, each one blaming
    # the example instead of the plumbing. Same check the wired branch makes.
    usbstate=$(cat "/sys/class/net/$usbif/operstate" 2>/dev/null)
    if [ "$usbstate" != up ]; then
        say "WARNING: $usbif is still '$usbstate' after 10 s."
        say "  Nothing will reach the board over NCM. Is an NCM example"
        say "  flashed and enumerated? (lsusb | grep ca01)"
    fi

    say "USB interface $usbif"

    # 1. Hands off, NetworkManager. Unmanaged rather than merely disconnected:
    #    a disconnected device is picked up again on the next carrier event,
    #    which during a test run happens every time the board is reflashed.
    if command -v nmcli >/dev/null 2>&1; then
        if nmcli device set "$usbif" managed no 2>/dev/null; then
            say "NetworkManager told to leave $usbif alone"
        else
            say "WARNING: could not set $usbif unmanaged - NetworkManager may"
            say "         take the interface back and the board will go quiet"
        fi
        # Whatever lease it already handed out has to go, or it stays alongside
        # ours and the source-address problem comes back in USB form.
        others=$(others_on_subnet "$usbif" "$subnet")
        for a in $(ip -o -4 addr show dev "$usbif" 2>/dev/null | awk '{print $4}'); do
            case "$a" in
                "$subnet.$PEER_HOST/24") continue ;;
            esac
            ip addr del "$a" dev "$usbif" 2>/dev/null \
                && say "removed $a from $usbif (NetworkManager's, not ours)"
        done
    fi

    # 2. The address the NCM examples expect to find the PC at.
    ip link set "$usbif" up 2>/dev/null

    # Anything else this interface holds on the subnet goes first, for the same
    # reason as on the wired side: a second address on the same /24 stays
    # PRIMARY and keeps the source, so .1 would be present and unused. Here it
    # is usually a NetworkManager profile generic enough to claim the board's
    # NIC (interface-name and mac-address both empty) - which is also what puts
    # 192.168.10.150 on it.
    for a in $(others_on_subnet "$usbif" "$subnet"); do
        ip addr del "$a" dev "$usbif" 2>/dev/null \
            && say "removed $a from $usbif"
    done

    if ip -o -4 addr show dev "$usbif" 2>/dev/null | grep -q " $subnet\.$PEER_HOST/24 "; then
        say "$subnet.$PEER_HOST/24 already on $usbif"
    else
        ip addr add "$subnet.$PEER_HOST/24" brd "$subnet.255" dev "$usbif" \
            || die "cannot add $subnet.$PEER_HOST/24 to $usbif"
        say "$subnet.$PEER_HOST/24 added to $usbif"
    fi

    # WHY THERE ARE NO PER-EXAMPLE HOST ROUTES HERE
    #
    # There were, briefly: a /32 for each NCM example's static address, to beat
    # the wired interface's identical /24. They did not survive contact with the
    # bench. Reflashing an NCM example RESETS the board, so the USB device
    # detaches and re-enumerates - a new kernel index, and with it every route
    # and the unmanaged flag gone. The routes did not outlive a single test, let
    # alone a run.
    #
    # What works instead is what the operator was already doing by hand: keep
    # the wired interface DOWN while testing NCM. With only one interface on the
    # subnet there is no ambiguity to resolve and nothing to re-apply after a
    # reflash. `up` and `usb` are therefore alternatives, not companions.

    # A DHCP server on THIS interface, for the examples that ask for a lease.
    #
    # ex70 and the clones of the DHCP examples come up as "IP --- (waiting for
    # DHCP)" and stay there: the bench's dnsmasq is bound to the WIRED
    # interface, which during an NCM run is down. Same server, same script,
    # pointed at the interface that has the board behind it.
    #
    # Started after the address is in place: bench_dhcp.sh refuses to run on an
    # interface holding no address in the subnet it would serve.
    if [ -x "$DHCP" ]; then
        # Bound to one interface at a time, so a server left over from an
        # Ethernet run has to go first.
        "$DHCP" status >/dev/null 2>&1 && "$DHCP" stop >/dev/null 2>&1
        if "$DHCP" start "$usbif" "$subnet"; then
            say "DHCP now serving on $usbif"
        else
            say "WARNING: DHCP did not start on $usbif - examples that wait for"
            say "         a lease (ex70 and the DHCP clones) will sit at 0.0.0.0"
        fi
    fi

    # The real question, and not the one asked at first: which INTERFACE, not
    # just which source address. A correct source out of the wrong interface is
    # exactly the failure that made ping fail while `ip route get` looked fine.
    out=$(ip -o route get "$subnet.69" 2>/dev/null)
    dev=$(printf '%s' "$out" | sed -n 's/.*dev \([^ ]*\).*/\1/p')
    if [ "$dev" = "$usbif" ]; then
        say "traffic for the NCM boards leaves $dev - correct"
    else
        say "WARNING: $subnet.69 would go out of ${dev:-nowhere}, not $usbif."
        say "         The board will not answer. Bring the wired side down:"
        say "             sudo ip link set <eth iface> down"
    fi
    ;;

peer)
    # Start one example's PC-side counterpart that needs a privileged port.
    #
    # WHY THIS EXISTS
    #
    # Most counterparts bind high ports and the bench runs them as an ordinary
    # user - ex40's 8080, ex77's 1883, ex79's 5001. Two do not: Modbus is 502
    # and syslog is 514, both below 1024, and the port numbers are compiled
    # into the firmware so they cannot be moved.
    #
    # Rather than widen the sudoers rule to `python3` - which would grant every
    # script on the machine root - the allowed set is named HERE, by example
    # number, and each maps to one known file inside this repository. The
    # argument is a number checked against that list; nothing from the caller
    # reaches a command line.
    #
    # --no-dhcp on all of them: the bench owns the addressing and the DHCP
    # server already, and what it wants from these scripts is the listener.
    need_root
    which="${2:-}"
    case "$which" in
        # nodhcp: does this counterpart configure the interface itself? The
        # ones that do take --no-dhcp to be told not to (the bench owns the
        # addressing); the ones that never touch it reject the flag outright.
        78) script=examples/example_78/modbus_server_ncm.py; port=502; nodhcp=1; udp=0 ;;
        74) script=examples/example_74/syslog_server_ncm.py; port=514; nodhcp=1; udp=1 ;;
        # --put: the TFTP peers serve reads by default and accept WRITES only
        # when asked. ex49's board offers 'tftp put' as well as 'get', and
        # without this its uploads are refused by a peer that is working as
        # designed - which reads as a firmware failure. Harmless for the read
        # path, so it is passed for both twins.
        # ex82 is nodhcp=1 like the rest: the lease comes from the bench's own
        # dnsmasq, which ncmLinkUp has already moved onto the board's link.
        # This script used to be started without the flag, and its --iface
        # defaults to 'auto' - so it brought up a dnsmasq of its OWN, unasked,
        # alongside the bench's. Two DHCP servers on one /24 do not share the
        # work: the board took the bench's offer and the other one NAKed it.
        # ex49 stays nodhcp=0 because its twin defaults --iface to None and so
        # never touches DHCP; the flag would be rejected.
        82) script=examples/example_82/test_tftp_ncm.py;     port=69;  nodhcp=1; udp=1; extra="--put" ;;
        49) script=examples/example_49/test_tftp_pc.py;      port=69;  nodhcp=0; udp=1; extra="--put" ;;
        43) script=examples/example_43/modbus_server.py;     port=502; nodhcp=1; udp=0 ;;
        39) script=examples/example_39/syslog_server.py;     port=514; nodhcp=1; udp=1 ;;
        # DNS, for ex37 (ETH) and ex72 (NCM). The bench's own dnsmasq runs with
        # --port=0 on purpose - DHCP only, nothing to hijack on a workstation -
        # so the resolver is a separate process on 53, started only for the two
        # examples that ask for a name. Both twins share one script.
        37|72) script=examples/example_72/dns_server.py;     port=53;  nodhcp=1; udp=1 ;;
        stop)
            pkill -f 'modbus_server(_ncm)?\.py' 2>/dev/null
            pkill -f 'syslog_server(_ncm)?\.py' 2>/dev/null
            pkill -f 'test_tftp(_ncm|_pc)?\.py' 2>/dev/null
            pkill -f 'dns_server\.py' 2>/dev/null
            # Some counterparts start a dnsmasq of their OWN - the TFTP scripts
            # mirror example_35's helper and serve 192.168.10.100-200. Left
            # behind, it competes with the bench's server (.50-.99) and the
            # board leases from whichever answers first: seen as ex82 holding
            # 192.168.10.140, outside the pool the test checks against, with
            # the bench's own lease file empty. They write named pid files, so
            # they can be stopped exactly rather than by pattern.
            for f in /tmp/dnsmasq-caos-ex*.pid; do
                [ -f "$f" ] || continue
                pid=$(cat "$f" 2>/dev/null)
                [ -n "$pid" ] && kill "$pid" 2>/dev/null
                rm -f "$f"
            done
            # Safety net for a dnsmasq whose pid file is gone.
            #
            # A pid file is not a property of the daemon, it is a file one
            # process writes and another deletes: the peer script removes it in
            # dhcp_stop(), and removes a stale one at startup too. A script that
            # dies between the two - which is exactly how the pkill above kills
            # it - leaves a daemon no pid file names any more, and the loop over
            # the files skips it in silence.
            #
            # The pattern wants the BINARY and then the name: 'dnsmasq-caos-ex'
            # alone would also catch a `tail -f /tmp/dnsmasq-caos-ex82.log` left
            # open to watch the DISCOVERs. And it does not touch the bench's own
            # dnsmasq (--pid-file=/run/caos-bench/dnsmasq.pid), which carries no
            # such name and is not a peer: bench_dhcp.sh owns it.
            pkill -f 'dnsmasq.*dnsmasq-caos-ex' 2>/dev/null
            say "privileged peers stopped"
            exit 0 ;;
        *) die "usage: $(basename "$0") peer 39|43|49|74|78|82 | stop" ;;
    esac

    [ -f "$HERE/../$script" ] || die "missing counterpart: $script"

    # TCP or UDP: TFTP (69) and syslog (514) are datagram services, so `ss -ltn`
    # would never see them - it lists LISTENING TCP sockets, and a UDP socket
    # does not listen. Getting this wrong reports "the peer did not start"
    # about a server that started perfectly.
    [ "$udp" = 1 ] && ss_flags="-lun" || ss_flags="-ltn"

    # Already listening? Leave it - the bench starts services once per run.
    #
    # Matched on ADDRESS:PORT, not on the port alone. Port 53 in particular is
    # held on this machine by systemd-resolved (127.0.0.53) and by the LXC
    # dnsmasq (10.0.3.1), neither of which serves the board: a bare ":53" match
    # found those and skipped starting the resolver the test needs. The others
    # (69, 514, 502) were never ambiguous, but the same rule is right for them.
    #
    # This pattern is the twin of INSPECT_ADDR_RE in test/hw_test/inc/
    # caosServices.h, which answers the same question from the C++ side. They
    # must accept the same spellings: when they disagree, this side reports the
    # peer already up while the bench keeps waiting for it, and the test dies
    # in setUp over a counterpart that is running.
    addr_re="(${SUBNET_DEFAULT}\\.1|0\\.0\\.0\\.0|\\*)(%[a-zA-Z0-9_-]+)?"
    if ss $ss_flags 2>/dev/null | grep -qE "$addr_re:$port([[:space:]]|$)"; then
        say "a peer is already listening on $port"
        exit 0
    fi

    log="/tmp/caos-bench-peer-$which.log"
    cd "$HERE/.." || die "cannot reach the repository root"
    if [ "$nodhcp" = 1 ]; then
        setsid nohup python3 -u "$script" --no-dhcp ${extra:-} > "$log" 2>&1 &
    else
        setsid nohup python3 -u "$script" ${extra:-} > "$log" 2>&1 &
    fi

    # Same pattern as the check above, not a bare ":$port": waiting on the port
    # alone would settle for a daemon on 127.0.0.1 that the board cannot reach,
    # and report a peer that is up for one that never started.
    for _ in $(seq 40); do
        ss $ss_flags 2>/dev/null | grep -qE "$addr_re:$port([[:space:]]|$)" && {
            say "peer for ex$which listening on $port (log: $log)"; exit 0; }
        sleep 0.2
    done
    say "the peer for ex$which did not open port $port - its log says:"
    sed 's/^/    | /' "$log" 2>/dev/null | tail -5
    exit 1
    ;;

tcpdump)
    # Capture on the board's link, for the bench to read.
    #
    # WHY THIS IS A SUBCOMMAND AND NOT A SUDO RULE FOR tcpdump
    #
    # Capturing needs CAP_NET_RAW, so it needs root. Granting `tcpdump` itself
    # through sudoers would be granting far more than a capture: -z runs a
    # command as root when the file rotates, -W/-w write files anywhere as root.
    # It is one of the standard ways a narrow sudo rule turns into a shell.
    #
    # So the arguments are not passed through. Only two values are taken - an
    # IPv4 address and a packet count - each checked against a strict pattern
    # before it is used, and the rest of the command line is fixed here. There
    # is no way to reach another flag through them.
    #
    # Read-only by construction: -w is never used, output goes to stdout, and
    # -c bounds the run so a capture cannot be left behind.
    need_root
    host="${2:-}"
    count="${3:-20}"

    case "$host" in
        ''|*[!0-9.]*) die "usage: $(basename "$0") tcpdump <ipv4> [count]" ;;
    esac
    case "$count" in
        ''|*[!0-9]*) die "count must be a number" ;;
    esac
    [ "$count" -ge 1 ] && [ "$count" -le 200 ] || die "count must be 1..200"

    # The interface facing that address. A third argument names it outright,
    # which is needed more often than it looks: during NCM work the wired and
    # the USB interface BOTH hold 192.168.10.x, and picking by subnet then
    # captured on the wrong one - silently, with an empty capture that looks
    # like "the board sent nothing".
    iface="${4:-}"
    if [ -n "$iface" ]; then
        check_iface "$iface"
    else
        pfx="${host%.*}"
        check_subnet "$pfx"
        for i in /sys/class/net/*; do
            n=$(basename "$i")
            [ "$n" = lo ] && continue
            [ -e "$i/device" ] || continue
            if ip -o -4 addr show dev "$n" 2>/dev/null | grep -q " $pfx\."; then
                iface="$n"; break
            fi
        done
        [ -n "$iface" ] || die "no interface holds an address on $pfx.0/24"
    fi
    say "capturing on $iface"

    command -v tcpdump >/dev/null 2>&1 || die "tcpdump is not installed"

    # -v prints the IP header fields and validates checksums, which is the
    # whole point when the question is "why is the peer dropping these".
    # -X prints the payload, hex and ASCII: without it a capture answers
    # "did a packet go by" and not "what was IN it", and the second question is
    # the one that separates a peer that is merely early (RST, refused) from
    # one that answered with bytes it should not have sent - ex67's SSH banner
    # coming back with invalid characters is exactly that case.
    # -n: no DNS. -c: bounded. No -w, no -z: nothing is written or executed.
    # -l: line-buffered. Without it tcpdump buffers its output whenever stdout
    # is not a terminal, which is exactly how a test reads it - into a file.
    # A capture killed before the buffer filled then leaves an EMPTY file, and
    # the test reports "no frames on the link" about a link that carried them:
    # observed here, 8 frames captured and 0 in the file.
    #
    # Through '| cat', not exec'd onto the caller's stdout. tcpdump runs under an
    # AppArmor profile that may not write to an INHERITED file outside the paths
    # it allows: when the bench (or Claude) redirected stdout to a file, the
    # kernel swapped it for /dev/null - 'apparmor="DENIED" operation=
    # "file_inherit" profile="tcpdump"' - and the capture came back empty, not
    # even 'listening on' (02/10). An anonymous pipe is allowed; cat is not
    # confined and writes wherever the caller pointed it.
    tcpdump -i "$iface" -nn -v -X -l -c "$count" "host $host" 2>&1 | cat
    exit "${PIPESTATUS[0]}"
    ;;

capture)
    # Like tcpdump above, but writes a .pcap and runs until `capstop`.
    #
    # WHY THE FILENAME IS NOT AN ARGUMENT
    #
    # This script is reachable through a NOPASSWD sudo rule, so a caller-chosen
    # -w path would be "write any file anywhere as root" - the very thing the
    # tcpdump subcommand refuses to allow. The path is fixed HERE; the caller
    # copies the result afterwards, as itself, wherever it likes.
    #
    # The other half of the reasoning is unchanged: only an IPv4 address and an
    # optional interface are taken, both checked, and the rest of the command
    # line is written out below.
    need_root
    host="${2:-}"
    case "$host" in
        ''|*[!0-9.]*) die "usage: $(basename "$0") capture <ipv4> [iface]" ;;
    esac

    iface="${3:-}"
    if [ -n "$iface" ]; then
        check_iface "$iface"
    else
        pfx="${host%.*}"
        check_subnet "$pfx"
        for i in /sys/class/net/*; do
            n=$(basename "$i")
            [ "$n" = lo ] && continue
            [ -e "$i/device" ] || continue
            if ip -o -4 addr show dev "$n" 2>/dev/null | grep -q " $pfx\."; then
                iface="$n"; break
            fi
        done
        [ -n "$iface" ] || die "no interface holds an address on $pfx.0/24"
    fi

    command -v tcpdump >/dev/null 2>&1 || die "tcpdump is not installed"
    mkdir -p "$STATEDIR"

    # One capture at a time: a leftover from a killed test would otherwise keep
    # writing into the same file and mix two runs into one pcap.
    if systemctl is-active --quiet "$CAP_UNIT" 2>/dev/null ||
       { [ -f "$STATEDIR/capture.pid" ] &&
         kill -0 "$(cat "$STATEDIR/capture.pid")" 2>/dev/null; }; then
        die "a capture is already running (capstop first)"
    fi

    rm -f "$STATEDIR/capture.pcap"
    # -U writes each packet as it arrives. Without it the last packets of a
    # short exchange are still in tcpdump's buffer when capstop kills it, and
    # the pcap ends before the interesting part - the same trap the -l above
    # documents for the text capture.
    #
    # Started as a transient systemd unit, not with '&'. tcpdump runs under an
    # AppArmor profile that accepts signals only from unconfined peers, and a
    # caller launched from a snap (VS Code, hence Claude) carries the label
    # snap.code.code down through sudo: its SIGTERM was DENIED, capstop
    # returned, and tcpdump stayed alive for ever (02/10: kernel audit
    # 'apparmor="DENIED" operation="signal" ... peer="snap.code.code"').
    # systemd stops the unit from PID 1, which the profile accepts.
    if command -v systemd-run >/dev/null 2>&1; then
        systemctl reset-failed "$CAP_UNIT" >/dev/null 2>&1 || true
        systemd-run --quiet --collect --unit="$CAP_UNIT" \
            tcpdump -i "$iface" -nn -U -s 0 -w "$STATEDIR/capture.pcap" \
                    "host $host" ||
            die "systemd-run could not start tcpdump"
        rm -f "$STATEDIR/capture.pid"
    else
        tcpdump -i "$iface" -nn -U -s 0 -w "$STATEDIR/capture.pcap" \
                "host $host" >/dev/null 2>&1 &
        echo $! > "$STATEDIR/capture.pid"
    fi
    # tcpdump needs a moment to attach to the link; packets sent before it is
    # listening are simply not in the file, which reads as "the board sent
    # nothing" - a false negative that costs an hour to see through.
    sleep 1
    say "capturing on $iface -> $STATEDIR/capture.pcap"
    ;;

capstop)
    # Stop the capture and hand the file to the invoking user, who otherwise
    # could not read a root-owned pcap.
    need_root
    pidf="$STATEDIR/capture.pid"
    # TERM, not KILL: tcpdump flushes and closes the file on TERM. On KILL the
    # pcap can end mid-record and tools refuse to open it. systemctl stop sends
    # TERM from PID 1 (see capture for why that matters) and waits.
    if systemctl is-active --quiet "$CAP_UNIT" 2>/dev/null; then
        systemctl stop "$CAP_UNIT" 2>/dev/null || true
    elif [ -f "$pidf" ]; then
        pid=$(cat "$pidf")
        kill -TERM "$pid" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.2
        done
    else
        die "no capture running"
    fi
    rm -f "$pidf"
    # Orphans of the old '&' path that AppArmor kept alive: only tcpdump
    # processes writing THIS fixed pcap, signalled through systemd as well.
    if pgrep -f "^tcpdump .* -w $STATEDIR/capture.pcap" >/dev/null 2>&1 &&
       command -v systemd-run >/dev/null 2>&1; then
        systemd-run --quiet --wait --collect \
            pkill -TERM -f "^tcpdump .* -w $STATEDIR/capture.pcap" >/dev/null 2>&1 || true
    fi
    if [ -f "$STATEDIR/capture.pcap" ]; then
        # SUDO_UID is set by sudo; without it this leaves a file the test
        # cannot open, and the diagnosis dies of a permission error.
        [ -n "${SUDO_UID:-}" ] &&
            chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$STATEDIR/capture.pcap"
        say "capture stopped: $STATEDIR/capture.pcap"
    else
        say "capture stopped (no file written)"
    fi
    ;;

echo84)
    # example_84's raw L2 echo generator.
    #
    # WHY IT GOES THROUGH HERE AND NOT THROUGH ITS OWN SUDO RULE
    #
    # Raw sockets need root, and the sudoers rule for this bench deliberately
    # names ONE file. Adding the Python script to it would mean a second
    # absolute path to maintain (the rule is already path-bound and has to be
    # recreated on a new machine) and NOPASSWD on a script that opens raw
    # sockets. Routing it through the file that already has the privilege costs
    # nothing and keeps the privileged surface at one entry.
    #
    # It does not belong under `peer` either: those start a listener in the
    # background and return. This one runs, measures and finishes, so it
    # follows the tcpdump shape - fixed command line, arguments checked
    # against strict patterns, output straight to stdout.
    need_root

    iface="${2:-}"
    count="${3:-10000}"
    size="${4:-1500}"
    mbps="${5:-0}"

    [ -n "$iface" ] || die "usage: $(basename "$0") echo84 <iface> [count] [size] [mbps]"
    check_iface "$iface"
    case "$count" in ''|*[!0-9]*) die "count must be a number" ;; esac
    case "$size"  in ''|*[!0-9]*) die "size must be a number"  ;; esac
    # One optional decimal: the rate is a ramp step, not an integer count.
    # Validated as strictly as the others - this runs as root.
    case "$mbps"  in ''|*[!0-9.]*|*.*.*) die "mbps must be a number" ;; esac
    [ "$count" -ge 1 ]  && [ "$count" -le 1000000 ] || die "count must be 1..1000000"
    [ "$size"  -ge 60 ] && [ "$size"  -le 1514 ]    || die "size must be 60..1514"
    # 0 means "unpaced"; the ceiling is wire rate for Fast Ethernet.
    awk -v m="$mbps" 'BEGIN{exit !(m>=0 && m<=100)}' \
        || die "mbps must be 0..100 (0 = unpaced)"

    script="$HERE/../examples/example_84/eth_echo_peer.py"
    [ -f "$script" ] || die "missing counterpart: examples/example_84/eth_echo_peer.py"

    # --json is accepted so the bench can keep the figures, but the path is
    # FIXED here rather than taken from the caller: a filename that came
    # through this rule would be a root-owned write to anywhere.
    json="/tmp/caos-echo-ex84.json"

    exec python3 "$script" --iface "$iface" --count "$count" \
                           --size "$size" --mbps "$mbps" --json "$json"
    ;;

eth)
    # Bring the wired interface down or up.
    #
    # This is the one operation the bench genuinely needs per RUN rather than
    # per session, and the reason it exists as a subcommand: ETH and NCM
    # examples put the board on the same /24, and with both interfaces up the
    # kernel has two identical routes and picks the wired one - where no NCM
    # board is. Host routes were tried and do not survive a reflash (the USB
    # device re-enumerates and takes them with it), so the answer is the one
    # the operator was already using by hand: only one interface on the subnet
    # at a time.
    #
    # `down` also stops the DHCP server if it is bound to that interface:
    # leaving dnsmasq on a link that is going away serves nothing and blocks
    # the NCM side from starting its own.
    need_root
    action="${2:-}"
    iface="${3:-}"
    # `eth bounce [seconds] [iface]`: for this one action the third argument is
    # a duration, because that is what one actually types. A name still works -
    # anything non-numeric is read as the interface, as for up and down.
    BOUNCE_SECS=3
    if [ "$action" = bounce ] && [ -n "$iface" ]; then
        case "$iface" in
            *[!0-9]*) ;;
            *) BOUNCE_SECS="$iface"; iface="${4:-}" ;;
        esac
    fi
    if [ -z "$iface" ]; then
        # The wired candidate: physical, not USB. Carrier is NOT required -
        # the whole point may be to bring back up an interface that is down.
        #
        # This asks detect_iface() rather than walking /sys itself, and that is
        # not only about duplicated code. The inline walk it replaces took the
        # FIRST physical non-USB interface and stopped there, which on a machine
        # with two wired ports is a coin flip: on 2026-09-08 `eth down` picked
        # enp6s0 - the house LAN - and ran `ip link set down` + `ip addr flush`
        # on it, taking out the address and the default route of a machine whose
        # board was on enp7s0. detect_iface() prefers the interface already on
        # the bench subnet, then one that does not carry the default route.
        iface=$(detect_iface "$SUBNET_DEFAULT") || iface=""
    fi
    [ -n "$iface" ] || die "no wired interface found"
    check_iface "$iface"

    case "$action" in
    down)
        if [ -x "$DHCP" ] && "$DHCP" status >/dev/null 2>&1; then
            if ps -o cmd= -p "$(cat "$STATEDIR/dnsmasq.pid" 2>/dev/null)" 2>/dev/null \
               | grep -q -- "--interface=$iface"; then
                "$DHCP" stop >/dev/null 2>&1 && say "DHCP stopped (it was on $iface)"
            fi
        fi
        ip link set "$iface" down || die "cannot bring $iface down"
        say "$iface is down - the NCM interface now owns the subnet"
        ip addr flush dev "$iface"
        ;;
    up)
        ip link set "$iface" up || die "cannot bring $iface up"
        # `ip link set up` succeeds even when the interface stays down, so the
        # command's exit status says nothing. Check the state, and give the
        # link a moment: negotiation takes a second or two and reporting "up"
        # while operstate is still `down` sent a whole session chasing the
        # board, the PHY and the examples for something that was simply an
        # interface the kernel had not finished bringing up.
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            [ "$(cat "/sys/class/net/$iface/operstate" 2>/dev/null)" = up ] && break
            sleep 1
        done
        st=$(cat "/sys/class/net/$iface/operstate" 2>/dev/null)
        if [ "$st" = up ]; then
            say "$iface is up"
        else
            say "$iface was asked to come up but is still '$st'."
            say "  carrier: $(cat "/sys/class/net/$iface/carrier" 2>/dev/null || echo 'n/a')"
            say "  Nothing will reach the board over this link. Check that the"
            say "  cable is in the NUCLEO, and that the example on the board is"
            say "  one that brings the ETH up - a non-ETH example never"
            say "  initialises the PHY, so the link stays dark."
        fi
        ;;
    bounce)
        # THE CABLE, NOT THE BENCH.
        #
        # `down` above tears the bench apart on purpose - it flushes the
        # addresses and stops the DHCP server, because it is used to hand the
        # subnet over to the NCM interface. That is the wrong tool for asking
        # "what does the board do when the link goes away and comes back":
        # it removes the peer the board would talk to afterwards, so whatever
        # fails next cannot be attributed to the board.
        #
        # This takes away ONLY the carrier. Addresses stay, DHCP keeps running,
        # leases stay valid - from the board's side it is indistinguishable
        # from somebody pulling the cable and pushing it back in, which is
        # precisely the event ex32 needs a human for today.
        #
        # The console is SERIAL, so the board stays reachable throughout and a
        # test can watch the whole thing happen.
        addr_before=$(ip -o -4 addr show dev "$iface" | awk '{print $4}' | tr '\n' ' ')
        say "$iface: carrier down for ${BOUNCE_SECS}s (addresses and DHCP stay)"

        ip link set "$iface" down || die "cannot bring $iface down"
        sleep "$BOUNCE_SECS"
        ip link set "$iface" up   || die "cannot bring $iface up"

        # Same wait as `eth up`: `ip link set up` returns before the link has
        # negotiated, and reporting "up" too early once sent a whole session
        # chasing the board for something that was just a slow interface.
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            [ "$(cat "/sys/class/net/$iface/operstate" 2>/dev/null)" = up ] && break
            sleep 1
        done
        st=$(cat "/sys/class/net/$iface/operstate" 2>/dev/null)

        # NetworkManager and friends may react to a carrier loss by taking the
        # address off. That would break every test AFTER this one, and silently
        # - so it is checked and put back rather than discovered later.
        addr_after=$(ip -o -4 addr show dev "$iface" | awk '{print $4}' | tr '\n' ' ')
        if [ "$addr_before" != "$addr_after" ]; then
            say "WARNING: addresses changed across the bounce:"
            say "  before: ${addr_before:-none}"
            say "  after : ${addr_after:-none}"
            case "$addr_after" in
                *"$SUBNET_DEFAULT.$PEER_HOST/24"*) ;;
                *)  if ip addr add "$(peer_ip "$SUBNET_DEFAULT")" dev "$iface" 2>/dev/null; then
                        say "  $(peer_ip "$SUBNET_DEFAULT") put back on $iface"
                    else
                        say "  could not put $(peer_ip "$SUBNET_DEFAULT") back - the bench"
                        say "  is now without its peer address: sudo $0 up"
                    fi ;;
            esac
        fi

        if [ "$st" = up ]; then
            say "$iface is back up"
        else
            say "$iface is still '$st' after the bounce - the board will see no link"
        fi
        ;;

    *)
        die "usage: $(basename "$0") eth up|down|bounce [iface] / eth bounce [seconds]" ;;
    esac
    ;;

usb-link)
    # The NCM interface up and off NetworkManager, holding NO address on the
    # board's subnet - and the wired side left exactly as it is.
    #
    # WHY, NEXT TO `usb`
    #
    # `usb` gives the subnet to the USB interface and expects the wired one
    # down: one owner, no ambiguity. ex94 is the case that wants BOTH links to
    # the same board at once - one stack, one address, reachable over ETH and
    # over NCM. The wired interface keeps 192.168.10.1 (`up`), and the test
    # reaches the board over USB with `ping -I <usb iface>`, which forces the
    # egress interface. An address on the USB side would give the kernel a
    # second route to the same /24 and the traffic would leave by whichever it
    # prefers; with none, nothing on this PC routes there by itself.
    need_root
    subnet="${3:-$SUBNET_DEFAULT}"
    check_subnet "$subnet"

    # Found by bus, and waited for: a reflash re-enumerates the device
    # (same reasons as in `usb`).
    usbif="${2:-}"
    if [ -z "$usbif" ]; then
        for _ in $(seq 30); do
            for i in /sys/class/net/*; do
                n=$(basename "$i")
                [ "$n" = lo ] && continue
                [ -e "$i/device" ] || continue
                case "$(readlink -f "$i/device")" in
                    */usb[0-9]*/*) usbif="$n"; break 2 ;;
                esac
            done
            sleep 0.5
        done
    fi
    [ -n "$usbif" ] || die "no USB network interface found after 15 s"
    check_iface "$usbif"

    # NetworkManager first, or it hands out a lease as soon as carrier comes.
    if command -v nmcli >/dev/null 2>&1; then
        nmcli device set "$usbif" managed no 2>/dev/null \
            || say "WARNING: could not set $usbif unmanaged"
    fi
    ip link set "$usbif" up 2>/dev/null || true
    for _ in $(seq 20); do
        [ "$(cat "/sys/class/net/$usbif/operstate" 2>/dev/null)" = "up" ] && break
        sleep 0.5
    done
    [ "$(cat "/sys/class/net/$usbif/operstate" 2>/dev/null)" = up ] \
        || say "WARNING: $usbif is not up - nothing will reach the board over NCM"

    # Every IPv4 address goes: a lease NM managed to hand out before being
    # told off would put the subnet on this interface after all.
    for a in $(ip -o -4 addr show dev "$usbif" 2>/dev/null | awk '{print $4}'); do
        ip addr del "$a" dev "$usbif" 2>/dev/null && say "removed $a from $usbif"
    done
    # ...and ONE goes back on: a link-local /32. Not for routing - a /32 makes
    # no route, so nothing towards the bench subnet changes - but because
    # Linux drops every packet arriving on an interface with NO IPv4 address
    # as soon as rp_filter is on (even loose, =2): the reply from the board
    # came in, was counted under TcpExtIPReversePathFilter and never reached
    # ping (ex94, H563, 14/10). With an address the loose check runs for real
    # and passes, the board's address being reachable via the wired side.
    ip addr add 169.254.94.1/32 dev "$usbif" 2>/dev/null \
        || die "cannot add 169.254.94.1/32 to $usbif"
    say "USB interface $usbif up, only 169.254.94.1/32 on it (reach the board with ping -I $usbif)"
    ;;

ns-usb)
    # The NCM interface moved into its own network namespace, as the HOST
    # behind the board (ex95, the NCM <-> ETH relay).
    #
    # WHY A NAMESPACE
    #
    # ex95 relays between the host on USB and the PC on ETH - and on the bench
    # both are this same machine. Left in one namespace, the kernel sees its
    # own two addresses and delivers host -> PC traffic internally: nothing
    # would ever cross the board. In caos_ncm the USB interface is a separate
    # host with its own routing table, and the only way out is the cable.
    #
    # WHY ONLY `ping` INSIDE IT
    #
    # This script runs as root without a password (sudoers). An "exec any
    # command in the namespace" subcommand would make that root for anything.
    # So each command allowed inside is spelled out here, with its arguments
    # checked.
    need_root
    NS=caos_ncm
    NS_ADDR=192.168.10.200/24
    case "${2:-}" in
    up)
        usbif=""
        for _ in $(seq 30); do
            for i in /sys/class/net/*; do
                n=$(basename "$i")
                [ "$n" = lo ] && continue
                [ -e "$i/device" ] || continue
                case "$(readlink -f "$i/device")" in
                    */usb[0-9]*/*) usbif="$n"; break 2 ;;
                esac
            done
            # already moved: it is no longer visible here
            ip netns exec "$NS" true 2>/dev/null \
                && ip -n "$NS" -o link show 2>/dev/null | grep -q -v ' lo:' \
                && { say "USB interface already in $NS"; exit 0; }
            sleep 0.5
        done
        [ -n "$usbif" ] || die "no USB network interface found after 15 s"
        check_iface "$usbif"
        command -v nmcli >/dev/null 2>&1 && nmcli device set "$usbif" managed no 2>/dev/null
        ip netns list 2>/dev/null | grep -q "^$NS\b" || ip netns add "$NS" \
            || die "cannot create namespace $NS"
        ip link set "$usbif" netns "$NS" || die "cannot move $usbif into $NS"
        ip -n "$NS" link set lo up
        ip -n "$NS" addr flush dev "$usbif"
        ip -n "$NS" addr add "$NS_ADDR" dev "$usbif" || die "cannot add $NS_ADDR"
        ip -n "$NS" link set "$usbif" up
        for _ in $(seq 20); do
            [ "$(ip netns exec "$NS" cat "/sys/class/net/$usbif/operstate" 2>/dev/null)" = up ] && break
            sleep 0.5
        done
        say "$usbif moved into $NS as $NS_ADDR"
        ;;
    down)
        # Deleting the namespace hands the physical interface back to the
        # main one. Nothing to do if it was never created.
        if ip netns list 2>/dev/null | grep -q "^$NS\b"; then
            ip netns del "$NS" && say "namespace $NS removed, USB interface back"
        fi
        ;;
    ping)
        ip_arg="${3:-}"
        cnt="${4:-3}"
        [[ "$ip_arg" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || die "ping: bad address '$ip_arg'"
        [[ "$cnt" =~ ^[0-9]{1,3}$ ]] && [ "$cnt" -ge 1 ] && [ "$cnt" -le 100 ] \
            || die "ping: count 1..100"
        exec ip netns exec "$NS" ping -c "$cnt" -W 2 "$ip_arg"
        ;;
    iperf)
        # iperf2 client inside the namespace, against an `iperf -s` the caller
        # runs unprivileged on the other side (ex96, the relay soak).
        #   up   = host -> PC, down = PC -> host (-R), dual = both on one
        #   socket (--full-duplex). CSV report (-y C): one final line per stream
        #   and direction, plus a SUM line (id -1) when streams > 1.
        # The outer timeout is the point: if the relay dies mid-run the TCP
        # client would otherwise sit there for minutes.
        ip_arg="${3:-}"
        secs="${4:-}"
        dir="${5:-up}"
        streams="${6:-1}"
        [[ "$ip_arg" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || die "iperf: bad address '$ip_arg'"
        [[ "$secs" =~ ^[0-9]{1,4}$ ]] && [ "$secs" -ge 1 ] && [ "$secs" -le 3600 ] \
            || die "iperf: seconds 1..3600"
        [[ "$streams" =~ ^[0-9]$ ]] && [ "$streams" -ge 1 ] && [ "$streams" -le 8 ] \
            || die "iperf: streams 1..8"
        case "$dir" in
            up)   extra=() ;;
            down) extra=(-R) ;;
            dual) extra=(--full-duplex) ;;
            *)    die "iperf: direction up|down|dual" ;;
        esac
        command -v iperf >/dev/null 2>&1 || die "iperf (iperf2) not installed"
        exec timeout --kill-after=5 $((secs + 30)) \
            ip netns exec "$NS" iperf -c "$ip_arg" -t "$secs" \
                -P "$streams" -y C "${extra[@]}"
        ;;
    status)
        # Read-only: what the host side looks like from inside
        ip -n "$NS" -s link show 2>&1
        ip -n "$NS" -4 addr show 2>&1 | grep inet
        ip -n "$NS" neigh show 2>&1
        ip -n "$NS" route show 2>&1
        ;;
    *)
        die "ns-usb: up | down | status | ping <ip> [count] | iperf <ip> <secs> [up|down|dual] [streams]"
        ;;
    esac
    ;;

ns-router)
    # The two PCs of workspace/app_3 (L3 router between two boards): each
    # board's NCM interface in its own namespace, as the host behind it.
    #
    #   caos_rA: NCM of board 1 (MAC 02:ca:05:00:03:31) as 192.168.10.1/24,
    #            routes 192.168.20.0/24 and 192.168.50.0/24 via 192.168.10.100
    #   caos_rB: NCM of board 2 (MAC 02:ca:05:00:03:32) as 192.168.20.1/24,
    #            routes 192.168.10.0/24 and 192.168.50.0/24 via 192.168.20.100
    #   (192.168.50.0/24 = the cable between the boards)
    #
    # The interfaces are found by MAC, not by name or order: the boards give
    # it in their NCM descriptor. As for ns-usb, each command allowed inside
    # a namespace is spelled out here with its arguments checked.
    need_root
    ns_for() { case "$1" in A|a) echo caos_rA ;; B|b) echo caos_rB ;; *) return 1 ;; esac; }
    case "${2:-}" in
    up)
        for side in A B; do
            if [ "$side" = A ]; then
                mac=02:ca:05:00:03:31; addr=192.168.10.1/24; far=192.168.20.0/24; gw=192.168.10.100
            else
                mac=02:ca:05:00:03:32; addr=192.168.20.1/24; far=192.168.10.0/24; gw=192.168.20.100
            fi
            ns=$(ns_for $side)
            if ip netns list 2>/dev/null | grep -q "^$ns\b" && \
               ip -n "$ns" -o link show 2>/dev/null | grep -qi "$mac"; then
                say "$ns: already up"
                continue
            fi
            ifc=""
            for _ in $(seq 30); do
                for i in /sys/class/net/*; do
                    [ "$(cat "$i/address" 2>/dev/null)" = "$mac" ] && { ifc=$(basename "$i"); break 2; }
                done
                sleep 0.5
            done
            [ -n "$ifc" ] || die "no NCM interface with MAC $mac (board $side running app_3?)"
            check_iface "$ifc"
            command -v nmcli >/dev/null 2>&1 && nmcli device set "$ifc" managed no 2>/dev/null
            ip netns list 2>/dev/null | grep -q "^$ns\b" || ip netns add "$ns" || die "cannot create $ns"
            ip link set "$ifc" netns "$ns" || die "cannot move $ifc into $ns"
            ip -n "$ns" link set lo up
            ip -n "$ns" addr flush dev "$ifc"
            ip -n "$ns" addr add "$addr" dev "$ifc" || die "cannot add $addr"
            # 1458 = 1500 - 42: a UDP packet on the encrypted port grows by up
            # to 42 bytes when the router seals it, and must still fit 1500
            ip -n "$ns" link set "$ifc" mtu 1458
            ip -n "$ns" link set "$ifc" up
            ip -n "$ns" route add "$far" via "$gw" dev "$ifc" || die "cannot add route $far via $gw"
            ip -n "$ns" route add 192.168.50.0/24 via "$gw" dev "$ifc" \
                || die "cannot add route 192.168.50.0/24 via $gw"
            say "$ns: $ifc as $addr, $far and 192.168.50.0/24 via $gw"
        done
        ;;
    down)
        for ns in caos_rA caos_rB; do
            if ip netns list 2>/dev/null | grep -q "^$ns\b"; then
                ip netns del "$ns" && say "namespace $ns removed"
            fi
        done
        ;;
    ping)
        ns=$(ns_for "${3:-}") || die "ns-router ping A|B <ip> [count]"
        ip_arg="${4:-}"
        cnt="${5:-3}"
        [[ "$ip_arg" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || die "ping: bad address '$ip_arg'"
        [[ "$cnt" =~ ^[0-9]{1,3}$ ]] && [ "$cnt" -ge 1 ] && [ "$cnt" -le 100 ] \
            || die "ping: count 1..100"
        exec ip netns exec "$ns" ping -c "$cnt" -W 2 "$ip_arg"
        ;;
    iperf-s)
        # iperf2 server inside A or B, for <secs> at most (the caller puts it
        # in background). udp|tcp, on <port>.
        ns=$(ns_for "${3:-}") || die "ns-router iperf-s A|B <port> <secs> udp|tcp"
        port="${4:-}"; secs="${5:-}"; proto="${6:-udp}"
        [[ "$port" =~ ^[0-9]{1,5}$ ]] && [ "$port" -ge 1024 ] && [ "$port" -le 65535 ] \
            || die "iperf-s: port 1024..65535"
        [[ "$secs" =~ ^[0-9]{1,4}$ ]] && [ "$secs" -ge 1 ] && [ "$secs" -le 3700 ] \
            || die "iperf-s: seconds 1..3700"
        case "$proto" in udp) u=(-u) ;; tcp) u=() ;; *) die "iperf-s: udp|tcp" ;; esac
        command -v iperf >/dev/null 2>&1 || die "iperf (iperf2) not installed"
        exec timeout --kill-after=5 "$secs" ip netns exec "$ns" iperf -s "${u[@]}" -p "$port" -y C
        ;;
    iperf)
        # iperf2 client inside A or B toward <ip>:<port>; udp at <mbps>.
        ns=$(ns_for "${3:-}") || die "ns-router iperf A|B <ip> <port> <secs> udp|tcp [mbps] [len]"
        ip_arg="${4:-}"; port="${5:-}"; secs="${6:-}"; proto="${7:-udp}"; mbps="${8:-1}"; len="${9:-1400}"
        [[ "$ip_arg" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || die "iperf: bad address '$ip_arg'"
        [[ "$port" =~ ^[0-9]{1,5}$ ]] && [ "$port" -ge 1024 ] && [ "$port" -le 65535 ] \
            || die "iperf: port 1024..65535"
        [[ "$secs" =~ ^[0-9]{1,4}$ ]] && [ "$secs" -ge 1 ] && [ "$secs" -le 3600 ] \
            || die "iperf: seconds 1..3600"
        [[ "$mbps" =~ ^[0-9]{1,3}(\.[0-9]+)?$ ]] || die "iperf: mbps"
        [[ "$len" =~ ^[0-9]{1,4}$ ]] && [ "$len" -ge 16 ] && [ "$len" -le 1472 ] \
            || die "iperf: len 16..1472"
        case "$proto" in
            udp) extra=(-u -b "${mbps}M" -l "$len") ;;
            tcp) extra=() ;;
            *)   die "iperf: udp|tcp" ;;
        esac
        command -v iperf >/dev/null 2>&1 || die "iperf (iperf2) not installed"
        exec timeout --kill-after=5 $((secs + 30)) \
            ip netns exec "$ns" iperf -c "$ip_arg" -p "$port" -t "$secs" -y C "${extra[@]}"
        ;;
    status)
        for ns in caos_rA caos_rB; do
            echo "== $ns"
            ip -n "$ns" -4 addr show 2>&1 | grep inet
            ip -n "$ns" route show 2>&1
            ip -n "$ns" neigh show 2>&1
        done
        ;;
    *)
        die "ns-router: up | down | status | ping A|B <ip> [count] | iperf-s A|B <port> <secs> udp|tcp | iperf A|B <ip> <port> <secs> udp|tcp [mbps] [len]"
        ;;
    esac
    ;;

usb-restore)
    # Give the USB interface back to NetworkManager.
    need_root
    usbif="${2:-}"
    if [ -z "$usbif" ]; then
        for i in /sys/class/net/*; do
            n=$(basename "$i")
            [ "$n" = lo ] && continue
            [ -e "$i/device" ] || continue
            case "$(readlink -f "$i/device")" in
                */usb[0-9]*/*) usbif="$n"; break ;;
            esac
        done
    fi
    [ -n "$usbif" ] || { say "no USB network interface present - nothing to restore"; exit 0; }
    check_iface "$usbif"

    # The DHCP server first: it is bound to this interface, and leaving it
    # running on a device about to change hands serves leases on a link the
    # bench no longer controls.
    if [ -x "$DHCP" ] && "$DHCP" status >/dev/null 2>&1; then
        "$DHCP" stop >/dev/null 2>&1 && say "DHCP stopped on $usbif"
    fi

    if command -v nmcli >/dev/null 2>&1; then
        nmcli device set "$usbif" managed yes 2>/dev/null \
            && say "$usbif handed back to NetworkManager"
    fi
    ;;

down)
    subnet="${3:-$SUBNET_DEFAULT}"
    check_subnet "$subnet"
    need_root

    [ -x "$DHCP" ] && "$DHCP" stop

    # Transmit queues back to what they were, for every interface we changed -
    # driven by the saved files rather than by $OURSFILE, because the queues
    # may have been reduced on an interface whose ADDRESS was not ours.
    for f in "$STATEDIR"/txq.*; do
        [ -e "$f" ] || continue
        txq_restore "$(basename "$f" | sed 's/^txq\.//')"
    done

    # Take .1 off again, but only if this script is the one that put it there:
    # a machine that already had 192.168.10.1 configured keeps it.
    if [ -f "$OURSFILE" ]; then
        iface=$(cat "$OURSFILE" 2>/dev/null)
        if [ -n "$iface" ] && [ -e "/sys/class/net/$iface" ]; then
            ip addr flush dev "$iface"
            ip addr del "$(peer_ip "$subnet")" dev "$iface" 2>/dev/null \
                && say "$(peer_ip "$subnet") removed from $iface"
        fi
        rm -f "$OURSFILE"
    else
        say "$(peer_ip "$subnet") was not ours - left in place"
    fi

    say "addresses cleared by 'up' are not restored here - netplan apply,"
    say "nmcli device reapply, or add them back by hand."
    ;;

status)
    subnet="${2:-$SUBNET_DEFAULT}"
    check_subnet "$subnet"
    rc=0

    iface=$(detect_iface "$subnet" 2>/dev/null) || iface=""
    if [ -n "$iface" ]; then
        echo "iface   : $iface"
    else
        echo "iface   : NONE on $subnet.0/24"; rc=1
    fi

    if peer_is_up "$subnet"; then
        echo "peer ip : $(peer_ip "$subnet") up"
    else
        echo "peer ip : $(peer_ip "$subnet") MISSING"; rc=1
    fi

    # Presence is not enough: with another address also on the subnet, .1 can
    # be up and still not be the address the board sees. Ask the kernel what it
    # would actually put in the packet.
    src=$(src_towards "$subnet")
    if [ "$src" = "$subnet.$PEER_HOST" ]; then
        echo "src     : $src (what the board will see)"
    elif [ -n "$src" ]; then
        echo "src     : $src - WRONG, the board would see this instead of $subnet.$PEER_HOST"
        rc=1
    else
        echo "src     : no route to $subnet.0/24"; rc=1
    fi

    # The cause behind a wrong src, named rather than left to be guessed.
    if [ -n "$iface" ]; then
        others=$(others_on_subnet "$iface" "$subnet" | tr '\n' ' ')
        [ -n "$others" ] && {
            echo "others  : $others on $iface - 'up' removes these"; rc=1; }
    fi

    if [ -x "$DHCP" ] && "$DHCP" status >/dev/null 2>&1; then
        # WHERE it is listening, not merely that it exists: a server left on the
        # USB NIC after an NCM class serves offers on a link the wired board
        # cannot hear, and "running" would be a green answer to the wrong
        # question.
        dhcpif=$(ps -o cmd= -p "$(cat "$STATEDIR/dnsmasq.pid" 2>/dev/null)" 2>/dev/null \
                 | grep -o -- "--interface=[^ ]*" | head -1 | cut -d= -f2)
        if [ -z "$iface" ] || [ "$dhcpif" = "$iface" ]; then
            echo "dhcp    : running on ${dhcpif:-?}"
        else
            echo "dhcp    : running on ${dhcpif:-?} - WRONG, the board is on $iface"
            rc=1
        fi
    else
        echo "dhcp    : not running"; rc=1
    fi

    # What actually took a lease - the only proof a board is on the link.
    if [ -s "$STATEDIR/dnsmasq.leases" ]; then
        echo "leases  :"
        awk '{printf "          %-16s %s\n", $3, $2}' "$STATEDIR/dnsmasq.leases"
    else
        echo "leases  : none yet"
    fi
    exit $rc
    ;;

txq)
    # Done on its own, for a link that is already up: `up` does it too, but
    # re-running the whole thing between measurements would restart DHCP and
    # bounce the board's lease.
    need_root
    action="${2:-status}"
    iface=$(resolve_iface "${3:-}" "$SUBNET_DEFAULT") || exit 1
    case "$action" in
    one)     txq_one "$iface" ;;
    restore) txq_restore "$iface" ;;
    status)
        # Says what WOULD happen as much as what is: run it on a machine before
        # trusting a parallel test's fairness there.
        if read -r kind cur < <(txq_current "$iface"); then
            say "$iface: $cur $kind channel(s), $(txq_hw_count "$iface") transmit queue(s)"
            [ "$cur" -gt 1 ] 2>/dev/null \
                && say "  'txq one' would run: ethtool -L $iface $kind 1"
        else
            say "$iface: no channel control, $(txq_hw_count "$iface") transmit queue(s)"
            say "  'txq one' would leave it alone"
        fi
        f="$(txq_file "$iface")"
        [ -f "$f" ] && say "  reduced by this script, was $(cat "$f")"
        # XPS off means the queue is chosen by the flow's hash, which is the
        # whole reason this subcommand exists - so report it either way.
        xps=$(cat /sys/class/net/"$iface"/queues/tx-0/xps_cpus 2>/dev/null | tr -d '0,')
        [ -z "$xps" ] && say "  XPS unconfigured: the queue comes from the flow hash"
        ;;
    *) die "usage: $(basename "$0") txq one|restore|status [iface]" ;;
    esac
    ;;

*)
    echo "usage: $(basename "$0") <command>
  up   [iface] [subnet]   ETH: clear the subnet, put .1 on it, start DHCP
  down [iface] [subnet]   stop DHCP and remove .1
  txq  one|restore|status [iface]
                          one transmit queue, so a parallel test measures the
                          board and not the NIC's queue hash (see ex85)
  usb  [iface] [subnet]   NCM: unmanage the USB NIC, give it .1, serve DHCP
  usb-restore [iface]     hand the USB NIC back to NetworkManager
  eth  up|down [iface]    the wired interface - down while testing NCM
  eth  bounce [seconds]   drop the carrier and bring it back (default 3 s),
                          leaving addresses, DHCP and leases in place: what a
                          board sees when the cable is pulled and replugged
  peer 39|43|49|74|78|82 | stop  a counterpart needing a privileged port
  tcpdump <ipv4> [count] [iface]  capture on that link (max 200 packets)
  capture <ipv4> [iface]  start a .pcap capture (fixed path, ends with capstop)
  capstop                 stop it and hand the file to the calling user
  echo84 <iface> [count] [size] [mbps]
                                 ex84: raw L2 echo generator (needs raw sockets).
                                 mbps 0 = unpaced; a rate paces the sender, to
                                 find where the board starts refusing frames
  status [subnet]         exit 0 when the ETH link is fully up" >&2
    exit 2
    ;;
esac
