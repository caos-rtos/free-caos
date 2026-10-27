#!/bin/bash
# SSH client for example_83 (SSH server on USB-NCM).
#
# Differences from ex67's run_ssh_client.sh (same service on ETH):
#   - default IP 192.168.10.83 (static, as ex67 uses .67)
#   - configures the NCM interface, which does not exist until the device has
#     enumerated: it finds it by the host-side MAC of the NCM descriptors (02:ca:05:*)
#
# POWER-ON ORDER: the BOARD first, then this script.
#
# Usage:
#   ./run_ssh_client_ncm.sh                 # auto-detect iface + default IP
#   ./run_ssh_client_ncm.sh 192.168.10.83   # explicit IP
#   IFACE=usb0 ./run_ssh_client_ncm.sh      # explicit interface

# Key resolved from the script location (not the CWD).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEY="$SCRIPT_DIR/../caos_ed25519/id_ed25519"

# git keeps only the execute bit, not the full permissions: after a clone
# the private key is 0664 and ssh REFUSES it ("UNPROTECTED PRIVATE KEY
# FILE", key ignored -> key authentication does not even start).
# Fixed here once and for all, instead of letting anyone who tries the
# example on a new machine run into it.
if [ -f "$KEY" ] && [ "$(stat -c %a "$KEY" 2>/dev/null)" != "600" ]; then
    chmod 600 "$KEY" && echo "[i] permissions of $KEY set to 600 (git does not track them)"
fi

IP="${1:-192.168.10.83}"
PC_IP="${PC_IP:-192.168.10.1}"

# --- NCM interface: explicit via IFACE, otherwise found by the MAC ----------
find_ncm_iface() {
    for _ in $(seq 30); do
        for d in /sys/class/net/*; do
            n="$(basename "$d")"
            [ "$n" = "lo" ] && continue
            mac="$(cat "$d/address" 2>/dev/null)"
            case "$mac" in
                02:ca:05:*) echo "$n"; return 0 ;;
            esac
        done
        sleep 1
    done
    return 1
}

if [ -z "$IFACE" ]; then
    echo "[*] Looking for the USB-NCM interface (MAC 02:ca:05:*)..."
    IFACE="$(find_ncm_iface)" || {
        echo "[!] No NCM interface found in 30s."
        echo "    Is the board on and enumerated? (dmesg | tail)"
        exit 1
    }
    echo "[*] NCM interface: $IFACE"
fi

# --- PC IP on the same subnet as the board ---------------------------------
if ! ip -4 addr show dev "$IFACE" | grep -q "$PC_IP"; then
    echo "[*] Configuring $IFACE -> $PC_IP/24 (sudo needed)"
    sudo ip addr add "$PC_IP/24" dev "$IFACE" 2>/dev/null
    sudo ip link set "$IFACE" up
fi

echo "[*] ssh -> $IP:22"
ssh -o IdentitiesOnly=yes -v -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no \
    -i "$KEY" "$IP" -p 22
