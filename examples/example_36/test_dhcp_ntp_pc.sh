#!/usr/bin/env bash
# test_dhcp_ntp_pc.sh <interface>
# Sets the PC up as gateway + DHCP server + NTP server to test caOS ex36.
# Needs: dnsmasq, ip, ping, nc (netcat-openbsd), python3
# Usage: sudo ./test_dhcp_ntp_pc.sh enp6s0

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
NTP_CONF="/tmp/ntpd-caos.conf"
NTP_DRIFT="/tmp/ntpd-caos.drift"

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
        echo "[*] Stop ntpd caos (pid $(cat $NTP_PID))"
        kill "$(cat $NTP_PID)" 2>/dev/null || true
        rm -f "$NTP_PID"
    fi
    rm -f "$NTP_CONF" "$NTP_DRIFT"
}

cleanup() {
    trap - EXIT INT TERM          # once: Ctrl-C used to run it twice
    echo ""
    echo "[*] Cleanup..."
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
echo " caOS EX36 - DHCP + NTP test  (interface: $IF)"
echo "============================================================"

# Check the interface
if ! ip link show "$IF" &>/dev/null; then
    echo "[!] Interface $IF not found."
    exit 1
fi

# Stop anything left running
stop_ntp
stop_dnsmasq

# Set the PC IP
echo "[*] Configure $IF  ->  $PC_IP/$MASK"
ip addr flush dev "$IF" 2>/dev/null || true
ip addr add "${PC_IP}/24" dev "$IF"
ip link set "$IF" up
echo "[*] Link UP"

# --------------------------------------------------------------------------
# NTP server: always stop the system service, start a minimal server that
# uses the local clock as a stratum 1 reference and opens full access to
# the test subnet.
# --------------------------------------------------------------------------
NTP_STOPPED_SERVICE=""

setup_ntp() {
    # Stop any system NTP service and any ntpd left running
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

    # Minimal NTP server in Python - answers at once, no sync
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
    if (data[0] & 0x07) != 3:   # mode must be client (3)
        continue

    t_ntp  = int(time.time()) + NTP_EPOCH
    resp   = bytearray(48)
    resp[0] = 0x24              # LI=0, VN=4, Mode=4 (server)
    resp[1] = 1                 # stratum 1
    resp[2] = data[2]           # poll
    resp[3] = 0xFA              # precision -6
    struct.pack_into('>II', resp, 16, t_ntp, 0)                          # ref ts
    struct.pack_into('>II', resp, 24, struct.unpack_from('>I', data, 40)[0], 0)  # orig ts
    struct.pack_into('>II', resp, 32, t_ntp, 0)                          # rx ts
    struct.pack_into('>II', resp, 40, t_ntp, 0)                          # tx ts
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
    echo "[*] Python NTP server running (pid: $NTP_BG_PID)"
    echo "[*] NTP log: tail -f $NTP_LOG"
}

setup_ntp

# --------------------------------------------------------------------------
# Write the dnsmasq conf
# --------------------------------------------------------------------------
cat > "$DHCP_CONF" <<EOF
# dnsmasq config for caOS EX36 - generated by test_dhcp_ntp_pc.sh
interface=${IF}
bind-interfaces
except-interface=lo
dhcp-range=${DHCP_START},${DHCP_END},${MASK},${LEASE}
dhcp-option=option:router,${PC_IP}
dhcp-option=option:dns-server,8.8.8.8
dhcp-option=option:netmask,${MASK}
dhcp-option=option:ntp-server,${PC_IP}
log-dhcp
no-resolv
no-hosts
EOF

# Start dnsmasq
rm -f /tmp/dnsmasq-caos.log
echo "[*] Starting dnsmasq  (pool $DHCP_START - $DHCP_END)"
/usr/sbin/dnsmasq -C "$DHCP_CONF" --pid-file="$DHCP_PID" \
    --log-facility=/tmp/dnsmasq-caos.log
sleep 0.5

echo "[*] dnsmasq running (pid: $(cat $DHCP_PID))"
echo "[*] DHCP log: tail -f /tmp/dnsmasq-caos.log"
echo "[*] NTP  log: tail -f $NTP_LOG"
echo ""
echo "------------------------------------------------------------"
echo " Connect the board and start the ex36 firmware."
echo " Wait for DHCP BOUND + NTP SYNC on the USB console."
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
        echo "[+] DHCP ACK sent to: $BOARD_IP"
        break
    fi
    printf "\r[*] Waiting for DHCP... %ds" "$i"
done
echo ""

if [[ -z "$BOARD_IP" ]]; then
    echo "[!] No ACK seen in 60s. Enter the IP by hand:"
    read -rp "    Board IP: " BOARD_IP
fi

# --------------------------------------------------------------------------
# Wait for the NTP sync (watch the NTP log)
# --------------------------------------------------------------------------
echo "[*] Waiting for NTP sync from the board..."
NTP_SYNCED=false
for i in $(seq 1 30); do
    sleep 1
    if grep -q "$BOARD_IP" "$NTP_LOG" 2>/dev/null; then
        echo "[+] NTP sync received from $BOARD_IP"
        grep "$BOARD_IP" "$NTP_LOG" | tail -1
        NTP_SYNCED=true
        break
    fi
    printf "\r[*] Waiting for NTP... %ds" "$i"
done
echo ""

if [[ "$NTP_SYNCED" != "true" ]]; then
    echo "[!] No NTP sync in 30s (it may come later - check the USB console)"
fi

echo ""
echo "============================================================"
echo " Test ping"
echo "============================================================"
ping -c 4 -W 2 "$BOARD_IP" && echo "[OK] ping" || echo "[!!] ping failed"

echo ""
echo "============================================================"
echo " UDP echo test (port 7961)"
echo "============================================================"
echo "[*] Sending 'hello caOS' to $BOARD_IP:7961 ..."
REPLY=$(udp_echo "$BOARD_IP" 7961 "hello caOS")
if [[ "$REPLY" == *"ECHO:"* ]]; then
    echo "[OK] Echo received: $REPLY"
else
    echo "[!!] No UDP reply"
    echo "     Try: echo 'hello' | nc -u $BOARD_IP 7961"
fi

echo ""
echo "============================================================"
echo " Test 2 simultaneous UDP clients"
echo "============================================================"
udp_echo "$BOARD_IP" 7961 "client_one" &
PID1=$!
udp_echo "$BOARD_IP" 7961 "client_two" &
PID2=$!
wait $PID1 $PID2 2>/dev/null || true
echo "[*] Both clients done (check the USB console for ECHO)"

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
echo "[*] Press Ctrl+C to quit (automatic cleanup)."
while true; do sleep 5; done
