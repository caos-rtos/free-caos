#!/usr/bin/env python3
"""
msink_peer.py - PC peer for caOS example 85, parallel TCP sink

Opens several TCP connections at once against a board that is listening, and
reports each stream plus their SUM. What it measures is not how fast the board
is - example 44 does that with one connection - but how it SHARES one buffer
pool between connections that compete for it.

Usage:
  sudo python3 msink_peer.py --parallel 4 [--iface enp7s0] [--pc-ip 192.168.10.1]
                             [--port 5001] [--dur 10]

  sudo python3 msink_peer.py --parallel 4 --no-dhcp --board-ip 192.168.10.80

  --parallel N  connections to open at once (1 is a plain single-stream run)
  --iface NAME  the wired interface (it asks, and lists them, if the name is wrong)
  --no-dhcp     skip DHCP/IP setup (address already arranged, as the bench does)

ORDER OF OPERATIONS

  1. start THIS script - it brings up the DHCP server
  2. wait for it to print the board's address
  3. THEN type `msink start` on the board console

Not the other way round: without this script running there is no DHCP server,
so the board has no address, and `msink start` refuses to listen without one.
The streams retry the connection for 60 s, which is what leaves time for
step 3.

READ THE SUM, NOT THE STREAMS

Expect each stream to be slower than a single-connection run and their TOTAL to
stay close to it: the limit is the per-packet cost of the stack, which is shared
however many connections there are. A per-stream figure near the single-stream
rate would mean the board is doing N times the work, and should be disbelieved
before it is celebrated.

Dependencies:
  apt install dnsmasq
"""

import argparse
import os
import signal
import socket
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------------------
# DHCP / network
# ---------------------------------------------------------------------------
DHCP_START = "192.168.10.100"
DHCP_END   = "192.168.10.200"
MASK       = "255.255.255.0"
DHCP_CONF  = "/tmp/dnsmasq-caos85.conf"
DHCP_PID   = "/tmp/dnsmasq-caos85.pid"
DHCP_LOG   = "/tmp/dnsmasq-caos85.log"

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


def iface_exists(iface):
    return os.path.isdir(f"/sys/class/net/{iface}")


def pick_iface(iface):
    """Return an interface that exists, asking if the requested one does not.

    The default is whatever the machine it was written on happened to call its
    Ethernet port, and that name does not travel: enp7s0 here, enp0s31f6 there.
    Failing with 'Cannot find device' and a Python traceback tells the user
    nothing about which names ARE available, so list the wired candidates and
    let them choose.
    """
    if iface_exists(iface):
        return iface

    print(f"[!] interface '{iface}' does not exist on this machine")
    try:
        names = sorted(os.listdir("/sys/class/net"))
    except OSError:
        names = []
    # Wireless ports have a wireless/ subdirectory; the board is on a cable.
    # Bridges are skipped too: lxcbr0 and friends are wired by that test but
    # are not where a NUCLEO is plugged in.
    wired = [n for n in names
             if n != "lo"
             and not os.path.isdir(f"/sys/class/net/{n}/wireless")
             and not os.path.isdir(f"/sys/class/net/{n}/bridge")]
    if not wired:
        print("[!] no wired interface found at all")
        sys.exit(1)

    if len(wired) == 1:
        # Nothing to choose between: take it rather than make the user type
        # the only possible answer. bench_net.sh detect_iface() does the same.
        print(f"[*] using the only wired interface: {wired[0]}")
        return wired[0]

    print("    available:")
    for i, n in enumerate(wired):
        cidr = get_iface_cidr(n) or "no address"
        print(f"      {i}) {n:<16} {cidr}")

    try:
        ans = input("    which one? (number or name, empty to abort): ").strip()
    except EOFError:
        ans = ""
    if not ans:
        print(f"[!] aborted - rerun with --iface <name>")
        sys.exit(1)
    if ans.isdigit() and int(ans) < len(wired):
        return wired[int(ans)]
    if ans in wired:
        return ans
    print(f"[!] '{ans}' is not one of the interfaces listed")
    sys.exit(1)


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

    subprocess.run(["pkill", "-f", "dnsmasq-caos85"], stderr=subprocess.DEVNULL)
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
        subprocess.run(["pkill", "-f", "dnsmasq-caos85"], stderr=subprocess.DEVNULL)
    for p in [DHCP_PID, DHCP_CONF]:
        try:
            os.remove(p)
        except Exception:
            pass


def rogue_dhcp_servers(iface, pc_ip):
    """Other dnsmasq instances that could answer the board before this one.

    A DHCP server left running by an earlier test serves the lease itself, and
    then nothing appears in THIS script's log however long it waits - the board
    is on the network and the peer reports 'no IP'. `pkill -f dnsmasq-caos85`
    only ever kills our own, by design, so the survivor has to be found and
    named rather than silently competed with.
    """
    try:
        out = subprocess.check_output(["pgrep", "-a", "dnsmasq"],
                                      stderr=subprocess.DEVNULL).decode()
    except Exception:
        return []
    # Only the ones that could answer OUR board: same interface, or a pool on
    # our /24. An lxcbr0 instance serving 10.0.3.x is not a competitor, and
    # naming it would cry wolf on a machine that runs containers.
    prefix = pc_ip.rsplit(".", 1)[0] + "."
    rogue = []
    for line in out.splitlines():
        line = line.strip()
        if not line or "dnsmasq-caos85" in line:
            continue                      # ours
        if iface in line or prefix in line:
            rogue.append(line)
    return rogue


