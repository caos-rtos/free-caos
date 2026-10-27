#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Angelo Coppi
#
# HW test of the USB MSC service (example_53, 128KB ramdisk).
#
# Sequence:
#   1) waits for and finds the caOS USB device (auto-detect by VID/size)
#   2) creates a primary FAT partition with fdisk  -> /dev/sdX1
#   3) mkfs.fat -> mount -> cp -> umount -> remount + cat (integrity check)
#
# Usage:
#   ./test_fat.sh                 # auto-detect the device
#   ./test_fat.sh /dev/sdb        # forced device (skips auto-detect)
#
# Needs: sudo (fdisk, mkfs.fat, mount), dosfstools, util-linux.

set -euo pipefail

# --- Parameters ---------------------------------------------------------------
VID_CAOS="ca01"          # idVendor of the caOS stack (see usb_mgmt)
EXPECT_KB=128            # expected ramdisk capacity (128KB U5/H7, 32KB F407)
SIZE_MIN_KB=16          # tolerance window for the match by size (F407=32KB)
SIZE_MAX_KB=512
WAIT_SECS=20            # timeout waiting for the device to enumerate
TEST_FILE="test.txt"
TEST_DATA="hello caOS MSC $(date '+%Y-%m-%d %H:%M:%S')"

DEV="${1:-}"            # optional device as first argument
MNT=""                 # temporary mountpoint (created later)

# --- Colors -------------------------------------------------------------------
RED=$'\e[31m'; GRN=$'\e[32m'; YEL=$'\e[33m'; RST=$'\e[0m'
ok()   { echo "${GRN}[OK]${RST}  $*"; }
info() { echo "${YEL}[..]${RST}  $*"; }
die()  { echo "${RED}[ERR]${RST} $*" >&2; exit 1; }

# --- Automatic cleanup --------------------------------------------------------
cleanup() {
    if [[ -n "$MNT" && -d "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# --- Prerequisites ------------------------------------------------------------
for t in fdisk mkfs.fat lsblk sudo udevadm; do
    command -v "$t" >/dev/null 2>&1 || die "missing command: $t (install dosfstools/util-linux)"
done

# --- 1) Find the caOS device --------------------------------------------------
# Returns the /dev/sdX path (whole disk, no partition number).
find_caos_disk() {
    local d name path vid sz_kb
    for d in /sys/block/sd*; do
        [[ -e "$d" ]] || continue
        name=$(basename "$d")
        path="/dev/$name"
        # capacity in KB (512B sectors)
        sz_kb=$(( $(cat "$d/size") / 2 ))
        # idVendor, walking up the USB tree with udevadm
        vid=$(udevadm info --query=property --name="$path" 2>/dev/null \
              | sed -n 's/^ID_VENDOR_ID=//p' | head -n1)
        if [[ "$vid" == "$VID_CAOS" ]]; then
            echo "$path"; return 0
        fi
        # fallback: match by size when the VID cannot be read
        if (( sz_kb >= SIZE_MIN_KB && sz_kb <= SIZE_MAX_KB )); then
            echo "$path"; return 0
        fi
    done
    return 1
}

if [[ -n "$DEV" ]]; then
    info "device forced by argument: $DEV"
    [[ -b "$DEV" ]] || die "$DEV is not a block device"
else
    info "waiting for the caOS device (VID=$VID_CAOS, ~${EXPECT_KB}KB) [max ${WAIT_SECS}s]..."
    for ((i = 0; i < WAIT_SECS; i++)); do
        if DEV=$(find_caos_disk); then break; fi
        DEV=""
        sleep 1
    done
    [[ -n "$DEV" ]] || die "caOS device not found. Connect the cable and check with 'lsblk'."
fi

# Summary + confirmation (destructive format)
SZ_KB=$(( $(cat "/sys/block/$(basename "$DEV")/size") / 2 ))
echo
lsblk -o NAME,SIZE,VENDOR,MODEL,TRAN "$DEV" || true
echo
read -r -p "${YEL}Format ${DEV} (${SZ_KB} KB) - ALL data will be lost. Continue? [y/N] ${RST}" ans
[[ "$ans" == "y" || "$ans" == "Y" ]] || die "cancelled by the user"
ok "device selected: $DEV (${SZ_KB} KB)"

# Unmount any partition already mounted
for p in "${DEV}"*; do
    [[ "$p" == "$DEV" ]] && continue
    sudo umount "$p" 2>/dev/null || true
done

# --- 2) FAT partition with fdisk ----------------------------------------------
info "creating DOS partition table + 1 FAT partition on $DEV..."
# n=new, p=primary, 1=part#1, <enter>=default first sector,
# <enter>=default last sector (whole disk), t=type, b=W95 FAT32 (LBA), w=write
sudo fdisk "$DEV" >/dev/null <<'EOF'
o
n
p
1


t
b
w
EOF

# Re-read the partition table and wait for udev to create /dev/sdX1
sudo partprobe "$DEV" 2>/dev/null || sudo blockdev --rereadpt "$DEV" 2>/dev/null || true
udevadm settle 2>/dev/null || true

# Find the partition node (sdb1, or nvme0n1p1 for NVMe-like naming)
if [[ -b "${DEV}1" ]]; then
    PART="${DEV}1"
elif [[ -b "${DEV}p1" ]]; then
    PART="${DEV}p1"
else
    sleep 2
    [[ -b "${DEV}1" ]] && PART="${DEV}1" || die "partition not found after fdisk"
fi
ok "partition created: $PART"

# --- 3) mkfs -> mount -> cp -> umount -> remount + cat ------------------------
info "mkfs.fat on $PART..."
sudo mkfs.fat -F 12 "$PART" >/dev/null
ok "FAT filesystem created"

MNT=$(mktemp -d /tmp/caos_msc.XXXXXX)
info "mount $PART -> $MNT..."
sudo mount "$PART" "$MNT"
ok "mounted"

info "writing $TEST_FILE..."
echo "$TEST_DATA" | sudo tee "$MNT/$TEST_FILE" >/dev/null
sync
ok "file written: \"$TEST_DATA\""

info "umount..."
sudo umount "$MNT"
ok "unmounted"

info "remount + cat (persistence check)..."
sudo mount "$PART" "$MNT"
READBACK=$(sudo cat "$MNT/$TEST_FILE")
sudo umount "$MNT"

echo
if [[ "$READBACK" == "$TEST_DATA" ]]; then
    ok "CHECK PASSED - read: \"$READBACK\""
    echo "${GRN}=== TEST USB MSC OK ===${RST}"
    exit 0
else
    echo "${RED}expected: \"$TEST_DATA\"${RST}"
    echo "${RED}read    : \"$READBACK\"${RST}"
    die "MISMATCH - data differ after remount"
fi
