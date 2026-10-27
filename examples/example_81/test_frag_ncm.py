#!/usr/bin/env python3
"""
test_frag_ncm.py - mini DHCP server + IP fragmentation test for caOS example 81

Phases:
  1. Automatically sets server-ip on the interface and restores it on exit
  2. Mini DHCP server: handles a complete DORA cycle and assigns the board an IP
  3. Waits until the board is reachable (ARP reply -> gets the board MAC)
  4. Sends a 1472 B UDP datagram fragmented in 4 IP pieces via sendp() (L2 raw)
  5. Receives the "DEFRAG OK: len=1472 crc32=XXXXXXXX" answer and checks integrity

Requirements:
  pip install scapy
  sudo python3 examples/example_81/test_frag_ncm.py --iface enp7s0

Note: requires root privileges (raw socket, DHCP port 67/68).
"""

import argparse
import atexit
import math
import signal
import socket
import sys
import subprocess
import time
from typing import Optional

try:
    from scapy.all import (
        Ether, IP, UDP, BOOTP, DHCP, Raw, ARP,
        conf, send, sendp, sniff, srp1, fragment,
        get_if_hwaddr, get_if_addr,
    )
    HAS_SCAPY = True
except ImportError:
    HAS_SCAPY = False


# ---------------------------------------------------------------------------
# Default
# ---------------------------------------------------------------------------
DEFAULT_IFACE      = None   # None = auto-detect from the NCM MAC
NCM_MAC_PREFIX     = "02:ca:05"   # host-side MAC announced by the NCM descriptors
DEFAULT_SERVER_IP  = "192.168.10.1"
DEFAULT_CLIENT_IP  = "192.168.10.10"
DEFAULT_NETMASK    = "255.255.255.0"
DEFAULT_PORT       = 7961
DEFAULT_DATA_SIZE  = 1472   # UDP data bytes (IP payload = 1472 + 8 hdr = 1480)
DEFAULT_N_FRAGS    = 4
SPORT              = 54321


# ---------------------------------------------------------------------------
# USB-NCM interface
# ---------------------------------------------------------------------------
def find_ncm_iface(timeout_s: int = 30) -> Optional[str]:
    """Look up the USB-NCM interface by MAC, waiting for it to appear.

    The interface comes up with USB enumeration, so it may not be there yet when
    the script starts: retry for timeout_s seconds.
    """
    import os
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


DHCP_TIMEOUT_S     = 60
ARP_TIMEOUT_S      = 10
UDP_TIMEOUT_S      = 5.0
LEASE_TIME_S       = 120


# ---------------------------------------------------------------------------
# Payload e CRC
# ---------------------------------------------------------------------------
def make_payload(size: int) -> bytes:
    return bytes(i & 0xFF for i in range(size))

def crc32_stm32(data: bytes) -> int:
    """CRC-32/MPEG-2 with word-wise LE input - replicates ca_crc32_get() on STM32."""
    poly    = 0x04C11DB7
    crc     = 0xFFFFFFFF
    n_words = len(data) // 4
    tail    = len(data) % 4
    for i in range(n_words):
        word = int.from_bytes(data[i*4:i*4+4], 'little')
        crc ^= word
        for _ in range(32):
            crc = ((crc << 1) ^ poly) & 0xFFFFFFFF if crc & 0x80000000 \
                  else (crc << 1) & 0xFFFFFFFF
    if tail:
        last = int.from_bytes(data[n_words*4:], 'little')
        crc ^= last
        for _ in range(32):
            crc = ((crc << 1) ^ poly) & 0xFFFFFFFF if crc & 0x80000000 \
                  else (crc << 1) & 0xFFFFFFFF
    return crc


# ---------------------------------------------------------------------------
# Phase 0: configure the IP on the PC - adds it and restores on exit
# ---------------------------------------------------------------------------
_cleanup_args: Optional[tuple] = None   # (iface, cidr) to remove on exit

def _netmask_to_prefix(netmask: str) -> int:
    return sum(bin(int(x)).count('1') for x in netmask.split('.'))

def _cleanup() -> None:
    if _cleanup_args is None:
        return
    iface, cidr = _cleanup_args
    print(f"\nCleanup: removing {cidr} from {iface}...")
    subprocess.run(["ip", "addr", "del", cidr, "dev", iface],
                   capture_output=True)
    print("  OK")

