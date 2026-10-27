#!/usr/bin/env python3
"""
iperf_peer.py - PC peer for caOS example 44, iperf TCP

Usage:
  sudo python3 iperf_peer.py --mode server [--iface enp7s0] [--pc-ip 192.168.10.1] [--port 5001]
      PC listens, the board connects and sends -> measures RX throughput

  sudo python3 iperf_peer.py --mode client [--iface enp7s0] [--pc-ip 192.168.10.1] [--port 5001] [--dur 10]
      PC connects to the board (which listens) and sends -> measures TX throughput

  --no-dhcp   skip DHCP/IP setup (IP already configured by hand)

Dependencies:
  apt install dnsmasq
"""

import argparse
import os
import signal
import socket
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# DHCP / network
# ---------------------------------------------------------------------------
DHCP_START = "192.168.10.100"
DHCP_END   = "192.168.10.200"
MASK       = "255.255.255.0"
DHCP_CONF  = "/tmp/dnsmasq-caos44.conf"
DHCP_PID   = "/tmp/dnsmasq-caos44.pid"
DHCP_LOG   = "/tmp/dnsmasq-caos44.log"

CHUNK = 65536


def get_iface_cidr(iface):
    try:
        out = subprocess.check_output(
            ["ip", "-4", "addr", "show", "dev", iface],
            stderr=subprocess.DEVNULL).decode()
        for line in out.splitlines():
            s = line.strip()
            if s.startswith("inet "):
                return s.split()[1]
    except Exception:
        pass
    return None


def setup_interface(iface, pc_ip):
    current_cidr = get_iface_cidr(iface)
    if current_cidr and current_cidr.split("/")[0] == pc_ip:
        print(f"[*] {iface} already configured with {pc_ip}")
        return
    if current_cidr:
        print(f"[*] {iface} has {current_cidr} - replacing with {pc_ip}/24")
    subprocess.run(["ip", "addr", "flush", "dev", iface], stderr=subprocess.DEVNULL)
    subprocess.run(["ip", "addr", "add", f"{pc_ip}/24", "dev", iface], check=True)
    subprocess.run(["ip", "link", "set", iface, "up"], check=True)
    print(f"[*] {iface} configured -> {pc_ip}/24")


def start_dnsmasq(iface, pc_ip):
    conf = (
        f"interface={iface}\n"
        f"bind-interfaces\n"
        f"except-interface=lo\n"
        f"dhcp-range={DHCP_START},{DHCP_END},{MASK},1h\n"
        f"dhcp-option=option:router,{pc_ip}\n"
        f"dhcp-option=option:dns-server,8.8.8.8\n"
        f"dhcp-option=option:netmask,{MASK}\n"
        f"log-dhcp\n"
        f"no-resolv\n"
        f"no-hosts\n"
    )
    with open(DHCP_CONF, "w") as f:
        f.write(conf)

    subprocess.run(["pkill", "-f", "dnsmasq-caos44"], stderr=subprocess.DEVNULL)
    for p in [DHCP_PID, DHCP_LOG]:
        try:
            os.remove(p)
        except FileNotFoundError:
            pass
    time.sleep(0.4)

    subprocess.run(
        ["/usr/sbin/dnsmasq", "-C", DHCP_CONF,
         f"--pid-file={DHCP_PID}",
         f"--log-facility={DHCP_LOG}"],
        check=True)
    time.sleep(0.5)

    if not os.path.exists(DHCP_PID):
        print(f"[!] dnsmasq did not start - see {DHCP_LOG}")
        try:
            print(open(DHCP_LOG).read())
        except Exception:
            pass
        sys.exit(1)

    pid = open(DHCP_PID).read().strip()
    print(f"[*] dnsmasq started (pid {pid})")
    print(f"[*] DHCP pool {DHCP_START} - {DHCP_END}")
    print(f"[*] DHCP log: tail -f {DHCP_LOG}")


def stop_dnsmasq():
    if os.path.exists(DHCP_PID):
        try:
            pid = int(open(DHCP_PID).read().strip())
            os.kill(pid, signal.SIGTERM)
            print(f"[*] dnsmasq stopped (pid {pid})")
        except Exception:
            pass
    else:
        subprocess.run(["pkill", "-f", "dnsmasq-caos44"], stderr=subprocess.DEVNULL)
    for p in [DHCP_PID, DHCP_CONF]:
        try:
            os.remove(p)
        except Exception:
            pass


def wait_board_ip(timeout_s=60):
    print(f"[*] Waiting for DHCP ACK from the board (max {timeout_s}s)...")
    for i in range(timeout_s):
        time.sleep(1)
        try:
            log = open(DHCP_LOG).read()
            for line in log.splitlines():
                if "DHCPACK" in line:
                    parts = line.split()
                    for p in parts:
                        if p.count(".") == 3 and not p.startswith("("):
                            print()
                            return p
        except FileNotFoundError:
            pass
        print(f"\r[*] Waiting for DHCP... {i+1}s", end="", flush=True)
    print()
    return None


