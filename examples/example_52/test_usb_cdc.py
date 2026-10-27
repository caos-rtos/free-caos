#!/usr/bin/env python3
"""
test_usb_cdc.py - CDC-ACM echo test for example_52

Opens /dev/ttyACM* of the caOS device (VID 0xCA01), sends lines and checks the echo.

Usage:
    python3 test_usb_cdc.py [--port /dev/ttyACM0] [--lines N]

Requires: pyserial  (pip install pyserial)
"""

import argparse
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
    ap.add_argument("--lines", type=int, default=100)
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print(f"No CDC port with VID 0x{VID:04x} found")
        sys.exit(1)
    print(f"Port: {port}")

    ser = serial.Serial(port, args.baud, timeout=2)
    time.sleep(0.3)
    # discard the welcome banner sent on open (DTR)
    ser.reset_input_buffer()

    ok = 0
    err = 0
    t0 = time.time()
    for i in range(args.lines):
        msg = f"line{i:04d}\n".encode()
        ser.write(msg)
        echo = ser.read(len(msg))
        if echo == msg:
            ok += 1
        else:
            err += 1
            if err <= 3:
                print(f"  MISMATCH sent={msg!r} got={echo!r}")
    elapsed = time.time() - t0

    total = ok * 9  # ~9 byte/line
    print(f"\nEcho: {ok}/{args.lines} OK  {err} err")
    print(f"({total} byte in {elapsed:.2f}s)")
    ser.close()

if __name__ == "__main__":
    main()
