#!/usr/bin/env bash
# test_dns_pc.sh <interface>
# Sets the PC up as gateway + DHCP server + NTP server + DNS server to test caOS ex37.
# Needs: dnsmasq, ip, ping, python3
# Usage: sudo ./test_dns_pc.sh enp6s0

set -euo pipefail

IF="${1:-}"
if [[ -z "$IF" ]]; then
    echo "Usage: sudo $0 <interface>  (e.g. enp6s0, enp7s0, eth0)"
    exit 1
fi

PC_IP="192.168.10.1"
DHCP_START="192.168.10.100"
DHCP_END="192.168.10.200"
MASK="255.255.255.0"
LEASE="1h"
DHCP_CONF="/tmp/dnsmasq-caos.conf"
DHCP_PID="/tmp/dnsmasq-caos.pid"
NTP_PID="/tmp/ntp-caos.pid"
NTP_LOG="/tmp/ntp-caos.log"
DNS_PID="/tmp/dns-caos.pid"
DNS_LOG="/tmp/dns-caos.log"

# --------------------------------------------------------------------------
stop_dnsmasq() {
    if [[ -f "$DHCP_PID" ]]; then
        echo "[*] Stop dnsmasq (pid $(cat $DHCP_PID))"
        kill "$(cat $DHCP_PID)" 2>/dev/null || true
        rm -f "$DHCP_PID"
    else
        pkill -f "dnsmasq-caos" 2>/dev/null || true
    fi
}

stop_ntp() {
    if [[ -f "$NTP_PID" ]]; then
        echo "[*] Stop NTP server (pid $(cat $NTP_PID))"
        kill "$(cat $NTP_PID)" 2>/dev/null || true
        rm -f "$NTP_PID"
    fi
}

stop_dns() {
    if [[ -f "$DNS_PID" ]]; then
        echo "[*] Stop DNS server (pid $(cat $DNS_PID))"
        kill "$(cat $DNS_PID)" 2>/dev/null || true
        rm -f "$DNS_PID"
    fi
}

NTP_STOPPED_SERVICE=""

cleanup() {
    trap - EXIT INT TERM          # once: Ctrl-C used to run it twice
    echo ""
    echo "[*] Cleanup..."
    stop_dns
    stop_ntp
    stop_dnsmasq
    ip addr del "${PC_IP}/24" dev "$IF" 2>/dev/null || true
    if [[ -n "$NTP_STOPPED_SERVICE" ]]; then
        echo "[*] Restoring $NTP_STOPPED_SERVICE"
        systemctl start "$NTP_STOPPED_SERVICE" 2>/dev/null || true
    fi
    echo "[*] Interface $IF restored."
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

# One UDP datagram to the board, its reply on stdout (nothing on timeout).
# python3, not 'nc -u': nc may keep listening after the reply (the script
# hung there), and the reply carries a NUL that bash warns about.
udp_echo() {
    python3 - "$1" "$2" "$3" <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2.0)
s.sendto(sys.argv[3].encode(), (sys.argv[1], int(sys.argv[2])))
try:
    print(s.recv(1500).replace(b"\0", b"").decode(errors="replace"))
except socket.timeout:
    pass
PY
}

# --------------------------------------------------------------------------
echo "============================================================"
echo " caOS EX37 - DHCP + NTP + DNS test  (interface: $IF)"
echo "============================================================"

if ! ip link show "$IF" &>/dev/null; then
    echo "[!] Interface $IF not found."
    exit 1
fi

stop_dns
stop_ntp
stop_dnsmasq

echo "[*] Configure $IF  ->  $PC_IP/$MASK"
ip addr flush dev "$IF" 2>/dev/null || true
ip addr add "${PC_IP}/24" dev "$IF"
ip link set "$IF" up
echo "[*] Link UP"

