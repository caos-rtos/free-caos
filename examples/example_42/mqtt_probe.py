#!/usr/bin/env python3
"""
caOS Example 42 - raw-socket MQTT probe (bench helper, QoS 0 only).

Sends what mosquitto_pub cannot: a PUBLISH on caos/led whose remaining length
is chosen by the caller, in several send() calls, followed by a normal "on".
Works against any broker (the bench uses it through mosquitto).

    python3 mqtt_probe.py [--host H] [--port P] oversize N

N is the remaining length of the first PUBLISH (topic 2+8 bytes + filler).
"""
import argparse
import socket
import struct
import time


def remlen(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.10.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--chunk", type=int, default=100)
    ap.add_argument("cmd", choices=["oversize"])
    ap.add_argument("n", type=int)
    a = ap.parse_args()

    s = socket.create_connection((a.host, a.port), 3)
    cid = b"benchprobe"
    p = b"\x00\x04MQTT\x04\x02\x00\x3c" + struct.pack("!H", len(cid)) + cid
    s.send(b"\x10" + remlen(len(p)) + p)
    time.sleep(0.5)
    s.recv(16)

    t = b"caos/led"
    body = struct.pack("!H", len(t)) + t + b"z" * max(0, a.n - 2 - len(t))
    raw = b"\x30" + remlen(len(body)) + body
    for i in range(0, len(raw), a.chunk):
        s.send(raw[i:i + a.chunk])
        time.sleep(0.05)
    print("oversize PUBLISH sent: remaining length %d" % len(body))
    time.sleep(1.0)
    v = struct.pack("!H", len(t)) + t + b"on"
    s.send(b"\x30" + remlen(len(v)) + v)
    print("normal PUBLISH sent: caos/led on")
    time.sleep(1.0)
    s.close()


if __name__ == "__main__":
    main()
