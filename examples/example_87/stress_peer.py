#!/usr/bin/env python3
"""
stress_peer.py - PC peer for caOS example 87: DHCP + ping flood

Network load for the lost wake-up bench. It serves DHCP to the board, waits for
its lease, then floods it with pings until Ctrl-C. Every request is answered,
so the board both receives and transmits: ETH interrupts and work for its
'net' task, landing at random points of whatever the bench is doing.

It does not touch the serial port: run the bench yourself (minicom,
'e87 start ...'), with the board under gdb if you want to catch a hang hot
(examples/example_87/debug_ex87.gdb).

Usage:
  sudo python3 stress_peer.py            # ping -f needs root
  python3 stress_peer.py                 # no root: ping -i 0.002, the user minimum
  python3 stress_peer.py --board-ip 192.168.10.55 --no-dhcp

The board (H753, H563, F767) asks for DHCP at boot with MAC 02:ca:05:00:00:87
and prints 'example_87: DHCP bound IP=...'. DHCP comes from
tools/bench_net.sh (PC at 192.168.10.1, sudoers NOPASSWD for that script).
"""

import argparse
import os
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BENCH_NET = os.path.join(REPO, "tools", "bench_net.sh")
BOARD_MAC = "02:ca:05:00:00:87"
LEASES = "/run/caos-bench/dnsmasq.leases"


def board_ip_from_leases():
    """dnsmasq lease line: <expiry> <mac> <ip> <name> <client-id>"""
    try:
        with open(LEASES) as f:
            for ln in f:
                p = ln.split()
                if len(p) >= 3 and p[1].lower() == BOARD_MAC:
                    return p[2]
    except OSError:
        pass
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="ex87: DHCP + ping flood on the board")
    ap.add_argument("--board-ip", default=None, help=f"default: the lease of {BOARD_MAC}")
    ap.add_argument("--no-dhcp", action="store_true", help="do not start the DHCP server")
    ap.add_argument("--wait", type=int, default=120, help="seconds to wait for the lease")
    args = ap.parse_args()

    if not args.no_dhcp:
        if subprocess.call(["sudo", "-n", BENCH_NET, "up"]) != 0:
            print("bench_net.sh up failed")
            return 1

    ip = args.board_ip
    if ip is None:
        print(f"waiting for the DHCP lease of {BOARD_MAC} (reset the board if it booted before)")
        t0 = time.monotonic()
        while ip is None and time.monotonic() - t0 < args.wait:
            ip = board_ip_from_leases()
            if ip is None:
                time.sleep(1)
        if ip is None:
            print(f"no lease in {args.wait} s")
            return 1
    print(f"board at {ip}")

    env = dict(os.environ, LC_ALL="C")          # the locale's decimal comma breaks ping
    rate = ["-f"] if os.geteuid() == 0 else ["-i", "0.002"]
    print(f"ping {' '.join(rate)} {ip} - Ctrl-C to stop")
    try:
        return subprocess.call(["ping", *rate, ip], env=env)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
