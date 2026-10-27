#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Angelo Coppi
#
# HW test of the caOS network stack on USB-NCM (example_55: ARP + ICMP echo).
#
# ex55 registers ONLY ARP and ICMP: the device opens NO TCP/UDP port, so it is
# not tested with nc -- nothing is listening to connect to. The right checks
# are the ones the example itself describes: arping (layer 2) and
# ping (layer 3).
#
# Usage:
#   ./test_net_ncm.sh                     # default usb0
#   ./test_net_ncm.sh enx02ca05000055
#
# Needs: sudo (ip, ping, arping), iproute2, iputils-arping.

set -euo pipefail

IFACE="${1:-usb0}"
IP_LOCAL="192.168.10.1/24"
IP_DEV="192.168.10.55"          # device IP, from ca_os_ex55.c
MAC_DEV="02:ca:05:00:00:55"     # device MAC, from ca_os_ex55.c
N_PING=5
N_ARP=3

# -- Colors -------------------------------------------------------------------
RED=$'\e[31m'; GRN=$'\e[32m'; YEL=$'\e[33m'; RST=$'\e[0m'
ok()   { echo "${GRN}[OK]${RST}  $*"; }
info() { echo "${YEL}[..]${RST}  $*"; }
die()  { echo "${RED}[ERR]${RST} $*" >&2; exit 1; }

fail=0
bad()  { echo "${RED}[NO]${RST}  $*"; fail=1; }

# -- Prerequisites ------------------------------------------------------------
for t in ip ping sudo; do
    command -v "$t" >/dev/null 2>&1 || die "missing command: $t"
done
command -v arping >/dev/null 2>&1 || info "arping missing: skipping the ARP test (apt install iputils-arping)"
[[ -e "/sys/class/net/$IFACE" ]] || die "interface '$IFACE' not found (check 'ip link')"

# -- Cleanup ------------------------------------------------------------------
cleanup() {
    sudo ip addr del "$IP_LOCAL" dev "$IFACE" 2>/dev/null || true
}
trap cleanup EXIT

# -- 1) Interface setup -------------------------------------------------------
info "interface: $IFACE"
sudo ip addr flush dev "$IFACE" 2>/dev/null || true
sudo ip addr add "$IP_LOCAL" dev "$IFACE"
sudo ip link set "$IFACE" up
ok "configured: $IP_LOCAL on $IFACE"

ip -br addr show "$IFACE"
echo

# The ARP cache has to be flushed: an entry left by an earlier run would let
# the ping pass without the device answering a single ARP.
sudo ip neigh flush dev "$IFACE" 2>/dev/null || true

# -- 2) ARP (layer 2) ---------------------------------------------------------
if command -v arping >/dev/null 2>&1; then
    info "arping $IP_DEV x$N_ARP (ARP request -> device reply)..."
    if sudo arping -c "$N_ARP" -I "$IFACE" "$IP_DEV"; then
        ok "ARP: the device answers"
    else
        bad "ARP: no reply from $IP_DEV"
    fi
    echo
fi

# -- 3) ICMP (layer 3) --------------------------------------------------------
# No "-i 0.3": sub-second intervals are privileged and some ping builds
# refuse a decimal interval altogether ("bad timing interval"), failing the
# command without sending a single packet.
info "ping $IP_DEV x$N_PING (ICMP echo request -> device reply)..."
if ping -c "$N_PING" -W 2 "$IP_DEV"; then
    ok "ICMP: the device answers"
else
    bad "ICMP: no reply from $IP_DEV"
fi
echo

# -- 4) ARP cache check -------------------------------------------------------
# Proves that the learned MAC really is the one declared in ca_os_ex55.c, not
# just any device holding the same IP.
info "ARP cache learned on $IFACE:"
neigh=$(ip neigh show dev "$IFACE" | grep -iw "$IP_DEV" || true)
if [[ -z "$neigh" ]]; then
    bad "no ARP entry for $IP_DEV"
else
    echo "  $neigh"
    if grep -qi "$MAC_DEV" <<<"$neigh"; then
        ok "MAC matches the example's ($MAC_DEV)"
    else
        bad "MAC differs from the expected one ($MAC_DEV)"
    fi
fi
echo

# -- Result -------------------------------------------------------------------
if (( fail )); then
    echo "${RED}=== TEST EX55 FAILED ===${RST}"
    exit 1
fi
echo "${GRN}=== TEST EX55 OK ===${RST}"