# ---------------------------------------------------------------------------
# iperf server: PC listens, the board sends
# ---------------------------------------------------------------------------
def run_server(pc_ip, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((pc_ip, port))
    srv.listen(1)
    print(f"[server] listening on {pc_ip}:{port} ...")

    conn, addr = srv.accept()
    print(f"[server] connection from {addr[0]}:{addr[1]}")
    srv.close()

    TCP_QUICKACK = 12
    conn.setsockopt(socket.IPPROTO_TCP, TCP_QUICKACK, 1)

    total = 0
    t0 = time.monotonic()
    try:
        while True:
            conn.setsockopt(socket.IPPROTO_TCP, TCP_QUICKACK, 1)  # Linux resets it each recv
            data = conn.recv(CHUNK)
            if not data:
                break
            total += len(data)
    except Exception as e:
        print(f"[server] recv error: {e}")
    finally:
        conn.close()

    elapsed = time.monotonic() - t0
    bps  = (total * 8) / elapsed if elapsed > 0 else 0
    mbps = bps / 1_000_000
    print(f"[server] received {total} bytes in {elapsed:.2f} s  ->  {bps:.0f} bps  ({mbps:.3f} Mbps)")


# ---------------------------------------------------------------------------
# iperf client: the PC connects to the board, which listens
# ---------------------------------------------------------------------------
def run_client(board_ip, port, dur, connect_timeout=60):
    """Connect to the board, which must already be listening.

    The board starts listening only when the user types "iperf server" on its
    console, which is possible only AFTER DHCP gave it an IP: so retry here while
    the listener is missing, instead of failing on the first attempt.
    """
    buf = bytes(CHUNK)
    deadline_conn = time.monotonic() + connect_timeout
    conn = None
    announced = False
    while True:
        try:
            conn = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            conn.settimeout(2.0)
            conn.connect((board_ip, port))
            conn.settimeout(None)
            break
        except OSError as e:   # ConnectionRefusedError/timeout are subclasses
            conn.close()
            if time.monotonic() >= deadline_conn:
                print(f"[client] cannot connect to {board_ip}:{port}: {e}")
                print(f"[client] is the board listening? type 'iperf server' on its console.")
                return
            if not announced:
                print(f"[client] {board_ip}:{port} not answering yet - "
                      f"type 'iperf server' on the board console (waiting {connect_timeout}s)")
                announced = True
            time.sleep(1.0)

    print(f"[client] connected to {board_ip}:{port}  duration={dur} s")

    total = 0
    t0 = time.monotonic()
    deadline = t0 + dur
    try:
        while time.monotonic() < deadline:
            sent = conn.send(buf)
            total += sent
    except Exception as e:
        print(f"[client] send error: {e}")
    finally:
        conn.close()

    elapsed = time.monotonic() - t0
    bps  = (total * 8) / elapsed if elapsed > 0 else 0
    mbps = bps / 1_000_000
    print(f"[client] sent {total} bytes in {elapsed:.2f} s  ->  {bps:.0f} bps  ({mbps:.3f} Mbps)")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description='iperf peer for caOS example 44')
    ap.add_argument('--mode',    choices=['client', 'server'], required=True)
    ap.add_argument('--iface',   default='enp7s0',       help='Ethernet interface')
    ap.add_argument('--pc-ip',   default='192.168.10.1', help='PC IP on the link')
    ap.add_argument('--port',    type=int, default=5001)
    ap.add_argument('--dur',     type=int, default=10,
                    help='transfer duration in seconds (mode=client only)')
    ap.add_argument('--no-dhcp', action='store_true',    help='skip DHCP/IP setup')
    # With --no-dhcp this script does not run the DHCP server, so it cannot
    # learn the board's address from the leases - and mode=client needs it.
    # The error message already told the user to pass --board-ip; the option
    # itself was missing. ex79's peer has it (iperf_peer_ncm.py:377).
    ap.add_argument('--board-ip', default=None,
                    help='board IP (skips DHCP discovery)')
    args = ap.parse_args()

    # Root is needed for dnsmasq and for configuring the interface - and for
    # nothing else. With --no-dhcp this script does neither: it binds an
    # unprivileged port and talks to a board that already has its address, so
    # demanding root there refused to run for a reason that did not apply.
    # The rest of main() already honours --no-dhcp (the interface setup right
    # below is skipped); only this check did not. Same fix as ex79's peer,
    # which had the identical defect.
    if os.geteuid() != 0 and not args.no_dhcp:
        print("[!] Requires root (dnsmasq and interface setup).")
        print("    Use --no-dhcp when the address and the DHCP server are")
        print("    already arranged - the test bench does that itself.")
        sys.exit(1)

    board_ip = args.board_ip

    if not args.no_dhcp:
        setup_interface(args.iface, args.pc_ip)
        start_dnsmasq(args.iface, args.pc_ip)
        board_ip = wait_board_ip(60)
        if board_ip is None:
            print("[!] The board got no IP - check the cable and retry.")
            stop_dnsmasq()
            sys.exit(1)
        print(f"[*] Board IP: {board_ip}")
    else:
        print("[*] --no-dhcp: skipping DHCP setup")

    print(f"\n{'='*50}")
    print(f"  iperf peer  mode={args.mode}  port={args.port}")
    print(f"  PC IP: {args.pc_ip}")
    if board_ip:
        print(f"  Board IP: {board_ip}")
    print(f"{'='*50}\n")

    try:
        if args.mode == 'server':
            print(f"[*] Now on the board console:  iperf client {args.pc_ip} {args.port}\n")
            run_server(args.pc_ip, args.port)
        else:
            if board_ip is None:
                print("[!] mode=client needs the board IP (from DHCP, or --no-dhcp plus --board-ip)")
                sys.exit(1)
            print(f"[*] Now on the board console:  iperf server {args.port}\n")
            run_client(board_ip, args.port, args.dur)
    finally:
        if not args.no_dhcp:
            stop_dnsmasq()


if __name__ == '__main__':
    main()
