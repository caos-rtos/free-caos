#!/usr/bin/env python3
"""
tls_strict.py - raw TLS 1.3 ClientHello phases for caOS example 45 (HTTPS)

Called by http_strict.py (http_test.py --strict <phase>). No crypto here: a
phase only has to get the board's FIRST answer to a ClientHello and look at
the record type.

The ClientHello is a real one: it is captured at run time from Python's own
ssl module connecting to a local listening socket, so there is no blob to go
stale. A small parser/rewriter takes it apart (record, handshake, fields,
extensions), lets a phase change one thing, and puts it back with every length
recomputed. Run as a script it self-tests that: a parse + rebuild must give the
captured bytes back unchanged.

Phases:
  tls_record_version  the legacy_record_version of the record is ignored on
                      receive (RFC 8446 5.1): 0x0301 (what OpenSSL sends),
                      0x0303 and 0x0201 all get a ServerHello (record 22)
  tls_strict_hello    a ClientHello that breaks a MUST gets a fatal alert
                      (record 21, level 2) and a close, and the next normal
                      session completes:
                        compression not [null]   illegal_parameter  47
                        no supported_groups      missing_extension  109
                        ALPN length incoherent   decode_error       50
"""

import socket
import ssl
import struct
import threading
import time

REC_ALERT, REC_HANDSHAKE = 21, 22
HS_SERVER_HELLO = 2
EXT_GROUPS, EXT_ALPN = 0x000A, 0x0010
AL_ILLEGAL_PARAM, AL_DECODE_ERROR, AL_MISSING_EXT = 47, 50, 109
SHOW = 400


# ---------------------------------------------------------------------------
# ClientHello: capture, parse, rebuild
# ---------------------------------------------------------------------------
def capture_client_hello(alpn=("http/1.1",), sni="caos.local", timeout=5):
    """The first TLS record of a real ssl client, as bytes (5-byte header + body)."""
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", 0))
    ls.listen(1)
    ls.settimeout(timeout)
    port = ls.getsockname()[1]

    def client():
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.maximum_version = ssl.TLSVersion.TLSv1_3
        ctx.set_alpn_protocols(list(alpn))
        try:
            with socket.create_connection(("127.0.0.1", port), timeout) as c:
                c.settimeout(timeout)
                with ctx.wrap_socket(c, server_hostname=sni):
                    pass
        except (OSError, ssl.SSLError):
            pass                       # expected: nobody answers

    th = threading.Thread(target=client, daemon=True)
    th.start()
    try:
        conn, _ = ls.accept()
    finally:
        ls.close()
    conn.settimeout(timeout)
    try:
        head = _read_exact(conn, 5)
        body = _read_exact(conn, struct.unpack(">H", head[3:5])[0])
    finally:
        conn.close()
    th.join(timeout)
    return head + body


def _read_exact(s, n):
    out = b""
    while len(out) < n:
        b = s.recv(n - len(out))
        if not b:
            raise ValueError("connection closed after %d of %d bytes" % (len(out), n))
        out += b
    return out


class Hello:
    """A ClientHello in parts. exts is a list of [type, data] (order kept)."""

    def __init__(self):
        self.rec_ver = 0x0301
        self.hs_ver = 0x0303
        self.random = b""
        self.sid = b""
        self.ciphers = b""
        self.comp = b"\x00"
        self.exts = []

    def ext(self, etype):
        for e in self.exts:
            if e[0] == etype:
                return e
        return None

    def drop_ext(self, etype):
        self.exts = [e for e in self.exts if e[0] != etype]

    def set_ext(self, etype, data):
        e = self.ext(etype)
        if e:
            e[1] = data
        else:
            self.exts.append([etype, data])