# --------------------------------------------------------------------------
# Minimal Python NTP server
# --------------------------------------------------------------------------
setup_ntp() {
    for svc in ntp ntpd chrony chronyd systemd-timesyncd; do
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            echo "[*] NTP: stopping system service: $svc"
            systemctl stop "$svc"
            NTP_STOPPED_SERVICE="$svc"
            sleep 0.5
            break
        fi
    done
    if killall ntpd 2>/dev/null; then
        echo "[*] NTP: killall leftover ntpd"
        sleep 1
    fi

    cat > /tmp/ntp_server_caos.py <<'PYEOF'
#!/usr/bin/env python3
import socket, struct, time, sys, os, datetime

NTP_EPOCH = 2208988800
PORT      = 123

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    sock.bind(('', PORT))
except PermissionError:
    print(f"[!] Port {PORT} needs root (sudo)", flush=True)
    sys.exit(1)
except OSError as e:
    print(f"[!] bind UDP {PORT} failed: {e}", flush=True)
    sys.exit(1)

print(f"[*] Python NTP server listening on UDP {PORT}", flush=True)

pid_file = os.environ.get('NTP_PID_FILE', '')
if pid_file:
    with open(pid_file, 'w') as f:
        f.write(str(os.getpid()))

while True:
    try:
        data, addr = sock.recvfrom(1024)
    except OSError:
        break
    if len(data) < 48:
        continue
    if (data[0] & 0x07) != 3:
        continue

    t_ntp = int(time.time()) + NTP_EPOCH
    resp  = bytearray(48)
    resp[0] = 0x24
    resp[1] = 1
    resp[2] = data[2]
    resp[3] = 0xFA
    struct.pack_into('>II', resp, 16, t_ntp, 0)
    struct.pack_into('>II', resp, 24, struct.unpack_from('>I', data, 40)[0], 0)
    struct.pack_into('>II', resp, 32, t_ntp, 0)
    struct.pack_into('>II', resp, 40, t_ntp, 0)
    sock.sendto(bytes(resp), addr)

    unix = t_ntp - NTP_EPOCH
    dt   = datetime.datetime.utcfromtimestamp(unix)
    print(f"  NTP -> {addr[0]}:{addr[1]}  unix={unix}  "
          f"{dt.strftime('%Y-%m-%d %H:%M:%S')} UTC", flush=True)
PYEOF

    rm -f "$NTP_LOG"
    echo "[*] Starting Python NTP server (UDP port 123)"
    NTP_PID_FILE="$NTP_PID" python3 /tmp/ntp_server_caos.py >> "$NTP_LOG" 2>&1 &
    local NTP_BG_PID=$!
    sleep 0.5

    if ! kill -0 "$NTP_BG_PID" 2>/dev/null; then
        echo "[!] NTP server did not start - see $NTP_LOG"
        cat "$NTP_LOG" 2>/dev/null || true
        exit 1
    fi
    [[ -f "$NTP_PID" ]] || echo "$NTP_BG_PID" > "$NTP_PID"
    echo "[*] NTP server running (pid: $NTP_BG_PID)"
}

