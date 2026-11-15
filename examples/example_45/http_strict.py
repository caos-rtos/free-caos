#!/usr/bin/env python3
"""
http_strict.py - raw-socket strict phases for caOS example 45 (HTTP + HTTPS)

Not run directly: http_test.py --strict <phase> calls it. The bench
(test/hw_test/src/test_ex45.cpp) drives it, one phase per call.

Why raw sockets: requests, http.client and curl hide exactly the defects these
phases look for (they drop extra bytes, tolerate malformed headers, refuse to
send malformed requests). Every phase here sends the bytes itself and checks
the reply byte by byte: status line, header lines, CRLF CRLF, body length.

A phase takes the TRANSPORT as a parameter: plain TCP on port 80, or a TLS 1.3
socket on port 443 (certificate not verified here: the bench's own HTTPS phase
verifies it with curl). Both go through ca_http_serve_connection on the board.

Output, one block per phase and transport:
  PHASE <name> <http|https> PASS|FAIL|SKIP <note>
  FAIL <what is wrong>
    got: <bytes received, truncated>
  SUMMARY pass=<n> fail=<n> skip=<n>
Exit code: 0 all passed, 1 at least one failure, 77 nothing ran (all skipped).
"""

import shutil
import socket
import ssl
import subprocess
import sys
import time

HTTP_PORT = 80
HTTPS_PORT = 443
# HTTP_RECV_TIMEOUT_MS on the board (caos/net/inc/http_service.h)
BOARD_RECV_TIMEOUT_S = 1.5
# Past the board's timeout, with room for a slow reply
SILENCE_WAIT_S = BOARD_RECV_TIMEOUT_S + 3.5
SHOW = 400   # bytes of a bad reply printed

