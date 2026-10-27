#!/usr/bin/env python3
"""
caOS Example 41 - TCP echo client PC + DHCP server

Starts DHCP (dnsmasq), detects the IP assigned to the board, connects
to TCP port 7 (RFC 862 echo server) and sends the lines you type.

Usage:
    sudo python3 tcp_client.py --iface enp7s0 [--port 7] [--pc-ip 192.168.10.1]
    sudo python3 tcp_client.py --host 192.168.10.146 --no-dhcp [--port 7]

Note:
    - Requires: dnsmasq, ip (iproute2)
    - Automatic cleanup on Ctrl+C or SIGTERM
    - Type 'quit' to close the connection
"""

import argparse
import datetime
import os
import signal
import socket
import subprocess
import sys
import time

DHCP_START = "192.168.10.100"
DHCP_END   = "192.168.10.200"
MASK       = "255.255.255.0"
DHCP_CONF  = "/tmp/dnsmasq-caos41.conf"
DHCP_PID   = "/tmp/dnsmasq-caos41.pid"
DHCP_LOG   = "/tmp/dnsmasq-caos41.log"

# --------------------------------------------------------------------------
# Interface
# --------------------------------------------------------------------------

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

def iface_exists(iface):
    return subprocess.run(
        ["ip", "link", "show", iface],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0

# --------------------------------------------------------------------------
# DHCP (dnsmasq)
# --------------------------------------------------------------------------

_original_cidr  = None
_iface_ip_added = False

def setup_interface(iface, pc_ip):
    global _iface_ip_added, _original_cidr
    current_cidr = get_iface_cidr(iface)
    if current_cidr and current_cidr.split("/")[0] == pc_ip:
        print(f"[*] {iface} already configured with {pc_ip}")
        return pc_ip
    if current_cidr:
        _original_cidr = current_cidr
        print(f"[*] {iface} has {current_cidr} - replacing with {pc_ip}/24")
    subprocess.run(["ip", "addr", "flush", "dev", iface], stderr=subprocess.DEVNULL)
    subprocess.run(["ip", "addr", "add", f"{pc_ip}/24", "dev", iface], check=True)
    subprocess.run(["ip", "link", "set", iface, "up"], check=True)
    _iface_ip_added = True
    print(f"[*] {iface} configured -> {pc_ip}/24")
    return pc_ip

def start_dnsmasq(iface, pc_ip):
    conf = (
        f"# dnsmasq caOS EX41\n"
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

    subprocess.run(["pkill", "-f", "dnsmasq-caos41"], stderr=subprocess.DEVNULL)
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
    print(f"[*] DHCP log: tail -f {DHCP_LOG}")

def stop_dnsmasq():
    if os.path.exists(DHCP_PID):
        try:
            pid = int(open(DHCP_PID).read().strip())
            os.kill(pid, signal.SIGTERM)
            print(f"[*] dnsmasq stopped (pid {pid})")
        except Exception:
            pass
        for p in [DHCP_PID, DHCP_CONF]:
            try:
                os.remove(p)
            except Exception:
                pass
    else:
        subprocess.run(["pkill", "-f", "dnsmasq-caos41"], stderr=subprocess.DEVNULL)

def wait_board_ip(timeout_s=60):
    """Wait for the first DHCP ACK in the dnsmasq log and return the board IP."""
    print(f"[*] Waiting for DHCP ACK from the board (max {timeout_s}s)...")
    for i in range(timeout_s):
        time.sleep(1)
        try:
            log = open(DHCP_LOG).read()
            for line in log.splitlines():
                if "DHCPACK" in line:
                    # es: "DHCPACK(enp7s0) 192.168.10.146 02:00:..."
                    parts = line.split()
                    for p in parts:
                        if p.count(".") == 3 and not p.startswith("("):
                            return p
        except FileNotFoundError:
            pass
        print(f"\r[*] Waiting for DHCP... {i+1}s", end="", flush=True)
    print()
    return None

# --------------------------------------------------------------------------
# Global cleanup
# --------------------------------------------------------------------------

_cleanup_done = False

def cleanup(iface, pc_ip, with_dhcp):
    global _cleanup_done
    if _cleanup_done:
        return
    _cleanup_done = True
    print("\n[*] Cleanup...")
    if with_dhcp:
        stop_dnsmasq()
    if _iface_ip_added:
        subprocess.run(["ip", "addr", "flush", "dev", iface], stderr=subprocess.DEVNULL)
        if _original_cidr:
            subprocess.run(
                ["ip", "addr", "add", _original_cidr, "dev", iface],
                stderr=subprocess.DEVNULL)
            subprocess.run(["ip", "link", "set", iface, "up"], stderr=subprocess.DEVNULL)
            print(f"[*] {iface}: restored {_original_cidr}")
        else:
            print(f"[*] {iface}: IP removed.")
    print("[*] Done.")

# --------------------------------------------------------------------------
# Echo client
# --------------------------------------------------------------------------

def run_echo(host, port):
    print(f"[*] Connecting to {host}:{port} ...")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((host, port))
        s.settimeout(10.0)
        print(f"[*] Connected. Type text + Enter. 'quit' to exit.\n")

        while True:
            try:
                line = input("> ")
            except EOFError:
                break
            if line.strip().lower() == "quit":
                break

            s.sendall((line + "\n").encode())

            try:
                echo = s.recv(1024)
                ts   = datetime.datetime.now().strftime("%H:%M:%S")
                print(f"[{ts}] echo ({len(echo)}B): {echo.decode(errors='replace').rstrip()}")
            except socket.timeout:
                print("  timeout - no answer")

    print("[*] Connection closed.")

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="caOS EX41 - TCP echo client + DHCP",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--iface",   type=str, default=None,
                        help="network interface (e.g. enp7s0) for DHCP")
    parser.add_argument("--host",    type=str, default=None,
                        help="board IP (alternative to --iface, skips DHCP)")
    parser.add_argument("--port",    type=int, default=7,
                        help="TCP echo server port on the board")
    parser.add_argument("--pc-ip",   type=str, default="192.168.10.1",
                        help="PC IP to configure on the interface")
    parser.add_argument("--no-dhcp", action="store_true",
                        help="Do not start dnsmasq (requires --host)")
    args = parser.parse_args()

    if not args.iface and not args.host:
        print("[!] Pass --iface (with DHCP) or --host <board_ip> --no-dhcp.")
        sys.exit(1)
    if args.no_dhcp and not args.host:
        print("[!] --no-dhcp requires --host <board_ip>.")
        sys.exit(1)

    with_dhcp    = bool(args.iface) and not args.no_dhcp
    actual_pc_ip = args.pc_ip
    board_ip     = args.host

    if args.iface:
        if not iface_exists(args.iface):
            print(f"[!] Interface '{args.iface}' not found.")
            sys.exit(1)
        actual_pc_ip = setup_interface(args.iface, args.pc_ip)
        if with_dhcp:
            start_dnsmasq(args.iface, actual_pc_ip)

    def _sig_handler(sig, frame):
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)
        sys.exit(0)

    signal.signal(signal.SIGINT,  _sig_handler)
    signal.signal(signal.SIGTERM, _sig_handler)

    # Detect the board IP when not given by hand
    if board_ip is None:
        board_ip = wait_board_ip(timeout_s=60)
        if board_ip is None:
            print("[!] No DHCP ACK in 60s. Enter the IP by hand:")
            board_ip = input("    IP board: ").strip()

    print("=" * 60)
    print(f" caOS EX41 - TCP echo client  ->  {board_ip}:{args.port}")
    print("=" * 60)

    try:
        run_echo(board_ip, args.port)
    finally:
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)


if __name__ == "__main__":
    main()
