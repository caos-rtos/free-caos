#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
"""
ssh_flow_test.py - checks that the ex67 SSH server obeys the CLIENT's flow
control on the shell channel (RFC 4254 5.1, 5.2).

OpenSSH cannot be used for this: it always grants a 2 MiB window and a 32 KiB
max packet, so a server that ignores both looks perfect. This is a minimal SSH
client (curve25519-sha256, ssh-ed25519, aes256-gcm@openssh.com - exactly what
the board offers) that opens the session channel with a SMALL window and a
SMALL max packet, runs one command with a long answer, and decrypts every
CHANNEL_DATA to check on the wire that:

  - no packet carries more than the max packet granted;
  - the server never sends past the window it has left;
  - when the window is spent the server WAITS: nothing arrives during a pause,
    and the output resumes after our WINDOW_ADJUST;
  - the answer arrives whole (its final line, then the next prompt).

The pause is kept short and the window not too small: the board's CLI capture
allows about 2 s to run the command plus 2 s to drain its output, and while
the SSH sender waits for window the stdout queue fills and DROPS lines
("stdout: warning queue is full!" in the output). Measured on F767 10/11/2026:
window 1024 passes (3 stalls), window 256 loses lines (13 stalls) - a limit of
the stdout path, not of the SSH flow control, which held in both.

Usage: ssh_flow_test.py [--host H] [--key FILE] [--window N] [--maxpkt N]
                        [--stall S] [--cmd CMD] [--end TEXT]
Prints one line "PASS ..." or "FAIL ..." and exits 0 / 1.
"""
import argparse
import hashlib
import os
import socket
import struct
import sys
import time

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

MSG_DISCONNECT, MSG_IGNORE, MSG_DEBUG = 1, 2, 4
MSG_SERVICE_REQUEST, MSG_SERVICE_ACCEPT = 5, 6
MSG_KEXINIT, MSG_NEWKEYS = 20, 21
MSG_KEX_ECDH_INIT, MSG_KEX_ECDH_REPLY = 30, 31
MSG_USERAUTH_REQUEST, MSG_USERAUTH_FAILURE, MSG_USERAUTH_SUCCESS = 50, 51, 52
MSG_GLOBAL_REQUEST, MSG_REQUEST_FAILURE = 80, 82
MSG_CHANNEL_OPEN, MSG_CHANNEL_OPEN_CONFIRMATION = 90, 91
MSG_CHANNEL_WINDOW_ADJUST, MSG_CHANNEL_DATA = 93, 94
MSG_CHANNEL_EOF, MSG_CHANNEL_CLOSE, MSG_CHANNEL_REQUEST = 96, 97, 98
MSG_CHANNEL_SUCCESS, MSG_CHANNEL_FAILURE = 99, 100


class Fail(Exception):
    pass


# ---- wire encoding ---------------------------------------------------------
def u32(v):
    return struct.pack(">I", v)


def sstr(b):
    if isinstance(b, str):
        b = b.encode()
    return u32(len(b)) + b


def mpint(b):
    b = b.lstrip(b"\0")
    if b and b[0] & 0x80:
        b = b"\0" + b
    return sstr(b)


class Reader:
    def __init__(self, b):
        self.b, self.o = b, 0

    def byte(self):
        if self.o + 1 > len(self.b):
            raise Fail("short packet")
        self.o += 1
        return self.b[self.o - 1]

    def u32(self):
        if self.o + 4 > len(self.b):
            raise Fail("short packet")
        self.o += 4
        return struct.unpack(">I", self.b[self.o - 4:self.o])[0]

    def str(self):
        n = self.u32()
        if self.o + n > len(self.b):
            raise Fail("short packet")
        self.o += n
        return self.b[self.o - n:self.o]


