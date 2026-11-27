#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""PC side of example 159: the board's RAM disk as the host sees it.

No root: udisks does the work (udisksctl mount/unmount, Drive.Eject over
D-Bus), as the file manager would. The drive is found by the serial caOS
gives it, <MCU_ID>-<USB_STR_SERIAL> (0753-0001 on an H753), so two boards on
the PC are not mixed up.

    disk_check.py <mcu_id> size                  bytes of the medium (0 = absent)
    disk_check.py <mcu_id> wait present|absent [s]   until the medium is (or is not) there
    disk_check.py <mcu_id> put NAME TEXT         mount, write NAME, unmount
    disk_check.py <mcu_id> get NAME              mount, print NAME, unmount
    disk_check.py <mcu_id> ls                    mount, list the root, unmount
    disk_check.py <mcu_id> eject                 Drive.Eject: the board gets the disk back

Every command prints one verdict line: 'DISK OK ...' or 'DISK FAIL ...'.
"""
import json
import os
import subprocess
import sys
import time


def run(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def find(mcu):
    """the block device whose USB serial is <mcu>-..., or None"""
    rc, out = run("lsblk", "-J", "-d", "-b", "-o", "NAME,VENDOR,SERIAL,SIZE")
    if rc != 0:
        return None, 0
    for d in json.loads(out).get("blockdevices", []):
        if (d.get("vendor") or "").strip() == "caOS" and (d.get("serial") or "").startswith(mcu + "-"):
            return "/dev/" + d["name"], int(d.get("size") or 0)
    return None, 0


def verdict(ok, what):
    print("DISK %s %s" % ("OK" if ok else "FAIL", what))
    return 0 if ok else 1


def mount(dev):
    rc, out = run("udisksctl", "mount", "-b", dev, "--no-user-interaction")
    if rc != 0:
        return None, out
    return out.split()[-1].rstrip("."), out


def unmount(dev):
    run("sync")
    return run("udisksctl", "unmount", "-b", dev, "--no-user-interaction")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    mcu, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    dev, size = find(mcu)
    if cmd == "size":
        return verdict(dev is not None, "size %d dev %s" % (size, dev))
    if cmd == "wait":
        want = args[0] == "present"
        limit = float(args[1]) if len(args) > 1 else 15.0
        t0 = time.time()
        while time.time() - t0 < limit:
            dev, size = find(mcu)
            if dev is not None and (size > 0) == want:
                return verdict(True, "medium %s after %.1f s, size %d" % (args[0], time.time() - t0, size))
            time.sleep(0.3)
        return verdict(False, "medium not %s within %.0f s (dev %s size %d)" % (args[0], limit, dev, size))
    if dev is None:
        return verdict(False, "no caOS drive with serial %s-*" % mcu)
    if cmd == "eject":
        rc, out = run("udisksctl", "info", "-b", dev)
        drive = next((l.split()[-1].strip("'") for l in out.splitlines() if l.strip().startswith("Drive:")), None)
        if drive is None:
            return verdict(False, "no udisks drive for %s: %s" % (dev, out))
        rc, out = run("gdbus", "call", "--system", "--dest", "org.freedesktop.UDisks2",
                      "--object-path", drive, "--method", "org.freedesktop.UDisks2.Drive.Eject",
                      '{"auth.no_user_interaction": <true>}')
        return verdict(rc == 0, "eject %s: %s" % (drive, out))
    mnt, out = mount(dev)
    if mnt is None:
        return verdict(False, "mount %s: %s" % (dev, out))
    try:
        if cmd == "put":
            with open(os.path.join(mnt, args[0]), "w") as f:
                f.write(" ".join(args[1:]))
            ok, what = True, "wrote %s" % args[0]
        elif cmd == "get":
            with open(os.path.join(mnt, args[0])) as f:
                print(f.read())
            ok, what = True, "read %s" % args[0]
        elif cmd == "ls":
            ok, what = True, "ls " + " ".join(sorted(os.listdir(mnt)))
        else:
            ok, what = False, "unknown command %s" % cmd
    except OSError as e:
        ok, what = False, "%s: %s" % (cmd, e)
    rc, out = unmount(dev)
    if rc != 0:
        return verdict(False, "unmount: %s" % out)
    return verdict(ok, what)


if __name__ == "__main__":
    sys.exit(main())
