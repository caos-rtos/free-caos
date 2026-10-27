#!/usr/bin/env bash
# test_dhcp_ntp_ncm_pc.sh <interface>
# Sets the PC up as gateway + DHCP server + NTP server to test
# caOS example 71 (USB-NCM).
# Needs: dnsmasq, ip, ping, nc (netcat-openbsd), python3
# Usage: sudo ./test_dhcp_ntp_ncm_pc.sh          (auto-detects the interface)
#        sudo ./test_dhcp_ntp_ncm_pc.sh usb0     (explicit interface)
#
# NB compared with ex36 (same test on ETH): there the NTP server came from the
# LAN, or anyway from a host other than the gateway. On a point-to-point USB
# link the PC is EVERYTHING -- gateway, DHCP server and NTP server -- so the
# firmware fallback (gateway when opt42 is missing) points to the opt42 address.

set -euo pipefail

# POWER-ON ORDER: the BOARD first, then this script.
# The USB network interface does not exist until the device has enumerated:
# it is not like eth0, which is always there. The firmware knows it and waits
# for the link indefinitely before starting the DORA, so there is no hurry on
# this side -- but the script needs the interface to exist to configure it.
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

IF="${1:-}"
if [[ -z "$IF" ]]; then
    echo "[*] Looking for the USB-NCM interface (MAC ${NCM_MAC_PREFIX}:*)..."
    for _ in $(seq 1 30); do
        IF="$(find_ncm_iface || true)"
        [[ -n "$IF" ]] && break
        sleep 1
    done
    if [[ -z "$IF" ]]; then
        echo "[!] No NCM interface found in 30s."
        echo "    Connect the USB cable and start the ex71 firmware, or"
        echo "    give the interface by hand:  sudo $0 <iface>"
        echo "    (list: ip -br link)"
        exit 1
    fi
    echo "[+] NCM interface: $IF"
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

NTP_STOPPED_SERVICE=""

# --------------------------------------------------------------------------
stop_dnsmasq() {
    if [[ -f "$DHCP_PID" ]]; then
        echo "[*] Stop dnsmasq (pid $(cat $DHCP_PID))"
        kill "$(cat $DHCP_PID)" 2>/dev/null || true
        rm -f "$DHCP_PID"
    else
        # Try anyway to kill any instance left with this conf
        pkill -f "dnsmasq-caos" 2>/dev/null || true
    fi
}

stop_ntp() {
    if [[ -f "$NTP_PID" ]]; then
        echo "[*] Stop NTP server caos (pid $(cat $NTP_PID))"
        kill "$(cat $NTP_PID)" 2>/dev/null || true
        rm -f "$NTP_PID"
    fi
    rm -f /tmp/ntp_server_caos.py
}

cleanup() {
    trap - EXIT INT TERM          # once: Ctrl-C used to run it twice
    echo ""
    echo "[*] Cleanup..."
    stop_dnsmasq
    stop_ntp
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
echo " caOS EX71 - DHCP + NTP test on USB-NCM  (interface: $IF)"
echo "============================================================"

# Check the interface
if ! ip link show "$IF" &>/dev/null; then
    echo "[!] Interface $IF not found."
    exit 1
fi

# Stop anything left running
stop_dnsmasq
stop_ntp

# Set the PC IP
echo "[*] Configure $IF  ->  $PC_IP/$MASK"
ip addr flush dev "$IF" 2>/dev/null || true
ip addr add "${PC_IP}/24" dev "$IF"
ip link set "$IF" up
echo "[*] Link UP"

# --------------------------------------------------------------------------
# NTP server: stop the system service (it would hold port 123) and start a
# minimal responder in Python, which answers at once without being synced
# itself - what matters here is that the board gets a timestamp, not its
# absolute accuracy.
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
}

setup_ntp

# Write the dnsmasq conf
cat > "$DHCP_CONF" <<EOF
# dnsmasq config for caOS EX71 - generated by test_dhcp_ntp_ncm_pc.sh
interface=${IF}
bind-interfaces
except-interface=lo
dhcp-range=${DHCP_START},${DHCP_END},${MASK},${LEASE}
dhcp-option=option:router,${PC_IP}
dhcp-option=option:dns-server,8.8.8.8
dhcp-option=option:ntp-server,${PC_IP}
dhcp-option=option:netmask,${MASK}
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
echo " The board is already connected (interface $IF exists)."
echo " The firmware waits for the USB link, then starts the DORA:"
echo " the serial console shows 'USB NCM UP', 'DHCP BOUND'"
echo " and right after 'NTP SYNC' (the bound releases the NTP client)."
echo ""
echo " Then press ENTER once the IP is assigned, or"
echo " read the lease obtained below:"
echo "------------------------------------------------------------"