# ---- transport -------------------------------------------------------------
class Conn:
    def __init__(self, host, port, timeout):
        self.s = socket.create_connection((host, port), timeout)
        self.s.settimeout(timeout)
        self.buf = b""
        self.enc_out = self.enc_in = None

    def recv_exact(self, n):
        while len(self.buf) < n:
            d = self.s.recv(4096)
            if not d:
                raise Fail("connection closed by the board")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv_line(self):
        while b"\n" not in self.buf:
            d = self.s.recv(4096)
            if not d:
                raise Fail("connection closed during the version exchange")
            self.buf += d
        line, self.buf = self.buf.split(b"\n", 1)
        return line.rstrip(b"\r")

    @staticmethod
    def _nonce(st):
        return st["fixed"] + struct.pack(">Q", st["ctr"])

    def send(self, payload):
        if self.enc_out is None:
            blk = 8
            pad = blk - ((5 + len(payload)) % blk)
            if pad < 4:
                pad += blk
            pkt = u32(1 + len(payload) + pad) + bytes([pad]) + payload + os.urandom(pad)
        else:
            st = self.enc_out
            pad = 16 - ((1 + len(payload)) % 16)
            if pad < 4:
                pad += 16
            plen = 1 + len(payload) + pad
            aad = u32(plen)
            ct = st["aes"].encrypt(self._nonce(st), bytes([pad]) + payload + os.urandom(pad), aad)
            st["ctr"] = (st["ctr"] + 1) & 0xFFFFFFFFFFFFFFFF
            pkt = aad + ct
        self.s.sendall(pkt)

    def recv(self):
        if self.enc_in is None:
            plen = struct.unpack(">I", self.recv_exact(4))[0]
            if plen > 35000:
                raise Fail("plain packet length %u" % plen)
            body = self.recv_exact(plen)
        else:
            st = self.enc_in
            aad = self.recv_exact(4)
            plen = struct.unpack(">I", aad)[0]
            if plen > 35000 or plen % 16:
                raise Fail("encrypted packet length %u" % plen)
            ct = self.recv_exact(plen + 16)
            try:
                body = st["aes"].decrypt(self._nonce(st), ct, aad)
            except Exception:
                raise Fail("GCM tag check failed on a packet from the board")
            st["ctr"] = (st["ctr"] + 1) & 0xFFFFFFFFFFFFFFFF
        pad = body[0]
        return body[1:len(body) - pad]

    def recv_msg(self):
        """Next packet, skipping IGNORE/DEBUG."""
        while True:
            p = self.recv()
            if p and p[0] in (MSG_IGNORE, MSG_DEBUG):
                continue
            if p and p[0] == MSG_DISCONNECT:
                raise Fail("the board sent DISCONNECT")
            return p


def kexinit():
    lists = ["curve25519-sha256", "ssh-ed25519",
             "aes256-gcm@openssh.com", "aes256-gcm@openssh.com",
             "hmac-sha2-256", "hmac-sha2-256", "none", "none", "", ""]
    p = bytes([MSG_KEXINIT]) + os.urandom(16)
    for n in lists:
        p += sstr(n)
    return p + b"\0" + u32(0)


