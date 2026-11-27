#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""PC side of example 155: what MicroPython draws in caos.FB('vnc') is what a
VNC viewer receives. Standard library only; the RFB client is example 139's
(vnc_check.py), plain RFB.

    vnc_py_check.py --script         the MicroPython script that draws the scene
    vnc_py_check.py --change-script  the script that adds the change (a cyan box)
    vnc_py_check.py <ip>             full update: every pixel of the scene
    vnc_py_check.py <ip> --watch     full update = scene, prints 'VNC WATCH READY',
                                     then incremental updates until the change
                                     shows (20 s): only pixels of the box change
    vnc_py_check.py <ip> --input     key 'a' press/release, pointer press/release
                                     at (12, 34): for the 'vncin' queue

The scene and its expected image come from the same lines below, so the two
cannot drift apart. Indices are palette entries of the board (0 black,
1 white, 2..7 red green blue yellow cyan magenta, 8..255 gray ramp).
"""
import importlib.util
import os
import socket
import struct
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "vnc_check", os.path.join(_HERE, "..", "example_139", "vnc_check.py"))
vc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vc)

FMT32 = (32, 0, (255, 255, 255), (16, 8, 0))
BOX = (200, 100, 30, 20, 6)            # the change: x y w h index

SCRIPT = """import caos, framebuf
c = caos.FB('vnc'); w = c.width(); h = c.height()
c.fill(0)
bw = w // 8
for i in range(8): c.fill_rect(i * bw, 0, bw, 24, i)
for x in range(0, w, 4): c.fill_rect(x, h - 16, 4, 16, 8 + (x * 247) // w)
b = bytearray(64 * 48); f = framebuf.FrameBuffer(b, 64, 48, framebuf.GS8)
f.fill(3); f.rect(0, 0, 64, 48, 1); f.fill_rect(10, 10, 8, 8, 4)
c.blit(b, 40, 40, 64, 48)
c.pixel(w - 1, h - 1, 7)
print('ok', w, h)
"""

CHANGE = "import caos\ncaos.FB('vnc').fill_rect(%d, %d, %d, %d, %d)\nprint('ok')\n" % BOX


def rect(img, w, h, x, y, rw, rh, c):
    for yy in range(max(y, 0), min(y + rh, h)):
        for xx in range(max(x, 0), min(x + rw, w)):
            img[yy * w + xx] = c


def scene(w, h):
    """the image SCRIPT draws, palette indices"""
    img = bytearray(w * h)
    bw = w // 8
    for i in range(8):
        rect(img, w, h, i * bw, 0, bw, 24, i)
    for x in range(0, w, 4):
        rect(img, w, h, x, h - 16, 4, 16, 8 + (x * 247) // w)
    rect(img, w, h, 40, 40, 64, 48, 3)
    for x in range(64):                 # framebuf rect(0, 0, 64, 48): the outline
        img[40 * w + 40 + x] = 1
        img[87 * w + 40 + x] = 1
    for y in range(48):
        img[(40 + y) * w + 40] = 1
        img[(40 + y) * w + 103] = 1
    rect(img, w, h, 50, 50, 8, 8, 4)
    img[(h - 1) * w + w - 1] = 7
    return img


def expected(img):
    pal = vc.palette()
    return [vc.convert(pal[i], FMT32) for i in img]


def full(c, w, h):
    screen = [None] * (w * h)
    c.request(0, 0, 0, w, h)
    c.update(FMT32, screen, w)
    return screen


def check(ip):
    c = vc.Client(ip, 5900)
    w, h, _, name = c.hello()
    print("VNC  server %ux%u name %r, security %s" % (w, h, name.decode(errors="replace"), c.security))
    screen = full(c, w, h)
    exp = expected(scene(w, h))
    bad = sum(1 for a, b in zip(screen, exp) if a != b)
    c.s.close()
    print("VNC  %s scene drawn by Python: %u wrong pixels of %u" % ("ok  " if bad == 0 else "FAIL", bad, w * h))
    print("VNC PY CHECK %s" % ("PASS" if bad == 0 else "FAIL"))
    return 0 if bad == 0 else 1


def watch(ip):
    c = vc.Client(ip, 5900)
    w, h, _, _ = c.hello()
    screen = full(c, w, h)
    img = scene(w, h)
    bad = sum(1 for a, b in zip(screen, expected(img)) if a != b)
    if bad:
        print("VNC  FAIL the scene is not on the screen before the change (%u wrong pixels)" % bad)
        print("VNC WATCH FAIL")
        return 1
    print("VNC WATCH READY", flush=True)
    rect(img, w, h, *BOX)
    exp = expected(img)
    t0 = time.time()
    rects = []
    c.s.settimeout(1.0)
    while time.time() - t0 < 20:
        c.request(1, 0, 0, w, h)
        try:
            rects += c.update(FMT32, screen, w)
        except socket.timeout:
            continue
        if screen == exp:
            break
    c.s.close()
    bad = sum(1 for a, b in zip(screen, exp) if a != b)
    x0, y0, bw, bh, _ = BOX
    near = all(x < x0 + bw + 16 and x + rw > x0 - 16 and y < y0 + bh + 16 and y + rh > y0 - 16
               for x, y, rw, rh in rects)
    ok = bad == 0 and rects and near
    print("VNC  %s change arrived in %.0f ms: %u rect(s) %s, %u wrong pixels"
          % ("ok  " if ok else "FAIL", (time.time() - t0) * 1000, len(rects),
             "around the box" if near else "NOT only around the box", bad))
    print("VNC WATCH %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def send_input(ip):
    c = vc.Client(ip, 5900)
    w, h, _, _ = c.hello()
    c.s.sendall(struct.pack(">BBxxI", 4, 1, 0x61) + struct.pack(">BBxxI", 4, 0, 0x61))
    c.s.sendall(struct.pack(">BBHH", 5, 1, 12, 34) + struct.pack(">BBHH", 5, 0, 12, 34))
    full(c, w, h)                       # a round trip: the events were read before it
    c.s.close()
    print("VNC INPUT SENT")
    return 0


def main():
    if "--script" in sys.argv:
        sys.stdout.write(SCRIPT)
        return 0
    if "--change-script" in sys.argv:
        sys.stdout.write(CHANGE)
        return 0
    pos = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not pos:
        print(__doc__)
        return 2
    if "--watch" in sys.argv:
        return watch(pos[0])
    if "--input" in sys.argv:
        return send_input(pos[0])
    return check(pos[0])


if __name__ == "__main__":
    sys.exit(main())
