#!/usr/bin/env python3
"""
caOS Example 42 - fake MQTT broker on a raw socket (bench counterpart).

mosquitto never sends a malformed packet, a QoS the client did not ask for, a
second CONNACK, or an oversize PUBLISH. This broker speaks just enough MQTT
3.1.1 to hold a session with the board (CONNECT/CONNACK, SUBSCRIBE/SUBACK,
PINGREQ/PINGRESP, PUBLISH both ways, PUBACK, DISCONNECT) and can be told, one
command at a time, to misbehave ONCE. After a fault it is conforming again.

It replaces mosquitto on the same port (never both): see `mqtt_server.py
--fake-broker`. It is driven by the commands that script reads on stdin.

Every packet in either direction is logged on stdout in a readable form; the
bench judges from that log as well as from the board console. Log grammar
(one event per line, prefix "[hh:mm:ss.mmm]"):

  CONN open id=N peer=a.b.c.d:p            a TCP connection was accepted
  CONN dummy ...                           a hold() filler connection
  CONN closed id=N reason=...              recv EOF / error / send failed
  CONN superseded id=N                     same client id connected again
  RX CONNECT proto="MQTT" level=4 flags=0x02 keepalive=60 remlen=21
             client_id="caos-h753" idlen=9 left=9 consistent=yes
  TX CONNACK flags=0x00 rc=0 [raw=20020000]
  RX SUBSCRIBE id=0x0001 topic="caos/led" qos=0
  TX SUBACK id=0x0001 rc=00
  RX PUBLISH topic="caos/status" qos=0 dup=0 retain=0 id=- len=NN payload="..."
  TX PUBLISH topic="caos/led" qos=1 id=0x1234 len=2 ...
  RX PUBACK id=0x1234 remlen=2
  RX PINGREQ / TX PINGRESP / RX DISCONNECT
  CMD <name> ... / CMD <name> done        injected-fault commands

Commands (handled by FakeBroker.command()):
  on | off                 PUBLISH "on"/"off" on caos/led to the board
  oversize [N]             PUBLISH with remaining length N (default 400),
                           sent in 90-byte chunks, then a normal "on"
  qos1 [hexid] [payload]   PUBLISH QoS 1 on caos/led (default 1234, "on")
  qos2                     PUBLISH QoS 2 on caos/led (the board must close)
  qos3                     PUBLISH with QoS bits = 3 (malformed)
  badtopic                 PUBLISH whose topic length exceeds the packet
  connack2                 a second CONNACK on the live session
  arm flags21|len3         the NEXT CONNACK is bad: flags 0x21, or 3 bytes
  disarm                   cancel an armed CONNACK fault
  hold                     stop accepting AND fill the accept queue, so a new
                           SYN goes unanswered (the board's connect stays
                           pending); the listener stays open
  release                  accept again
  status                   one line per connection
"""

import select
import socket
import struct
import sys
import threading
import time


def _ts():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)