BASE_HDRS = [(b"Host", b"caos.local"), (b"User-Agent", b"caos-strict/1")]


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
class Target:
    def __init__(self, ip, kind, port):
        self.ip, self.kind, self.port = ip, kind, port

    def connect(self, timeout=8):
        s = socket.create_connection((self.ip, self.port), timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if self.kind == "https":
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            ctx.minimum_version = ssl.TLSVersion.TLSv1_3
            ctx.maximum_version = ssl.TLSVersion.TLSv1_3
            s.settimeout(timeout)
            s = ctx.wrap_socket(s, server_hostname="caos.local")
        return s


class Reply:
    def __init__(self):
        self.raw = b""
        self.eof = False
        self.err = None
        self.t_done = None      # seconds from first send to EOF


def exchange(t, segments, wait=SILENCE_WAIT_S):
    """Send [(bytes, delay_before_s), ...], then read until EOF or `wait` s."""
    s = t.connect()
    r = Reply()
    t0 = time.monotonic()
    try:
        for data, delay in segments:
            if delay:
                time.sleep(delay)
            s.sendall(data)
    except OSError as e:
        r.err = "send: %s" % e
    deadline = time.monotonic() + wait
    s.settimeout(0.25)
    while time.monotonic() < deadline:
        try:
            b = s.recv(4096)
        except socket.timeout:
            continue
        except ssl.SSLWantReadError:
            continue
        except (ssl.SSLZeroReturnError, ssl.SSLEOFError):
            r.eof = True
            r.t_done = time.monotonic() - t0
            break
        except OSError as e:
            r.err = "recv: %s" % e
            r.eof = True
            r.t_done = time.monotonic() - t0
            break
        if not b:
            r.eof = True
            r.t_done = time.monotonic() - t0
            break
        r.raw += b
    try:
        s.close()
    except OSError:
        pass
    return r


# ---------------------------------------------------------------------------
# Building requests and parsing replies
# ---------------------------------------------------------------------------
def build(line, hdrs=None, body=b"", blank=True):
    """One request as bytes. `line` without CRLF; hdrs = [(name, value)]."""
    out = line + b"\r\n"
    for n, v in (BASE_HDRS if hdrs is None else hdrs):
        out += n + b": " + v + b"\r\n"
    if blank:
        out += b"\r\n"
    return out + body


def post_cli(cmd=b"cmd=free", cl=None, extra=b"", hdrs=None):
    h = list(BASE_HDRS) if hdrs is None else list(hdrs)
    if cl is not None:
        h.append((b"Content-Length", str(cl).encode()))
    return build(b"POST /cli HTTP/1.1", h, cmd + extra)


class Resp:
    def __init__(self):
        self.ok = False
        self.why = ""
        self.status = None
        self.reason = b""
        self.headers = []
        self.body = b""

    def hdr(self, name):
        n = name.lower()
        return [v for k, v in self.headers if k.lower() == n]

    def content_length(self):
        v = self.hdr(b"content-length")
        if len(v) != 1:
            return None
        try:
            return int(v[0].strip(b" \t"))
        except ValueError:
            return None


def parse_response(raw):
    """Strict parse. resp.ok False + resp.why when the bytes are not HTTP."""
    r = Resp()
    if not raw:
        r.why = "no bytes received (connection closed without an answer)"
        return r
    i = raw.find(b"\r\n\r\n")
    if i < 0:
        r.why = "headers never ended: no CRLF CRLF in the reply"
        return r
    head, r.body = raw[:i], raw[i + 4:]
    lines = head.split(b"\r\n")
    sl = lines[0]
    if (not sl.startswith(b"HTTP/1.1 ") or len(sl) < 12 or
            not sl[9:12].isdigit() or (len(sl) > 12 and sl[12:13] != b" ")):
        r.why = "malformed status line %r" % sl[:60]
        return r
    r.status = int(sl[9:12])
    r.reason = sl[13:]
    for ln in lines[1:]:
        if b"\n" in ln or b"\r" in ln:
            r.why = "bare CR or LF inside a header line %r" % ln[:60]
            return r
        k, sep, v = ln.partition(b":")
        if not sep or not k or k != k.strip():
            r.why = "malformed header line %r" % ln[:60]
            return r
        r.headers.append((k, v.strip(b" \t")))
    r.ok = True
    return r


class Result:
    def __init__(self):
        self.problems = []   # (text, raw bytes or None)
        self.notes = []
        self.skips = []

    def fail(self, text, raw=None):
        self.problems.append((text, raw))

    def note(self, text):
        self.notes.append(text)


def framed(res, label, reply, resp, head=False):
    """The reply parses, and (unless HEAD) its body is exactly Content-Length."""
    if not resp.ok:
        res.fail("%s: %s" % (label, resp.why), reply.raw)
        return False
    cl = resp.content_length()
    if not head:
        if cl is None:
            res.fail("%s: no single valid Content-Length" % label, reply.raw)
            return False
        if len(resp.body) != cl:
            res.fail("%s: body is %d bytes but Content-Length says %d"
                     % (label, len(resp.body), cl), reply.raw)
            return False
    return True


def get_ok(t, path, res, label=None):
    label = label or "GET %s" % path
    rp = exchange(t, [(build(b"GET " + path + b" HTTP/1.1"), 0)])
    rs = parse_response(rp.raw)
    if framed(res, label, rp, rs) and rs.status != 200:
        res.fail("%s answered %d, not 200" % (label, rs.status), rp.raw)
        return None
    return rs if rs.ok and rs.status == 200 else None


def want_status(res, label, rp, status, head=False):
    rs = parse_response(rp.raw)
    if not framed(res, label, rp, rs, head):
        return rs
    if rs.status != status:
        res.fail("%s answered %d, expected %d" % (label, rs.status, status), rp.raw)
    return rs


def allow_set(rs):
    v = rs.hdr(b"allow")
    if len(v) != 1:
        return None
    return sorted(x.strip() for x in v[0].split(b","))


# ---------------------------------------------------------------------------
# Phases: each takes the Target and returns a Result
# ---------------------------------------------------------------------------
def phase_post_body_exact(t):
    """D1: the handler sees exactly Content-Length bytes after the blank line.
    ex45 has no body-echo route, so the body is read through POST /cli: the
    handler runs the command in `cmd=` and the output says what it saw."""
    res = Result()
    res.note("no echo route in ex45: body read through POST /cli (cmd=free)")

    # 8 declared, 12 sent: the handler must see "cmd=free" and not "...freeZZZZ"
    rp = exchange(t, [(post_cli(b"cmd=free", 8, b"ZZZZ"), 0)])
    rs = want_status(res, "CL 8, 12 bytes sent", rp, 200)
    if rs.ok and rs.status == 200 and b"Memory available" not in rs.body:
        res.fail("CL 8 with 4 trailing bytes: the handler did not run 'free' "
                 "cleanly (it saw more or other bytes than the 8 declared)", rp.raw)

    # 4 declared, 8 sent: the handler sees "cmd=" (empty command) -> 400
    rp = exchange(t, [(post_cli(b"cmd=", 4, b"free"), 0)])
    rs = want_status(res, "CL 4, 8 bytes sent", rp, 400)
    if rs.ok and b"Memory available" in rs.body:
        res.fail("CL 4: the handler ran 'free': bytes past Content-Length "
                 "reached it", rp.raw)
    return res


def phase_post_body_incomplete(t):
    """D1, D5: declared body never completes -> 400 after the server timeout,
    never 200 and the handler is not run."""
    res = Result()
    for label, body in (("3 of 20 bytes", b"abc"), ("8 of 20 bytes (cmd=free)", b"cmd=free")):
        rp = exchange(t, [(post_cli(body, 20), 0)])
        rs = parse_response(rp.raw)
        if not rs.ok:
            res.fail("CL 20, %s then silence: %s" % (label, rs.why), rp.raw)
            continue
        if rs.status == 200 or b"Memory available" in rs.body:
            res.fail("CL 20, %s then silence: served with status %d - an "
                     "incomplete body must never reach the handler"
                     % (label, rs.status), rp.raw)
        elif rs.status != 400:
            res.fail("CL 20, %s then silence: status %d, expected 400"
                     % (label, rs.status), rp.raw)
        framed(res, "CL 20, " + label, rp, rs)
    return res


def phase_options_wellformed(t):
    """D2: OPTIONS -> exactly 204, Allow, headers closed by CRLF CRLF, nothing after."""
    res = Result()
    want = {b"/": [b"GET", b"HEAD", b"OPTIONS"], b"/status": [b"GET", b"HEAD", b"OPTIONS"],
            b"/help": [b"GET", b"HEAD", b"OPTIONS"], b"/cli": [b"OPTIONS", b"POST"]}
    for path in sorted(want):
        label = "OPTIONS " + path.decode()
        rp = exchange(t, [(build(b"OPTIONS " + path + b" HTTP/1.1"), 0)])
        if not rp.raw.startswith(b"HTTP/1.1 204 No Content\r\n"):
            res.fail("%s: status line is not 'HTTP/1.1 204 No Content'" % label, rp.raw)
            continue
        rs = parse_response(rp.raw)
        if not rs.ok:
            res.fail("%s: %s" % (label, rs.why), rp.raw)
            continue
        if not rp.raw.endswith(b"\r\n\r\n"):
            res.fail("%s: reply does not end with CRLF CRLF" % label, rp.raw)
        if rs.body:
            res.fail("%s: %d byte(s) after the headers (a 204 has no body)"
                     % (label, len(rs.body)), rp.raw)
        got = allow_set(rs)
        if got != sorted(want[path]):
            res.fail("%s: Allow is %r, expected %r" % (label, got, sorted(want[path])), rp.raw)
        if not rp.eof:
            res.fail("%s: connection still open after the reply" % label, rp.raw)
    return res


def phase_content_length_strict(t):
    """D7: ambiguous or non-numeric Content-Length -> 400; lower-case name served."""
    res = Result()
    cases = [
        ("two different Content-Length (8 then 4)",
         [(b"Content-Length", b"8"), (b"Content-Length", b"4")]),
        ("Content-Length: 5abc", [(b"Content-Length", b"5abc")]),
        ("10-digit Content-Length", [(b"Content-Length", b"1000000000")]),
        ("Content-Length that wraps u32 to 8", [(b"Content-Length", b"4294967304")]),
    ]
    for label, cl in cases:
        rp = exchange(t, [(build(b"POST /cli HTTP/1.1", BASE_HDRS + cl, b"cmd=free"), 0)])
        rs = want_status(res, label, rp, 400)
        if rs.ok and (rs.status == 200 or b"Memory available" in rs.body):
            res.fail("%s was served" % label, rp.raw)
    rp = exchange(t, [(build(b"POST /cli HTTP/1.1",
                             BASE_HDRS + [(b"content-length", b"8")], b"cmd=free"), 0)])
    rs = want_status(res, "lower-case content-length: 8", rp, 200)
    if rs.ok and rs.status == 200 and b"Memory available" not in rs.body:
        res.fail("lower-case content-length: answered 200 but the body was not "
                 "seen (header name not matched case-insensitively)", rp.raw)
    return res


def phase_head_no_body(t):
    """D3, D4h: HEAD -> headers only; Content-Length equals the GET's."""
    res = Result()
    for path in (b"/", b"/help", b"/status"):
        label = path.decode()
        g = exchange(t, [(build(b"GET " + path + b" HTTP/1.1"), 0)])
        gs = parse_response(g.raw)
        if not framed(res, "GET " + label, g, gs) or gs.status != 200:
            res.fail("GET %s (the reference for HEAD) did not answer 200" % label, g.raw)
            continue
        h = exchange(t, [(build(b"HEAD " + path + b" HTTP/1.1"), 0)], wait=5)
        hs = parse_response(h.raw)
        if not framed(res, "HEAD " + label, h, hs, head=True):
            continue
        if hs.status != 200:
            res.fail("HEAD %s answered %d" % (label, hs.status), h.raw)
        if hs.body:
            res.fail("HEAD %s: %d byte(s) after the headers, expected 0 "
                     "(handler wrote its body anyway)" % (label, len(hs.body)), h.raw)
        if not h.eof:
            res.fail("HEAD %s: connection still open after the headers" % label, h.raw)
        cl_h, cl_g = hs.content_length(), gs.content_length()
        # /status carries the uptime: its length may change by a digit
        slack = 2 if path == b"/status" else 0
        if cl_h is None:
            res.fail("HEAD %s: no valid Content-Length" % label, h.raw)
        elif abs(cl_h - cl_g) > slack:
            res.fail("HEAD %s: Content-Length %d, the GET's is %d" % (label, cl_h, cl_g), h.raw)
        if hs.hdr(b"content-type") != gs.hdr(b"content-type"):
            res.fail("HEAD %s: Content-Type differs from the GET's" % label, h.raw)
    res.note("HEAD on a chunked route NOT covered: ex45 has no chunked route")
    return res


def phase_bad_request_400(t):
    """D5, D8: a request the parser must refuse gets a 400, not a silent close
    and not a 200."""
    res = Result()
    cases = [
        ("unknown method", [(build(b"FOO / HTTP/1.1"), 0)]),
        ("request line without version", [(build(b"GET /"), 0)]),
        ("empty target", [(build(b"GET  HTTP/1.1"), 0)]),
        ("HTTP/2.0", [(build(b"GET / HTTP/2.0"), 0)]),
        ("no blank line, then silence",
         [(build(b"GET / HTTP/1.1", blank=False), 0)]),
        ("bare LF line endings, then silence",
         [(b"GET / HTTP/1.1\nHost: caos.local\n\n", 0)]),
    ]
    for label, seg in cases:
        rp = exchange(t, seg)
        rs = want_status(res, label, rp, 400)
        if rs.ok and rs.status == 200:
            res.fail("%s was served with 200" % label, rp.raw)
    return res


def phase_allow_on_405(t):
    """D6: a method the route does not take -> 405 with Allow."""
    res = Result()
    cases = [("GET /cli", build(b"GET /cli HTTP/1.1"), [b"OPTIONS", b"POST"]),
             ("POST /", build(b"POST / HTTP/1.1", BASE_HDRS + [(b"Content-Length", b"0")]),
              [b"GET", b"HEAD", b"OPTIONS"]),
             ("POST /status", build(b"POST /status HTTP/1.1",
                                    BASE_HDRS + [(b"Content-Length", b"0")]),
              [b"GET", b"HEAD", b"OPTIONS"])]
    for label, req, allow in cases:
        rp = exchange(t, [(req, 0)])
        rs = want_status(res, label, rp, 405)
        if rs.ok and rs.status == 405:
            got = allow_set(rs)
            if got != sorted(allow):
                res.fail("%s: Allow is %r, expected %r" % (label, got, sorted(allow)), rp.raw)
    return res


def phase_split_request(t):
    """Not a regression of the strict parser: a request split over segments is
    served, and a plain GET does not wait for the server's timeout."""
    res = Result()
    cases = [
        ("line | header | blank line, 100 ms apart",
         [(b"GET /status HTTP/1.1\r\n", 0), (b"Host: caos.local\r\n", 0.1), (b"\r\n", 0.1)],
         b"uptime_s"),
        ("request line cut in the middle",
         [(b"GET /sta", 0), (b"tus HTTP/1.1\r\nHost: caos.local\r\n\r\n", 0.1)],
         b"uptime_s"),
        ("POST: headers first, body 100 ms later",
         [(build(b"POST /cli HTTP/1.1", BASE_HDRS + [(b"Content-Length", b"8")]), 0),
          (b"cmd=free", 0.1)],
         b"Memory available"),
    ]
    for label, seg, marker in cases:
        rp = exchange(t, seg)
        rs = want_status(res, label, rp, 200)
        if rs.ok and rs.status == 200 and marker not in rs.body:
            res.fail("%s: 200 but the body lacks %r" % (label, marker), rp.raw)

    # a normal GET answers at once: a server that waits for a body that is
    # never coming costs HTTP_RECV_TIMEOUT_MS (1.5 s). Best of three.
    best = None
    for _ in range(3):
        rp = exchange(t, [(build(b"GET /status HTTP/1.1"), 0)], wait=5)
        if rp.eof and rp.t_done is not None:
            best = rp.t_done if best is None else min(best, rp.t_done)
    if best is None:
        res.fail("plain GET /status never completed")
    elif best > 1.0:
        res.fail("plain GET /status took %.2f s at best: the server is waiting "
                 "for its %.1f s timeout before answering" % (best, BOARD_RECV_TIMEOUT_S))
    else:
        res.note("plain GET /status complete in %.0f ms (best of 3)" % (best * 1000))
    return res


# -- real clients ------------------------------------------------------------
def _run(cmd, timeout):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        return -1, b"", str(e).encode()


def _clients_curl(t, res):
    if not shutil.which("curl"):
        res.skips.append("curl: not installed")
        return
    scheme = "https" if t.kind == "https" else "http"
    base = "%s://%s:%d" % (scheme, t.ip, t.port)
    opt = ["-sS", "--max-time", "10"] + (["-k", "--tlsv1.3"] if t.kind == "https" else [])
    tests = [
        ("GET /status", ["-w", "\n%{http_code}", base + "/status"],
         lambda o: o.rstrip().endswith(b"200") and t.ip.encode() in o),
        ("POST /cli cmd=free", ["-d", "cmd=free", "-w", "\n%{http_code}", base + "/cli"],
         lambda o: b"Memory available" in o and o.rstrip().endswith(b"200")),
        ("HEAD /", ["-I", base + "/"], lambda o: o.startswith(b"HTTP/1.1 200")),
        ("OPTIONS /", ["-i", "-X", "OPTIONS", base + "/"],
         lambda o: o.startswith(b"HTTP/1.1 204") and b"Allow:" in o),
        ("GET /no-such-route", ["-w", "\n%{http_code}", base + "/no-such-route"],
         lambda o: o.rstrip().endswith(b"404")),
    ]
    for label, args, ok in tests:
        rc, out, err = _run(["curl"] + opt + args, 15)
        if rc != 0 or not ok(out):
            res.fail("curl %s %s: exit %d, %s" % (t.kind, label, rc,
                     err.decode("ascii", "replace")[:120]), out)
    res.note("curl: %d requests" % len(tests))


def _clients_urllib(t, res):
    import urllib.error
    import urllib.request
    scheme = "https" if t.kind == "https" else "http"
    base = "%s://%s:%d" % (scheme, t.ip, t.port)
    ctx = None
    if t.kind == "https":
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3

    def go(label, req, ok):
        try:
            with urllib.request.urlopen(req, timeout=10, context=ctx) as r:
                body = r.read()
                if not ok(r.status, body):
                    res.fail("urllib %s %s: status %d" % (t.kind, label, r.status), body)
        except Exception as e:
            res.fail("urllib %s %s: %s" % (t.kind, label, e))

    go("GET /status", urllib.request.Request(base + "/status"),
       lambda st, b: st == 200 and t.ip.encode() in b)
    go("POST /cli", urllib.request.Request(base + "/cli", data=b"cmd=free"),
       lambda st, b: st == 200 and b"Memory available" in b)
    res.note("urllib: 2 requests")


def _clients_requests(t, res):
    try:
        import requests
        import urllib3
    except ImportError:
        res.skips.append("requests: not installed")
        return
    urllib3.disable_warnings()
    scheme = "https" if t.kind == "https" else "http"
    base = "%s://%s:%d" % (scheme, t.ip, t.port)
    try:
        r = requests.get(base + "/status", timeout=10, verify=False)
        if r.status_code != 200 or t.ip not in r.text:
            res.fail("requests %s GET /status: %d" % (t.kind, r.status_code), r.content)
        r = requests.post(base + "/cli", data={"cmd": "free"}, timeout=10, verify=False)
        if r.status_code != 200 or "Memory available" not in r.text:
            res.fail("requests %s POST /cli: %d" % (t.kind, r.status_code), r.content)
        r = requests.head(base + "/", timeout=10, verify=False)
        if r.status_code != 200:
            res.fail("requests %s HEAD /: %d" % (t.kind, r.status_code))
        r = requests.options(base + "/", timeout=10, verify=False)
        if r.status_code != 204 or "Allow" not in r.headers:
            res.fail("requests %s OPTIONS /: %d" % (t.kind, r.status_code))
        res.note("requests: 4 requests")
    except Exception as e:
        res.fail("requests %s: %s" % (t.kind, e))


def phase_real_clients(t):
    """Non regression with real clients: curl, urllib, requests (those present)."""
    res = Result()
    _clients_curl(t, res)
    _clients_urllib(t, res)
    _clients_requests(t, res)
    return res


HTTP_PHASES = [
    ("post_body_exact", phase_post_body_exact),
    ("post_body_incomplete", phase_post_body_incomplete),
    ("options_wellformed", phase_options_wellformed),
    ("content_length_strict", phase_content_length_strict),
    ("head_no_body", phase_head_no_body),
    ("bad_request_400", phase_bad_request_400),
    ("allow_on_405", phase_allow_on_405),
    ("split_request", phase_split_request),
    ("real_clients", phase_real_clients),
]


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------
def show(raw):
    return repr(raw[:SHOW]) + (" ...(+%d)" % (len(raw) - SHOW) if len(raw) > SHOW else "")


def run_phase(name, fn, t):
    try:
        res = fn(t)
    except Exception as e:     # a transport failure is a failure, with its text
        res = Result()
        res.fail("%s: %s: %s" % (t.kind, type(e).__name__, e))
    if res.problems:
        status = "FAIL"
    elif res.skips and not res.notes:
        status = "SKIP"
    else:
        status = "PASS"
    extra = res.notes + res.skips if status != "FAIL" else []
    print("PHASE %s %s %s %s" % (name, t.kind, status, "; ".join(extra)))
    for text, raw in res.problems:
        print("  FAIL %s" % text)
        if raw is not None:
            print("    got: %s" % show(raw))
    if status == "PASS" and res.skips:
        for s in res.skips:
            print("  SKIP %s" % s)
    sys.stdout.flush()
    return status


def run(ip, names, transports, http_port=HTTP_PORT, https_port=HTTPS_PORT):
    """names: phase names, or ['all']. Returns the process exit code."""
    import tls_strict
    http_names = [n for n, _ in HTTP_PHASES]
    tls_names = [n for n, _ in tls_strict.TLS_PHASES]
    if names == ["all"]:
        names = http_names + tls_names
    unknown = [n for n in names if n not in http_names + tls_names]
    if unknown:
        print("unknown phase(s): %s (known: %s)" % (", ".join(unknown),
              ", ".join(http_names + tls_names)))
        return 2
    counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    for n in names:
        if n in tls_names:
            fn = dict(tls_strict.TLS_PHASES)[n]
            st = run_phase(n, lambda _t, fn=fn: fn(ip, https_port),
                           Target(ip, "tls", https_port))
            counts[st] += 1
            continue
        fn = dict(HTTP_PHASES)[n]
        for kind in transports:
            t = Target(ip, kind, https_port if kind == "https" else http_port)
            counts[run_phase(n, fn, t)] += 1
    print("SUMMARY pass=%d fail=%d skip=%d" % (counts["PASS"], counts["FAIL"], counts["SKIP"]))
    if counts["FAIL"]:
        return 1
    if counts["PASS"] == 0:
        return 77
    return 0
