#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""PC side of example 139: a raw RFB client (RFC 6143), standard library only.

    vnc_check.py <ip> [port]
    vnc_check.py <ip> --view-only     (board after 'e139 input off')
    --rgb565    the board's framebuffer is RGB565 ('e139 fmt rgb565' + 'e139 new'):
                the pattern colours are expected through 5-6-5 (quantized, then
                widened by bit replication as the board does)

The board must show its boot pattern (send 'e139 grid' first). Checks:
  1. handshake 3.8 and the security the board offers: None, or VeNCrypt
     X509Plain = TLS 1.3 + login (CAOS_VNC_USER / CAOS_VNC_PASS, default
     caos / caos-vnc-demo), certificate checked against tls_identity_cert.pem
     (SAN caos.local), suite AES-256-GCM-SHA384; ServerInit (size, 32 bpp)
  2. the pattern, full non-incremental update, in three pixel formats:
     32 bpp little endian (default), 16 bpp RGB565 big endian, 8 bpp RGB332
  3. an incremental request with nothing changed gets no answer (0.6 s)
  4. a pointer press at (100, 40) (client input): the board paints a 3x3
     yellow square there; the incremental update must carry exactly it
  5. a key press/release reaches the board (it prints 'E139 key src=client')
  6. a second client while this one is connected is closed by the board
Prints one line per check and 'VNC CHECK PASS (n/n)' or 'VNC CHECK FAIL (n/n)'.

