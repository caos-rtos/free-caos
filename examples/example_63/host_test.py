#!/usr/bin/env python3
"""Example 63 - host test for UART DMA with variable-length protocol.

Frame:  [SOF 0xCA][type][len_lo][len_hi] payload[len] [crc_lo][crc_hi]
CRC16-CCITT (poly 0x1021, init 0xFFFF) over type+len+payload.
The board echoes back with type|0x80 and the same payload.

Usage:  python3 host_test.py /dev/ttyACM0 [baud]
"""

import os
import sys
import time

import serial

SOF = 0xCA
TYPE_ECHO = 0x01


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def build_frame(ftype: int, payload: bytes) -> bytes:
    hdr = bytes([ftype, len(payload) & 0xFF, len(payload) >> 8])
    crc = crc16_ccitt(hdr + payload)
    return bytes([SOF]) + hdr + payload + bytes([crc & 0xFF, crc >> 8])


def read_frame(ser: serial.Serial, expected_len: int):
    raw = ser.read(4 + expected_len + 2)
    if len(raw) < 4 + expected_len + 2:
        return None, f"timeout: received {len(raw)} bytes"
    if raw[0] != SOF:
        return None, f"bad SOF: {raw[0]:#04x}"
    ftype = raw[1]
    flen = raw[2] | (raw[3] << 8)
    if flen != expected_len:
        return None, f"bad len: {flen} != {expected_len}"
    payload = raw[4:4 + flen]
    rxcrc = raw[4 + flen] | (raw[5 + flen] << 8)
    if crc16_ccitt(raw[1:4 + flen]) != rxcrc:
        return None, "bad echo CRC"
    return (ftype, payload), None


def expect_echo(ser: serial.Serial, payload: bytes) -> str:
    ser.write(build_frame(TYPE_ECHO, payload))
    frame, err = read_frame(ser, len(payload))
    if err:
        return err
    ftype, rx = frame
    if ftype != (TYPE_ECHO | 0x80):
        return f"bad echo type: {ftype:#04x}"
    if rx != payload:
        return "echo payload mismatch"
    return ""


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    port = sys.argv[1]
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    ser = serial.Serial(port, baud, timeout=2)
    time.sleep(0.2)
    ser.reset_input_buffer()
    failed = 0

    # 1. ping-pong echo over growing sizes (256/257 = old ring boundary)
    for size in (1, 2, 16, 128, 255, 256, 257, 512, 1024):
        err = expect_echo(ser, os.urandom(size))
        print(f"echo {size:5d} B : {'OK' if not err else 'FAIL - ' + err}")
        failed += bool(err)

    # 2. corrupted CRC: the board must NOT answer
    bad = bytearray(build_frame(TYPE_ECHO, os.urandom(32)))
    bad[-1] ^= 0xFF
    ser.write(bytes(bad))
    silent = ser.read(1) == b""
    print(f"bad CRC      : {'OK (no echo)' if silent else 'FAIL - unexpected echo'}")
    failed += not silent

    # 3. resync: garbage without SOF, then a valid frame
    garbage = bytes(b for b in os.urandom(64) if b != SOF)
    ser.write(garbage)
    time.sleep(0.1)
    err = expect_echo(ser, os.urandom(64))
    print(f"resync       : {'OK' if not err else 'FAIL - ' + err}")
    failed += bool(err)

    # 4. truncated frame: a header announcing 64 B, only 10 B of it, then the
    #    peer falls silent. The board must give the frame up (DMA receive
    #    timeout) and take the next one - not swallow it as the missing tail.
    trunc = build_frame(TYPE_ECHO, os.urandom(64))[:4 + 10]
    ser.write(trunc)
    time.sleep(1.0)
    ser.reset_input_buffer()
    err = expect_echo(ser, os.urandom(32))
    print(f"truncated    : {'OK (next frame echoed)' if not err else 'FAIL - ' + err}")
    failed += bool(err)

    # 5. throughput over 50 frames of 1024 B (round trip)
    n, size = 50, 1024
    t0 = time.time()
    for _ in range(n):
        err = expect_echo(ser, os.urandom(size))
        if err:
            print(f"throughput   : FAIL - {err}")
            failed += 1
            break
    else:
        dt = time.time() - t0
        kbs = n * size * 2 / dt / 1024
        print(f"throughput   : OK - {n} frame x {size} B in {dt:.2f} s ({kbs:.1f} KiB/s)")

    print("---")
    print("PASS" if failed == 0 else f"FAIL ({failed} test)")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