def board_ip_from_neigh(pc_ip):
    """The board's address from the kernel's neighbour table.

    Second way of answering the same question, for when the lease came from
    somewhere else: anything on our /24 that is not us has to be the board,
    because this link has exactly two ends.
    """
    prefix = pc_ip.rsplit(".", 1)[0] + "."
    try:
        out = subprocess.check_output(["ip", "-4", "neigh", "show"],
                                      stderr=subprocess.DEVNULL).decode()
    except Exception:
        return None
    for line in out.splitlines():
        parts = line.split()
        if not parts:
            continue
        ip = parts[0]
        if not ip.startswith(prefix) or ip == pc_ip:
            continue
        if "FAILED" in line or "INCOMPLETE" in line:
            continue
        return ip
    return None


def wait_board_ip(iface, pc_ip, timeout_s=60):
    print(f"[*] Waiting for DHCP ACK from the board (max {timeout_s}s)...")
    warned = False
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

        # Our log is silent. Before concluding the board is absent, check
        # whether it is simply on the network with a lease somebody else gave
        # it - which is what happens when a previous test left its DHCP server
        # running (see project_ex82_two_dhcp_servers: two servers on one /24
        # NAK each other, and the state of the PC afterwards is not the moment
        # of the fault).
        if i >= 4 and not warned:
            rogue = rogue_dhcp_servers(iface, pc_ip)
            if rogue:
                print()
                print("[!] another DHCP server is running and may have answered first:")
                for r in rogue:
                    print(f"      {r}")
                print("[!] kill it, or the two will NAK each other on this /24")
                warned = True

        if i >= 4:
            ip = board_ip_from_neigh(pc_ip)
            if ip:
                print()
                print(f"[*] the board is at {ip}, from the neighbour table - "
                      f"its lease did NOT come from this script")
                return ip

        print(f"\r[*] Waiting for DHCP... {i+1}s", end="", flush=True)
    print()
    return None


# ---------------------------------------------------------------------------
# client: the PC connects to the board, which is listening (msink start)
# ---------------------------------------------------------------------------
def run_client(board_ip, port, dur, connect_timeout=60, tag=None, result=None):
    """Connect to the board, which must already be listening.

    The board starts listening only when the user types "msink start" on its
    console, which is possible only AFTER DHCP gave it an IP: so retry here while
    the listener is missing, instead of failing on the first attempt.

    With `tag` set this is one stream of a parallel run: the tag goes in every
    line so the streams can be told apart, and the totals are handed back
    through `result` for the caller to sum.
    """
    label = "client" if tag is None else f"client {tag}"
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
                print(f"[{label}] cannot connect to {board_ip}:{port}: {e}")
                print(f"[{label}] is the board listening? type 'msink start' on its console.")
                return
            if not announced:
                print(f"[{label}] {board_ip}:{port} not answering yet - "
                      f"type 'msink start' on the board console (waiting {connect_timeout}s)")
                announced = True
            time.sleep(1.0)

    print(f"[{label}] connected to {board_ip}:{port}  duration={dur} s")

    total = 0
    t0 = time.monotonic()
    deadline = t0 + dur
    try:
        while time.monotonic() < deadline:
            sent = conn.send(buf)
            total += sent
    except Exception as e:
        print(f"[{label}] send error: {e}")
    finally:
        conn.close()

    t1 = time.monotonic()
    elapsed = t1 - t0
    bps  = (total * 8) / elapsed if elapsed > 0 else 0
    mbps = bps / 1_000_000
    print(f"[{label}] sent {total} bytes in {elapsed:.2f} s  ->  {bps:.0f} bps  ({mbps:.3f} Mbps)")
    if result is not None:
        result["bytes"] = total
        result["elapsed"] = elapsed
        result["bps"] = bps
        # Absolute instants, so the caller can work out the window in which the
        # streams were ACTUALLY transferring together. Each stream spends an
        # unpredictable while retrying the connection first, and counting that
        # as transfer time understates the total by exactly that much.
        result["t0"] = t0
        result["t1"] = t1


