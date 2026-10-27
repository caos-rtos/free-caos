#!/usr/bin/env python3
# Proprietary - All rights reserved
# Copyright (c) 2026 Angelo Coppi
#
# Minimal DNS resolver for caOS ex37 (ETH) and ex72 (USB-NCM).
#
# WHY THIS FILE EXISTS SEPARATELY
#
# It used to live only inside test_dhcp_dns_ncm_pc.sh, written out to /tmp by
# a heredoc. That is fine for running the example by hand, but the test bench
# needs to start the resolver WITHOUT the rest of that script, which also
# reconfigures the interface, runs its own DHCP server and replaces the host's
# NTP daemon. The bench arranges all of those itself.
#
# So the resolver is extracted here and the bench starts it through
# bench_net.sh (port 53 is privileged). Behaviour is unchanged: static answers
# for the .caos.local names, everything else forwarded upstream.
#
# NOTE the heredoc it came from was UNQUOTED, so the shell collapsed every
# doubled backslash on its way to /tmp. Extracting the raw text without doing
# the same left b'\\xc0\\xa8...' - the 16-character text, not the four bytes of
# an address - and the server answered 92.120.99.48, which is that text read
# as ASCII.

#!/usr/bin/env python3
"""
Minimal DNS server for caOS EX72.
Static resolution for .caos.local names; forward to 8.8.4.4 for the others.
"""
import socket, struct, sys, os

DNS_PORT    = 53
UPSTREAM    = ('8.8.8.8', 53)
FORWARD_TO  = 8  # upstream timeout, seconds

# Static hostname -> IPv4 map
STATIC = {
    b'test.caos.local':    b'\xc0\xa8\x0a\x01',  # 192.168.10.1
    b'gateway.caos.local': b'\xc0\xa8\x0a\x01',  # 192.168.10.1
}

def parse_qname(data, off):
    """Reads a compressed DNS qname, returns (name_bytes, offset_after)."""
    labels = []
    visited = set()
    while True:
        if off >= len(data):
            break
        ln = data[off]
        if ln == 0:
            off += 1
            break
        if (ln & 0xC0) == 0xC0:          # compressed pointer
            ptr = ((ln & 0x3F) << 8) | data[off + 1]
            off += 2
            if ptr in visited:
                break
            visited.add(ptr)
            lbl, _ = parse_qname(data, ptr)
            labels.append(lbl)
            break
        off += 1
        labels.append(data[off:off + ln].lower())
        off += ln
    return b'.'.join(labels), off

def build_reply_a(request, ip4_bytes):
    """Builds a DNS A record answer from the original query."""
    txid   = request[:2]
    flags  = b'\x81\x80'               # QR=1 AA=0 RD=1 RA=1 RCODE=0
    qdcnt  = request[4:6]
    ancnt  = b'\x00\x01'
    nscnt  = b'\x00\x00'
    arcnt  = b'\x00\x00'
    header = txid + flags + qdcnt + ancnt + nscnt + arcnt

    # question section (copied from the request)
    qsec = request[12:]

    # answer: pointer to the question qname (0xC00C), type A, class IN, TTL 30s, rdata
    answer  = b'\xc0\x0c'             # pointer to the qname in the question section
    answer += b'\x00\x01'             # type A
    answer += b'\x00\x01'             # class IN
    answer += b'\x00\x00\x00\x1e'  # TTL 30
    answer += b'\x00\x04'             # rdlength = 4
    answer += ip4_bytes

    return header + qsec + answer

def build_nxdomain(request):
    """NXDOMAIN answer."""
    txid  = request[:2]
    flags = b'\x81\x83'               # QR=1 RCODE=3 (NXDOMAIN)
    rest  = request[4:]
    return txid + flags + rest

def forward_upstream(data, timeout=FORWARD_TO):
    """Sends the query to 8.8.4.4 and waits for the answer."""
    try:
        up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        up.settimeout(timeout)
        up.sendto(data, UPSTREAM)
        resp, _ = up.recvfrom(4096)
        up.close()
        return resp
    except OSError:
        return None

def run():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(('', DNS_PORT))
    except PermissionError:
        print(f"[!] Port {DNS_PORT} requires root (sudo)", flush=True)
        sys.exit(1)
    except OSError as e:
        print(f"[!] bind UDP {DNS_PORT} failed: {e}", flush=True)
        sys.exit(1)

    pid_file = os.environ.get('DNS_PID_FILE', '')
    if pid_file:
        with open(pid_file, 'w') as f:
            f.write(str(os.getpid()))

    print(f"[*] Python DNS server listening on UDP {DNS_PORT}", flush=True)
    print(f"    Static: {list(STATIC.keys())}", flush=True)
    print(f"    Upstream: {UPSTREAM[0]}", flush=True)

    while True:
        try:
            data, addr = sock.recvfrom(512)
        except OSError:
            break

        if len(data) < 12:
            continue

        # Queries only (QR=0) with at least 1 question
        flags_word = (data[2] << 8) | data[3]
        if (flags_word & 0x8000) != 0:
            continue                # an answer, ignore it
        qdcount = (data[4] << 8) | data[5]
        if qdcount == 0:
            continue

        qname, qoff = parse_qname(data, 12)
        if qoff + 4 > len(data):
            continue
        qtype  = (data[qoff] << 8) | data[qoff + 1]
        qclass = (data[qoff + 2] << 8) | data[qoff + 3]

        print(f"  DNS query: {qname.decode(errors='replace')} type={qtype} "
              f"from {addr[0]}:{addr[1]}", flush=True)

        if qtype == 1 and qclass == 1:      # A IN
            ip4 = STATIC.get(qname)
            if ip4:
                reply = build_reply_a(data, ip4)
                ip_str = '.'.join(str(b) for b in ip4)
                print(f"       -> {ip_str} (static)", flush=True)
            else:
                reply = forward_upstream(data)
                if reply:
                    print(f"       -> forwarded to {UPSTREAM[0]}", flush=True)
                else:
                    reply = build_nxdomain(data)
                    print(f"       -> NXDOMAIN (upstream unreachable)", flush=True)
        else:
            # Not A-IN: just forward it
            reply = forward_upstream(data)
            if not reply:
                reply = build_nxdomain(data)

        if reply:
            sock.sendto(reply, addr)

if __name__ == '__main__':
    run()
