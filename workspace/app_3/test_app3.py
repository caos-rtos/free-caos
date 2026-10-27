#!/usr/bin/env python3
"""Bench test of workspace/app_3: the encrypted L3 router between TWO boards.

The hw_test bench drives one board per class; app_3 needs two at once, so
this script drives them directly: one console per ST-LINK (caos_shell with
CAOS_TTY), the two PCs in namespaces (bench_net.sh ns-router), iperf2 inside
them.

    any two of H753, H563, F767 (--boards); the roles are negotiated by the
    boards on the cable at boot, the script only finds out which is which:
    board 1 = eth .50.100, ncm .10.100          PC A = caos_rA .10.1
    board 2 = eth .50.200, ncm .20.100          PC B = caos_rB .20.1
    the two ETH ports on a direct cable

Phases (each PASS/FAIL, exit 0 only if all pass):
    flash         build_caos.sh + prog.sh on both (skip with --no-flash)
    consoles      the two consoles answer 'router' as board 1 and board 2
                  (roles negotiated: one of each)
    selftest      'link status' is not SELFTEST FAIL on both
    password      'link pass' on both; the password never comes back on the
                  console (echoed as '*') and is not in the CLI history
    link_up       both UP, at least one session each
    pings         every hop from A, and B -> A
    traffic       UDP 5000 (encrypted) A->B and UDP 6000 (clear) B->A at the
                  same time, 0 datagrams lost; --secs long runs in chunks of
                  1 h, the boards checked after each (link, tags, pool, heap)
    rekey_live    new password on both while UDP 5000 runs; afterwards UP
                  again and a fresh run loses nothing
    peer_reset    board 2 reset: it gets role 2 back from the peer, password
                  again, new handshake, traffic ok
    invariants    0 tag failures, net_buf pools full, heap check PASS

Usage:
    python3 workspace/app_3/test_app3.py [--boards H753 H563] [--no-flash] [--secs N]
                                         [--password P --name N]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))  # workspace/app_3/ -> repo
# caos_shell: the dev tree builds it in test/hw_test/build, the FREE package ships it in test/
SHELL = next((p for p in (os.path.join(ROOT, "test", "hw_test", "build", "caos_shell"),
                          os.path.join(ROOT, "test", "caos_shell")) if os.path.exists(p)),
             os.path.join(ROOT, "test", "hw_test", "build", "caos_shell"))
BENCH = ["sudo", "-n", os.path.join(ROOT, "tools/bench_net.sh")]

results = []


def phase(name, ok, detail=""):
    results.append((name, ok))
    print("[app3] %-12s %s  %s" % (name, "PASS" if ok else "FAIL", detail), flush=True)
    return ok


def run(cmd, timeout=600, env=None):
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                       timeout=timeout, env=env)
    return p.returncode, p.stdout + p.stderr


def stlink_ttys():
    d = "/dev/serial/by-id"
    # case-insensitive: V3 shows as STLINK-V3, the V2.1 of the F767 as STLink
    return [os.path.join(d, f) for f in sorted(os.listdir(d))
            if "stlink" in f.lower() and f.endswith("-if02")]


def shell(tty, *cmds, t=3, until=None):
    args = [SHELL, "-q", "-t", str(t)]
    if until:
        args += ["-u", until]
    env = dict(os.environ, CAOS_TTY=tty)
    _, out = run(args + list(cmds), timeout=t * len(cmds) + 60, env=env)
    return out


def num(out, key):
    m = re.search(r"\b%s=(\d+)" % re.escape(key), out)
    return int(m.group(1)) if m else -1


def iperf_pair(secs, rate5000="0.5", rate6000="1"):
    """UDP 5000 A->B and 6000 B->A together; returns the server CSV lines."""
    tmp = tempfile.mkdtemp(prefix="app3_")
    srv = {}
    procs = []
    for side, port in (("B", 5000), ("A", 6000)):
        f = open(os.path.join(tmp, "s%d" % port), "w")
        srv[port] = f.name
        procs.append(subprocess.Popen(BENCH + ["ns-router", "iperf-s", side, str(port),
                                               str(secs + 20), "udp"],
                                      cwd=ROOT, stdout=f, stderr=subprocess.STDOUT))
    time.sleep(1.5)
    cli = [threading.Thread(target=run, args=(BENCH + ["ns-router", "iperf", "A",
                            "192.168.20.1", "5000", str(secs), "udp", rate5000, "1400"],),
                            kwargs={"timeout": secs + 60}),
           threading.Thread(target=run, args=(BENCH + ["ns-router", "iperf", "B",
                            "192.168.10.1", "6000", str(secs), "udp", rate6000, "1400"],),
                            kwargs={"timeout": secs + 60})]
    for c in cli:
        c.start()
    return procs, cli, srv


def iperf_wait(procs, cli, srv):
    for c in cli:
        c.join()
    for p in procs:
        p.wait()
    rep = {}
    for port, path in srv.items():
        line = ""
        with open(path) as f:
            for ln in f:
                if ln.count(",") >= 12:
                    line = ln.strip()
        # CSV: ...,interval,bytes,bps,jitter,lost,total,pct,out_of_order
        fl = line.split(",") if line else []
        rep[port] = (int(fl[10]), int(fl[11])) if len(fl) > 11 else (-1, -1)
    return rep


CHUNK = 3600                         # bench_net.sh iperf accepts 1..3600 s


def board_health(tty):
    """'' if both boards are fine: link UP, 0 tag failures, pool full,
    heap intact. Otherwise what is wrong."""
    bad = []
    for b in (1, 2):
        # a reply cut short is not a link down: retry until every line is in
        for attempt in range(3):
            o = shell(tty[b], "link status", "eth pool", "free", t=4 + 2 * attempt)
            if re.search(r"^link \S", o, re.M) and "pool_free=" in o and "heap check" in o:
                break
        else:
            bad.append("b%d unreadable" % b)
            continue
        m = re.search(r"pool_free=(\d+)/(\d+)", o)
        pool_ok = bool(m) and int(m.group(1)) + max(num(o, "driver_held"), 0) == int(m.group(2))
        if "link UP" not in o:
            st = re.search(r"^link (.*?)  port", o, re.M)
            bad.append("b%d link %s" % (b, st.group(1) if st else "?"))
        if num(o, "tag_fail") != 0:
            bad.append("b%d tag_fail=%d" % (b, num(o, "tag_fail")))
        if not pool_ok:
            bad.append("b%d pool %s" % (b, m.group(0) if m else "?"))
        if "heap check: PASS" not in o:
            bad.append("b%d heap" % b)
    return ", ".join(bad)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--boards", nargs=2, default=["H753", "H563"],
                    metavar="MCU", help="the two parts on the bench (H753 H563 F767)")
    ap.add_argument("--chunk", type=int, default=CHUNK,
                    help="traffic chunk, s (max %d); shorter only to try the chunking" % CHUNK)
    ap.add_argument("--no-flash", action="store_true")
    ap.add_argument("--secs", type=int, default=60,
                    help="traffic phase length; long runs go in chunks of 1 h, "
                         "checked after each (e.g. 43200 = 12 h)")
    ap.add_argument("--password", default="banco-caos-15")
    ap.add_argument("--name", default="link-banco-1")
    a = ap.parse_args()
    a.chunk = max(10, min(a.chunk, CHUNK))

    # flash
    if not a.no_flash:
        rc, out = run(["bash", "-c", "source ./source_this >/dev/null 2>&1; "
                       "./build_caos.sh %s --app 3" % " ".join(a.boards)], timeout=900)
        ok = rc == 0
        for m in a.boards:
            r, _ = run(["./prog.sh", "-s", "-n", "-a", "3"], env=dict(os.environ, CAOS_MCU=m))
            ok = ok and r == 0
        if not phase("flash", ok, "app_3 built and flashed on %s" % " + ".join(a.boards)):
            return 1
        time.sleep(8)                  # boot + role negotiation

    # consoles: which tty is which board
    tty = {}
    for t in stlink_ttys():
        out = shell(t, "router")
        m = re.search(r"router board (\d)", out)
        if m:
            tty[int(m.group(1))] = t
    if not phase("consoles", set(tty) == {1, 2}, "board(s) found: %s" % sorted(tty)):
        return 1

    # selftest
    st = {b: shell(tty[b], "link status") for b in (1, 2)}
    ok = all("SELFTEST FAIL" not in st[b] and "link " in st[b] for b in (1, 2))
    phase("selftest", ok, "crypto self-tests passed at boot on both")

    # password: masked echo, not in history
    def key_both():
        outs = {}
        for b in (1, 2):
            outs[b] = shell(tty[b], "link pass %s %s" % (a.password, a.name),
                            t=15, until="LINK keys")
        return outs
    outs = key_both()
    hist = {b: shell(tty[b], "cli_stat") for b in (1, 2)}
    leak = [b for b in (1, 2) if a.password in outs[b] or a.password in hist[b]
            or "link pass" in hist[b]]
    derived = all("LINK keys derived" in outs[b] for b in (1, 2))
    phase("password", derived and not leak,
          "keys derived on both; password %s on console/history" %
          ("FOUND (boards %s)" % leak if leak else "never"))

    # link up
    time.sleep(6)
    st = {b: shell(tty[b], "link status") for b in (1, 2)}
    up = all("link UP" in st[b] for b in (1, 2))
    sess = [num(st[b], "sessions") for b in (1, 2)]
    # >= 1, not == 1: with the boards already keyed (--no-flash) the first
    # one re-keyed pairs with the other's OLD link (same password, same
    # static keys) and again with its new one. That the keys agree is what
    # the traffic phase proves.
    phase("link_up", up and min(sess) >= 1, "UP=%s sessions=%s" % (up, sess))

    # namespaces + pings
    run(BENCH + ["ns-router", "down"])
    rc, out = run(BENCH + ["ns-router", "up"])
    hops = [("A", h) for h in ("192.168.10.100", "192.168.50.100", "192.168.50.200",
                               "192.168.20.100", "192.168.20.1")] + [("B", "192.168.10.1")]
    bad = []
    for side, h in hops:
        _, o = run(BENCH + ["ns-router", "ping", side, h, "3"])
        if " 3 received" not in o:
            bad.append("%s->%s" % (side, h))
    phase("pings", rc == 0 and not bad, "6 hops 3/3" if not bad else "failed: %s" % bad)

    # traffic, in chunks of at most CHUNK s: after each one the boards are
    # checked, so a fault at hour 3 of 12 fails at hour 3
    left, k, tot = a.secs, 0, {5000: [0, 0], 6000: [0, 0]}
    ok, why = True, ""
    while left > 0 and ok:
        t = min(left, a.chunk)
        k += 1
        p, c, s = iperf_pair(t)
        rep = iperf_wait(p, c, s)
        for pt in (5000, 6000):
            tot[pt][0] += max(rep[pt][0], 0)
            tot[pt][1] += max(rep[pt][1], 0)
        bad = [pt for pt in (5000, 6000) if rep[pt][0] != 0 or rep[pt][1] <= 0]
        health = board_health(tty)
        if bad or health:
            ok, why = False, "chunk %d: %s %s" % (k, "lost on %s %s" % (bad, rep) if bad else "",
                                                  health)
        left -= t
        print("[app3]   chunk %d (%d s): 5000 %s  6000 %s  %s" %
              (k, t, rep[5000], rep[6000], "ok" if ok else "FAIL " + why), flush=True)
    phase("traffic", ok, "%d s in %d chunk(s): 5000 enc lost/total=%s  6000 clear=%s%s" %
          (a.secs - left, k, tuple(tot[5000]), tuple(tot[6000]),
           "" if ok else "  -- " + why))

    # rekey with traffic running
    p, c, s = iperf_pair(30)
    time.sleep(8)
    key_both()
    iperf_wait(p, c, s)
    time.sleep(6)
    st = {b: shell(tty[b], "link status") for b in (1, 2)}
    up = all("link UP" in st[b] for b in (1, 2))
    p, c, s = iperf_pair(10)
    rep = iperf_wait(p, c, s)
    phase("rekey_live", up and rep[5000][0] == 0 and rep[5000][1] > 0,
          "new password under traffic: UP=%s, then 10 s lost/total=%s" % (up, rep[5000]))

    # peer reset
    sn = os.path.basename(tty[2]).split("_")[-1].split("-")[0]
    run(["bash", "./reset_mcu.sh"], env=dict(os.environ, CAOS_STLINK_SN=sn))
    time.sleep(8)
    back = re.search(r"router board (\d)", shell(tty[2], "router"))
    same_role = bool(back) and back.group(1) == "2"
    run(BENCH + ["ns-router", "down"])
    run(BENCH + ["ns-router", "up"])
    shell(tty[2], "link pass %s %s" % (a.password, a.name), t=15, until="LINK keys")
    time.sleep(8)
    st = {b: shell(tty[b], "link status") for b in (1, 2)}
    up = all("link UP" in st[b] for b in (1, 2))
    p, c, s = iperf_pair(10)
    rep = iperf_wait(p, c, s)
    phase("peer_reset", same_role and up and rep[5000][0] == 0 and rep[5000][1] > 0,
          "board 2 reset: role 2 again=%s, UP=%s, 10 s lost/total=%s" %
          (same_role, up, rep[5000]))

    # invariants
    ok, det = True, []
    for b in (1, 2):
        o = shell(tty[b], "link status", "eth pool", "free")
        tf, pf, held = num(o, "tag_fail"), num(o, "pool_free"), num(o, "driver_held")
        m = re.search(r"pool_free=(\d+)/(\d+)", o)
        tot = int(m.group(2)) if m else -1
        pf = int(m.group(1)) if m else -1
        good = tf == 0 and pf + held == tot and "heap check: PASS" in o
        ok = ok and good
        det.append("b%d tag_fail=%d pool %d+%d/%d heap %s" %
                   (b, tf, pf, held, tot, "PASS" if "heap check: PASS" in o else "?"))
    phase("invariants", ok, "; ".join(det))

    passed = sum(1 for _, k in results if k)
    print("APP3 RESULT %s %d/%d" % ("PASS" if passed == len(results) else "FAIL",
                                    passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