def parse_hello(rec):
    if len(rec) < 9 or rec[0] != REC_HANDSHAKE:
        raise ValueError("not a handshake record")
    h = Hello()
    h.rec_ver = struct.unpack(">H", rec[1:3])[0]
    rlen = struct.unpack(">H", rec[3:5])[0]
    if len(rec) != 5 + rlen:
        raise ValueError("record length %d does not match %d bytes" % (rlen, len(rec) - 5))
    hs = rec[5:]
    if hs[0] != 1:
        raise ValueError("not a ClientHello")
    hlen = int.from_bytes(hs[1:4], "big")
    if len(hs) != 4 + hlen:
        raise ValueError("handshake length mismatch (fragmented ClientHello?)")
    p = hs[4:]
    h.hs_ver = struct.unpack(">H", p[0:2])[0]
    h.random = p[2:34]
    i = 34
    n = p[i]; i += 1
    h.sid = p[i:i + n]; i += n
    n = struct.unpack(">H", p[i:i + 2])[0]; i += 2
    h.ciphers = p[i:i + n]; i += n
    n = p[i]; i += 1
    h.comp = p[i:i + n]; i += n
    el = struct.unpack(">H", p[i:i + 2])[0]; i += 2
    ext = p[i:]
    if len(ext) != el:
        raise ValueError("extensions length mismatch")
    j = 0
    while j < len(ext):
        et, ln = struct.unpack(">HH", ext[j:j + 4])
        h.exts.append([et, ext[j + 4:j + 4 + ln]])
        j += 4 + ln
    if j != len(ext):
        raise ValueError("extension overruns the block")
    return h


def build_hello(h, rec_ver=None):
    ext = b"".join(struct.pack(">HH", t, len(d)) + d for t, d in h.exts)
    body = (struct.pack(">H", h.hs_ver) + h.random + bytes([len(h.sid)]) + h.sid +
            struct.pack(">H", len(h.ciphers)) + h.ciphers +
            bytes([len(h.comp)]) + h.comp + struct.pack(">H", len(ext)) + ext)
    hs = b"\x01" + len(body).to_bytes(3, "big") + body
    rv = h.rec_ver if rec_ver is None else rec_ver
    return bytes([REC_HANDSHAKE]) + struct.pack(">HH", rv, len(hs)) + hs


def alpn_data(names, list_len_delta=0, name_len_delta=None):
    """ALPN extension data. list_len_delta != 0 makes the list length lie;
    name_len_delta (on the first name) makes a name overrun its list."""
    items = b""
    for k, nm in enumerate(names):
        nl = len(nm) + (name_len_delta if (k == 0 and name_len_delta) else 0)
        items += bytes([nl]) + nm
    return struct.pack(">H", len(items) + list_len_delta) + items


# ---------------------------------------------------------------------------
# Talking to the board
# ---------------------------------------------------------------------------
class Probe:
    def __init__(self):
        self.rtype = None
        self.rver = None
        self.payload = b""
        self.raw = b""
        self.closed = False
        self.err = None


def probe(ip, port, data, wait=5.0, linger=2.0):
    """Send `data`, read the first record. If it is an alert, also see whether
    the board then closes the connection (within `linger` s)."""
    p = Probe()
    s = socket.create_connection((ip, port), 8)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        s.sendall(data)
        s.settimeout(0.25)
        deadline = time.monotonic() + wait
        buf = b""
        while time.monotonic() < deadline:
            if len(buf) >= 5 and len(buf) >= 5 + struct.unpack(">H", buf[3:5])[0]:
                break
            try:
                b = s.recv(4096)
            except socket.timeout:
                continue
            except OSError as e:
                p.err = str(e)
                p.closed = True
                break
            if not b:
                p.closed = True
                break
            buf += b
        p.raw = buf
        if len(buf) >= 5:
            p.rtype = buf[0]
            p.rver = struct.unpack(">H", buf[1:3])[0]
            ln = struct.unpack(">H", buf[3:5])[0]
            p.payload = buf[5:5 + ln]
            if p.rtype == REC_ALERT and not p.closed:
                end = time.monotonic() + linger
                while time.monotonic() < end:
                    try:
                        b = s.recv(4096)
                    except socket.timeout:
                        continue
                    except OSError:
                        p.closed = True
                        break
                    if not b:
                        p.closed = True
                        break
                    p.raw += b
    finally:
        s.close()
    return p


def _show(raw):
    return repr(raw[:SHOW]) + (" ...(+%d)" % (len(raw) - SHOW) if len(raw) > SHOW else "")


def _fail(res, text, raw=None):
    res.fail(text, raw)


def _import_result():
    from http_strict import Result
    return Result()


def good_session(ip, port, res, label):
    """A complete TLS 1.3 session + GET /status must work right after."""
    from http_strict import Target, exchange, build, parse_response
    try:
        rp = exchange(Target(ip, "https", port), [(build(b"GET /status HTTP/1.1"), 0)], wait=8)
    except Exception as e:
        _fail(res, "%s: the normal TLS session that follows failed: %s: %s"
              % (label, type(e).__name__, e))
        return
    rs = parse_response(rp.raw)
    if not rs.ok or rs.status != 200:
        _fail(res, "%s: the normal TLS session that follows got no 200" % label, rp.raw)