def run_client_parallel(board_ip, port, dur, streams, connect_timeout=60):
    """Several client streams at once, against a board that is listening.

    WHAT THIS IS FOR

    One stream measures how fast the board can take a transfer. Several
    measure something different: how the receive buffer pool is SHARED. The
    pool is one (NET_BUF_COUNT) and every connection's receive ring holds
    buffers out of it, so four connections at full ring depth would ask for
    more than exists - which is why the board hands out a per-connection quota
    computed at runtime rather than a fixed ring depth.

    What this checks is that the quota does its job: the streams should slow
    down individually while their SUM stays close to what one stream alone
    achieves, because the limit is the per-packet cost of the stack, which is
    shared. A sum that collapses means the sharing costs more than it saves; a
    stream that stalls at zero means one connection starved the others.

    All threads are started before any of them transfers, so they overlap for
    real instead of running one after another.
    """
    results = [dict() for _ in range(streams)]
    threads = []
    for i in range(streams):
        t = threading.Thread(target=run_client,
                             args=(board_ip, port, dur, connect_timeout),
                             kwargs={'tag': i + 1, 'result': results[i]},
                             daemon=True)
        threads.append(t)

    print(f"[*] {streams} parallel streams, {dur} s each")
    t0 = time.monotonic()
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=connect_timeout + dur + 30)
    done  = [r for r in results if r.get('bytes')]
    total = sum(r['bytes'] for r in done)

    # The window is from the FIRST byte to the LAST, not from when this
    # function was entered: the threads spend an unbounded time retrying the
    # connection while the user types `msink start`, and including that in the
    # divisor is how a 95 Mbps total gets reported as 43. Nor is it the sum of
    # the per-stream durations - they overlap, and adding them would divide by
    # about N times too much.
    if done:
        span = max(r['t1'] for r in done) - min(r['t0'] for r in done)
    else:
        span = 0.0
    bps = (total * 8) / span if span > 0 else 0

    # How much of that window every stream was actually running. Below ~90% the
    # streams did not really compete, and the total is a sum of things that
    # partly took turns - which is not what this measures.
    overlap = 0.0
    if done and span > 0:
        lo = max(r['t0'] for r in done)
        hi = min(r['t1'] for r in done)
        overlap = (hi - lo) / span if hi > lo else 0.0

    print(f"\n[parallel] {len(done)}/{streams} streams, transferring for "
          f"{span:.2f} s, overlap {overlap*100:.0f}%")
    for i, r in enumerate(results):
        if r.get('bytes'):
            print(f"[parallel]   stream {i+1}: {r['bytes']} bytes  "
                  f"{r['bps']/1e6:.3f} Mbps")
        else:
            print(f"[parallel]   stream {i+1}: NO DATA (never connected, "
                  f"or the board refused it)")
    print(f"[parallel] TOTAL {total} bytes  ->  {bps:.0f} bps  "
          f"({bps/1e6:.3f} Mbps)")
    if done and overlap < 0.9:
        print(f"[parallel] WARNING: overlap {overlap*100:.0f}% - the streams "
              f"only partly ran together, so the total is not a measure of "
              f"contention")
    return len(done), total, bps


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description='parallel TCP sink peer for caOS example 85')
    # The board is always the RECEIVER in this example (msink is a sink), so
    # there is no server mode to choose: the PC sends. Kept as an option only
    # so the flag can still be passed by habit from ex44's peer.
    ap.add_argument('--mode',    choices=['client'], default='client',
                    help=argparse.SUPPRESS)
    ap.add_argument('--iface',   default='enp7s0',       help='Ethernet interface')
    ap.add_argument('--pc-ip',   default='192.168.10.1', help='PC IP on the link')
    ap.add_argument('--port',    type=int, default=5001)
    ap.add_argument('--dur',     type=int, default=10,
                    help='transfer duration in seconds (mode=client only)')
    # 4 by default: this example exists to measure connections competing for
    # the pool, and a single stream is what example 44 already does.
    ap.add_argument('--parallel', type=int, default=4, metavar='N',
                    help='mode=client: open N connections at once instead of '
                         'one, to exercise how the board shares its buffer '
                         'pool between connections')
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
        iface = pick_iface(args.iface)
        setup_interface(iface, args.pc_ip)
        start_dnsmasq(iface, args.pc_ip)
        board_ip = wait_board_ip(iface, args.pc_ip, 60)
        if board_ip is None:
            print("[!] The board got no IP - check the cable and retry.")
            stop_dnsmasq()
            sys.exit(1)
        print(f"[*] Board IP: {board_ip}")
    else:
        print("[*] --no-dhcp: skipping DHCP setup")

    print(f"\n{'='*50}")
    print(f"  msink peer  streams={args.parallel}  port={args.port}")
    print(f"  PC IP: {args.pc_ip}")
    if board_ip:
        print(f"  Board IP: {board_ip}")
    print(f"{'='*50}\n")

    try:
        if board_ip is None:
            print("[!] the board IP is needed (from DHCP, or --no-dhcp plus --board-ip)")
            sys.exit(1)

        # NOW, not before. The board cannot take a lease until this script is
        # running its DHCP server, so telling the user to type msink start
        # first asks for something that cannot be done: the board has no
        # address yet, and msink start refuses without one. The streams below
        # retry the connection for a full minute, which is what makes this
        # order work - the peer waits for the board, not the other way round.
        print(f"[*] NOW type on the board console:  msink start {args.port}")
        print(f"[*] (the streams retry for 60 s, so there is no hurry)\n")
        if args.parallel > 1:
            run_client_parallel(board_ip, args.port, args.dur, args.parallel)
        else:
            run_client(board_ip, args.port, args.dur)
    finally:
        if not args.no_dhcp:
            stop_dnsmasq()


if __name__ == '__main__':
    main()
