#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Angelo Coppi
#
# HW test of the 3-class USB composite (example_56: CDC + MSC + NCM).
#
# One script for all three functions, because the point of the example is
# not that each class works alone -- ex52, ex53 and ex55 prove that already --
# but that the three COEXIST on the same device, using up the 5 data EPs of
# the OTG_FS. So they are exercised in the same session, without restarting
# the target in between.
#
# Usage:
#   ./test_composite.sh                      # autodetect everything
#   ./test_composite.sh -i enx02ca050000a6   # force the NCM interface
#   ./test_composite.sh -t /dev/ttyACM1      # force the CDC port
#
# The CDC port is found by VID through udevadm, not by index: on a Nucleo
# /dev/ttyACM0 is the ST-Link VCP, not the device under test.
#   ./test_composite.sh -s                   # skip the MSC test (writes to disk)
#
# The MSC test formats the device ramdisk: 128 KB of RAM on the target,
# no PC data is touched. It still needs sudo.
#
# Needs: sudo, iproute2, ping, lsusb; for the MSC test also fdisk and
# mkfs.fat (util-linux + dosfstools).

set -uo pipefail

IFACE=""; TTY=""; SKIP_MSC=0
IP_LOCAL="192.168.10.1/24"
IP_DEV="192.168.10.56"          # from ca_os_ex56.c
MAC_DEV="02:ca:05:00:00:56"     # from ca_os_ex56.c
VID="ca01"
N_PING=5

while getopts "i:t:sh" o; do
    case "$o" in
        i) IFACE="$OPTARG" ;;
        t) TTY="$OPTARG" ;;
        s) SKIP_MSC=1 ;;
        h) sed -n '4,25p' "$0"; exit 0 ;;
        *) exit 1 ;;
    esac
done

RED=$'\e[31m'; GRN=$'\e[32m'; YEL=$'\e[33m'; BLD=$'\e[1m'; RST=$'\e[0m'
ok()   { echo "${GRN}[OK]${RST}  $*"; }
info() { echo "${YEL}[..]${RST}  $*"; }
bad()  { echo "${RED}[NO]${RST}  $*"; fail=1; }
die()  { echo "${RED}[ERR]${RST} $*" >&2; exit 1; }
head_() { echo; echo "${BLD}--- $* ---${RST}"; }

fail=0
cleanup() { [[ -n "$IFACE" ]] && sudo ip addr del "$IP_LOCAL" dev "$IFACE" 2>/dev/null; true; }
trap cleanup EXIT

for t in sudo ip ping lsusb; do
    command -v "$t" >/dev/null 2>&1 || die "missing command: $t"
done

# =============================================================================
head_ "0) Enumeration: the 3 classes on a single device"
# =============================================================================
lsusb -d "$VID:" >/dev/null 2>&1 || die "no device VID=$VID (target on and connected?)"
lsusb -d "$VID:"

# The three classes must show up in the SAME device: this is what ex52/53/55
# taken one by one do not prove.
desc=$(lsusb -v -d "$VID:" 2>/dev/null)
for pair in "CDC:Communications|CDC Data" "MSC:Mass Storage" "NCM:Ethernet Networking|NCM"; do
    name="${pair%%:*}"; pat="${pair#*:}"
    if grep -Eqi "$pat" <<<"$desc"; then ok "$name present in the descriptors"
    else bad "$name NOT found in the descriptors"; fi
done
grep -qi "Interface Association" <<<"$desc" && ok "IAD present (needed by the CDC composite)" \
                                            || info "IAD not seen (lsusb -v needs permissions)"

# =============================================================================
head_ "1) CDC: serial echo"
# =============================================================================
# NOT starting from ttyACM0: on a Nucleo that is the ST-Link VCP, and testing
# it would give the echo of the debug console instead of the device CDC.
# The right port is told by the VID, not by the index: sysfs walks up from each
# ttyACM to the USB device that exposes it.
if [[ -z "$TTY" ]]; then
    for d in /dev/ttyACM*; do
        [[ -e "$d" ]] || continue
        v=$(udevadm info -q property -n "$d" 2>/dev/null | sed -n 's/^ID_VENDOR_ID=//p')
        if [[ "$v" == "$VID" ]]; then TTY="$d"; break; fi
    done
fi
# Fallback when udevadm is missing: explicitly skip ttyACM0 (ST-Link).
if [[ -z "$TTY" ]]; then
    for d in /dev/ttyACM*; do
        [[ -e "$d" && "$d" != "/dev/ttyACM0" ]] && TTY="$d" && \
            info "port chosen without udevadm: check that it is not the ST-Link" && break
    done
fi
if [[ -z "$TTY" || ! -e "$TTY" ]]; then
    bad "no /dev/ttyACM* port found"