def setup_pc_ip(iface: str, server_ip: str, netmask: str) -> None:
    global _cleanup_args
    prefix = _netmask_to_prefix(netmask)
    cidr   = f"{server_ip}/{prefix}"

    subprocess.run(["ip", "link", "set", iface, "up"], capture_output=True)

    r = subprocess.run(["ip", "addr", "add", cidr, "dev", iface],
                       capture_output=True, text=True)
    if r.returncode == 0:
        print(f"  IP configured: {cidr} on {iface}")
        _cleanup_args = (iface, cidr)
        atexit.register(_cleanup)
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, lambda s, f: sys.exit(0))
    elif "already" in r.stderr or "exists" in r.stderr:
        print(f"  IP {cidr} already present on {iface} - no change")
    else:
        print(f"  WARN ip addr add: {r.stderr.strip()}")


# ---------------------------------------------------------------------------
# Phase 1: mini DHCP server - one DORA cycle
# ---------------------------------------------------------------------------
def run_dhcp_server(iface: str, server_ip: str, client_ip: str,
                    netmask: str, timeout: int = DHCP_TIMEOUT_S) -> Optional[str]:
    """Handles DISCOVER->OFFER->REQUEST->ACK and returns the assigned IP or None."""
    conf.verb = 0
    assigned_ip   = [None]
    client_mac_b  = [None]
    my_mac        = get_if_hwaddr(iface)

    def on_dhcp(pkt):
        if not (BOOTP in pkt and DHCP in pkt):
            return

        # options is a mixed list: ('key', val) tuples or 'end'/'pad' strings
        opts = {}
        for opt in pkt[DHCP].options:
            if isinstance(opt, (list, tuple)) and len(opt) >= 2:
                opts[opt[0]] = opt[1]
        msg_type = opts.get('message-type', 0)

        if msg_type == 1:   # DISCOVER
            client_mac_b[0] = pkt[Ether].src
            xid = pkt[BOOTP].xid
            chaddr = pkt[BOOTP].chaddr
            print(f"  DHCP DISCOVER  client={client_mac_b[0]}")
            offer = (
                Ether(dst=client_mac_b[0], src=my_mac) /
                IP(src=server_ip, dst='255.255.255.255') /
                UDP(sport=67, dport=68) /
                BOOTP(op=2, yiaddr=client_ip, siaddr=server_ip,
                      giaddr='0.0.0.0', chaddr=chaddr, xid=xid) /
                DHCP(options=[
                    ('message-type', 'offer'),
                    ('server_id',    server_ip),
                    ('lease_time',   LEASE_TIME_S),
                    ('subnet_mask',  netmask),
                    ('router',       server_ip),
                    ('name_server',  '8.8.8.8'),
                    'end'
                ])
            )
            sendp(offer, iface=iface, verbose=False)
            print(f"  DHCP OFFER     -> {client_ip}")

        elif msg_type == 3:  # REQUEST
            xid    = pkt[BOOTP].xid
            chaddr = pkt[BOOTP].chaddr
            print(f"  DHCP REQUEST   client={client_mac_b[0]}")
            ack = (
                Ether(dst=client_mac_b[0], src=my_mac) /
                IP(src=server_ip, dst='255.255.255.255') /
                UDP(sport=67, dport=68) /
                BOOTP(op=2, yiaddr=client_ip, siaddr=server_ip,
                      giaddr='0.0.0.0', chaddr=chaddr, xid=xid) /
                DHCP(options=[
                    ('message-type', 'ack'),
                    ('server_id',    server_ip),
                    ('lease_time',   LEASE_TIME_S),
                    ('subnet_mask',  netmask),
                    ('router',       server_ip),
                    ('name_server',  '8.8.8.8'),
                    'end'
                ])
            )
            sendp(ack, iface=iface, verbose=False)
            print(f"  DHCP ACK       -> {client_ip}  (lease={LEASE_TIME_S}s)")
            assigned_ip[0] = client_ip

    sniff(
        iface=iface,
        filter='udp and (port 67 or port 68)',
        prn=on_dhcp,
        stop_filter=lambda p: assigned_ip[0] is not None,
        timeout=timeout,
        store=False,
    )
    return assigned_ip[0]


