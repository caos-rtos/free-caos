#!/usr/bin/env python3
"""
http_test.py - PC test for caOS example 45, HTTP server

Usage:
  sudo python3 http_test.py [--iface enp7s0] [--pc-ip 192.168.10.1]
      Starts DHCP, waits for the board IP, opens a browser on http://<board_ip>/

  sudo python3 http_test.py --no-dhcp --board-ip 192.168.10.158
      Skips DHCP, opens the browser straight on the given IP

  python3 http_test.py --no-dhcp --board-ip 192.168.10.158 --cmd "top"
      Runs a single CLI command via POST and prints the answer (no browser)

Dependencies:
  apt install dnsmasq
"""

import argparse
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
DHCP_START = "192.168.10.100"
DHCP_END   = "192.168.10.200"
MASK       = "255.255.255.0"
DHCP_CONF  = "/tmp/dnsmasq-caos45.conf"
DHCP_PID   = "/tmp/dnsmasq-caos45.pid"
DHCP_LOG   = "/tmp/dnsmasq-caos45.log"
BOARD_MAC  = "02:00:00:01:02:45"   # eth_cfg.mac in ca_os_ex45.c

_dnsmasq_proc = None


# ---------------------------------------------------------------------------
# Interface / DHCP
# ---------------------------------------------------------------------------
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
    current = get_iface_cidr(iface)
    if current and current.split("/")[0] == pc_ip:
        print(f"[*] {iface} already on {pc_ip}")
        return
    if current:
        print(f"[*] {iface} has {current} - replacing with {pc_ip}/24")
    subprocess.run(["ip", "addr", "flush", "dev", iface], stderr=subprocess.DEVNULL)
    subprocess.run(["ip", "addr", "add", f"{pc_ip}/24", "dev", iface], check=True)
    subprocess.run(["ip", "link", "set", iface, "up"], check=True)
    print(f"[*] {iface} configured: {pc_ip}/24")


