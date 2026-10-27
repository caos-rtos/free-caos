#!/usr/bin/env python3
"""
caOS Example 74 - Syslog server PC (RFC 3164) + DHCP server, over USB-NCM

Usage:
    sudo python3 syslog_server_ncm.py                     # auto-detect usb0
    sudo python3 syslog_server_ncm.py --iface usb0        # explicit interface
    sudo python3 syslog_server_ncm.py --no-dhcp           # syslog only

Note:
    - Requires: dnsmasq, ip (iproute2)
    - Port 514 requires root privileges (sudo)
    - Always sets --pc-ip on the interface (restores the old IP on exit)
    - Automatic cleanup on Ctrl+C or SIGTERM

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

DHCP_START   = "192.168.10.100"
DHCP_END     = "192.168.10.200"
MASK         = "255.255.255.0"
DHCP_CONF    = "/tmp/dnsmasq-caos74.conf"
DHCP_PID     = "/tmp/dnsmasq-caos74.pid"
DHCP_LOG     = "/tmp/dnsmasq-caos74.log"

SEVERITIES = {
    0: "EMERG",  1: "ALERT",  2: "CRIT",   3: "ERR",
    4: "WARN",   5: "NOTICE", 6: "INFO",   7: "DEBUG",
}

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
                return s.split()[1]   # e.g. "192.168.10.70/24"
    except Exception:
        pass
    return None

def get_iface_ip(iface):
    """Return just the IP (no prefix), or None."""
    cidr = get_iface_cidr(iface)
    return cidr.split("/")[0] if cidr else None

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

_original_cidr  = None   # Pre-existing CIDR to restore (e.g. "192.168.10.70/24")
_iface_ip_added = False   # True if we changed the interface

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
        f"# dnsmasq caOS EX74\n"
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

    # Kill any leftover instance and remove old files
    subprocess.run(["pkill", "-f", "dnsmasq-caos74"], stderr=subprocess.DEVNULL)
    for f in [DHCP_PID, DHCP_LOG]:
        try:
            os.remove(f)
        except FileNotFoundError:
            pass
    time.sleep(0.4)

    subprocess.run(
        ["/usr/sbin/dnsmasq", "-C", DHCP_CONF,
         f"--pid-file={DHCP_PID}",
         f"--log-facility={DHCP_LOG}"],
        check=True)
    time.sleep(0.5)

    # Startup check
    if not os.path.exists(DHCP_PID):
        print(f"[!] dnsmasq did not start - see {DHCP_LOG}")
        _print_log(DHCP_LOG)
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
        subprocess.run(["pkill", "-f", "dnsmasq-caos74"], stderr=subprocess.DEVNULL)

def _print_log(path):
    try:
        with open(path) as f:
            print(f.read())
    except Exception:
        pass

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
        subprocess.run(
            ["ip", "addr", "flush", "dev", iface],
            stderr=subprocess.DEVNULL)
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
# Syslog parser
# --------------------------------------------------------------------------

def parse_pri(msg):
    if msg.startswith("<"):
        end = msg.find(">")
        if end > 0:
            try:
                pri = int(msg[1:end])
                return pri >> 3, pri & 0x07, msg[end + 1:]
            except ValueError:
                pass
    return None, None, msg

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="caOS EX74 - syslog UDP server + DHCP",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--iface",   type=str, default=None,
                        help="Network interface (default: auto-detect NCM)")
    parser.add_argument("--port",    type=int, default=514,
                        help="Syslog UDP port")
    parser.add_argument("--pc-ip",   type=str, default="192.168.10.1",
                        help="PC IP to configure on the interface if missing")
    parser.add_argument("--no-dhcp", action="store_true",
                        help="Disable the DHCP server (syslog only)")
    args = parser.parse_args()

    # Auto-detect the NCM interface when not given. Not for --no-dhcp:
    # there syslog listens on 0.0.0.0 and the interface is not needed.
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

    with_dhcp = bool(args.iface) and not args.no_dhcp
    bind_ip   = "0.0.0.0"
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
        print("    Or use --no-dhcp for syslog on 0.0.0.0 only.")
        sys.exit(1)

    # Signals for guaranteed cleanup

    # Socket syslog
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind_ip, args.port))

    print("=" * 60)
    print(f" caOS EX74 - syslog server  {bind_ip}:{args.port}")
    if with_dhcp:
        print(f" DHCP pool  {DHCP_START} - {DHCP_END}")
    print("=" * 60)
    print(" Ctrl+C to stop (automatic cleanup).")
    print("-" * 60)

    try:
        while True:
            data, addr = sock.recvfrom(2048)
            ts  = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            raw = data.decode("utf-8", errors="replace").rstrip("\r\n")
            fac, sev, text = parse_pri(raw)
            sev_str = SEVERITIES.get(sev, f"SEV{sev}") if sev is not None else "?"
            print(f"[{ts}] {addr[0]}:{addr[1]}  [{sev_str:6s}]  {text}")
    finally:
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)

if __name__ == "__main__":
    main()
