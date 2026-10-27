#!/usr/bin/env python3
"""
iperf_peer_ncm.py - PC peer for caOS example 79, iperf TCP over USB-NCM

Usage:
  sudo python3 iperf_peer_ncm.py --mode server [--iface usb0] [--port 5001]
      PC listens, the board connects and sends -> measures RX throughput

  sudo python3 iperf_peer_ncm.py --mode client [--iface usb0] [--port 5001] [--dur 10]
      PC connects to the board (which listens) and sends -> measures TX throughput

  --no-dhcp          skip DHCP/IP setup (IP already configured by hand)
  --board-ip <ip>    skip board IP discovery

POWER-UP ORDER: the BOARD first, then this script.
Without --iface the USB-NCM interface is looked up by MAC 02:ca:05:* and awaited
for up to 30s: it appears with enumeration, it is not always there as eth0.

IF THE BOARD ALREADY HAS A LEASE: in BOUND state its DHCP client sleeps until T1
(one hour), so no new DHCPACK will show up in the log - which is cleared on every
dnsmasq start. The script notices and after 5s looks for the board via ARP;
alternatively pass --board-ip, or type 'dhcp renew' on the board console WHILE
the script is waiting.

MANDATORY ORDER: this script FIRST, THEN the command on the board. It is a
circular dependency: the board accepts 'iperf' commands only AFTER it got its
IP from DHCP, which runs here. Issuing it earlier yields "iperf: local IP not
available". This script waits for the board instead of failing right away:
  --mode client -> retries the connection for 60s, while you type
                   'iperf server 5001 30' on the board console
                   (the idle default is 3 s, too tight: pass 30)
  --mode server -> keeps listening, then on the board 'iperf client <pc-ip> 5001'

MIND THE NUMBERS: the total printed by the CLIENT is what the LOCAL kernel
accepted with send(), not what reached the destination -- bytes still queued at
close time are dropped. The good measurement is ALWAYS the one from the RECEIVING
side. Over USB Full Speed (12 Mbit/s raw signalling) do not expect ETH figures.

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
DHCP_CONF  = "/tmp/dnsmasq-caos79.conf"
DHCP_PID   = "/tmp/dnsmasq-caos79.pid"
DHCP_LOG   = "/tmp/dnsmasq-caos79.log"

CHUNK = 65536


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


def warn_conflicting_routes(iface, pc_ip):
    """Report other interfaces holding a route on OUR subnet.

    A real trap, not a theoretical one: the ETH examples (ex44) and this one both
    use 192.168.10.0/24. If the wired NIC keeps its address from an earlier test,
    the kernel has TWO routes for the same network and picks the one with the
    lower metric -- usually the Ethernet, where the board is not. Packets leave
    through the wrong interface and the connection TIMES OUT (not 'connection
    refused': nothing comes back at all).
    """
    net = ".".join(pc_ip.split(".")[:3]) + ".0"
    try:
        out = subprocess.check_output(["ip", "-4", "route", "show"],
                                      stderr=subprocess.DEVNULL).decode()
    except Exception:
        return
    others = []
    for line in out.splitlines():
        if line.startswith(net + "/") and " dev " in line:
            dev = line.split(" dev ")[1].split()[0]
            if dev != iface:
                others.append((dev, line.strip()))
    if not others:
        return
    print()
    print("[!] WARNING: other interfaces hold a route on %s/24:" % net)
    for dev, line in others:
        print(f"      {line}")
    print("    The kernel may route the board packets THERE instead of")
    print("    through %s, and the connection would time out." % iface)
    print("    Fix:  sudo ip addr flush dev %s" % others[0][0])
    print()


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

    subprocess.run(["pkill", "-f", "dnsmasq-caos79"], stderr=subprocess.DEVNULL)
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
        subprocess.run(["pkill", "-f", "dnsmasq-caos79"], stderr=subprocess.DEVNULL)
    for p in [DHCP_PID, DHCP_CONF]:
        try:
            os.remove(p)
        except Exception:
            pass


def _ip_from_arp(iface):
    """Look up the board IP in the kernel ARP/neighbour table.

    Needed when the board ALREADY has a lease: in BOUND state the DHCP client
    sleeps until T1 (one hour), so it sends nothing and no new DHCPACK will ever
    show up in the log -- which start_dnsmasq() just cleared. But the board does
    answer ARP, and its MAC starts with 02:ca:05.
    """
    try:
        out = subprocess.check_output(["ip", "neigh", "show", "dev", iface],
                                      stderr=subprocess.DEVNULL).decode()
    except Exception:
        return None
    for line in out.splitlines():
        if NCM_MAC_PREFIX in line.lower():
            ip = line.split()[0]
            if ip.count(".") == 3:
                return ip
    return None


def wait_board_ip(timeout_s=60, iface=None):
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

        # Fallback: the board may already hold a lease from an earlier session
        # and will not ask for anything until T1. After a few seconds (time for
        # a DORA in progress, if any) look it up via ARP.
        if i >= 5 and iface:
            ip = _ip_from_arp(iface)
            if ip:
                print(f"\n[*] Board found via ARP: {ip} (pre-existing lease)")
                return ip

        print(f"\r[*] Waiting for DHCP... {i+1}s", end="", flush=True)
    print()
    print("[!] No lease detected. If the board already has an IP, give it a")
    print("    'dhcp renew' on its console WHILE this script is waiting,")
    print("    or pass the address by hand with --board-ip <ip>.")
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
# iperf client: the PC connects to the listening board
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
    print("[client] NB: 'sent' = bytes accepted by the LOCAL kernel via send(),")
    print("         not bytes that reached the destination: those still queued at")
    print("         close time are dropped. The valid measurement is the one the")
    print("         BOARD prints ('iperf RESULT'), counting what it actually")
    print("         received. If the two numbers diverge a lot, check")
    print("         'tcp buf' on the board: rx_drop=0 means the")
    print("         missing bytes never left this side.")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description='iperf peer for caOS example 79 (USB-NCM)')
    ap.add_argument('--mode',    choices=['client', 'server'], required=True)
    ap.add_argument('--iface',   default=None,           help='interface (default: auto-detect NCM)')
    ap.add_argument('--pc-ip',   default='192.168.10.1', help='PC IP on the link')
    ap.add_argument('--port',    type=int, default=5001)
    ap.add_argument('--dur',     type=int, default=10,
                    help='transfer duration in seconds (mode=client only)')
    ap.add_argument('--no-dhcp', action='store_true',    help='skip DHCP/IP setup')
    ap.add_argument('--board-ip', default=None,
                    help='board IP (skips DHCP/ARP discovery)')
    args = ap.parse_args()

    # Root is needed for dnsmasq and for configuring the interface - and for
    # nothing else. With --no-dhcp this script does neither: it binds an
    # unprivileged port and talks to a board that already has its address, so
    # demanding root there refused to run for a reason that did not apply.
    # The rest of main() already honours --no-dhcp (the interface lookup right
    # below is skipped); only this check did not.
    if os.geteuid() != 0 and not args.no_dhcp:
        print("[!] Requires root (dnsmasq and interface setup).")
        print("    Use --no-dhcp when the address and the DHCP server are")
        print("    already arranged - the test bench does that itself.")
        sys.exit(1)

    # Auto-detect the NCM interface when not given.
    if args.iface is None and not args.no_dhcp:
        print(f"[*] Looking for the USB-NCM interface (MAC {NCM_MAC_PREFIX}:*)...")
        args.iface = find_ncm_iface()
        if args.iface is None:
            print("[!] No NCM interface found within 30s.")
            print("    Plug the USB cable and run the ex79 firmware, or")
            print("    pass the interface by hand:  sudo %s --iface <iface>"
                  % os.path.basename(sys.argv[0]))
            print("    (list them: ip -br link)")
            sys.exit(1)
        print(f"[+] NCM interface: {args.iface}")

    # Handlers installed BEFORE starting dnsmasq: without them a Ctrl+C during
    # the lease wait (up to 60s) would skip cleanup and leave dnsmasq alive
    # holding the IP, making the next test fail with "Address already in use".
    def _sig_handler(sig, frame):
        if not args.no_dhcp:
            stop_dnsmasq()
        sys.exit(0)

    signal.signal(signal.SIGINT,  _sig_handler)
    signal.signal(signal.SIGTERM, _sig_handler)

    # Reap a leftover dnsmasq from an earlier run that ended badly.
    if not args.no_dhcp:
        stop_dnsmasq()

    board_ip = args.board_ip
    if board_ip:
        print(f"[*] Board IP forced from the command line: {board_ip}")

    if not args.no_dhcp and board_ip is None:
        setup_interface(args.iface, args.pc_ip)
        warn_conflicting_routes(args.iface, args.pc_ip)
        start_dnsmasq(args.iface, args.pc_ip)
        board_ip = wait_board_ip(60, args.iface)
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
