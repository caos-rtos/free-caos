#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""PC side of example 141: drives the LVGL screen over VNC, standard library only.

    lvgl_check.py <ip> [port]

Uses the RFB client of example 139 (vnc_check.py: None or VeNCrypt X509None).
The board must be fresh from boot (button never clicked, toggle off, text
area empty). Checks:
  1. handshake, ServerInit size
  2. the frame is drawn: more than 8 distinct colours; outside the text area
     (its cursor blinks) two full updates are identical
  3. a click on 'Click me' (x = w/20 + 10, y = 72): the button row changes
     (the 'Clicks: 1' label)
  4. a click on 'Toggle' (x = w - w/20 - 10, y = 72): the button row changes
  5. a click on the text area (w/2, 36) to give it the focus back from the
     toggle, then keys 'c' 'a' 'o' 's' and Return: the text area changes by
     more than its cursor alone (100 pixels)
Each check compares full updates taken 0.4 s after the action: the text area
cursor redraws by itself, so incremental updates prove nothing here.
The board prints the events (E141 button clicked 1, E141 toggle on,
E141 text "caos", E141 ready "caos"): the bench checks them on the console.
Prints one line per check, the frame hash ('LVGL hash 0x........') and
'LVGL CHECK PASS (n/n)' or 'LVGL CHECK FAIL (n/n)'.

Press and release are 150 ms apart: LVGL reads the pointer every 30 ms and
keeps only its latest state, a press and release in the same period would
be one release.
"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "example_139"))
from vnc_check import Client  # noqa: E402

FMT = (32, 0, (255, 255, 255), (16, 8, 0))


def fnv(screen):
    h = 0x811C9DC5
    for v in screen:
        for k in (0, 8, 16):
            h = ((h ^ ((v >> k) & 255)) * 0x01000193) & 0xFFFFFFFF
    return h


def full(c, w, h):
    screen = [0] * (w * h)
    c.request(0, 0, 0, w, h)
    c.update(FMT, screen, w)
    return screen


TA = (18, 60)        # text area band (y): its cursor blinks
ROW = (60, 120)      # button row band (y)


def band_diff(a, b, w, y0, y1):
    return sum(1 for i in range(y0 * w, y1 * w) if a[i] != b[i])


def outside_ta_equal(a, b, w):
    return a[:TA[0] * w] == b[:TA[0] * w] and a[TA[1] * w:] == b[TA[1] * w:]


def click(c, x, y):
    c.s.sendall(struct.pack(">BBHH", 5, 0, x, y))     # move there first
    time.sleep(0.1)
    c.s.sendall(struct.pack(">BBHH", 5, 1, x, y))
    time.sleep(0.15)
    c.s.sendall(struct.pack(">BBHH", 5, 0, x, y))
    time.sleep(0.15)


def key(c, sym):
    c.s.sendall(struct.pack(">BBxxI", 4, 1, sym))
    time.sleep(0.05)
    c.s.sendall(struct.pack(">BBxxI", 4, 0, sym))
    time.sleep(0.05)


def main():
    pos = [a for a in sys.argv[1:] if not a.startswith("--")]
    ip = pos[0]
    port = int(pos[1]) if len(pos) > 1 else 5900
    ok = []

    def check(cond, what):
        ok.append(bool(cond))
        print("LVGL %s %s" % ("ok  " if cond else "FAIL", what), flush=True)
        return cond

    c = Client(ip, port)
    w, h, _, name = c.hello()
    print("LVGL server %ux%u name %r, security %s%s" % (w, h, name.decode(errors="replace"), c.security,
                                                         (", " + c.cipher) if c.cipher else ""))
    check(w >= 320 and h >= 200, "ServerInit size %ux%u" % (w, h))

    s1 = full(c, w, h)
    time.sleep(0.6)
    s2 = full(c, w, h)
    colours = len(set(s1))
    print("LVGL hash 0x%08X (whole frame, text area cursor included)" % fnv(s1))
    check(colours > 8 and outside_ta_equal(s1, s2, w),
          "frame drawn: %u colours, stable outside the text area" % colours)

    before = s2
    click(c, w // 20 + 10, 72)
    time.sleep(0.4)
    after = full(c, w, h)
    d = band_diff(before, after, w, *ROW)
    check(d > 0, "click on 'Click me' redraws the button row (%u px)" % d)

    before = after
    click(c, w - w // 20 - 10, 72)
    time.sleep(0.4)
    after = full(c, w, h)
    d = band_diff(before, after, w, *ROW)
    check(d > 0, "click on 'Toggle' redraws the button row (%u px)" % d)

    # the click on Toggle took the focus: keys would go to it (Return would
    # toggle it back). Click the text area first, as a user does.
    click(c, w // 2, 36)
    time.sleep(0.4)
    before = full(c, w, h)
    for sym in (0x63, 0x61, 0x6F, 0x73, 0xFF0D):
        key(c, sym)
    time.sleep(0.4)
    after = full(c, w, h)
    d = band_diff(before, after, w, *TA)
    check(d > 100, "keys 'caos' + Return redraw the text area (%u px)" % d)

    c.s.close()
    n = len(ok)
    print("LVGL CHECK %s (%u/%u)" % ("PASS" if all(ok) else "FAIL", sum(ok), n))
    return 0 if all(ok) else 1


if __name__ == "__main__":
    sys.exit(main())
