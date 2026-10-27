#!/usr/bin/env python3
"""
caOS Example 40 - TCP server PC (HTTP/1.0) + DHCP server

Listens on TCP 8080, answers every GET with a text body and closes.
Starts a DHCP server (dnsmasq) to assign the board an IP.
Always sets --pc-ip on the interface; restores the original IP on exit.

Usage:
    sudo python3 tcp_server.py --iface enp7s0 [--port 8080] [--pc-ip 192.168.10.1]
    sudo python3 tcp_server.py --iface enp7s0 --no-dhcp   # TCP server only

Note:
    - Requires: dnsmasq, ip (iproute2)
    - Automatic cleanup on Ctrl+C or SIGTERM
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
DHCP_CONF  = "/tmp/dnsmasq-caos40.conf"
DHCP_PID   = "/tmp/dnsmasq-caos40.pid"
DHCP_LOG   = "/tmp/dnsmasq-caos40.log"

# --------------------------------------------------------------------------
# Interface
# --------------------------------------------------------------------------

def get_iface_cidr(iface):
    """Return the first IPv4 CIDR of the interface (e.g. '192.168.10.70/24'), or None."""
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
    """Always set pc_ip/24 on the interface; save the original CIDR for restore."""
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
        f"# dnsmasq caOS EX40\n"
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

    subprocess.run(["pkill", "-f", "dnsmasq-caos40"], stderr=subprocess.DEVNULL)
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
        subprocess.run(["pkill", "-f", "dnsmasq-caos40"], stderr=subprocess.DEVNULL)

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
            print(f"[*] {iface}: IP removed (it was absent before).")
    print("[*] Done.")

# --------------------------------------------------------------------------
# TCP handler
# --------------------------------------------------------------------------

def handle(conn, addr):
    ts = datetime.datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] connect from {addr[0]}:{addr[1]}")
    try:
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(256)
            if not chunk:
                break
            data += chunk
        request     = data.decode(errors="replace")
        first_line  = request.splitlines()[0] if request else ""
        print(f"  >> {first_line}")

        body = (
            f"caOS EX40 TCP server reply\r\n"
            f"time={datetime.datetime.now():%Y-%m-%d %H:%M:%S}\r\n"
            f"client={addr[0]}:{addr[1]}\r\n"
        )
        response = (
            f"HTTP/1.0 200 OK\r\n"
            f"Content-Type: text/plain\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
            f"{body}"
        )
        conn.sendall(response.encode())
        print(f"  << {len(response)} byte sent")
        time.sleep(0.5)   # give the client time to read before the FIN
    except Exception as e:
        print(f"  error: {e}")
    finally:
        conn.close()

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="caOS EX40 - TCP server + DHCP",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--iface",   type=str, default=None,
                        help="Network interface (e.g. enp7s0)")
    parser.add_argument("--port",    type=int, default=8080,
                        help="TCP port")
    parser.add_argument("--pc-ip",   type=str, default="192.168.10.1",
                        help="PC IP to configure on the interface")
    parser.add_argument("--no-dhcp", action="store_true",
                        help="Disable the DHCP server (TCP only)")
    args = parser.parse_args()

    with_dhcp    = bool(args.iface) and not args.no_dhcp
    bind_ip      = "0.0.0.0"
    actual_pc_ip = args.pc_ip

    if args.iface:
        if not iface_exists(args.iface):
            print(f"[!] Interface '{args.iface}' not found.")
            sys.exit(1)
        actual_pc_ip = setup_interface(args.iface, args.pc_ip)
        bind_ip = actual_pc_ip
        if with_dhcp:
            start_dnsmasq(args.iface, actual_pc_ip)
    elif not args.no_dhcp:
        print("[!] Pass --iface to start the DHCP server.")
        print("    Or use --no-dhcp for TCP on 0.0.0.0 only.")
        sys.exit(1)

    def _sig_handler(sig, frame):
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)
        sys.exit(0)

    signal.signal(signal.SIGINT,  _sig_handler)
    signal.signal(signal.SIGTERM, _sig_handler)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind_ip, args.port))
    srv.listen(4)

    print("=" * 60)
    print(f" caOS EX40 - TCP server  {bind_ip}:{args.port}")
    if with_dhcp:
        print(f" DHCP pool  {DHCP_START} - {DHCP_END}")
    print("=" * 60)
    print(" Ctrl+C to stop (automatic cleanup).")
    print("-" * 60)

    try:
        while True:
            conn, addr = srv.accept()
            handle(conn, addr)
    finally:
        srv.close()
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)


if __name__ == "__main__":
    main()
