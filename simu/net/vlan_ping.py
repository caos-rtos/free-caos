#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""ICMP echo to a caOS board inside an 802.1Q tag, and the replies counted.

caOS has no VLANs (a VLAN is made on the switch, with an untagged access
port). What it does take is the priority tag - VID 0, 802.1p priority only,
which some managed switches add when QoS is on: net.c strips it on entry. A
tag with a VLAN id is dropped and counted (rx_vlan in 'ip stats').

So: --vid 0 must be answered, any other VID must not. The board's MAC comes
from a plain ARP first; the replies are matched by ICMP id and printed as one
line the bench reads:

    vid=<v> sent=<n> replies=<r> tagged_replies=<t>

(a reply is expected untagged: the board never sends a tag).

Raw socket = root: on the bench it runs through
`sudo tools/bench_net.sh vlan-ping <ip> <vid> [count]`, which checks the
arguments.
"""
import argparse
import sys
import time

from scapy.all import (ARP, ICMP, IP, AsyncSniffer, Dot1Q, Ether, conf,
                       get_if_addr, get_if_hwaddr, sendp, srp1)

ICMP_ID = 0xCA05


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dst", required=True)
    ap.add_argument("--iface", required=True)
    ap.add_argument("--vid", type=int, required=True)
    ap.add_argument("--count", type=int, default=5)
    a = ap.parse_args()
    conf.verb = 0

    my_mac = get_if_hwaddr(a.iface)
    my_ip = get_if_addr(a.iface)
    r = srp1(Ether(dst="ff:ff:ff:ff:ff:ff", src=my_mac) / ARP(pdst=a.dst, psrc=my_ip),
             iface=a.iface, timeout=2, retry=2)
    if r is None or ARP not in r:
        print("vlan_ping: %s does not answer ARP on %s" % (a.dst, a.iface))
        return 2
    board_mac = r[ARP].hwsrc

    sn = AsyncSniffer(iface=a.iface, store=True,
                      lfilter=lambda p: ICMP in p and p[ICMP].type == 0 and
                      p[ICMP].id == ICMP_ID and IP in p and p[IP].src == a.dst)
    sn.start()
    time.sleep(0.3)
    for i in range(a.count):
        sendp(Ether(src=my_mac, dst=board_mac) / Dot1Q(prio=5, vlan=a.vid) /
              IP(src=my_ip, dst=a.dst) / ICMP(type=8, id=ICMP_ID, seq=i) /
              b"caos-vlan-ping", iface=a.iface)
        time.sleep(0.2)
    time.sleep(1.0)
    sn.stop()
    got = sn.results or []
    seqs = set(p[ICMP].seq for p in got)
    tagged = sum(1 for p in got if Dot1Q in p)
    print("vid=%d sent=%d replies=%d tagged_replies=%d" % (a.vid, a.count, len(seqs), tagged))
    return 0


if __name__ == "__main__":
    sys.exit(main())
