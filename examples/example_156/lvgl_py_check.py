#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""PC side of example 156: LVGL widgets built from MicroPython, seen through
VNC. Standard library only; the RFB client is example 139's (plain RFB).

    lvgl_py_check.py <ip> --save FILE             park the pointer at (0, 0), full frame -> FILE
    lvgl_py_check.py <ip> --compare FILE          park the pointer, full frame == FILE ?
    lvgl_py_check.py <ip> --rect X Y W H RGB      every pixel of the rectangle is 0xRRGGBB ?
    lvgl_py_check.py <ip> --click X Y [--delay S] press and release there (after S seconds)
    lvgl_py_check.py <ip> --keys SYM[,SYM...] [--delay S]   X11 keysyms (hex), press + release

The pointer parks at (0, 0) before a frame is taken: LVGL draws its cursor
where the pointer is, and the board boots with it there. Prints one verdict
line: 'LVGL CHECK PASS ...' or 'LVGL CHECK FAIL ...'.
"""
import importlib.util
import os
import struct
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "vnc_check", os.path.join(_HERE, "..", "example_139", "vnc_check.py"))
vc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vc)

FMT32 = (32, 0, (255, 255, 255), (16, 8, 0))


def connect(ip):
    c = vc.Client(ip, 5900)
    w, h, _, _ = c.hello()
    return c, w, h


def pointer(c, x, y, buttons):
    c.s.sendall(struct.pack(">BBHH", 5, buttons, x, y))


def frame(c, w, h):
    screen = [None] * (w * h)
    c.request(0, 0, 0, w, h)
    c.update(FMT32, screen, w)
    return screen


def parked_frame(ip):
    c, w, h = connect(ip)
    pointer(c, 0, 0, 0)
    time.sleep(0.3)                       # LVGL moves its cursor, VNC sees the change
    f = frame(c, w, h)
    c.s.close()
    return f, w, h


def verdict(ok, what):
    print("LVGL CHECK %s %s" % ("PASS" if ok else "FAIL", what))
    return 0 if ok else 1


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith("--"):
        print(__doc__)
        return 2
    ip = a[0]
    delay = float(a[a.index("--delay") + 1]) if "--delay" in a else 0.0
    if delay:
        time.sleep(delay)
    if "--save" in a:
        f, w, h = parked_frame(ip)
        with open(a[a.index("--save") + 1], "w") as fp:
            fp.write("%d %d\n" % (w, h))
            fp.write(" ".join("%x" % v for v in f))
        return verdict(True, "saved %ux%u" % (w, h))
    if "--compare" in a:
        with open(a[a.index("--compare") + 1]) as fp:
            w0, h0 = (int(v) for v in fp.readline().split())
            ref = [int(v, 16) for v in fp.read().split()]
        f, w, h = parked_frame(ip)
        if (w, h) != (w0, h0):
            return verdict(False, "size %ux%u, saved %ux%u" % (w, h, w0, h0))
        bad = sum(1 for x, y in zip(f, ref) if x != y)
        return verdict(bad == 0, "frame equal to the saved one: %u different pixels" % bad)
    if "--rect" in a:
        i = a.index("--rect")
        x, y, rw, rh = (int(v) for v in a[i + 1:i + 5])
        rgb = int(a[i + 5], 16)
        f, w, h = parked_frame(ip)
        vals = [f[(y + j) * w + x + k] & 0xFFFFFF for j in range(rh) for k in range(rw)]
        bad = sum(1 for v in vals if v != rgb)
        sample = "%06x" % vals[0] if vals else "-"
        return verdict(bad == 0, "rect %d,%d %dx%d = %06x: %u of %u differ (first %s)"
                       % (x, y, rw, rh, rgb, bad, len(vals), sample))
    if "--click" in a:
        i = a.index("--click")
        x, y = int(a[i + 1]), int(a[i + 2])
        c, _, _ = connect(ip)
        pointer(c, x, y, 0)
        time.sleep(0.1)
        pointer(c, x, y, 1)
        time.sleep(0.15)
        pointer(c, x, y, 0)
        time.sleep(0.3)
        pointer(c, 0, 0, 0)                 # back where the frames are taken
        time.sleep(0.2)
        c.s.close()
        return verdict(True, "clicked %d,%d" % (x, y))
    if "--keys" in a:
        syms = [int(v, 16) for v in a[a.index("--keys") + 1].split(",")]
        c, _, _ = connect(ip)
        for s in syms:
            c.s.sendall(struct.pack(">BBxxI", 4, 1, s))
            time.sleep(0.05)
            c.s.sendall(struct.pack(">BBxxI", 4, 0, s))
            time.sleep(0.1)
        time.sleep(0.2)
        c.s.close()
        return verdict(True, "keys %s" % ",".join("%x" % s for s in syms))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
