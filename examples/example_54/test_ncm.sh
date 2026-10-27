#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Angelo Coppi
#
# HW test of the USB NCM service (example_54, L2 loopback with SA<->DA swap).
#
# Sets the USB Ethernet interface up with a static IP, brings it up and starts
# tcpdump to watch the frames bouncing back from the device (SA<->DA swapped).
#
# Usage:
#   sudo ./test_ncm.sh                # default usb0
#   sudo ./test_ncm.sh enx02ca05000001
#
# Needs: sudo (ip, tcpdump), iproute2, tcpdump.

set -euo pipefail

IFACE="${1:-usb0}"
IP_LOCAL="192.168.7.1/24"
IP_PEER="192.168.7.2"          # dummy IP in the subnet (triggers ARP)

# --- Colors -------------------------------------------------------------------
RED=$'\e[31m'; GRN=$'\e[32m'; YEL=$'\e[33m'; RST=$'\e[0m'
ok()   { echo "${GRN}[OK]${RST}  $*"; }
info() { echo "${YEL}[..]${RST}  $*"; }
die()  { echo "${RED}[ERR]${RST} $*" >&2; exit 1; }

# --- Prerequisites ------------------------------------------------------------
for t in ip tcpdump sudo; do
    command -v "$t" >/dev/null 2>&1 || die "missing command: $t"
done
[[ -e "/sys/class/net/$IFACE" ]] || die "interface '$IFACE' not found (check 'ip link')"

# --- Automatic cleanup --------------------------------------------------------
cleanup() {
    sudo ip addr del "$IP_LOCAL" dev "$IFACE" 2>/dev/null || true
}
trap cleanup EXIT

# --- 1) Interface setup -------------------------------------------------------
info "interface: $IFACE"
sudo ip addr flush dev "$IFACE" 2>/dev/null || true
sudo ip addr add "$IP_LOCAL" dev "$IFACE"
sudo ip link set "$IFACE" up
ok "configured: $IP_LOCAL on $IFACE"

ip -br addr show "$IFACE"
ip -br link show "$IFACE"
echo

# --- 2) Unicast loopback test with custom EtherType 0x88B5 --------------------
# Injects unicast Ethernet frames: DA=dummy MAC, SA=host MAC.
# The device swaps SA<->DA and sends them back -> they return with DA=host MAC,
# SA=dummy MAC (both valid) -> Linux accepts them -> tcpdump shows them.
# EtherType 0x88B5 (Local Experimental) to filter them out of the network noise.
HOST_MAC=$(cat "/sys/class/net/$IFACE/address")
PEER_MAC="02:de:ad:be:ef:99"          # dummy destination MAC
N_FRAMES=5

info "host MAC: $HOST_MAC   peer MAC (dummy): $PEER_MAC"
info "starting tcpdump (EtherType 0x88B5, Ctrl-C to stop)..."
echo "    TX host : $HOST_MAC > $PEER_MAC"
echo "    RX echo : $PEER_MAC > $HOST_MAC   (the device swapped SA<->DA)"
echo

# tcpdump in background, filtered on our EtherType
sudo tcpdump -i "$IFACE" -e -n -l 'ether proto 0x88B5' &
TCPDUMP_PID=$!
trap 'sudo kill $TCPDUMP_PID 2>/dev/null; cleanup' EXIT
sleep 1

# Inject the frames through a raw socket (Python stdlib, no dependencies)
info "injecting $N_FRAMES unicast test frames..."
sudo python3 - "$IFACE" "$HOST_MAC" "$PEER_MAC" "$N_FRAMES" <<'PYEOF'
import socket, sys, time

iface, host_mac, peer_mac, n = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
def mac(s): return bytes(int(b, 16) for b in s.split(":"))
da, sa = mac(peer_mac), mac(host_mac)        # DA=peer, SA=host
etype  = b"\x88\xB5"

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
s.bind((iface, 0))
for i in range(n):
    payload = b"caOS-NCM-LOOPBACK-%03d" % i
    payload += b"\x00" * (46 - len(payload))  # pad to the minimum frame
    s.send(da + sa + etype + payload)
    time.sleep(0.2)
s.close()
print("  inviati %d frame" % n)
PYEOF

sleep 2
echo
ok "injection done - check the RX lines above ($PEER_MAC > $HOST_MAC)"
info "press Ctrl-C to stop tcpdump"
wait $TCPDUMP_PID 2>/dev/null || true