def encode_remlen(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def match_filter(flt, topic):
    """MQTT topic filter match with '+' and '#'."""
    f = flt.split("/")
    t = topic.split("/")
    for i, part in enumerate(f):
        if part == "#":
            return True
        if i >= len(t):
            return False
        if part != "+" and part != t[i]:
            return False
    return len(f) == len(t)


class Conn:
    def __init__(self, sock, peer, cid):
        self.sock = sock
        self.peer = peer
        self.id = cid
        self.client_id = None
        self.connected = False      # CONNECT received
        self.subs = []              # topic filters
        self.alive = True
        self.wlock = threading.Lock()


class FakeBroker:
    def __init__(self, host, port, out=None):
        self.host = host
        self.port = port
        self._out = out or self._stdout
        self._olock = threading.Lock()
        self.conns = []
        self.lock = threading.Lock()
        self.next_id = 1
        self.srv = None
        self.hold_on = False
        self.dummies = []           # client-side filler sockets
        self.dummy_ports = set()
        self.arm_connack = None     # one-shot: "flags21" | "len3"
        self.connect_count = 0
        self._stop = False
        self._thr = None

    # ------------------------------------------------------------ logging
    def _stdout(self, s):
        # one write per line: threads must not interleave a line with the next
        with self._olock:
            sys.stdout.write(s + "\n")
            sys.stdout.flush()

    def log(self, msg):
        self._out("[%s] %s" % (_ts(), msg))

    # ------------------------------------------------------------ lifecycle
    def start(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.host, self.port))
        s.listen(16)
        s.setblocking(False)
        self.srv = s
        self._thr = threading.Thread(target=self._accept_loop, daemon=True)
        self._thr.start()
        self.log("FAKE BROKER listening on %s:%d" % (self.host, self.port))

    def stop(self):
        self._stop = True
        self._close_dummies()
        with self.lock:
            cs = list(self.conns)
        for c in cs:
            self._close(c, "broker stop")
        try:
            self.srv.close()
        except Exception:
            pass

    # ------------------------------------------------------------ accept
    def _accept_loop(self):
        while not self._stop:
            if self.hold_on:
                time.sleep(0.05)
                continue
            try:
                r, _, _ = select.select([self.srv], [], [], 0.1)
            except Exception:
                return
            if not r or self.hold_on:
                continue
            try:
                sock, peer = self.srv.accept()
            except (BlockingIOError, InterruptedError):
                continue
            except Exception:
                return
            sock.setblocking(True)
            if peer[1] in self.dummy_ports:
                self.log("CONN dummy (hold filler) peer=%s:%d" % peer)
                self.dummy_ports.discard(peer[1])
                try:
                    sock.close()
                except Exception:
                    pass
                continue
            with self.lock:
                c = Conn(sock, peer, self.next_id)
                self.next_id += 1
                self.conns.append(c)
            self.log("CONN open id=%d peer=%s:%d" % (c.id, peer[0], peer[1]))
            threading.Thread(target=self._conn_loop, args=(c,), daemon=True).start()

    # ------------------------------------------------------------ helpers
    def _close(self, c, reason):
        if not c.alive:
            return
        c.alive = False
        try:
            c.sock.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass
        try:
            c.sock.close()
        except Exception:
            pass
        with self.lock:
            if c in self.conns:
                self.conns.remove(c)
        self.log("CONN closed id=%d reason=%s" % (c.id, reason))

    def _send(self, c, data, desc, chunk=0, gap=0.0):
        """Send raw bytes; log first. A send failure closes the connection (the
        board aborts without RST, so this is where the broker notices)."""
        self.log("TX %s" % desc)
        try:
            with c.wlock:
                if chunk:
                    for i in range(0, len(data), chunk):
                        c.sock.sendall(data[i:i + chunk])
                        time.sleep(gap)
                else:
                    c.sock.sendall(data)
            return True
        except Exception as e:
            self.log("TX failed id=%d (%s)" % (c.id, e))
            self._close(c, "send failed")
            return False

    def target(self):
        """Newest live connection that completed CONNECT and subscribed to
        caos/led: the board."""
        with self.lock:
            cs = [c for c in self.conns
                  if c.alive and c.connected and
                  any(match_filter(f, "caos/led") for f in c.subs)]
        return cs[-1] if cs else None

    @staticmethod
    def _txt(b, limit=120):
        s = bytes(b[:limit]).decode("ascii", errors="replace")
        s = "".join(ch if 32 <= ord(ch) < 127 and ch != '"' else "." for ch in s)
        return s + ("..." if len(b) > limit else "")

    @staticmethod
    def build_publish(topic, payload, qos=0, pid=None, retain=False, dup=False):
        t = topic.encode()
        body = struct.pack("!H", len(t)) + t
        if qos:
            body += struct.pack("!H", pid)
        body += payload
        hdr = 0x30 | (qos << 1) | (1 if retain else 0) | (8 if dup else 0)
        return bytes([hdr]) + encode_remlen(len(body)) + body

    # ------------------------------------------------------------ RX path
    def _conn_loop(self, c):
        buf = bytearray()
        try:
            while c.alive and not self._stop:
                data = c.sock.recv(4096)
                if not data:
                    self._close(c, "recv eof")
                    return
                buf += data
                while True:
                    pkt = self._take(buf)
                    if pkt is None:
                        break
                    if pkt == "bad":
                        self.log("RX malformed remaining length id=%d" % c.id)
                        self._close(c, "malformed")
                        return
                    self._handle(c, pkt[0], pkt[1])
                    if not c.alive:
                        return
        except Exception as e:
            self._close(c, "recv error %s" % e)

    @staticmethod
    def _take(buf):
        if len(buf) < 2:
            return None
        mult, val, i = 1, 0, 1
        while True:
            if i >= len(buf):
                return None
            b = buf[i]
            val += (b & 0x7F) * mult
            i += 1
            if not (b & 0x80):
                break
            mult *= 128
            if mult > 128 ** 3:
                return "bad"
        if len(buf) < i + val:
            return None
        hdr = buf[0]
        body = bytes(buf[i:i + val])
        del buf[:i + val]
        return (hdr, body)

    def _handle(self, c, hdr, body):
        typ = hdr >> 4
        if typ == 1:
            self._on_connect(c, hdr, body)
        elif typ == 3:
            self._on_publish(c, hdr, body)
        elif typ == 4:
            if len(body) == 2:
                self.log("RX PUBACK id=0x%04X remlen=2" % struct.unpack("!H", body))
            else:
                self.log("RX PUBACK malformed remlen=%d" % len(body))
        elif typ == 8:
            self._on_subscribe(c, hdr, body)
        elif typ == 12:
            self.log("RX PINGREQ")
            self._send(c, b"\xD0\x00", "PINGRESP")
        elif typ == 14:
            self.log("RX DISCONNECT")
            self._close(c, "client disconnect")
        else:
            self.log("RX UNKNOWN hdr=0x%02X remlen=%d" % (hdr, len(body)))

    def _on_connect(self, c, hdr, body):
        try:
            nlen = struct.unpack("!H", body[0:2])[0]
            proto = body[2:2 + nlen].decode("ascii", "replace")
            p = 2 + nlen
            level, flags, keep = body[p], body[p + 1], struct.unpack("!H", body[p + 2:p + 4])[0]
            p += 4
            idlen = struct.unpack("!H", body[p:p + 2])[0]
            p += 2
            cid = body[p:p + idlen].decode("ascii", "replace")
            left = len(body) - p
        except Exception:
            self.log("RX CONNECT malformed remlen=%d" % len(body))
            self._close(c, "bad CONNECT")
            return
        ok = "yes" if (left == idlen and hdr == 0x10) else "NO"
        self.log('RX CONNECT proto="%s" level=%d flags=0x%02X keepalive=%d remlen=%d '
                 'client_id="%s" idlen=%d left=%d consistent=%s'
                 % (proto, level, flags, keep, len(body), cid, idlen, left, ok))
        c.client_id = cid
        c.connected = True
        self.connect_count += 1
        # A real broker evicts an older session with the same client id
        with self.lock:
            old = [o for o in self.conns if o is not c and o.client_id == cid]
        for o in old:
            self.log("CONN superseded id=%d" % o.id)
            self._close(o, "superseded")
        fault, self.arm_connack = self.arm_connack, None
        if fault == "flags21":
            raw = b"\x20\x02\x21\x00"
        elif fault == "len3":
            raw = b"\x20\x03\x00\x00\x00"
        else:
            raw = b"\x20\x02\x00\x00"
        self._send(c, raw, "CONNACK flags=0x%02X rc=%d raw=%s%s"
                   % (raw[2], raw[3], raw.hex(), " FAULT" if fault else ""))

    def _on_subscribe(self, c, hdr, body):
        try:
            pid = struct.unpack("!H", body[0:2])[0]
            p = 2
            rcs = bytearray()
            while p < len(body):
                tl = struct.unpack("!H", body[p:p + 2])[0]
                topic = body[p + 2:p + 2 + tl].decode("ascii", "replace")
                qos = body[p + 2 + tl]
                p += 3 + tl
                self.log('RX SUBSCRIBE id=0x%04X topic="%s" qos=%d' % (pid, topic, qos))
                c.subs.append(topic)
                rcs.append(0x00)      # granted QoS 0: always allowed
        except Exception:
            self.log("RX SUBSCRIBE malformed")
            self._close(c, "bad SUBSCRIBE")
            return
        raw = b"\x90" + encode_remlen(2 + len(rcs)) + struct.pack("!H", pid) + bytes(rcs)
        self._send(c, raw, "SUBACK id=0x%04X rc=%s" % (pid, bytes(rcs).hex()))

    def _on_publish(self, c, hdr, body):
        qos = (hdr >> 1) & 3
        dup = (hdr >> 3) & 1
        retain = hdr & 1
        try:
            tl = struct.unpack("!H", body[0:2])[0]
            topic = body[2:2 + tl].decode("ascii", "replace")
            p = 2 + tl
            pid = None
            if qos:
                pid = struct.unpack("!H", body[p:p + 2])[0]
                p += 2
            payload = body[p:]
        except Exception:
            self.log("RX PUBLISH malformed")
            self._close(c, "bad PUBLISH")
            return
        self.log('RX PUBLISH topic="%s" qos=%d dup=%d retain=%d id=%s len=%d payload="%s"'
                 % (topic, qos, dup, retain,
                    ("0x%04X" % pid) if pid is not None else "-",
                    len(payload), self._txt(payload)))
        if qos == 2 or qos == 3:
            self.log("RX PUBLISH qos=%d not supported by the fake broker" % qos)
            self._close(c, "unsupported qos")
            return
        if qos == 1:
            self._send(c, b"\x40\x02" + struct.pack("!H", pid), "PUBACK id=0x%04X" % pid)
        # route at QoS 0 to every other connection with a matching filter
        with self.lock:
            dst = [o for o in self.conns
                   if o is not c and o.alive and any(match_filter(f, topic) for f in o.subs)]
        if dst:
            raw = self.build_publish(topic, payload)
            for o in dst:
                self._send(o, raw, 'PUBLISH topic="%s" qos=0 len=%d (route to id=%d)'
                           % (topic, len(payload), o.id))

    # ------------------------------------------------------------ commands
    def _fill_accept_queue(self):
        """Open filler clients until one cannot complete the handshake: the
        accept queue is then full and the kernel drops further SYNs, which is
        what keeps a connect pending on the board."""
        self.srv.listen(1)
        for _ in range(16):
            d = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            d.setblocking(False)
            try:
                d.bind((self.host, 0))
                self.dummy_ports.add(d.getsockname()[1])
            except Exception:
                pass
            d.connect_ex((self.host, self.port))
            self.dummies.append(d)
            _, w, _ = select.select([], [d], [], 0.25)
            if not w:
                return len(self.dummies)
        return len(self.dummies)

    def _close_dummies(self):
        for d in self.dummies:
            try:
                d.close()
            except Exception:
                pass
        self.dummies = []

    def command(self, line):
        """Run one command; returns True if it was one of ours."""
        parts = line.split()
        if not parts:
            return True
        name, args = parts[0], parts[1:]

        if name in ("on", "off"):
            c = self.target()
            if not c:
                self.log("CMD %s: no session subscribed to caos/led" % name)
                return True
            self._send(c, self.build_publish("caos/led", name.encode()),
                       'PUBLISH topic="caos/led" qos=0 len=%d' % len(name))
            self.log("CMD %s done" % name)
            return True

        if name == "oversize":
            n = int(args[0]) if args else 400
            c = self.target()
            if not c:
                self.log("CMD oversize: no session subscribed to caos/led")
                return True
            t = b"caos/led"
            fill = n - 2 - len(t)
            if fill < 0:
                fill = 0
            body = struct.pack("!H", len(t)) + t + b"z" * fill
            raw = b"\x30" + encode_remlen(len(body)) + body
            self.log("CMD oversize remlen=%d" % len(body))
            if self._send(c, raw, 'PUBLISH topic="caos/led" qos=0 remlen=%d '
                          '(chunks of 90 bytes)' % len(body), chunk=90, gap=0.03):
                time.sleep(0.2)
                self._send(c, self.build_publish("caos/led", b"on"),
                           'PUBLISH topic="caos/led" qos=0 len=2 (normal, after oversize)')
            self.log("CMD oversize done")
            return True

        if name == "qos1":
            pid = int(args[0], 16) if args else 0x1234
            pay = args[1].encode() if len(args) > 1 else b"on"
            c = self.target()
            if not c:
                self.log("CMD qos1: no session subscribed to caos/led")
                return True
            self.log("CMD qos1 id=0x%04X" % pid)
            self._send(c, self.build_publish("caos/led", pay, qos=1, pid=pid),
                       'PUBLISH topic="caos/led" qos=1 id=0x%04X len=%d' % (pid, len(pay)))
            self.log("CMD qos1 done")
            return True

        if name in ("qos2", "qos3", "badtopic", "connack2"):
            c = self.target()
            if not c:
                self.log("CMD %s: no session subscribed to caos/led" % name)
                return True
            self.log("CMD %s" % name)
            if name == "qos2":
                raw = self.build_publish("caos/led", b"on", qos=2, pid=0x2222)
                d = 'PUBLISH topic="caos/led" qos=2 id=0x2222 len=2'
            elif name == "qos3":
                t = b"caos/led"
                body = struct.pack("!H", len(t)) + t + b"on"
                raw = bytes([0x36]) + encode_remlen(len(body)) + body
                d = 'PUBLISH qos=3 (malformed) topic="caos/led" len=2'
            elif name == "badtopic":
                body = struct.pack("!H", 64) + b"caos"      # topic len 64 > 4 left
                raw = b"\x30" + encode_remlen(len(body)) + body
                d = "PUBLISH malformed: topic length 64 in a packet of remlen=%d" % len(body)
            else:
                raw = b"\x20\x02\x00\x00"
                d = "CONNACK flags=0x00 rc=0 raw=20020000 (SECOND, session already up)"
            self._send(c, raw, d)
            self.log("CMD %s done" % name)
            return True

        if name == "arm":
            if args and args[0] in ("flags21", "len3"):
                self.arm_connack = args[0]
                self.log("CMD arm %s done" % args[0])
            else:
                self.log("CMD arm: use flags21 or len3")
            return True

        if name == "disarm":
            self.arm_connack = None
            self.log("CMD disarm done")
            return True

        if name == "hold":
            self.hold_on = True
            time.sleep(0.2)     # the accept loop is parked
            n = self._fill_accept_queue()
            self.log("CMD hold done (accept queue filled with %d filler(s))" % n)
            return True

        if name == "release":
            self._close_dummies()
            self.srv.listen(16)
            self.hold_on = False
            self.log("CMD release done")
            return True

        if name == "status":
            with self.lock:
                cs = list(self.conns)
            self.log("CMD status: %d connection(s), connects=%d, hold=%s, armed=%s"
                     % (len(cs), self.connect_count, self.hold_on, self.arm_connack))
            for c in cs:
                self.log("  id=%d peer=%s:%d client_id=%s subs=%s"
                         % (c.id, c.peer[0], c.peer[1], c.client_id, c.subs))
            return True

        return False