# --------------------------------------------------------------------------
# Python DNS server
#
# Static resolutions:
#   test.caos.local     -> 192.168.10.1  (the PC itself)
#   gateway.caos.local  -> 192.168.10.1  (the PC itself)
#   pool.ntp.org        -> forwarded to 8.8.4.4 (if internet is available)
#                         -> 192.168.10.1 as fallback
#
# Every other name: forwarded to 8.8.4.4, or NXDOMAIN when offline.
# --------------------------------------------------------------------------
setup_dns() {
    # Free port 53: systemd-resolved and/or other DNS services
    for svc in systemd-resolved named bind9; do
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            echo "[*] DNS: stop $svc (holds port 53)"
            systemctl stop "$svc"
            sleep 0.5
        fi
    done
    # Kill any process left on port 53
    fuser -k 53/udp 2>/dev/null || true
    sleep 0.3

    cat > /tmp/dns_server_caos.py <<PYEOF
#!/usr/bin/env python3
"""
Minimal DNS server for caOS EX37.
Static resolutions for .caos.local names; forwarded to 8.8.4.4 for the others.
"""
import socket, struct, sys, os

DNS_PORT    = 53
UPSTREAM    = ('8.8.8.8', 53)
FORWARD_TO  = 8  # upstream timeout, seconds

# Static hostname -> IPv4 map
STATIC = {
    b'test.caos.local':    b'\\xc0\\xa8\\x0a\\x01',  # 192.168.10.1
    b'gateway.caos.local': b'\\xc0\\xa8\\x0a\\x01',  # 192.168.10.1
}

def parse_qname(data, off):
    """Reads a compressed DNS qname, returns (name_bytes, offset_after)."""
    labels = []
    visited = set()
    while True:
        if off >= len(data):
            break
        ln = data[off]
        if ln == 0:
            off += 1
            break
        if (ln & 0xC0) == 0xC0:          # compressed pointer
            ptr = ((ln & 0x3F) << 8) | data[off + 1]
            off += 2
            if ptr in visited:
                break
            visited.add(ptr)
            lbl, _ = parse_qname(data, ptr)
            labels.append(lbl)
            break
        off += 1
        labels.append(data[off:off + ln].lower())
        off += ln
    return b'.'.join(labels), off

def build_reply_a(request, ip4_bytes):
    """Builds a DNS A record reply from the original query."""
    txid   = request[:2]
    flags  = b'\\x81\\x80'               # QR=1 AA=0 RD=1 RA=1 RCODE=0
    qdcnt  = request[4:6]
    ancnt  = b'\\x00\\x01'
    nscnt  = b'\\x00\\x00'
    arcnt  = b'\\x00\\x00'
    header = txid + flags + qdcnt + ancnt + nscnt + arcnt

    # question section (copied from the request)
    qsec = request[12:]

    # answer: pointer to the question qname (0xC00C), type A, class IN, TTL 30s, rdata
    answer  = b'\\xc0\\x0c'             # pointer to the qname in the question section
    answer += b'\\x00\\x01'             # type A
    answer += b'\\x00\\x01'             # class IN
    answer += b'\\x00\\x00\\x00\\x1e'  # TTL 30
    answer += b'\\x00\\x04'             # rdlength = 4
    answer += ip4_bytes

    return header + qsec + answer

def build_nxdomain(request):
    """NXDOMAIN reply."""
    txid  = request[:2]
    flags = b'\\x81\\x83'               # QR=1 RCODE=3 (NXDOMAIN)
    rest  = request[4:]
    return txid + flags + rest

def forward_upstream(data, timeout=FORWARD_TO):
    """Sends the query to 8.8.4.4 and waits for the reply."""
    try:
        up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        up.settimeout(timeout)
        up.sendto(data, UPSTREAM)
        resp, _ = up.recvfrom(4096)
        up.close()
        return resp
    except OSError:
        return None

def run():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(('', DNS_PORT))
    except PermissionError:
        print(f"[!] Port {DNS_PORT} needs root (sudo)", flush=True)
        sys.exit(1)
    except OSError as e:
        print(f"[!] bind UDP {DNS_PORT} failed: {e}", flush=True)
        sys.exit(1)

    pid_file = os.environ.get('DNS_PID_FILE', '')
    if pid_file:
        with open(pid_file, 'w') as f:
            f.write(str(os.getpid()))

    print(f"[*] Python DNS server listening on UDP {DNS_PORT}", flush=True)
    print(f"    Statici: {list(STATIC.keys())}", flush=True)
    print(f"    Upstream: {UPSTREAM[0]}", flush=True)

    while True:
        try:
            data, addr = sock.recvfrom(512)
        except OSError:
            break

        if len(data) < 12:
            continue

        # Queries only (QR=0) with at least 1 question
        flags_word = (data[2] << 8) | data[3]
        if (flags_word & 0x8000) != 0:
            continue                # a reply, ignore it
        qdcount = (data[4] << 8) | data[5]
        if qdcount == 0:
            continue

        qname, qoff = parse_qname(data, 12)
        if qoff + 4 > len(data):
            continue
        qtype  = (data[qoff] << 8) | data[qoff + 1]
        qclass = (data[qoff + 2] << 8) | data[qoff + 3]

        print(f"  DNS query: {qname.decode(errors='replace')} type={qtype} "
              f"from {addr[0]}:{addr[1]}", flush=True)

        if qtype == 1 and qclass == 1:      # A IN
            ip4 = STATIC.get(qname)
            if ip4:
                reply = build_reply_a(data, ip4)
                ip_str = '.'.join(str(b) for b in ip4)
                print(f"       -> {ip_str} (static)", flush=True)
            else:
                reply = forward_upstream(data)
                if reply:
                    print(f"       -> forwarded to {UPSTREAM[0]}", flush=True)
                else:
                    reply = build_nxdomain(data)
                    print(f"       -> NXDOMAIN (upstream unreachable)", flush=True)
        else:
            # Not A-IN: just forward it
            reply = forward_upstream(data)
            if not reply:
                reply = build_nxdomain(data)

        if reply:
            sock.sendto(reply, addr)

if __name__ == '__main__':
    run()
PYEOF

    rm -f "$DNS_LOG"
    echo "[*] Starting Python DNS server (UDP port 53)"
    DNS_PID_FILE="$DNS_PID" python3 /tmp/dns_server_caos.py >> "$DNS_LOG" 2>&1 &
    local DNS_BG_PID=$!
    sleep 0.5

    if ! kill -0 "$DNS_BG_PID" 2>/dev/null; then
        echo "[!] DNS server did not start - see $DNS_LOG"
        cat "$DNS_LOG" 2>/dev/null || true
        exit 1
    fi
    [[ -f "$DNS_PID" ]] || echo "$DNS_BG_PID" > "$DNS_PID"
    echo "[*] DNS server running (pid: $DNS_BG_PID)"
    echo "[*] DNS log: tail -f $DNS_LOG"
}

setup_ntp
setup_dns

# --------------------------------------------------------------------------
# dnsmasq: DHCP with DNS server = the PC itself (local port 53)
# --------------------------------------------------------------------------
cat > "$DHCP_CONF" <<EOF
# dnsmasq config for caOS EX37 - generated by test_dns_pc.sh
interface=${IF}
bind-interfaces
except-interface=lo
dhcp-range=${DHCP_START},${DHCP_END},${MASK},${LEASE}
dhcp-option=option:router,${PC_IP}
dhcp-option=option:dns-server,${PC_IP}
dhcp-option=option:netmask,${MASK}
dhcp-option=option:ntp-server,${PC_IP}
log-dhcp
no-resolv
no-hosts
port=0
EOF