def start_dhcp(iface, pc_ip):
    global _dnsmasq_proc
    stop_dhcp()

    conf = (
        f"interface={iface}\n"
        f"dhcp-range={DHCP_START},{DHCP_END},{MASK},120s\n"
        f"dhcp-option=3,{pc_ip}\n"          # gateway = PC
        f"dhcp-option=6,{pc_ip}\n"          # DNS = PC
        f"log-dhcp\n"
        f"log-facility={DHCP_LOG}\n"
        f"pid-file={DHCP_PID}\n"
        f"no-resolv\n"
        f"bind-interfaces\n"
    )
    with open(DHCP_CONF, "w") as f:
        f.write(conf)

    for logf in (DHCP_LOG, DHCP_PID):
        try:
            os.remove(logf)
        except FileNotFoundError:
            pass

    _dnsmasq_proc = subprocess.Popen(
        ["dnsmasq", "--no-daemon", f"--conf-file={DHCP_CONF}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("[*] dnsmasq started")


def stop_dhcp():
    global _dnsmasq_proc
    if _dnsmasq_proc:
        _dnsmasq_proc.terminate()
        try:
            _dnsmasq_proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            _dnsmasq_proc.kill()
        _dnsmasq_proc = None
    # safety killall
    subprocess.run(["killall", "-q", "dnsmasq"], stderr=subprocess.DEVNULL)
    for f in (DHCP_CONF, DHCP_PID):
        try:
            os.remove(f)
        except FileNotFoundError:
            pass


def wait_board_ip(mac, timeout=60):
    """Waits for dnsmasq to assign an IP to the given MAC.
    Reads the dnsmasq log looking for 'DHCPACK(<iface>) <ip> <mac>'."""
    mac_lo = mac.lower()
    print(f"[*] Waiting for DHCP for MAC {mac} (max {timeout}s)...")
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(DHCP_LOG) as lf:
                for line in lf:
                    if "DHCPACK" in line and mac_lo in line.lower():
                        parts = line.split()
                        for i, p in enumerate(parts):
                            if "DHCPACK" in p and i + 1 < len(parts):
                                ip = parts[i + 1]
                                if ip.count(".") == 3:
                                    return ip
        except FileNotFoundError:
            pass
        time.sleep(1)
        print(".", end="", flush=True)
    print()
    return None


# ---------------------------------------------------------------------------
# HTTP helpers (stdlib, no dependencies)
# ---------------------------------------------------------------------------
def http_get(ip, path="/", timeout=5):
    url = f"http://{ip}{path}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read().decode("utf-8", errors="replace")
    except Exception as e:
        return f"ERROR: {e}"


def http_post_cli(ip, cmd, timeout=10):
    url  = f"http://{ip}/cli"
    data = urllib.parse.urlencode({"cmd": cmd}).encode()
    req  = urllib.request.Request(url, data=data,
                                  headers={"Content-Type":
                                           "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read().decode("utf-8", errors="replace")
    except Exception as e:
        return f"ERROR: {e}"


def wait_http_ready(ip, retries=20, delay=1):
    """Wait for the board HTTP server to answer."""
    print(f"[*] Waiting for HTTP on {ip}...", end="", flush=True)
    for _ in range(retries):
        try:
            urllib.request.urlopen(f"http://{ip}/", timeout=2)
            print(" OK")
            return True
        except Exception:
            print(".", end="", flush=True)
            time.sleep(delay)
    print(" TIMEOUT")
    return False


# ---------------------------------------------------------------------------
# Interactive CLI mode (fallback without a browser)
# ---------------------------------------------------------------------------
def interactive_cli(ip):
    print(f"\n[*] Interactive CLI mode - board {ip}")
    print("    (empty Enter = quit, Ctrl-C = quit)\n")
    while True:
        try:
            cmd = input("caOS> ").strip()
        except (KeyboardInterrupt, EOFError):
            print()
            break
        if not cmd:
            break
        resp = http_post_cli(ip, cmd)
        print(resp)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="PC test for caOS example 45 HTTP server")
    ap.add_argument("--iface",    default="enp7s0",       help="ethernet interface")
    ap.add_argument("--pc-ip",    default="192.168.10.1",  help="IP to assign to the PC")
    ap.add_argument("--board-ip", default=None,            help="board IP (skips DHCP)")
    ap.add_argument("--no-dhcp",  action="store_true",     help="do not start DHCP")
    ap.add_argument("--cmd",      default=None,
                    help="run a single CLI command and print the answer (no browser)")
    ap.add_argument("--no-browser", action="store_true",
                    help="do not open a browser, use the interactive CLI")
    args = ap.parse_args()

    # -- Determine the board IP -------------------------------------------
    board_ip = args.board_ip

    if not args.no_dhcp:
        if os.geteuid() != 0:
            print("ERROR: root required for DHCP (sudo)")
            sys.exit(1)
        setup_interface(args.iface, args.pc_ip)
        start_dhcp(args.iface, args.pc_ip)

        def _cleanup(sig=None, frame=None):
            print("\n[*] Cleanup...")
            stop_dhcp()
            sys.exit(0)

        signal.signal(signal.SIGINT,  _cleanup)
        signal.signal(signal.SIGTERM, _cleanup)

        if not board_ip:
            board_ip = wait_board_ip(BOARD_MAC)
            if not board_ip:
                print("ERROR: board not detected via DHCP")
                stop_dhcp()
                sys.exit(1)
            print(f"\n[+] Board IP: {board_ip}")

    if not board_ip:
        print("ERROR: pass --board-ip or drop --no-dhcp")
        sys.exit(1)

    # -- Single command mode ----------------------------------------------
    if args.cmd:
        if not wait_http_ready(board_ip):
            sys.exit(1)
        resp = http_post_cli(board_ip, args.cmd)
        print(resp)
        if not args.no_dhcp:
            stop_dhcp()
        return

    # -- Wait for HTTP to be ready ----------------------------------------
    if not wait_http_ready(board_ip):
        if not args.no_dhcp:
            stop_dhcp()
        sys.exit(1)

    url = f"http://{board_ip}/"
    print(f"[+] HTTP server: {url}")

    # -- Open a browser or the interactive CLI ----------------------------
    if args.no_browser:
        interactive_cli(board_ip)
    else:
        print(f"[*] Opening browser: {url}")
        webbrowser.open(url)
        print("[*] Press Ctrl-C to stop")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print()

    if not args.no_dhcp:
        stop_dhcp()


if __name__ == "__main__":
    main()