def handshake(c, key):
    vc = b"SSH-2.0-caOS_flow_test"
    c.s.sendall(vc + b"\r\n")
    while True:
        vs = c.recv_line()
        if vs.startswith(b"SSH-"):
            break
    ic = kexinit()
    c.send(ic)
    i_s = c.recv_msg()
    if not i_s or i_s[0] != MSG_KEXINIT:
        raise Fail("no KEXINIT from the board")

    eph = x25519.X25519PrivateKey.generate()
    qc = eph.public_key().public_bytes(serialization.Encoding.Raw,
                                       serialization.PublicFormat.Raw)
    c.send(bytes([MSG_KEX_ECDH_INIT]) + sstr(qc))
    rep = c.recv_msg()
    if not rep or rep[0] != MSG_KEX_ECDH_REPLY:
        raise Fail("no KEX_ECDH_REPLY from the board")
    r = Reader(rep)
    r.byte()
    ks, qs, sig = r.str(), r.str(), r.str()
    k = eph.exchange(x25519.X25519PublicKey.from_public_bytes(qs))
    kenc = mpint(k)
    h = hashlib.sha256(sstr(vc) + sstr(vs) + sstr(ic) + sstr(i_s) +
                       sstr(ks) + sstr(qc) + sstr(qs) + kenc).digest()
    rk, rs = Reader(ks), Reader(sig)
    if rk.str() != b"ssh-ed25519" or rs.str() != b"ssh-ed25519":
        raise Fail("host key or signature is not ssh-ed25519")
    try:
        ed25519.Ed25519PublicKey.from_public_bytes(rk.str()).verify(rs.str(), h)
    except Exception:
        raise Fail("the board's host key signature does not verify")

    c.send(bytes([MSG_NEWKEYS]))
    nk = c.recv_msg()
    if not nk or nk[0] != MSG_NEWKEYS:
        raise Fail("no NEWKEYS from the board")

    sid = h

    def derive(letter, n):
        return hashlib.sha256(kenc + h + letter + sid).digest()[:n]

    iv_c, iv_s = derive(b"A", 12), derive(b"B", 12)
    c.enc_out = {"aes": AESGCM(derive(b"C", 32)), "fixed": iv_c[:4],
                 "ctr": struct.unpack(">Q", iv_c[4:])[0]}
    c.enc_in = {"aes": AESGCM(derive(b"D", 32)), "fixed": iv_s[:4],
                "ctr": struct.unpack(">Q", iv_s[4:])[0]}

    c.send(bytes([MSG_SERVICE_REQUEST]) + sstr("ssh-userauth"))
    p = c.recv_msg()
    if not p or p[0] != MSG_SERVICE_ACCEPT:
        raise Fail("ssh-userauth service not accepted")

    pub = key.public_key().public_bytes(serialization.Encoding.Raw,
                                        serialization.PublicFormat.Raw)
    blob = sstr("ssh-ed25519") + sstr(pub)
    body = (sstr("caos") + sstr("ssh-connection") + sstr("publickey") + b"\1" +
            sstr("ssh-ed25519") + sstr(blob))
    signed = sstr(sid) + bytes([MSG_USERAUTH_REQUEST]) + body
    sigblob = sstr("ssh-ed25519") + sstr(key.sign(signed))
    c.send(bytes([MSG_USERAUTH_REQUEST]) + body + sstr(sigblob))
    p = c.recv_msg()
    if not p or p[0] != MSG_USERAUTH_SUCCESS:
        raise Fail("public-key authentication refused (msg %s)" % (p[0] if p else None))


