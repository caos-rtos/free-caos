#!/usr/bin/env python3
# Proprietary - All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""
echo_latency.py - objective CDC echo latency measurement for example_52

Release -O3 wakeup bug hunt (2026-07-17): the symptom is NOT data loss but
latency (the echo task wakes up late/in bursts). This test sends 1 byte at a
time and measures the round trip of each, giving a PASS/FAIL verdict
repeatable - instead of the "type and look" that already gave false verdicts.

Usage:
    python3 echo_latency.py [--port /dev/ttyACM0] [--count 60] [--gap 0.3]

PASS if: median RTT < 100 ms  AND  no RTT > 1000 ms
Requires: pyserial
"""

import argparse
import statistics
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("pyserial is required: pip install pyserial")
    sys.exit(1)

VID = 0xCA01

def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == VID:
            return p.device
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--count", type=int, default=60, help="bytes to send")
    ap.add_argument("--gap", type=float, default=0.05, help="pause between bytes [s]")
    ap.add_argument("--timeout", type=float, default=5.0, help="timeout RTT [s]")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("CDC port not found (VID 0xCA01) - use --port")
        sys.exit(1)

    ser = serial.Serial(port, 115200, timeout=0.05)
    time.sleep(0.5)
    ser.reset_input_buffer()

    rtts = []
    lost = 0
    print(f"port {port}: {args.count} bytes, one every {args.gap}s")
    for i in range(args.count):
        ch = bytes([0x41 + (i % 26)])          # 'A'..'Z'
        t0 = time.monotonic()
        ser.write(ch)
        deadline = t0 + args.timeout
        rtt = None
        while time.monotonic() < deadline:
            rx = ser.read(1)
            if rx == ch:
                rtt = (time.monotonic() - t0) * 1000.0
                break
            # stray bytes (late bursts): discard and go on
        if rtt is None:
            lost += 1
            print(f"  [{i:3}] LOST (>{args.timeout}s)")
        else:
            rtts.append(rtt)
            if rtt > 500.0:
                print(f"  [{i:3}] RTT {rtt:7.1f} ms  <-- SLOW")
        time.sleep(args.gap)

    ser.close()

    if not rtts:
        print("FAIL: no echo received")
        sys.exit(1)

    med = statistics.median(rtts)
    mx = max(rtts)
    over1s = sum(1 for r in rtts if r > 1000.0)
    print("---")
    print(f"echoes recvd  : {len(rtts)}/{args.count}  (lost {lost})")
    print(f"RTT median    : {med:7.1f} ms")
    print(f"RTT max       : {mx:7.1f} ms   (>1s: {over1s})")

    ok = (med < 100.0) and (over1s == 0) and (lost == 0)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