else
    info "CDC port: $TTY"
    sudo stty -F "$TTY" 115200 raw -echo min 0 time 20 2>/dev/null \
        || bad "stty failed on $TTY"
    # Opening the port raises DTR: the device sends the banner and then echoes.
    # exec keeps ONE open for the whole test, otherwise every redirection would
    # raise DTR again and mix banner and echo.
    if exec 3<>"$TTY" 2>/dev/null; then
        sleep 0.5
        timeout 2 cat <&3 >/dev/null 2>&1     # drop the DTR banner
        MSG="caOS-ex56-echo"
        printf '%s\n' "$MSG" >&3
        got=$(timeout 3 head -c ${#MSG} <&3 2>/dev/null | tr -d '\r\n')
        exec 3<&-; exec 3>&-
        if [[ "$got" == "$MSG" ]]; then ok "echo correct: '$got'"
        elif [[ -n "$got" ]];   then bad "echo differs: expected '$MSG', read '$got'"
        else                         bad "no reply from the CDC"; fi
    else
        bad "cannot open $TTY (permissions? dialout group)"
    fi
fi

# =============================================================================
head_ "2) NCM: ARP + ICMP with the caOS stack"
# =============================================================================
if [[ -z "$IFACE" ]]; then
    # The composite interface has the host-side MAC 02:ca:05:00:00:a6.
    for n in /sys/class/net/*; do
        [[ -e "$n/address" ]] || continue
        a=$(cat "$n/address")
        [[ "$a" == 02:ca:05:* ]] && IFACE=$(basename "$n") && break
    done
fi
if [[ -z "$IFACE" || ! -e "/sys/class/net/$IFACE" ]]; then
    bad "NCM interface not found (give it with -i, see 'ip link')"
    IFACE=""
else
    info "NCM interface: $IFACE"
    sudo ip addr flush dev "$IFACE" 2>/dev/null
    sudo ip addr add "$IP_LOCAL" dev "$IFACE" && sudo ip link set "$IFACE" up
    # The ARP cache has to be flushed: an entry from an earlier run would let
    # the ping pass without the device answering a single ARP.
    sudo ip neigh flush dev "$IFACE" 2>/dev/null
    sleep 1

    info "ping $IP_DEV x$N_PING..."
    if ping -c "$N_PING" -W 2 "$IP_DEV"; then ok "ICMP: the device answers"
    else bad "ICMP: no reply from $IP_DEV"; fi

    # Check that the learned MAC is the example's, not another device
    # holding the same IP.
    neigh=$(ip neigh show dev "$IFACE" | grep -iw "$IP_DEV")
    if [[ -z "$neigh" ]]; then bad "no ARP entry for $IP_DEV"
    else
        echo "  $neigh"
        grep -qi "$MAC_DEV" <<<"$neigh" && ok "MAC matches ($MAC_DEV)" \
                                        || bad "MAC differs from the expected one ($MAC_DEV)"
    fi
fi

# =============================================================================
head_ "3) MSC: FAT on the ramdisk"
# =============================================================================
if (( SKIP_MSC )); then
    info "skipped (-s)"
elif ! command -v fdisk >/dev/null 2>&1 || ! command -v mkfs.fat >/dev/null 2>&1; then
    bad "fdisk or mkfs.fat missing (apt install util-linux dosfstools), or use -s"
else
    DEV=""
    for d in /sys/block/sd*; do
        [[ -e "$d" ]] || continue
        n=$(basename "$d")
        v=$(cat "$d/device/vendor" 2>/dev/null | tr -d ' ')
        [[ "$v" == "caOS" ]] && DEV="/dev/$n" && break
    done
    if [[ -z "$DEV" ]]; then
        bad "no block device with vendor 'caOS' (lsblk -o NAME,VENDOR)"
    else
        lsblk -o NAME,SIZE,VENDOR,MODEL,TRAN "$DEV"
        info "formatting $DEV (128 KB ramdisk on the target, no PC data)"

        # Partitioned instead of formatting the whole disk, as ex53 does:
        # on 128 KB mkfs.fat on the raw device does not hold, and a partition left
        # by an earlier run would keep the device busy anyway.
        for pp in $(lsblk -ln -o PATH "$DEV" 2>/dev/null); do
            [[ "$pp" == "$DEV" ]] && continue
            sudo umount "$pp" 2>/dev/null
        done

        sudo fdisk "$DEV" >/dev/null 2>&1 <<'FDISK'
o
n
p
1


t
b
w
FDISK
        sudo partprobe "$DEV" 2>/dev/null || sudo blockdev --rereadpt "$DEV" 2>/dev/null
        udevadm settle 2>/dev/null
        PART=""
        for try in 1 2 3; do
            if   [[ -b "${DEV}1"  ]]; then PART="${DEV}1";  break
            elif [[ -b "${DEV}p1" ]]; then PART="${DEV}p1"; break
            fi
            sleep 1
        done

        if [[ -z "$PART" ]]; then
            bad "partition not created on $DEV after fdisk"
        else
            info "partition: $PART"
            MNT=$(mktemp -d /tmp/caos_ex56.XXXXXX)
            if sudo mkfs.fat -F 12 "$PART" >/dev/null 2>&1 && sudo mount "$PART" "$MNT"; then
                STAMP="ex56 composite $(date '+%F %T')"
                if echo "$STAMP" | sudo tee "$MNT/test.txt" >/dev/null && sudo sync; then
                    sudo umount "$MNT"
                    # The remount is the point of the test: the umount forces the flush of
                    # the kernel cache, so reading back afterwards proves that the data
                    # landed on the device, not in a buffer.
                    if sudo mount "$PART" "$MNT"; then
                        got=$(sudo cat "$MNT/test.txt" 2>/dev/null)
                        sudo umount "$MNT"
                        [[ "$got" == "$STAMP" ]] && ok "persistence checked: \"$got\"" \
                                                 || bad "read back differs: \"$got\""
                    else bad "remount failed"; fi
                else bad "write failed"; sudo umount "$MNT" 2>/dev/null; fi
            else
                bad "mkfs/mount failed on $PART"
            fi
            rmdir "$MNT" 2>/dev/null
        fi
    fi
fi

# =============================================================================
echo
if (( fail )); then
    echo "${RED}=== TEST EX56 FAILED ===${RST}"; exit 1
fi
echo "${GRN}=== TEST EX56 OK (CDC + MSC + NCM together) ===${RST}"
