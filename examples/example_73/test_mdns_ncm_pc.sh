#!/usr/bin/env bash
# test_mdns_ncm_pc.sh - PC-side test harness for caOS example 73
#                       (mDNS responder on USB-NCM)
#
# PC requirements:
#   sudo apt install dnsmasq avahi-utils nmap python3
#
# What it does:
#   1. Sets up DHCP (dnsmasq) - same setup as ex70/71/72
#   2. Checks that the PC answers on 224.0.0.251:5353 (avahi)
#   3. Resolves "caos-board.local" with avahi-resolve and with a Python script
#   4. Sends a raw mDNS query (no avahi) to test the direct reply
#
# NB unlike DHCP/NTP/DNS, mDNS does NOT need a server on the PC: it is
# peer-to-peer multicast. Avahi is only the CLIENT that queries the board;
# the raw Python test works without it too.

set -euo pipefail

# --- Configuration ------------------------------------------------------------
# POWER-ON ORDER: the BOARD first, then this script.
# The USB network interface does not exist until the device has enumerated:
# it is not like eth0, which is always there.
NCM_MAC_PREFIX="02:ca:05"        # host-side MAC announced by the NCM descriptors

find_ncm_iface() {
    local i
    for i in $(ls /sys/class/net); do
        [[ "$i" == "lo" ]] && continue
        if [[ "$(cat /sys/class/net/$i/address 2>/dev/null)" == ${NCM_MAC_PREFIX}* ]]; then
            echo "$i"; return 0
        fi
    done
    return 1
}

# Interface: first argument, the IFACE environment variable, or auto-detect.
IFACE="${1:-${IFACE:-}}"
if [[ -z "$IFACE" ]]; then
    echo "[*] Looking for the USB-NCM interface (MAC ${NCM_MAC_PREFIX}:*)..."
    for _ in $(seq 1 30); do
        IFACE="$(find_ncm_iface || true)"
        [[ -n "$IFACE" ]] && break
        sleep 1
    done
    if [[ -z "$IFACE" ]]; then
        echo "[!] No NCM interface found in 30s."
        echo "    Connect the USB cable and start the ex73 firmware, or"
        echo "    give the interface by hand:  sudo $0 <iface>"
        echo "    (list: ip -br link)"
        exit 1
    fi
    echo "[+] NCM interface: $IFACE"
fi
PC_IP="192.168.10.1"
PC_NET="192.168.10.0/24"
DHCP_RANGE_START="192.168.10.100"
DHCP_RANGE_END="192.168.10.200"
DHCP_LEASE_TIME="12h"
MDNS_HOSTNAME="caos-board"            # must match MDNS_HOSTNAME in ex73

# --- Helpers ------------------------------------------------------------------
info()  { echo -e "\033[1;34m[INFO]\033[0m  $*"; }
ok()    { echo -e "\033[1;32m[ OK ]\033[0m  $*"; }
warn()  { echo -e "\033[1;33m[WARN]\033[0m  $*"; }
fail()  { echo -e "\033[1;31m[FAIL]\033[0m  $*"; exit 1; }

require_root() { [[ $EUID -eq 0 ]] || fail "Run as root: sudo $0"; }

# --- PC interface setup -------------------------------------------------------
setup_iface() {
    ip link show "$IFACE" &>/dev/null || \
        fail "Interface '$IFACE' not found. Available: $(ip -br link show | awk '{print $1}' | grep -v '^lo$' | tr '\n' ' ')"
    info "Configuring interface $IFACE -> $PC_IP"
    ip addr flush dev "$IFACE" 2>/dev/null || true
    ip addr add "$PC_IP/24" dev "$IFACE" 2>/dev/null || true
    ip link set "$IFACE" up
    ok "Interface configured"
}

# --- DHCP with dnsmasq ---------------------------------------------------------
DNSMASQ_PID=""
DNSMASQ_CONF=$(mktemp /tmp/dnsmasq_ex73.XXXXXX.conf)

