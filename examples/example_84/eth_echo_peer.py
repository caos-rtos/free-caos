#!/usr/bin/env python3
# Proprietary - All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""
example_84 peer: raw L2 echo generator.

Sends N Ethernet frames of EtherType 0x88B5 at the board and counts how many
come back. The board swaps source and destination MAC and returns the same
buffer, so a frame that arrives here was echoed by the board and nothing else
on the network could have produced it.

WHAT THIS MEASURES, AND WHY IT IS NOT iperf

iperf over TCP puts the driver, the net task, the TCP receive ring and the
application into a single number. On the H753 that number disagreed with the
PC by 13.6x and there was no way to tell which layer was responsible. This
sends raw L2 frames that never reach IP: whatever rate comes back is what the
driver and the net task can do between them.

The board is the slower end by construction, so the interesting figure is not
"did every frame come back" - it is how many came back and at what rate when
the sender pushes harder than the receiver can echo. Loss here is data, not
failure, which is why nothing in this script treats it as an error.

    sudo python3 eth_echo_peer.py --iface enp3s0
    sudo python3 eth_echo_peer.py --iface enp3s0 --count 20000 --size 1500

Root is required: raw sockets always are.
"""

import argparse
import json
import socket
import struct
import sys
import time

ETHERTYPE = 0x88B5
BOARD_MAC = bytes.fromhex("020000000084")


def parse_args():
    p = argparse.ArgumentParser(description="example_84 raw L2 echo peer")
    p.add_argument("--iface", required=True,
                   help="network interface facing the board (e.g. enp3s0)")
    p.add_argument("--count", type=int, default=10000,
                   help="frames to send (default 10000)")
    p.add_argument("--size", type=int, default=1500,
                   help="total frame size in bytes, 60-1514 (default 1500)")
    p.add_argument("--board-mac", default=BOARD_MAC.hex(),
                   help="board MAC without separators (default 020000000084)")
    p.add_argument("--skip", type=int, default=0,
                   help="exclude the first N frames from the score. They are "
                        "still sent - the receiver must see the same traffic - "
                        "but the window in which the link is still settling "
                        "after a reset is not the receiver's fault. Beats a "
                        "throwaway warm-up burst, which leaves the DMA ring on "
                        "an arbitrary slot and makes the next measurement start "
                        "from a different state")
    p.add_argument("--mbps", type=float, default=0.0,
                   help="offered rate in Mbit/s; 0 (default) = as fast as the "
                        "socket takes it. Use it to find the rate at which the "
                        "board starts refusing: sending at wire rate only ever "
                        "measures how fast THIS PC is")
    p.add_argument("--timeout", type=float, default=2.0,
                   help="seconds to keep draining after the last send")
    p.add_argument("--json", metavar="PATH",
                   help="also write the result as JSON to PATH")
    return p.parse_args()


def main():
    a = parse_args()

    size = max(60, min(1514, a.size))
    board = bytes.fromhex(a.board_mac)

    try:
        tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                           socket.htons(ETHERTYPE))
        tx.bind((a.iface, 0))
    except PermissionError:
        print("raw sockets need root: run with sudo", file=sys.stderr)
        return 2
    except OSError as e:
        print(f"cannot open {a.iface}: {e}", file=sys.stderr)
        return 2

    src = tx.getsockname()[4][:6]

    # The echo comes back with src/dst swapped, so it arrives addressed to us
    # with the board as source. A separate socket for RX keeps the send loop
    # from having to drain, and the large buffer is what stops the kernel from
    # dropping echoes while we are busy sending.
    rx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                       socket.htons(ETHERTYPE))
    rx.bind((a.iface, 0))
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 * 1024 * 1024)
    rx.setblocking(False)

    # dst | src | ethertype | sequence | padding
    header = board + src + struct.pack("!H", ETHERTYPE)
    payload_len = size - len(header) - 4
    padding = bytes(payload_len) if payload_len > 0 else b""

    print(f"[echo] {a.iface}: {src.hex(':')} -> {board.hex(':')}  "
          f"ethertype 0x{ETHERTYPE:04X}")
    print(f"[echo] sending {a.count} frames of {size} bytes")

    got = 0
    seqs_seen = set()
    t0 = time.monotonic()

    # Pacing, when asked for. The schedule is computed from the START time, not
    # by sleeping a fixed gap per frame: a per-frame sleep accumulates its own
    # overhead and the actual rate drifts below the requested one, which would
    # be read as the board refusing early.
    # Frame time on the wire includes the preamble, the inter-frame gap and the
    # CRC - 24 bytes on top of the payload - so a rate asked for in Mbit/s is
    # honoured as the wire would see it, not as the payload alone.
    bits_per_frame = (a.size + 24) * 8
    period = (bits_per_frame / (a.mbps * 1e6)) if a.mbps > 0 else 0.0

    for i in range(a.count):
        if period:
            due = t0 + i * period
            now = time.monotonic()
            if due > now:
                time.sleep(due - now)
        try:
            tx.send(header + struct.pack("!I", i) + padding)
        except OSError as e:
            print(f"[echo] send failed at frame {i}: {e}", file=sys.stderr)
            break

        # Drain opportunistically: waiting until the end would need the kernel
        # to hold every echo, and a burst big enough to be interesting is
        # bigger than any socket buffer.
        while True:
            try:
                f = rx.recv(2048)
            except BlockingIOError:
                break
            if len(f) >= 18 and f[6:12] == board:
                got += 1
                seqs_seen.add(struct.unpack("!I", f[14:18])[0])

    t_send = time.monotonic() - t0

    # Keep draining: the last echoes are still in flight.
    deadline = time.monotonic() + a.timeout
    while time.monotonic() < deadline:
        try:
            f = rx.recv(2048)
        except BlockingIOError:
            time.sleep(0.001)
            continue
        if len(f) >= 18 and f[6:12] == board:
            got += 1
            seqs_seen.add(struct.unpack("!I", f[14:18])[0])
            deadline = time.monotonic() + 0.2   # still arriving: wait a bit more

    elapsed = time.monotonic() - t0
    sent_bits = a.count * size * 8
    echo_bits = got * size * 8

    # The score, with the settling window excluded. Computed from the sequence
    # numbers rather than by not counting during the loop: the frames were sent
    # and the receiver saw them, they simply do not COUNT AGAINST it.
    skip    = max(0, min(a.skip, a.count))
    scored  = a.count - skip
    got_sc  = sum(1 for s in seqs_seen if s >= skip)
    pct     = (100.0 * got_sc / scored) if scored else 0.0

    print()
    print(f"[echo] sent     {a.count:>9} frames  "
          f"{sent_bits / t_send / 1e6:8.3f} Mbps over {t_send:.2f} s")
    print(f"[echo] echoed   {got:>9} frames  "
          f"{echo_bits / elapsed / 1e6:8.3f} Mbps over {elapsed:.2f} s")
    if skip:
        print(f"[echo] first {skip} frames not scored (link settling)")
    print(f"[echo] returned {pct:.2f}% "
          f"({scored - got_sc} not echoed)")
    if len(seqs_seen) != got:
        print(f"[echo] {got - len(seqs_seen)} duplicate sequence numbers "
              f"came back - a frame was echoed more than once")

    if a.json:
        with open(a.json, "w") as fh:
            json.dump({
                "iface": a.iface,
                # What was ASKED for, next to what was achieved: a step that
                # could not be offered at the requested rate (the PC itself
                # falling behind) must not be read as the board refusing.
                "offered_mbps": a.mbps,
                "frames_sent": a.count,
                "frames_echoed": got,
                "unique_echoed": len(seqs_seen),
                "frame_size": size,
                "send_seconds": round(t_send, 3),
                "total_seconds": round(elapsed, 3),
                "send_mbps": round(sent_bits / t_send / 1e6, 3),
                "echo_mbps": round(echo_bits / elapsed / 1e6, 3),
                "returned_pct": round(100.0 * got / a.count, 2) if a.count else 0,
                # WHERE the losses are, not just how many. Frames missing from
                # the HEAD of the burst mean the link was not carrying yet -
                # the receiver had not finished coming up - while losses spread
                # through the run are the receiver being outrun, which is the
                # thing this example exists to measure. Telling them apart by
                # eye is impossible; the sequence numbers say it exactly.
                "first_echoed_seq": min(seqs_seen) if seqs_seen else None,
                "last_echoed_seq": max(seqs_seen) if seqs_seen else None,
            }, fh, indent=2)
        print(f"[echo] wrote {a.json}")

    # Loss is a measurement here, not a failure: the board is the slower end by
    # design. Only a board that echoed NOTHING is a failure to report.
    return 0 if got > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
