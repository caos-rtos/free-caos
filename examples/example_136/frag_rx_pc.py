#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""PC receiver for example 136 (IP fragmentation on TX under load).

usage: frag_rx_pc.py [port] [--idle S] [--expect N]

Listens on UDP <port> (default 7962), checks every datagram the board's burst
tasks send: the header (task, 0x36 marker, seq, run) and every byte of the
pattern. The board fragments anything over its 'ip mtu'; this PC reassembles
it, so a datagram that arrives is a whole datagram - the check is that it is
the RIGHT one: no fragments of two datagrams mixed, no stale buffer reused.

Ends after --idle seconds without traffic (default 3, counted from the first
datagram) and prints one summary line per run plus the verdict:
    RX RESULT : OK    every datagram intact, none twice
    RX RESULT : FAIL  a corrupted or duplicated datagram
Lost datagrams are counted, not failed (the board reports its drops), unless
--expect N is given: then fewer than N received is a FAIL too.
pc_rcvbuf_drop counts datagrams THIS PC reassembled and then dropped at the
socket (Udp RcvbufErrors): Python is slower than the board at full burst.
/proc/net/snmp ReasmFails must not grow. No root needed.
"""
import argparse
import socket
import struct
import sys
import time


def snmp(proto="Ip:"):
    with open("/proc/net/snmp") as f:
        rows = [ln.split() for ln in f if ln.startswith(proto)]
    return dict(zip(rows[0][1:], map(int, rows[1][1:])))


def snmp_ip():
    return snmp("Ip:")


def expected(task, seq, size):
    return bytes(((i * 7 + seq + task * 13) & 0xFF) for i in range(8, size))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", type=int, default=7962)
    ap.add_argument("--idle", type=float, default=3.0)
    ap.add_argument("--expect", type=int, default=0)
    ap.add_argument("--wait", type=float, default=60.0,
                    help="give up if nothing arrives within this many seconds")
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 23)
    s.bind(("", a.port))
    print("listening on UDP %d" % a.port, flush=True)

    before = snmp_ip()
    ubefore = snmp("Udp:")
    seen = set()
    good = bad = dup = 0
    sizes = set()
    runs = {}
    first = None
    s.settimeout(a.wait)
    cache = {}
    while True:
        try:
            d, src = s.recvfrom(65535)
        except socket.timeout:
            break
        if first is None:
            first = time.time()
            s.settimeout(a.idle)
        if len(d) < 8 or d[1] != 0x36:
            bad += 1
            continue
        task = d[0]
        seq, run = struct.unpack("!IH", d[2:8])
        key = (len(d), task, seq)
        want = cache.get(key)
        if want is None:
            want = expected(task, seq, len(d))
            if len(cache) < 4096:
                cache[key] = want
        if d[8:] != want:
            bad += 1
            print("BAD  task=%u seq=%u run=%u len=%u" % (task, seq, run, len(d)))
            continue
        if (run, task, seq) in seen:
            dup += 1
            continue
        seen.add((run, task, seq))
        good += 1
        sizes.add(len(d))
        runs.setdefault(run, {}).setdefault(task, 0)
        runs[run][task] += 1

    after = snmp_ip()
    uafter = snmp("Udp:")
    # Reassembled by this kernel but dropped at the socket: Python too slow
    # to drain it. Board-side these datagrams were delivered.
    rcvbuf = uafter["RcvbufErrors"] - ubefore["RcvbufErrors"]
    rfail = after["ReasmFails"] - before["ReasmFails"]
    roks = after["ReasmOKs"] - before["ReasmOKs"]
    dt = (time.time() - first - a.idle) if first else 0.0
    for run in sorted(runs):
        per = " ".join("t%u=%u" % (t, n) for t, n in sorted(runs[run].items()))
        print("run %u: %s" % (run, per))
    print("rx good=%d bad=%d dup=%d sizes=%s ReasmOKs +%d ReasmFails +%d "
          "pc_rcvbuf_drop=%d  %.1fs"
          % (good, bad, dup, sorted(sizes), roks, rfail, rcvbuf, max(dt, 0.0)))
    ok = bad == 0 and dup == 0 and rfail == 0 and good > 0
    if a.expect and good < a.expect:
        print("expected %d, got %d" % (a.expect, good))
        ok = False
    print("RX RESULT : %s" % ("OK" if ok else "FAIL"), flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