# ---- the flow-control check ------------------------------------------------
def run(a):
    with open(a.key, "rb") as f:
        key = serialization.load_ssh_private_key(f.read(), password=None)
    c = Conn(a.host, 22, 10.0)
    handshake(c, key)

    c.send(bytes([MSG_CHANNEL_OPEN]) + sstr("session") + u32(0) +
           u32(a.window) + u32(a.maxpkt))
    p = c.recv_msg()
    if not p or p[0] != MSG_CHANNEL_OPEN_CONFIRMATION:
        raise Fail("session channel not opened")
    r = Reader(p)
    r.byte()
    r.u32()
    peer = r.u32()

    st = {"left": a.window, "pkts": 0, "bytes": 0, "max": 0, "stalls": 0}
    out = bytearray()

    def on_data(pl):
        r = Reader(pl)
        r.byte()
        r.u32()
        d = r.str()
        n = len(d)
        if n == 0:
            raise Fail("empty CHANNEL_DATA")
        if n > a.maxpkt:
            raise Fail("CHANNEL_DATA of %u bytes, max packet granted %u" % (n, a.maxpkt))
        if n > st["left"]:
            raise Fail("CHANNEL_DATA of %u bytes with %u left in the window "
                       "(overrun after %u bytes)" % (n, st["left"], st["bytes"]))
        st["left"] -= n
        st["pkts"] += 1
        st["bytes"] += n
        st["max"] = max(st["max"], n)
        out.extend(d)

    def handle(p):
        t = p[0]
        if t == MSG_CHANNEL_DATA:
            on_data(p)
        elif t == MSG_GLOBAL_REQUEST:
            r = Reader(p)
            r.byte()
            r.str()
            if r.byte():
                c.send(bytes([MSG_REQUEST_FAILURE]))
        elif t == MSG_CHANNEL_CLOSE:
            raise Fail("the board closed the channel")
        # WINDOW_ADJUST for our direction, SUCCESS/FAILURE, EOF: nothing to do

    def pump(until, start, deadline):
        """Read until `until` is in the output past `start`, reopening the
        window when it is spent - after a pause in which nothing may arrive."""
        while out.find(until, start) < 0:
            if out.find(b"caOS> ", start) >= 0 and out.find(b"queue is full", start) >= 0:
                raise Fail("the answer ended without %r and the board printed "
                           "'stdout: warning queue is full!': lines were DROPPED "
                           "by the stdout queue while the sender waited for window "
                           "(flow control itself held: %u bytes, %u stalls, no "
                           "overrun)" % (until, len(out), st["stalls"]))
            if time.time() > deadline:
                raise Fail("timed out waiting for %r; got %u bytes, last: %r"
                           % (until, len(out), bytes(out[-120:])))
            if st["left"] == 0:
                # The window is spent: anything the board sends now is an
                # overrun, which on_data reports. Then reopen it.
                c.s.settimeout(a.stall)
                try:
                    while True:
                        handle(c.recv_msg())
                except socket.timeout:
                    pass
                c.s.settimeout(10.0)
                st["stalls"] += 1
                c.send(bytes([MSG_CHANNEL_WINDOW_ADJUST]) + u32(peer) + u32(a.window))
                st["left"] += a.window
                continue
            try:
                handle(c.recv_msg())
            except socket.timeout:
                if a.dump:
                    with open(a.dump, "wb") as f:
                        f.write(bytes(out))
                raise Fail("the board went silent waiting for %r with %u bytes "
                           "of window left; %u bytes, %u stalls so far, last: %r"
                           % (until, st["left"], len(out), st["stalls"],
                              bytes(out[-120:])))

    c.send(bytes([MSG_CHANNEL_REQUEST]) + u32(peer) + sstr("shell") + b"\1")
    pump(b"caOS> ", 0, time.time() + 15)
    mark = len(out)

    c.send(bytes([MSG_CHANNEL_DATA]) + u32(peer) + sstr(a.cmd + "\r"))
    pump(a.end.encode(), mark, time.time() + 30)
    # and the prompt after the answer: the tail of the output was not lost
    pump(b"caOS> ", out.find(a.end.encode(), mark), time.time() + 10)
    answer = len(out) - mark
    if a.dump:
        with open(a.dump, "wb") as f:
            f.write(bytes(out))

    try:
        c.send(bytes([MSG_CHANNEL_CLOSE]) + u32(peer))
        c.s.close()
    except OSError:
        pass

    if st["stalls"] == 0:
        raise Fail("the window was never spent (%u bytes of answer, window %u): "
                   "the wait for WINDOW_ADJUST was not exercised - use a longer "
                   "command or a smaller --window" % (answer, a.window))
    return ("PASS window=%u maxpkt=%u answer=%u bytes data_pkts=%u largest=%u stalls=%u"
            % (a.window, a.maxpkt, answer, st["pkts"], st["max"], st["stalls"]))


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default="192.168.10.67")
    ap.add_argument("--key", default=os.path.join(root, "examples", "caos_ed25519", "id_ed25519"))
    ap.add_argument("--window", type=int, default=1024)
    # 32: the board writes line by line (up to ~50 bytes), so a larger max
    # packet would never actually split anything
    ap.add_argument("--maxpkt", type=int, default=32)
    ap.add_argument("--stall", type=float, default=0.3)
    ap.add_argument("--cmd", default="taskall")
    ap.add_argument("--end", default="END TASK LIST")
    ap.add_argument("--dump", help="write the channel output to this file")
    a = ap.parse_args()
    try:
        print(run(a))
        return 0
    except (Fail, OSError, socket.timeout) as e:
        print("FAIL %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
