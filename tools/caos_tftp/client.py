"""TFTP write (RFC 1350, octet mode): push one file to a caOS board.

The board listens with ca_tftp_receive() (caos/net/src/tftp_service.c); the
PC is the client, so no privileged port is needed on this side.
"""
import socket
import struct
import time

OP_WRQ, OP_DATA, OP_ACK, OP_ERROR = 2, 3, 4, 5
BLOCK = 512


class TftpError(Exception):
    """The transfer failed: timeout, refusal, or an ERROR from the board."""


def put(host, data, name, port=69, timeout=5.0, first_timeout=60.0,
        retries=5, progress=None):
    """Send `data` (bytes) to host:port as file `name`.

    first_timeout: how long to wait for the ACK of block 0. The board answers
    after its WRQ callback, which may erase flash first (seconds on H7).
    progress(sent_bytes, total_bytes) is called after every acknowledged block.
    Returns a dict with blocks, bytes, seconds. Raises TftpError on failure.
    """
    total = len(data)
    nblocks = total // BLOCK + 1          # a final short (maybe empty) block ends it
    wrq = struct.pack("!H", OP_WRQ) + name.encode() + b"\0octet\0"

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        t0 = time.monotonic()
        peer = _exchange(sock, wrq, (host, port), 0, first_timeout, retries,
                         learn_tid=True)
        for n in range(1, nblocks + 1):
            chunk = data[(n - 1) * BLOCK:n * BLOCK]
            pkt = struct.pack("!HH", OP_DATA, n & 0xFFFF) + chunk
            _exchange(sock, pkt, peer, n & 0xFFFF, timeout, retries)
            if progress:
                progress(min(n * BLOCK, total), total)
        return {"blocks": nblocks, "bytes": total,
                "seconds": time.monotonic() - t0}
    finally:
        sock.close()


def _exchange(sock, pkt, dst, block, timeout, retries, learn_tid=False):
    """Send pkt, wait for ACK `block`; resend on timeout. Returns the peer
    address (the board answers from a new port, its TID)."""
    host = dst[0]
    for _ in range(retries + 1):
        sock.sendto(pkt, dst)
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            sock.settimeout(left)
            try:
                reply, src = sock.recvfrom(1024)
            except socket.timeout:
                break
            if src[0] != host or len(reply) < 4:
                continue
            if not learn_tid and src != dst:
                continue                      # RFC 1350: unknown TID, ignore
            op, val = struct.unpack("!HH", reply[:4])
            if op == OP_ERROR:
                msg = reply[4:].split(b"\0")[0].decode(errors="replace")
                raise TftpError("board answered ERROR %d \"%s\"" % (val, msg))
            if op == OP_ACK and val == block:
                return src
            # a duplicate ACK of the previous block: keep waiting
    raise TftpError("no ACK of block %d from %s after %d tries" %
                    (block, host, retries + 1))