# ---------------------------------------------------------------------------
# Phases (ip, port) -> Result
# ---------------------------------------------------------------------------
def phase_tls_record_version(ip, port):
    res = _import_result()
    hello = parse_hello(capture_client_hello())
    rec = build_hello(hello)
    for label, ver in (("0x0301 as captured (control)", None), ("0x0303", 0x0303),
                       ("0x0201", 0x0201)):
        pr = probe(ip, port, build_hello(hello, ver))
        if pr.rtype != REC_HANDSHAKE or not pr.payload or pr.payload[0] != HS_SERVER_HELLO:
            what = ("an ALERT %r" % pr.payload[:2]) if pr.rtype == REC_ALERT else (
                "nothing" if pr.rtype is None else "record type %d" % pr.rtype)
            _fail(res, "record version %s: expected a ServerHello (record 22), "
                       "got %s" % (label, what), pr.raw)
        time.sleep(0.3)
    res.note("ClientHello of %d bytes, record versions 0x0301 0x0303 0x0201 all got a ServerHello"
             % len(rec))
    return res


def _variants(hello):
    """(label, ClientHello bytes, expected alert description)."""
    out = []

    def mod(fn):
        h = parse_hello(build_hello(hello))
        fn(h)
        return build_hello(h)

    out.append(("compression [1] (not null)",
                mod(lambda h: setattr(h, "comp", b"\x01")), AL_ILLEGAL_PARAM))
    out.append(("compression [0, 1]",
                mod(lambda h: setattr(h, "comp", b"\x00\x01")), AL_ILLEGAL_PARAM))
    out.append(("supported_groups removed",
                mod(lambda h: h.drop_ext(EXT_GROUPS)), AL_MISSING_EXT))
    out.append(("ALPN list length 3 too long",
                mod(lambda h: h.set_ext(EXT_ALPN, alpn_data([b"http/1.1"], list_len_delta=3))),
                AL_DECODE_ERROR))
    out.append(("ALPN name length overruns the list",
                mod(lambda h: h.set_ext(EXT_ALPN, alpn_data([b"http/1.1"], name_len_delta=5))),
                AL_DECODE_ERROR))
    out.append(("supported_groups of odd length",
                mod(lambda h: h.set_ext(EXT_GROUPS, b"\x00\x03\x00\x1d\x00")),
                AL_DECODE_ERROR))
    return out


def phase_tls_strict_hello(ip, port):
    res = _import_result()
    hello = parse_hello(capture_client_hello())
    if hello.ext(EXT_ALPN) is None:
        hello.set_ext(EXT_ALPN, alpn_data([b"http/1.1"]))
    for label, data, want in _variants(hello):
        pr = probe(ip, port, data)
        if pr.rtype != REC_ALERT:
            what = "nothing" if pr.rtype is None else (
                "a ServerHello" if pr.rtype == REC_HANDSHAKE else "record type %d" % pr.rtype)
            _fail(res, "%s: expected a fatal alert (record 21), got %s" % (label, what), pr.raw)
        elif len(pr.payload) != 2 or pr.payload[0] != 2:
            _fail(res, "%s: alert is not fatal level 2 and 2 bytes: %r" % (label, pr.payload), pr.raw)
        else:
            if pr.payload[1] != want:
                _fail(res, "%s: alert description %d, RFC 8446 says %d"
                      % (label, pr.payload[1], want), pr.raw)
            if not pr.closed:
                _fail(res, "%s: connection not closed after the fatal alert" % label, pr.raw)
        time.sleep(0.3)
        good_session(ip, port, res, label)
    res.note("%d malformed ClientHellos, each refused with the fatal alert, a good session after each"
             % len(_variants(hello)))
    return res


TLS_PHASES = [
    ("tls_record_version", phase_tls_record_version),
    ("tls_strict_hello", phase_tls_strict_hello),
]


if __name__ == "__main__":
    rec = capture_client_hello()
    h = parse_hello(rec)
    assert build_hello(h) == rec, "rewriter does not rebuild the captured ClientHello"
    print("captured ClientHello: %d bytes, record version 0x%04x, %d extensions %s"
          % (len(rec), h.rec_ver, len(h.exts), [hex(e[0]) for e in h.exts]))
    print("round trip OK")