rm -f /tmp/dnsmasq-caos.log
echo "[*] Starting dnsmasq  (pool $DHCP_START - $DHCP_END)"
/usr/sbin/dnsmasq -C "$DHCP_CONF" --pid-file="$DHCP_PID" \
    --log-facility=/tmp/dnsmasq-caos.log
sleep 0.5

echo "[*] dnsmasq running (pid: $(cat $DHCP_PID))"
echo "[*] DHCP log: tail -f /tmp/dnsmasq-caos.log"
echo ""
echo "------------------------------------------------------------"
echo " Connect the board and start the ex37 firmware."
echo " Expected sequence on the USB console:"
echo "   DHCP BOUND  IP=192.168.10.x ..."
echo "   NTP SYNC    unix=..."
echo "   DNS: starting demo lookups..."
echo "   DNS  test.caos.local       ->  192.168.10.1"
echo "   DNS  gateway.caos.local    ->  192.168.10.1"
echo "   DNS  pool.ntp.org          ->  <real IP or FAILED>"
echo "------------------------------------------------------------"

# --------------------------------------------------------------------------
# Wait for the DHCP ACK
# --------------------------------------------------------------------------
echo "[*] Waiting for DHCP ACK..."
BOARD_IP=""
for i in $(seq 1 60); do
    sleep 1
    BOARD_IP=$(grep -oP "DHCPACK\([^)]+\) \K[0-9.]+" /tmp/dnsmasq-caos.log 2>/dev/null \
               | tail -1 || true)
    if [[ -n "$BOARD_IP" ]]; then
        echo ""
        echo "[+] DHCP ACK -> $BOARD_IP"
        break
    fi
    printf "\r[*] Waiting for DHCP... %ds" "$i"
done
echo ""

if [[ -z "$BOARD_IP" ]]; then
    echo "[!] No ACK in 60s. Enter the IP by hand:"
    read -rp "    Board IP: " BOARD_IP
fi

# --------------------------------------------------------------------------
# Wait for DNS queries from the board in the log
# --------------------------------------------------------------------------
echo "[*] Waiting for DNS queries from the board ($BOARD_IP)..."
DNS_SEEN=false
for i in $(seq 1 30); do
    sleep 1
    if grep -q "$BOARD_IP" "$DNS_LOG" 2>/dev/null; then
        echo "[+] DNS query received from $BOARD_IP"
        grep "$BOARD_IP" "$DNS_LOG" | head -5
        DNS_SEEN=true
        break
    fi
    printf "\r[*] Waiting for DNS... %ds" "$i"
done
echo ""

if [[ "$DNS_SEEN" != "true" ]]; then
    echo "[!] No DNS query in 30s (the board may still be in DORA)"
fi

# --------------------------------------------------------------------------
# Ping
# --------------------------------------------------------------------------
echo ""
echo "============================================================"
echo " Test ping"
echo "============================================================"
ping -c 4 -W 2 "$BOARD_IP" && echo "[OK] ping" || echo "[!!] ping failed"

# --------------------------------------------------------------------------
# DNS test from the PC to the board (checks the board answers on port 53)
# Note: the board is NOT a DNS server - this is only an extra test
# of ARP/reachability on the UDP port.
# --------------------------------------------------------------------------
echo ""
echo "============================================================"
echo " DNS query test to the board (expected NXDOMAIN/no reply)"
echo "============================================================"
echo "[*] The board is not a DNS server - the test checks reachability only."
if command -v dig &>/dev/null; then
    dig @"$BOARD_IP" test.caos.local +time=2 +tries=1 || true
else
    echo "[*] dig not available - skip"
fi

# --------------------------------------------------------------------------
# UDP echo test (port 7961)
# --------------------------------------------------------------------------
echo ""
echo "============================================================"
echo " UDP echo test (port 7961)"
echo "============================================================"
echo "[*] Sending 'hello caOS ex37' to $BOARD_IP:7961 ..."
REPLY=$(udp_echo "$BOARD_IP" 7961 "hello caOS ex37")
if [[ "$REPLY" == *"ECHO:"* ]]; then
    echo "[OK] Echo: $REPLY"
else
    echo "[!!] No UDP reply"
fi

# --------------------------------------------------------------------------
echo ""
echo "============================================================"
echo " Log DHCP:"
echo "============================================================"
cat /tmp/dnsmasq-caos.log

echo ""
echo "============================================================"
echo " Log NTP:"
echo "============================================================"
cat "$NTP_LOG"

echo ""
echo "============================================================"
echo " Log DNS:"
echo "============================================================"
cat "$DNS_LOG"

echo ""
echo "[*] Press Ctrl+C to quit (automatic cleanup)."
while true; do sleep 5; done
