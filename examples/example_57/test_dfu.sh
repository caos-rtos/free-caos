#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Angelo Coppi
#
# HW test of the USB DFU runtime service (example_57).
#
# DFU runtime has no data endpoint: everything goes through EP0. The test
# therefore checks the descriptors (class 0xFE/0x01/0x01 + functional
# descriptor) and the class requests, not a data path.
#
# Usage:
#   ./test_dfu.sh          # enumeration + state + DETACH
#   ./test_dfu.sh -n       # skips the DETACH (does not touch the device state)
#
# The DETACH is the last step because the device declares WILL_DETACH: after
# the request it leaves the bus by itself, so any later check would find
# nothing. The target console shows the line
# "[USB] DFU_DETACH received"; from the CLI, "usb status" shows detach_req=1.
#
# Needs: dfu-util, lsusb, sudo.

set -uo pipefail

VID="ca01"
PID_DFU="0040"
DO_DETACH=1

while getopts "nh" o; do
    case "$o" in
        n) DO_DETACH=0 ;;
        h) sed -n '4,20p' "$0"; exit 0 ;;
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
for t in dfu-util lsusb sudo; do
    command -v "$t" >/dev/null 2>&1 || die "missing command: $t"
done

# =============================================================================
head_ "1) Enumerazione"
# =============================================================================
lsusb -d "$VID:" >/dev/null 2>&1 || die "no device VID=$VID (target on and connected?)"
lsusb -d "$VID:"

if lsusb -d "$VID:$PID_DFU" >/dev/null 2>&1; then
    ok "PID $PID_DFU (DFU) as expected"
else
    # Not fatal: the PID is the sum of the contributions of the active classes,
    # so it changes when the config enables others.
    info "PID differs from $PID_DFU: does the config enable other classes?"
fi

# =============================================================================
head_ "2) Descriptors: DFU class 0xFE/0x01/0x01"
# =============================================================================
desc=$(sudo lsusb -v -d "$VID:" 2>/dev/null)
if [[ -z "$desc" ]]; then
    bad "lsusb -v produced no output (sudo needed)"
else
    # bInterfaceClass 254 = Application Specific, SubClass 1 = DFU,
    # Protocol 1 = runtime (2 would be DFU mode).
    grep -Eqi "bInterfaceClass +254" <<<"$desc" \
        && ok "bInterfaceClass 254 (Application Specific)" \
        || bad "bInterfaceClass 254 not found"
    grep -Eqi "bInterfaceSubClass +1" <<<"$desc" \
        && ok "bInterfaceSubClass 1 (DFU)" \
        || bad "bInterfaceSubClass 1 not found"
    grep -Eqi "bInterfaceProtocol +1" <<<"$desc" \
        && ok "bInterfaceProtocol 1 (runtime)" \
        || bad "bInterfaceProtocol 1 not found"

    # DFU runtime must NOT have data endpoints: EP0 only.
    neps=$(grep -Eai "bNumEndpoints" <<<"$desc" | grep -Eo '[0-9]+' | head -1)
    [[ "$neps" == "0" ]] && ok "bNumEndpoints 0 (EP0 only, correct for DFU runtime)" \
                         || info "bNumEndpoints=$neps (expected 0 if DFU is the only class)"

    # Functional descriptor: transfer size and attributes declared by the example.
    grep -qi "wTransferSize *1024" <<<"$desc" \
        && ok "wTransferSize 1024 (as in ca_os_ex57.c)" \
        || info "wTransferSize differs from 1024"
    grep -qi "Will Detach" <<<"$desc" \
        && ok "WILL_DETACH attribute declared" \
        || info "WILL_DETACH not seen in the functional descriptor"
fi

# =============================================================================
head_ "3) dfu-util -l : the device is recognised as DFU"
# =============================================================================
info "dfu-util -l"
out=$(sudo dfu-util -l 2>&1)
echo "$out" | grep -i "$VID" || true
if grep -qi "$VID" <<<"$out"; then
    ok "dfu-util recognises the device"
    grep -qi "caOS DFU Runtime" <<<"$out" \
        && ok "iface string \"caOS DFU Runtime\" present" \
        || info "iface string not shown by dfu-util 0.9"
else
    bad "dfu-util does not list the device"
fi

# =============================================================================
head_ "4) DFU_DETACH"
# =============================================================================
if (( ! DO_DETACH )); then
    info "skipped (-n)"
else
    info "dfu-util -e  (sends DFU_DETACH)"
    # dfu-util exits with an error when the device leaves right after the
    # DETACH: it is the behaviour WILL_DETACH expects, not a failure.
    out=$(sudo dfu-util -e -d "$VID:" 2>&1); rc=$?
    echo "$out" | tail -5
    if grep -qiE "detach|disconnect" <<<"$out" || (( rc == 0 )); then
        ok "DETACH sent"
    else
        bad "DETACH failed (rc=$rc)"
    fi
    echo
    info "the target console must show:"
    echo "      [USB] DFU_DETACH received - the app would switch to DFU mode"
    info "or from the CLI:  usb status   ->   detach_req=1"
fi

# =============================================================================
echo
if (( fail )); then
    echo "${RED}=== TEST EX57 FAILED ===${RST}"; exit 1
fi
echo "${GRN}=== TEST EX57 OK ===${RST}"
(( DO_DETACH )) && info "check the last line on the target console"
exit 0
