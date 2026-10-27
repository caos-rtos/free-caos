#!/usr/bin/env python3
"""test_boot_pc.py - end-to-end test of the app_2 bootloader on one board.

Flashes app_2, then drives the whole update cycle over the network and reads
the console across every reset:

  1 recovery     empty slots: the bootloader stays up and gets a DHCP address
  2 refused      a tampered image is received but NOT valid; a non .cimg name
                 is refused
  3 trial        app_4 pushed into slot A: pending, trial with the IWDG,
                 app_4 confirms itself
  4 image update app_4 for slot B pushed to the RUNNING image: A becomes the
                 active slot, B goes on trial and confirms
  5 rollback     app_1 (never confirms, never feeds the IWDG) pushed into A:
                 three trials, rollback, A erased, B starts again

Needs, for the MCU: elfs/<MCU>/<mcu>_app_2.elf, _app_4_slotA/B.elf and
_app_1_slotA.elf (./build_caos.sh <MCU> -wp), the bench network up
(tools/bench_net.sh) and the board free. Ends with RUN RESULT : OK|FAIL.

  python3 workspace/app_2/test_boot_pc.py --mcu H563 --tty /dev/serial/by-id/...-if02
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# caos_shell: the dev tree builds it in test/hw_test/build, the FREE package ships it in test/
SHELL = next((p for p in (os.path.join(ROOT, "test", "hw_test", "build", "caos_shell"),
                          os.path.join(ROOT, "test", "caos_shell")) if os.path.exists(p)),
             os.path.join(ROOT, "test", "hw_test", "build", "caos_shell"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from caos_tftp import put, TftpError          # noqa: E402


def run(cmd, env=None, timeout=120):
    r = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=timeout)
    return r.returncode, r.stdout.decode(errors="replace")


class Board:
    def __init__(self, mcu, tty):
        self.env = dict(os.environ, CAOS_MCU=mcu, CAOS_TTY=tty)

    def console(self, cmd, until, seconds):
        """Send cmd, return everything the board says until `until` (regex)."""
        _, out = run([SHELL, "-q", "-t", str(seconds), "-u", until, cmd],
                     env=self.env, timeout=seconds + 30)
        return out

    def while_pushing(self, cmd, until, seconds, action):
        """Keep the console open (cmd/until/seconds) while action() runs."""
        box = {}
        t = threading.Thread(target=lambda: box.update(out=self.console(cmd, until, seconds)))
        t.start()
        time.sleep(1.5)
        try:
            box["act"] = action()
        except TftpError as e:
            box["act"] = "TFTP: %s" % e
        t.join()
        return box.get("out", ""), box.get("act")


MAC = "02:00:00:00:b0:07"          # app_2 and app_4
LEASES = "/run/caos-bench/dnsmasq.leases"


def lease(mac, seconds):
    """The board's address from the bench DHCP server (tools/bench_net.sh)."""
    end = time.time() + seconds
    while time.time() < end:
        try:
            for line in open(LEASES):
                f = line.split()
                if len(f) >= 3 and f[1].lower() == mac:
                    return f[2]
        except OSError:
            pass
        time.sleep(1)
    return None


def has(text, *needles):
    return all(n in text for n in needles)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--mcu", required=True, help="H563 or H753")
    ap.add_argument("--tty", required=True, help="console of that board")
    a = ap.parse_args()
    mcu, low = a.mcu.upper(), a.mcu.lower()
    elfs = os.path.join(ROOT, "elfs", mcu)
    work = tempfile.mkdtemp(prefix="app2_test_")
    b = Board(mcu, a.tty)
    ok = 0
    total = 5

    def verdict(n, good, what, detail=""):
        print("PHASE %d %s - %s%s" % (n, "OK" if good else "FAIL", what,
                                      "" if good else "\n" + detail[-1500:]))
        return 1 if good else 0

    # images
    imgs = {}
    for name, elf, ver in (("A", "app_4_slotA", 1), ("B", "app_4_slotB", 2),
                           ("bad", "app_1_slotA", 9)):
        out = os.path.join(work, "%s.cimg" % name)
        rc, txt = run([sys.executable, "tools/caos_image.py",
                       os.path.join(elfs, "%s_%s.elf" % (low, elf)),
                       "--version", str(ver), "--tag", "test %s" % name, "--sign", "-o", out])
        if rc != 0:
            print("cannot build image %s: %s" % (name, txt))
            return 2
        imgs[name] = out
    data = bytearray(open(imgs["A"], "rb").read())   # empty slots: it goes to A
    data[0x400 + 5000] ^= 0xFF
    imgs["tampered"] = os.path.join(work, "tampered.cimg")
    open(imgs["tampered"], "wb").write(data)

    # 1 recovery
    rc, txt = run(["./prog.sh", "-n", "-s", "-f", os.path.join(elfs, "%s_app_2.elf" % low)],
                  env=b.env)
    if rc != 0:
        print("prog.sh failed: %s" % txt)
        return 2
    out = b.console("boot erase A", r"slot A erased", 30)
    out += b.console("boot erase B", r"slot B erased", 30)
    ip = lease(MAC, 40)
    ok += verdict(1, ip is not None and "slot B erased" in out,
                  "recovery with DHCP address %s" % ip, out)
    if not ip:
        print("  RUN RESULT : FAIL  (%d/%d)" % (ok, total))
        return 1

    # 2 refused
    out, _ = b.while_pushing("boot info", r"NOT valid", 30,
                             lambda: put(ip, open(imgs["tampered"], "rb").read(), "t.cimg"))
    try:
        put(ip, b"x" * 100, "x.bin")
        refused = False
    except TftpError as e:
        refused = "refused" in str(e)
    ok += verdict(2, "SHA-256 mismatch" in out and refused,
                  "tampered image not valid, x.bin refused", out)

    # 3 trial of A, confirmed
    out, _ = b.while_pushing("uptime", r"APP4: update -", 90,
                             lambda: put(ip, open(imgs["A"], "rb").read(), "a.cimg"))
    ok += verdict(3, has(out, "pending, reset into its trial", "trial of slot A, start 1",
                         "TRIAL, IWDG taken over", "APP4: confirmed"),
                  "app_4 in A: trial, IWDG, confirmed", out)

    # 4 update from the running image into B
    out, _ = b.while_pushing("img info", r"APP4: confirmed", 90,
                             lambda: put(ip, open(imgs["B"], "rb").read(), "b.cimg"))
    ok += verdict(4, has(out, "reset into the bootloader", "slot A confirmed, now the active one",
                         "slot B v2 pending", "trial of slot B, start 1", "APP4: confirmed"),
                  "update from the running image: A committed, B on trial and confirmed", out)

    # 5 rollback
    out, _ = b.while_pushing("img info", r"APP4: ip", 150,
                             lambda: put(ip, open(imgs["bad"], "rb").read(), "bad.cimg"))
    ok += verdict(5, has(out, "slot B confirmed, now the active one", "start 3 of 3",
                         "ROLLBACK - slot A never confirmed, erased; back to slot B",
                         'APP4: slot B v2 "test B"'),
                  "image that never confirms: three trials, rollback to B", out)

    print("  RUN RESULT : %s  (%d/%d)" % ("OK" if ok == total else "FAIL", ok, total))
    return 0 if ok == total else 1


if __name__ == "__main__":
    sys.exit(main())