# Wait for a lease to show up in the log
echo "[*] Waiting for DHCP ACK..."
BOARD_IP=""
for i in $(seq 1 60); do
    sleep 1
    # dnsmasq log: "DHCPACK(enp6s0) 192.168.10.135 02:00:..."
    BOARD_IP=$(grep -oP "DHCPACK\([^)]+\) \K[0-9.]+" /tmp/dnsmasq-caos.log 2>/dev/null | tail -1 || true)
    if [[ -n "$BOARD_IP" ]]; then
        echo ""
        echo "[+] DHCP ACK sent to: $BOARD_IP"
        break
    fi
    printf "\r[*] Waiting for DHCP... %ds" "$i"
done
echo ""

if [[ -z "$BOARD_IP" ]]; then
    echo "[!] No ACK seen in 60s."
    echo ""
    echo "    Things to check, in order:"
    echo "      1. does the board serial console show 'USB NCM UP'?"
    echo "         If it stays 'DOWN', the host has not selected alt setting 1:"
    echo "         check 'ip link set $IF up' and that the cable carries data, not only power."
    echo "      2. after 'USB NCM UP' it must print 'DHCP: starting DORA...'"
    echo "         (each DISCOVER is not printed: step 3 shows them)."
    echo "      3. does the DISCOVER reach this PC?  tail -f /tmp/dnsmasq-caos.log"
    echo ""
    echo "    The firmware retries forever (3 DISCOVER x 4s, backoff 5s),"
    echo "    so you can leave everything on and run this script again."
    echo ""
    read -rp "    Board IP (ENTER to quit): " BOARD_IP
    [[ -z "$BOARD_IP" ]] && exit 1
fi

echo ""
echo "============================================================"
echo " Waiting for NTP sync"
echo "============================================================"
# The NTP client is downstream of DHCP: on_dhcp_bound calls net_ready, so the
# first request leaves only after the lease. If DHCP passed but nothing gets
# here, the problem is NTP (opt42 / port 123), not the network.
echo "[*] Waiting for the NTP request from the board..."
NTP_SYNCED=false
for i in $(seq 1 30); do
    sleep 1
    if grep -q "$BOARD_IP" "$NTP_LOG" 2>/dev/null; then
        echo ""
        echo "[+] NTP request served to $BOARD_IP"
        grep "$BOARD_IP" "$NTP_LOG" | tail -1
        NTP_SYNCED=true
        break
    fi
    printf "\r[*] Waiting for NTP... %ds" "$i"
done
echo ""

if [[ "$NTP_SYNCED" != "true" ]]; then
    echo "[!] No NTP request in 30s."
    echo "    Check, in order:"
    echo "      1. does the console show 'DHCP BOUND'? Without a lease the NTP client stays idle."
    echo "      2. does the lease carry option 42? (grep ntp /tmp/dnsmasq-caos.log)"
    echo "         Without it the firmware falls back to the gateway, here $PC_IP:"
    echo "         both ways lead to this PC, so it must work."
    echo "      3. is port 123 free? (ss -ulnp | grep :123)"
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
    echo "[!!] No UDP reply (or nc not available)"
    echo "     Try by hand: echo 'hello' | nc -u $BOARD_IP 7961"
fi

echo ""
echo "============================================================"
echo " Test 2 simultaneous UDP clients"
echo "============================================================"
echo "[*] Client 1..."
udp_echo "$BOARD_IP" 7961 "client_one" &
PID1=$!
echo "[*] Client 2..."
udp_echo "$BOARD_IP" 7961 "client_two" &
PID2=$!
wait $PID1 $PID2 2>/dev/null || true
echo "[*] Both clients done (check the USB console for ECHO)"

echo ""
echo "============================================================"
echo " Full DHCP log:"
echo "============================================================"
cat /tmp/dnsmasq-caos.log

echo ""
echo "============================================================"
echo " Log NTP:"
echo "============================================================"
cat "$NTP_LOG"

echo ""
echo "[*] Press Ctrl+C to quit (automatic cleanup)."
# Wait for a signal
while true; do sleep 5; done
