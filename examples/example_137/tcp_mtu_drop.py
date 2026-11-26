#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""PC side of example 137: a TCP connection that lives through an MTU drop.

usage: tcp_mtu_drop.py <board_ip> [--port 7] [--size 4000] [--go FILE]
                       [--timeout 20] [--mss 472]

  phase A  connect while the board is at MTU 1500, echo <size> bytes
           (the board announced MSS 1460: full segments of 1460)
  --mid    the wait below happens HALF WAY through phase A instead
           ('PHASE A MID'), with segments of 730/1460 in flight: those
           retransmitted after the drop are cut by ip_frag (tcp_oversize)
  -------  prints 'PHASE A DONE', then waits for FILE to exist: meanwhile
           the board is switched to 'ip mtu 512' (bench or by hand)
  phase B  on the SAME connection, echo <size> bytes again. The board's
           segments of 1460 no longer fit the MTU
  phase C  a NEW connection: the board must announce MSS <= <mss> and echo

One line per phase: 'PHASE X OK ...' or 'PHASE X FAIL ...', then
'TCP MTU RESULT : OK|FAIL'. exit 0 only when all three pass. No root needed.
"""
import argparse
import os
import select
import socket
import struct
import sys
import time

TCP_INFO_SND_MSS = 16          # tcpi_snd_mss: 8 bytes of u8, then u32 rto, ato, snd_mss


def payload(n, seed):
    return bytes(((i * 13 + seed) & 0xFF) for i in range(n))


def echo(s, n, seed, timeout, at_half=None):
    """Send and read at the same time. sendall() first and recv() after
    deadlocks on a big echo: the replies nobody reads fill the buffers, the
    board's window closes and the send never ends. `timeout` is the longest
    the connection may stay silent, not the whole transfer."""
    want = payload(n, seed)
    s.setblocking(False)
    t0 = time.time()
    last = t0
    sent = 0
    got = bytearray()
    try:
        while len(got) < n and time.time() - last < timeout:
            wl = [s] if sent < n else []
            r, w, _ = select.select([s], wl, [], 0.2)
            if w:
                k = s.send(want[sent:sent + 65536])
                if k > 0:
                    sent += k
                    last = time.time()
                if at_half and sent >= n // 2:
                    at_half()           # the MTU drops with segments in flight
                    at_half = None
                    last = time.time()
            if r:
                chunk = s.recv(65536)
                if not chunk:
                    break
                got += chunk
                last = time.time()
    except (BlockingIOError, InterruptedError):
        pass
    except OSError:
        pass
    s.setblocking(True)
    s.settimeout(5)
    return bytes(got) == want, len(got), time.time() - t0


def snd_mss(s):
    info = s.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 104)
    return struct.unpack_from("I", info, TCP_INFO_SND_MSS)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--port", type=int, default=7)
    ap.add_argument("--size", type=int, default=4000)
    ap.add_argument("--go", default="")
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--mss", type=int, default=472)
    ap.add_argument("--mid", action="store_true",
                    help="drop the MTU half way through phase A, segments in flight")
    a = ap.parse_args()
    ok_all = True

    s = socket.create_connection((a.ip, a.port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    mss_a = snd_mss(s)

    def wait_go():
        print("PHASE A MID", flush=True)
        if a.go:
            t0 = time.time()
            while not os.path.exists(a.go) and time.time() - t0 < 120:
                time.sleep(0.05)
        else:
            input("set 'ip mtu 512' on the board, then press Enter ")

    ok, n, dt = echo(s, a.size, 1, a.timeout, wait_go if a.mid else None)
    print("PHASE A %s  echo %d/%d B in %.2fs  board MSS %d"
          % ("OK" if ok else "FAIL", n, a.size, dt, mss_a), flush=True)
    ok_all &= ok
    print("PHASE A DONE", flush=True)

    if a.mid:
        pass                    # the drop already happened inside phase A
    elif a.go:
        t0 = time.time()
        while not os.path.exists(a.go):
            if time.time() - t0 > 120:
                print("PHASE B FAIL  nobody created %s" % a.go, flush=True)
                sys.exit(1)
            time.sleep(0.1)
    else:
        input("set 'ip mtu 512' on the board, then press Enter ")

    ok, n, dt = echo(s, a.size, 2, a.timeout)
    print("PHASE B %s  same connection after the MTU drop: echo %d/%d B in %.2fs"
          % ("OK" if ok else "FAIL", n, a.size, dt), flush=True)
    ok_all &= ok
    try:
        s.close()
    except OSError:
        pass

    try:
        c = socket.create_connection((a.ip, a.port), timeout=5)
        mss_c = snd_mss(c)
        ok, n, dt = echo(c, a.size, 3, a.timeout)
        ok_c = ok and mss_c <= a.mss
        print("PHASE C %s  new connection: board MSS %d (max %d), echo %d/%d B in %.2fs"
              % ("OK" if ok_c else "FAIL", mss_c, a.mss, n, a.size, dt), flush=True)
        c.close()
    except OSError as e:
        ok_c = False
        print("PHASE C FAIL  new connection: %s" % e, flush=True)
    ok_all &= ok_c

    print("TCP MTU RESULT : %s" % ("OK" if ok_all else "FAIL"), flush=True)
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
