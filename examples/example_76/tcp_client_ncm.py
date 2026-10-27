#!/usr/bin/env python3
"""
caOS Example 76 - TCP echo client PC + DHCP server, over USB-NCM

Starts DHCP (dnsmasq), detects the IP assigned to the board, connects
to TCP port 7 (RFC 862 echo server) and sends the lines you type.

Here the roles are REVERSED compared to ex75: the board is the server, the PC the
client. DHCP is still the PC's job.

Usage:
    sudo python3 tcp_client_ncm.py                      # auto-detect usb0
    sudo python3 tcp_client_ncm.py --iface usb0         # explicit interface
    sudo python3 tcp_client_ncm.py --host 192.168.10.128 --no-dhcp

Note:
    - Requires: dnsmasq, ip (iproute2)
    - Automatic cleanup on Ctrl+C or SIGTERM
    - Type 'quit' to close the connection

POWER-UP ORDER: the BOARD first, then this script.
The USB interface does not exist until the device has enumerated: it is not like
eth0, which is always there. Without --iface the script looks it up by the
host-side MAC announced in the NCM descriptors (02:ca:05:*) and waits up to 30s
for it to appear.
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
DHCP_CONF  = "/tmp/dnsmasq-caos76.conf"
DHCP_PID   = "/tmp/dnsmasq-caos76.pid"
DHCP_LOG   = "/tmp/dnsmasq-caos76.log"

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

NCM_MAC_PREFIX = "02:ca:05"   # host-side MAC announced by the NCM descriptors


def find_ncm_iface(timeout_s=30):
    """Look up the USB-NCM interface by MAC, waiting for it to appear.

    The interface comes up with USB enumeration, so it may not be there yet when
    the script starts: retry for timeout_s seconds.
    """
    net_dir = "/sys/class/net"
    for _ in range(timeout_s):
        try:
            for name in os.listdir(net_dir):
                if name == "lo":
                    continue
                try:
                    with open(os.path.join(net_dir, name, "address")) as f:
                        if f.read().strip().lower().startswith(NCM_MAC_PREFIX):
                            return name
                except OSError:
                    continue
        except OSError:
            pass
        time.sleep(1)
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
        f"# dnsmasq caOS EX76\n"
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

    subprocess.run(["pkill", "-f", "dnsmasq-caos76"], stderr=subprocess.DEVNULL)
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
        subprocess.run(["pkill", "-f", "dnsmasq-caos76"], stderr=subprocess.DEVNULL)

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
        description="caOS EX76 - TCP echo client + DHCP",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--iface",   type=str, default=None,
                        help="Network interface (default: auto-detect NCM)")
    parser.add_argument("--host",    type=str, default=None,
                        help="board IP (alternative to --iface, skips DHCP)")
    parser.add_argument("--port",    type=int, default=7,
                        help="TCP echo server port on the board")
    parser.add_argument("--pc-ip",   type=str, default="192.168.10.1",
                        help="PC IP to configure on the interface")
    parser.add_argument("--no-dhcp", action="store_true",
                        help="Do not start dnsmasq (requires --host)")
    args = parser.parse_args()

    # Auto-detect the NCM interface when not given. Not for --no-dhcp:
    # there you pass --host directly and the interface is not needed.
    if args.iface is None and not args.no_dhcp:
        print(f"[*] Looking for the USB-NCM interface (MAC {NCM_MAC_PREFIX}:*)...")
        args.iface = find_ncm_iface()
        if args.iface is None:
            print("[!] No NCM interface found within 30s.")
            print("    Plug the USB cable and run the firmware, or")
            print("    pass the interface by hand:  sudo %s --iface <iface>"
                  % os.path.basename(sys.argv[0]))
            print("    (list them: ip -br link)")
            sys.exit(1)
        print(f"[+] NCM interface: {args.iface}")

    if not args.iface and not args.host:
        print("[!] Pass --iface (with DHCP) or --host <board_ip> --no-dhcp.")
        sys.exit(1)
    if args.no_dhcp and not args.host:
        print("[!] --no-dhcp requires --host <board_ip>.")
        sys.exit(1)

    with_dhcp    = bool(args.iface) and not args.no_dhcp
    actual_pc_ip = args.pc_ip

    # Handlers installed BEFORE touching interface and dnsmasq: if cleanup were
    # hooked only afterwards, a Ctrl+C (or an exception) during startup would
    # leave dnsmasq alive holding the IP, and the example
    # next one would fail with 'Address already in use'.
    def _sig_handler(sig, frame):
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)
        sys.exit(0)

    signal.signal(signal.SIGINT,  _sig_handler)
    signal.signal(signal.SIGTERM, _sig_handler)

    # Reap a leftover dnsmasq from a previous run that ended
    # badly: without this the IP stays taken and the bind fails with
    # 'Address already in use'.
    if with_dhcp:
        stop_dnsmasq()
    board_ip     = args.host

    if args.iface:
        if not iface_exists(args.iface):
            print(f"[!] Interface '{args.iface}' not found.")
            sys.exit(1)
        actual_pc_ip = setup_interface(args.iface, args.pc_ip)
        if with_dhcp:
            start_dnsmasq(args.iface, actual_pc_ip)


    # Detect the board IP when not given by hand
    if board_ip is None:
        board_ip = wait_board_ip(timeout_s=60)
        if board_ip is None:
            print("[!] No DHCP ACK in 60s. Enter the IP by hand:")
            board_ip = input("    IP board: ").strip()

    print("=" * 60)
    print(f" caOS EX76 - TCP echo client  ->  {board_ip}:{args.port}")
    print("=" * 60)

    try:
        run_echo(board_ip, args.port)
    finally:
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)


if __name__ == "__main__":
    main()