setup_dhcp() {
    info "Starting dnsmasq (DHCP on $IFACE)"

    # Stop earlier instances and remove stale files
    pkill -f "dnsmasq.*ex73" 2>/dev/null || true
    sleep 0.5
    rm -f /tmp/dnsmasq_ex73.pid /tmp/dnsmasq_ex73.log

    cat > "$DNSMASQ_CONF" <<EOF
port=0
interface=$IFACE
bind-interfaces
dhcp-range=$DHCP_RANGE_START,$DHCP_RANGE_END,$DHCP_LEASE_TIME
dhcp-option=option:router,$PC_IP
dhcp-option=option:dns-server,$PC_IP
dhcp-option=option:ntp-server,$PC_IP
log-dhcp
EOF

    dnsmasq --conf-file="$DNSMASQ_CONF" --pid-file=/tmp/dnsmasq_ex73.pid \
            --log-facility=/tmp/dnsmasq_ex73.log
    sleep 1

    # dnsmasq forks: read the PID from the pid-file instead of using $!
    if [[ -f /tmp/dnsmasq_ex73.pid ]]; then
        DNSMASQ_PID=$(cat /tmp/dnsmasq_ex73.pid)
        if kill -0 "$DNSMASQ_PID" 2>/dev/null; then
            ok "dnsmasq started (PID=$DNSMASQ_PID)"
        else
            fail "dnsmasq stopped at once - check /tmp/dnsmasq_ex73.log"
        fi
    else
        fail "dnsmasq did not start - check /tmp/dnsmasq_ex73.log"
    fi
}

# --- Avahi (PC native mDNS) ---------------------------------------------------
setup_avahi() {
    info "Checking avahi-daemon..."
    if systemctl is-active --quiet avahi-daemon 2>/dev/null; then
        ok "avahi-daemon active"
    else
        warn "avahi-daemon not active - starting it..."
        systemctl start avahi-daemon 2>/dev/null || \
            warn "avahi not available, avahi test skipped"
    fi
}

# --- Test: avahi-resolve resolution -------------------------------------------
test_avahi_resolve() {
    info "Test avahi-resolve ${MDNS_HOSTNAME}.local ..."

    if ! command -v avahi-resolve &>/dev/null; then
        warn "avahi-resolve not found (sudo apt install avahi-utils)"
        return
    fi

    local result
    if result=$(avahi-resolve -n "${MDNS_HOSTNAME}.local" 2>&1); then
        ok "avahi-resolve: $result"
    else
        warn "avahi-resolve FAILED: $result"
        warn "Make sure the board is connected and DHCP has completed"
    fi
}

# --- Test: ping .local ---------------------------------------------------------
test_ping_local() {
    info "Test ping ${MDNS_HOSTNAME}.local (3 packets)..."
    if ping -c 3 -W 2 "${MDNS_HOSTNAME}.local" 2>&1; then
        ok "ping ${MDNS_HOSTNAME}.local OK"
    else
        warn "ping ${MDNS_HOSTNAME}.local FAILED"
        warn "Possible cause: DHCP not completed or mDNS not announced yet"
    fi
}

# --- Test: raw mDNS query (Python) - bypasses avahi ---------------------------
MDNS_PY=$(mktemp /tmp/mdns_query_XXXXXX.py)
cat > "$MDNS_PY" <<'PYEOF'
#!/usr/bin/env python3
"""
Sends an mDNS A-record query for <hostname>.local to 224.0.0.251:5353
and waits for the direct reply from the board (without going through avahi).
"""
import socket
import struct
import sys
import time

def encode_name(name: str) -> bytes:
    out = b""
    for label in name.split("."):
        enc = label.encode("ascii")
        out += bytes([len(enc)]) + enc
    return out + b"\x00"

def build_query(txid: int, hostname: str) -> bytes:
    name = encode_name(f"{hostname}.local")
    hdr  = struct.pack("!HHHHHH", txid, 0, 1, 0, 0, 0)  # QR=0 query, QDCOUNT=1
    q    = name + struct.pack("!HH", 1, 1)               # TYPE=A, CLASS=IN
    return hdr + q

def parse_response(data: bytes, hostname: str):
    if len(data) < 12:
        return None
    _txid, flags, qdcount, ancount = struct.unpack("!HHHH", data[:8])
    # QR bit must be 1 (response/announce)
    if not (flags & 0x8000):
        return None
    if ancount == 0:
        return None

    off = 12
    # Skip exactly qdcount questions (mDNS announces have qdcount=0)
    for _ in range(qdcount):
        while off < len(data):
            if data[off] & 0xC0 == 0xC0:
                off += 2; break
            if data[off] == 0:
                off += 1; break
            off += data[off] + 1
        off += 4  # QTYPE + QCLASS

    # Parse answer records
    for _ in range(ancount):
        if off >= len(data):
            break
        if data[off] & 0xC0 == 0xC0:
            off += 2
        else:
            while off < len(data) and data[off] != 0:
                off += data[off] + 1
            off += 1
        if off + 10 > len(data):
            break
        rtype, _, _, rdlen = struct.unpack("!HHIH", data[off:off+10])
        off += 10
        if rtype == 1 and rdlen == 4 and off + 4 <= len(data):
            return ".".join(str(b) for b in data[off:off+4])
        off += rdlen
    return None

