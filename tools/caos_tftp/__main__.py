"""Command line: python3 tools/caos_tftp put <board-ip> <file> [options]

The board must be waiting: 'tftp recv' on the CLI of an example with the
TFTP service, or the app_2 bootloader (recovery, or 'boot load').
"""
import argparse
import os
import sys

if __package__ in (None, ""):
    # run as 'python3 tools/caos_tftp': make the package importable
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from caos_tftp.client import put, TftpError
else:
    from .client import put, TftpError


def main():
    ap = argparse.ArgumentParser(prog="caos_tftp",
                                 description="Push a file to a caOS board over TFTP.")
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("put", help="send a file (TFTP WRQ)")
    p.add_argument("host", help="board IP address")
    p.add_argument("file", help="local file")
    p.add_argument("--name", help="name sent to the board (default: file basename)")
    p.add_argument("--port", type=int, default=69, help="board port (default 69)")
    p.add_argument("--timeout", type=float, default=5.0, help="per-block timeout, s")
    p.add_argument("--first-timeout", type=float, default=60.0,
                   help="wait for the first ACK, s (the board may erase flash first)")
    p.add_argument("--retries", type=int, default=5)
    p.add_argument("-q", "--quiet", action="store_true", help="no progress line")
    a = ap.parse_args()
    if a.cmd != "put":
        ap.print_help()
        return 2

    data = open(a.file, "rb").read()
    name = a.name or os.path.basename(a.file)
    last = [-1]

    def progress(sent, total):
        pct = 100 * sent // total if total else 100
        if pct != last[0]:
            last[0] = pct
            sys.stdout.write("\r%s: %d/%d bytes (%d%%)" % (name, sent, total, pct))
            sys.stdout.flush()

    print("TFTP put %s (%d bytes) -> %s:%d" % (name, len(data), a.host, a.port))
    try:
        r = put(a.host, data, name, port=a.port, timeout=a.timeout,
                first_timeout=a.first_timeout, retries=a.retries,
                progress=None if (a.quiet or not sys.stdout.isatty()) else progress)
    except TftpError as e:
        print("\nFAIL: %s" % e)
        return 1
    rate = r["bytes"] / r["seconds"] / 1024 if r["seconds"] > 0 else 0
    print("\nOK: %d blocks, %d bytes in %.1f s (%.1f KB/s)" %
          (r["blocks"], r["bytes"], r["seconds"], rate))
    return 0


if __name__ == "__main__":
    sys.exit(main())