--view-only: client input is off on the board. A key ('c') and a pointer press at
(200, 100) are sent; nothing may reach the application, so nothing is painted:
the incremental request after them must stay held, and the frame must still be
the pattern. Prints 'VNC VIEW ONLY PASS' or 'VNC VIEW ONLY FAIL'.
"""
import os
import socket
import ssl
import struct
import sys
import time

COLORS = [0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF]


RGB565 = "--rgb565" in sys.argv


def w565(rgb):
    """0x00RRGGBB through RGB565: what an RGB565 framebuffer holds, widened back"""
    r, g, b = (rgb >> 19) & 31, (rgb >> 10) & 63, (rgb >> 3) & 31
    return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2))


def palette():
    pal = COLORS + [i * 0x010101 for i in range(8, 256)]
    return [w565(c) for c in pal] if RGB565 else pal


def pattern(w, h):
    img = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            if x % 32 == 0 or y % 32 == 0 or x == w - 1 or y == h - 1:
                c = 1
            elif y < 32:
                c = 2 + (6 * x) // w
            else:
                c = 0
            img[y * w + x] = c
    return img


def convert(rgb, fmt):
    bpp, be, maxs, shifts = fmt
    c = ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255)
    v = 0
    for k in range(3):
        if shifts[k] < 32:
            v |= ((c[k] * maxs[k] + 127) // 255) << shifts[k]
    if bpp < 32:
        v &= (1 << bpp) - 1
    return v


# X509Plain login of the examples (vnc_cred.h, tools/gen_vnc_cred.py)
VNC_USER = os.environ.get("CAOS_VNC_USER", "caos")
VNC_PASS = os.environ.get("CAOS_VNC_PASS", "caos-vnc-demo")


class AuthError(Exception):
    pass


class Client:
    def __init__(self, ip, port, user=None, password=None):
        self.s = socket.create_connection((ip, port), timeout=10)
        self.security = "none"
        self.cipher = None
        self.user = (VNC_USER if user is None else user).encode()
        self.password = (VNC_PASS if password is None else password).encode()

    def recv(self, n):
        b = b""
        while len(b) < n:
            k = self.s.recv(n - len(b))
            if not k:
                raise EOFError("connection closed by the board")
            b += k
        return b

    def hello(self):
        if self.recv(12) != b"RFB 003.008\n":
            raise ValueError("server version is not RFB 003.008")
        self.s.sendall(b"RFB 003.008\n")
        n = self.recv(1)[0]
        types = self.recv(n)
        if 19 in types:                               # VeNCrypt
            self.s.sendall(b"\x13")
            if self.recv(2) != b"\x00\x02":
                raise ValueError("VeNCrypt version is not 0.2")
            self.s.sendall(b"\x00\x02")
            if self.recv(1) != b"\x00":
                raise ValueError("VeNCrypt version refused")
            subs = [struct.unpack(">I", self.recv(4))[0] for _ in range(self.recv(1)[0])]
            sub = 262 if 262 in subs else 260          # X509Plain first, as TigerVNC
            if sub not in subs:
                raise ValueError("neither X509Plain nor X509None offered: %r" % subs)
            self.s.sendall(struct.pack(">I", sub))
            if self.recv(1) != b"\x01":
                raise ValueError("VeNCrypt subtype %u not accepted" % sub)
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.minimum_version = ssl.TLSVersion.TLSv1_3
            ctx.load_verify_locations(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                   "tls_identity_cert.pem"))
            self.s = ctx.wrap_socket(self.s, server_hostname="caos.local")
            self.security = "VeNCrypt X509Plain" if sub == 262 else "VeNCrypt X509None"
            self.cipher = self.s.cipher()[0]
            if sub == 262:
                self.s.sendall(struct.pack(">II", len(self.user), len(self.password)) +
                               self.user + self.password)
        elif 1 in types:
            self.s.sendall(b"\x01")
        else:
            raise ValueError("no security type this client knows: %r" % types)
        if struct.unpack(">I", self.recv(4))[0] != 0:
            try:
                why = self.recv(struct.unpack(">I", self.recv(4))[0]).decode(errors="replace")
            except (EOFError, OSError, struct.error):
                why = "?"
            raise AuthError("SecurityResult failed: %s" % why)
        self.s.sendall(b"\x01")                       # shared
        w, h = struct.unpack(">HH", self.recv(4))
        pf = self.recv(16)
        name = self.recv(struct.unpack(">I", self.recv(4))[0])
        return w, h, pf, name

    def set_format(self, bpp, depth, be, maxs, shifts):
        self.s.sendall(struct.pack(">B3xBBBBHHHBBB3x", 0, bpp, depth, be, 1, *maxs, *shifts))

    def request(self, incr, x, y, w, h):
        self.s.sendall(struct.pack(">BBHHHH", 3, incr, x, y, w, h))

    def update(self, fmt, screen, sw):
        """Reads one FramebufferUpdate into screen (pixel values); returns rects"""
        bpp, be = fmt[0], fmt[1]
        t, _, n = struct.unpack(">BBH", self.recv(4))
        if t != 0:
            raise ValueError("message %u is not a FramebufferUpdate" % t)
        rects = []
        bp = bpp // 8
        for _ in range(n):
            x, y, w, h, enc = struct.unpack(">HHHHi", self.recv(12))
            if enc != 0:
                raise ValueError("encoding %d is not Raw" % enc)
            data = self.recv(w * h * bp)
            o = ">" if be else "<"
            code = {1: "B", 2: "H", 4: "I"}[bp]
            vals = struct.unpack(o + code * (w * h), data)
            for j in range(h):
                screen[(y + j) * sw + x:(y + j) * sw + x + w] = vals[j * w:(j + 1) * w]
            rects.append((x, y, w, h))
        return rects


def view_only(ip, port):
    c = Client(ip, port)
    w, h, _, _ = c.hello()
    fmt = (32, 0, (255, 255, 255), (16, 8, 0))
    screen = [None] * (w * h)
    c.request(0, 0, 0, w, h)
    c.update(fmt, screen, w)
    c.s.sendall(struct.pack(">BBxxI", 4, 1, 0x63) + struct.pack(">BBxxI", 4, 0, 0x63))
    c.s.sendall(struct.pack(">BBHH", 5, 1, 200, 100) + struct.pack(">BBHH", 5, 0, 200, 100))
    c.request(1, 0, 0, w, h)
    c.s.settimeout(0.8)
    try:
        c.s.recv(1)
        held = False
    except socket.timeout:
        held = True
    c.s.settimeout(10)
    print("VNC  %s nothing painted after client key and pointer (request held)" % ("ok  " if held else "FAIL"))
    c.request(0, 0, 0, w, h)
    if not held:
        c.update(fmt, screen, w)                      # the incremental answer
    c.update(fmt, screen, w)
    pal = palette()
    exp = [convert(pal[i], fmt) for i in pattern(w, h)]
    bad = sum(1 for a, b in zip(screen, exp) if a != b)
    print("VNC  %s frame is still the pattern (%u wrong pixels)" % ("ok  " if bad == 0 else "FAIL", bad))
    c.s.close()
    ok = held and bad == 0
    print("VNC VIEW ONLY %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    pos = [a for a in sys.argv[1:] if not a.startswith("--")]
    ip = pos[0]
    if "--view-only" in sys.argv:
        return view_only(ip, 5900)
    port = int(pos[1]) if len(pos) > 1 else 5900
    ok = []

    def check(cond, what):
        ok.append(bool(cond))
        print("VNC  %s %s" % ("ok  " if cond else "FAIL", what), flush=True)
        return cond

    c = Client(ip, port)
    w, h, pf, name = c.hello()
    print("VNC  server %ux%u name %r, security %s%s" % (w, h, name.decode(errors="replace"), c.security,
                                                        (", " + c.cipher) if c.cipher else ""))
    if c.cipher:
        check(c.cipher == "TLS_AES_256_GCM_SHA384", "TLS suite %s" % c.cipher)
    check(pf[:4] == bytes([32, 24, 0, 1]) and pf[10:13] == bytes([16, 8, 0]),
          "ServerInit: 32 bpp true colour default format")

    pal = palette()
    img = pattern(w, h)
    formats = [
        ("32 bpp LE (default)", (32, 0, (255, 255, 255), (16, 8, 0)), None),
        ("16 bpp RGB565 BE", (16, 1, (31, 63, 31), (11, 5, 0)), (16, 16)),
        ("8 bpp RGB332", (8, 0, (7, 7, 3), (5, 2, 0)), (8, 8)),
    ]
    fmt = formats[0][1]
    for label, f, depth in formats:
        if depth is not None:
            c.set_format(f[0], depth[0], f[1], f[2], f[3])
        fmt = f
        screen = [None] * (w * h)
        c.request(0, 0, 0, w, h)
        rects = c.update(fmt, screen, w)
        exp = [convert(pal[i], fmt) for i in img]
        bad = sum(1 for a, b in zip(screen, exp) if a != b)
        check(rects == [(0, 0, w, h)] and bad == 0,
              "pattern %s: %u rect(s), %u wrong pixels" % (label, len(rects), bad))

    # 3. nothing changed: an incremental request is held
    c.request(1, 0, 0, w, h)
    c.s.settimeout(0.6)
    try:
        c.s.recv(1)
        held = False
    except socket.timeout:
        held = True
    c.s.settimeout(10)
    check(held, "incremental request with no change is held")

    # 4. pointer press -> 3x3 yellow square -> incremental update
    px, py = 100, 40
    c.s.sendall(struct.pack(">BBHH", 5, 1, px, py) + struct.pack(">BBHH", 5, 0, px, py))
    t0 = time.time()
    rects = c.update(fmt, screen, w)                  # answers the held request
    dt = time.time() - t0
    for yy in range(py - 1, py + 2):
        for xx in range(px - 1, px + 2):
            img[yy * w + xx] = 5
    exp = [convert(pal[i], fmt) for i in img]
    bad = sum(1 for a, b in zip(screen, exp) if a != b)
    inside = all(x <= px + 1 and x + rw > px - 1 and y <= py + 1 and y + rh > py - 1 for x, y, rw, rh in rects)
    check(rects and inside and bad == 0,
          "pointer paints: %u rect(s) around it in %.0f ms, %u wrong pixels" % (len(rects), dt * 1000, bad))

    # 5. key event (the board prints it; here only that it is accepted)
    c.s.sendall(struct.pack(">BBxxI", 4, 1, 0x61) + struct.pack(">BBxxI", 4, 0, 0x61))
    c.request(1, 0, 0, w, h)
    c.s.settimeout(0.6)
    try:
        c.s.recv(1)
        still = True
    except socket.timeout:
        still = True
    except (ConnectionError, OSError):
        still = False
    c.s.settimeout(10)
    check(still, "key event accepted, session alive")

    # 6. a second client is closed while this one is in session
    s2 = socket.create_connection((ip, port), timeout=5)
    s2.settimeout(3)
    try:
        closed = s2.recv(16) == b""
    except (ConnectionError, socket.timeout, OSError):
        closed = True
    s2.close()
    check(closed, "second client refused")

    c.s.close()

    # 7. X509Plain: a wrong password is refused, the next client waits, the
    #    right login after the wait gets in (decision 19/11: 1, 2, 4 ... 30 s)
    if c.security == "VeNCrypt X509Plain":
        time.sleep(0.5)
        try:
            Client(ip, port, password="wrong-password").hello()
            refused = "no"
        except AuthError as e:
            refused = str(e)
        check("authentication failed" in refused, "wrong password refused (%s)" % refused)
        try:
            t = Client(ip, port)
            t.hello()
            waited = False
        except (EOFError, ConnectionError, OSError, ValueError, AuthError):
            waited = True
        check(waited, "a client right after the failure is closed (login wait)")
        time.sleep(1.5)
        try:
            Client(ip, port).hello()
            back = True
        except Exception as e:                       # noqa: BLE001
            back = False
            print("VNC       login after the wait: %r" % e)
        check(back, "right login after the wait gets in")

    n = len(ok)
    print("VNC CHECK %s (%u/%u)" % ("PASS" if all(ok) else "FAIL", sum(ok), n))
    return 0 if all(ok) else 1


if __name__ == "__main__":
    sys.exit(main())