# ---------------------------------------------------------------------------
# Phase 2: wait for the ARP reply from the board, return its MAC or None
# srp1() is atomic: send + wait for the reply with no race condition
# ---------------------------------------------------------------------------
def wait_arp(iface: str, client_ip: str, server_ip: str,
             timeout: int = ARP_TIMEOUT_S) -> Optional[str]:
    conf.verb = 0
    my_mac   = get_if_hwaddr(iface)
    deadline = time.time() + timeout

    arp_req = (
        Ether(dst='ff:ff:ff:ff:ff:ff', src=my_mac) /
        ARP(op=1, hwsrc=my_mac, psrc=server_ip,
            hwdst='00:00:00:00:00:00', pdst=client_ip)
    )
    while time.time() < deadline:
        reply = srp1(arp_req, iface=iface, timeout=2, verbose=False)
        if reply and ARP in reply and reply[ARP].psrc == client_ip:
            return reply[ARP].hwsrc
        time.sleep(0.2)
    return None


# ---------------------------------------------------------------------------
# Phase 3: send fragments via sendp() (direct L2, no kernel routing)
# ---------------------------------------------------------------------------
def send_fragmented(iface: str, dst_ip: str, dst_mac: str,
                    port: int, payload: bytes, n_frags: int) -> None:
    conf.verb = 0
    my_mac     = get_if_hwaddr(iface)
    ip_payload = len(payload) + 8   # data + UDP header
    fragsize   = ((math.ceil(ip_payload / n_frags) + 7) // 8) * 8

    pkt   = IP(dst=dst_ip) / UDP(sport=SPORT, dport=port) / Raw(load=payload)
    frags = fragment(pkt, fragsize=fragsize)

    print(f"  Sending {len(frags)} fragments via {iface} -> {dst_mac} "
          f"(fragsize={fragsize}B, IP payload={ip_payload}B):")
    for i, f in enumerate(frags):
        ff   = f[IP].frag
        mf   = bool(f[IP].flags & 0x1)
        dlen = len(bytes(f[IP].payload))
        print(f"    [{i+1}/{len(frags)}] offset={ff*8:4d}B  "
              f"len={dlen:4d}B  MF={int(mf)}")
        # sendp = L2 raw, bypasses kernel routing - works even without a configured IP
        sendp(Ether(dst=dst_mac, src=my_mac) / f,
              iface=iface, verbose=False)
        time.sleep(0.005)


# ---------------------------------------------------------------------------
# Phase 4: receive the answer
# ---------------------------------------------------------------------------
def receive_reply(timeout: float) -> Optional[str]:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", SPORT))
    sock.settimeout(timeout)
    try:
        data, _ = sock.recvfrom(256)
        return data.decode("ascii", errors="replace").strip()
    except socket.timeout:
        return None
    finally:
        sock.close()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(
        description="mini DHCP server + IP fragment test for caOS example 81"
    )
    parser.add_argument("--iface",      default=DEFAULT_IFACE,
                        help="NCM interface (default: auto-detect from MAC 02:ca:05)")
    parser.add_argument("--server-ip",  default=DEFAULT_SERVER_IP,
                        help="PC IP (DHCP server)")
    parser.add_argument("--client-ip",  default=DEFAULT_CLIENT_IP,
                        help="IP to assign to the board")
    parser.add_argument("--netmask",    default=DEFAULT_NETMASK)
    parser.add_argument("--port",       default=DEFAULT_PORT,   type=int)
    parser.add_argument("--size",       default=DEFAULT_DATA_SIZE, type=int,
                        help="UDP data bytes (max 1472)")
    parser.add_argument("--frags",      default=DEFAULT_N_FRAGS, type=int,
                        help="Number of IP fragments")
    parser.add_argument("--skip-dhcp",  action="store_true",
                        help="Skip DHCP (the board already has IP --client-ip)")
    parser.add_argument("--repeat",     default=1, type=int)
    args = parser.parse_args()

    if not HAS_SCAPY:
        print("ERROR: scapy not installed.  pip install scapy", file=sys.stderr)
        return 1

    if args.size > 1472:
        print("ERROR: --size max 1472 (IP payload = size+8 <= 1480)", file=sys.stderr)
        return 1

    # The USB-NCM interface does not exist until the device has enumerated:
    # not like eth0, always there. ORDER: the BOARD first, then the script.
    if args.iface is None:
        print("Looking for the USB-NCM interface (MAC 02:ca:05:*)...")
        args.iface = find_ncm_iface()
        if args.iface is None:
            print("ERROR: no NCM interface found within 30s.",
                  file=sys.stderr)
            print("        Is the board powered and enumerated? (dmesg | tail)",
                  file=sys.stderr)
            return 1
        print(f"  NCM interface: {args.iface}")

    print("=== test_frag_ncm - caOS ip_defrag_service (example 81, USB-NCM) ===")
    print(f"  iface     : {args.iface}")
    print(f"  server IP : {args.server_ip}  (PC)")
    print(f"  client IP : {args.client_ip}  (board)")
    print(f"  UDP port  : {args.port}")
    print(f"  payload   : {args.size} B  ->  {args.frags} fragments")
    print()

    # --- Phase 0: IP on the PC (always - removed on exit) ---
    print("Phase 0: configuring the PC IP...")
    setup_pc_ip(args.iface, args.server_ip, args.netmask)

    # --- Phase 1: DHCP server ---
    client_ip = args.client_ip
    if not args.skip_dhcp:
        print(f"Phase 1: DHCP server listening on {args.iface} "
              f"(timeout {DHCP_TIMEOUT_S}s)...")
        print("        Power on or reset the board now.")
        ip = run_dhcp_server(args.iface, args.server_ip, args.client_ip,
                             args.netmask)
        if not ip:
            print("ERROR: DHCP timeout - no board completed DORA.")
            return 1
        client_ip = ip
        print(f"  Board got IP: {client_ip}")
        print()
        time.sleep(1.5)   # wait for the board to complete DHCP bound and the ARP service
    else:
        print(f"Phase 1: DHCP skipped - board IP = {client_ip}")

    # --- Phase 2: ARP alive check + fetch the board MAC ---
    print(f"Phase 2: checking {client_ip} reachability via ARP...")
    board_mac = wait_arp(args.iface, client_ip, args.server_ip)
    if board_mac is None:
        print(f"  ERROR: no ARP reply from {client_ip}.")
        print(f"  Check that the board completed DHCP and is UP.")
        return 1
    print(f"  ARP OK - board MAC={board_mac}")
    print()

    # --- Payload e CRC ---
    payload = make_payload(args.size)
    crc_pc  = crc32_stm32(payload)
    print(f"Payload: {args.size} B  crc32=0x{crc_pc:08X}  "
          f"(pattern 0x00..0xFF ciclico)")
    print()

    # --- Phase 3+4: test (possibly repeated) ---
    passed = 0
    failed = 0

    for run in range(args.repeat):
        if args.repeat > 1:
            print(f"--- Run {run+1}/{args.repeat} ---")

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("", SPORT))
        sock.settimeout(UDP_TIMEOUT_S)

        try:
            print(f"Phase 3: sending fragments -> {client_ip}:{args.port}")
            send_fragmented(args.iface, client_ip, board_mac,
                            args.port, payload, args.frags)

            print(f"Phase 4: waiting for the echo (timeout {UDP_TIMEOUT_S}s)...")
            try:
                rxdata, addr = sock.recvfrom(2048)
                print(f"  answer from {addr[0]}: {len(rxdata)} B")
                len_ok  = (len(rxdata) == args.size)
                data_ok = (rxdata == payload)
                ok = len_ok and data_ok
                print(f"  len  : board={len(rxdata)}  expected={args.size}  "
                      f"{'OK' if len_ok else '*** MISMATCH ***'}")
                if len_ok:
                    print(f"  data : {'OK (echo intact)' if data_ok else '*** MISMATCH ***'}")
                    if not data_ok:
                        for i, (a, b) in enumerate(zip(rxdata, payload)):
                            if a != b:
                                print(f"    first diff @ byte {i}: board=0x{a:02X}  expected=0x{b:02X}")
                                break
                print(f"  => {'PASS' if ok else 'FAIL'}")
                if ok:
                    passed += 1
                else:
                    failed += 1

            except socket.timeout:
                print(f"  TIMEOUT - no answer - FAIL")
                failed += 1
        finally:
            sock.close()

        if args.repeat > 1 and run < args.repeat - 1:
            time.sleep(0.5)

    if args.repeat > 1:
        print(f"\nRiepilogo: {passed}/{args.repeat} PASS, "
              f"{failed}/{args.repeat} FAIL")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