def main():
    if len(sys.argv) < 3:
        print("uso: mdns_query.py <hostname> <local_ip> [timeout_s]")
        sys.exit(1)
    hostname  = sys.argv[1]
    local_ip  = sys.argv[2]
    timeout   = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0

    txid = 0  # mDNS (RFC 6762): transaction ID must be 0
    pkt  = build_query(txid, hostname)

    MDNS_GRP  = "224.0.0.251"
    MDNS_ADDR = (MDNS_GRP, 5353)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    # Force outgoing multicast on the correct interface (not loopback or another NIC)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                    socket.inet_aton(local_ip))
    # Join the multicast group on the same interface to receive responses
    mreq = struct.pack("4s4s", socket.inet_aton(MDNS_GRP), socket.inet_aton(local_ip))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    sock.bind(("", 5353))
    sock.settimeout(timeout)

    print(f"[mDNS] query for {hostname}.local ...")
    sock.sendto(pkt, MDNS_ADDR)

    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data, addr = sock.recvfrom(1500)
        except socket.timeout:
            break
        ip = parse_response(data, hostname)
        if ip:
            print(f"[mDNS] {hostname}.local  ->  {ip}  (from {addr[0]})")
            sock.close()
            sys.exit(0)

    print(f"[mDNS] TIMEOUT - no reply for {hostname}.local")
    sock.close()
    sys.exit(1)

main()
PYEOF

test_mdns_raw() {
    info "Test raw mDNS (Python, bypassing avahi) ..."
    # IP_MULTICAST_IF needs an address REALLY assigned to the interface,
    # otherwise setsockopt fails with Errno 99 before the query is even sent
    # (and the error message would wrongly blame the board). Do not take
    # $PC_IP for granted: NetworkManager or an earlier setup may have left a
    # different address on the NIC.
    local real_ip
    real_ip="$(ip -4 -br addr show "$IFACE" | awk '{print $3}' | cut -d/ -f1)"
    if [[ -z "$real_ip" ]]; then
        warn "raw mDNS SKIPPED - no IPv4 on $IFACE"
        return
    fi
    if [[ "$real_ip" != "$PC_IP" ]]; then
        warn "Real IP of $IFACE = $real_ip (expected $PC_IP): using the real one"
    fi
    if python3 "$MDNS_PY" "$MDNS_HOSTNAME" "$real_ip" 5; then
        ok "mDNS raw OK"
    else
        warn "raw mDNS FAILED - no multicast reply from $MDNS_HOSTNAME.local"
    fi
}

# --- Cleanup ------------------------------------------------------------------
cleanup() {
    info "Cleanup..."
    [[ -n "$DNSMASQ_PID" ]] && kill "$DNSMASQ_PID" 2>/dev/null || true
    pkill -f "dnsmasq.*ex73" 2>/dev/null || true
    rm -f "$DNSMASQ_CONF" "$MDNS_PY"
    ok "Cleanup done"
}
trap cleanup EXIT

# --- Main ---------------------------------------------------------------------
require_root

echo ""
echo "============================================================"
echo "  caOS Example 73 - mDNS responder test on USB-NCM"
echo "  Interface   : $IFACE"
echo "  PC IP       : $PC_IP"
echo "  mDNS target : ${MDNS_HOSTNAME}.local"
echo "============================================================"
echo ""

setup_iface
setup_dhcp
setup_avahi

echo ""
info "=== Waiting for the board online (DHCP + mDNS announce) ==="
# The board is already connected: auto-detect has just found it by its MAC.
# The wait is for the DORA, which starts only now that dnsmasq is up, and for
# the mDNS announce that follows it (it is hooked to on_dhcp_bound).
info "    dnsmasq has just started: waiting for the lease and the announce (~5s) ..."
sleep 5

echo ""
info "=== Test 1: avahi-resolve ==="
test_avahi_resolve

echo ""
info "=== Test 2: ping .local ==="
test_ping_local

echo ""
info "=== Test 3: query mDNS raw ==="
test_mdns_raw

echo ""
ok "========================================"
ok " Tests done"
ok " Hostname: ${MDNS_HOSTNAME}.local"
ok "========================================"
echo ""
info "Monitor DHCP leases: tail -f /tmp/dnsmasq_ex73.log"
info "Monitor mDNS:        sudo tcpdump -i $IFACE udp port 5353 -vvv"
info ""
info "Press Ctrl+C to quit and clean up."
sleep infinity
