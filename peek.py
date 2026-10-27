#!/usr/bin/env python3
"""peek.py - read (or write) a variable of a running caOS firmware, by name.

The precompiled libraries have no debug information, but the .elf that ./do
builds keeps the symbol table of YOUR code: it knows where every global and
static variable lives. peek.py looks the name up there and turns it into the
CLI command that reads that memory from inside the board (mem / hexdump) -
no debugger, no probe.

  ./peek.py elfs/H563/h563_app_1.elf s_counter            print the CLI command
  ./peek.py elfs/H563/h563_app_1.elf s_counter -p /dev/ttyACM0   send it, decode
  ./peek.py elfs/H563/h563_app_1.elf s_counter -p /dev/ttyACM0 --set 2A
  ./peek.py elfs/H563/h563_app_1.elf --list s_            names starting with s_

Only variables with a fixed address: globals and statics (a static inside a
function shows up as name.N). Locals live in registers or on a task stack.
--set writes one 32-bit word (hex, as the CLI wants it): 4-byte variables only.
Close any other terminal on the port first: two readers split the replies.

Python 3.6+, standard library only; needs arm-none-eabi-nm on the PATH.
"""
import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import termios
import time

DATA_TYPES = set("bBdDrRgGsS")          # nm: bss, data, read-only, small data


def nm_symbols(elf):
    """[(name, addr, size, type)] of the data symbols of the elf."""
    nm = shutil.which("arm-none-eabi-nm")
    if not nm:
        sys.exit("ERROR: arm-none-eabi-nm not found in PATH (Arm GNU Toolchain)")
    out = subprocess.run([nm, "-S", "--defined-only", elf], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, universal_newlines=True)
    if out.returncode != 0:
        sys.exit("ERROR: nm %s: %s" % (elf, out.stderr.strip()))
    syms = []
    for line in out.stdout.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in DATA_TYPES:
            syms.append((p[3], int(p[0], 16), int(p[1], 16), p[2]))
    return syms


def find(syms, name):
    """Exact name, else a function static name.N; error when ambiguous."""
    exact = [s for s in syms if s[0] == name]
    if exact:
        return exact[0]
    local = [s for s in syms if re.match(re.escape(name) + r"\.\d+$", s[0])]
    if len(local) == 1:
        return local[0]
    if local:
        sys.exit("ERROR: '%s' is a static in %d functions: %s - pick one"
                 % (name, len(local), ", ".join(s[0] for s in local)))
    sys.exit("ERROR: no data symbol '%s' in the elf (--list %s to search)" % (name, name[:3]))


def cli_command(addr, size, value=None):
    if value is not None:
        if size != 4 or addr & 3:
            sys.exit("ERROR: --set writes a 32-bit word: only 4-byte aligned variables")
        return "mem %08X %s" % (addr, value)
    if size == 4 and not addr & 3:
        return "mem %08X" % addr
    return "hexdump %08X %d" % (addr, min(32, max(1, (size + 15) // 16)))


def open_port(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0                                        # iflag: raw
    a[1] = 0                                        # oflag
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0                                        # lflag: no echo, no canon
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def ask(port, cmd, timeout=2.0):
    """Send one CLI line, return what the board printed until it went quiet."""
    fd = open_port(port)
    try:
        os.write(fd, (cmd + "\r").encode())
        buf, end, quiet = b"", time.time() + timeout, None
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                buf += os.read(fd, 4096)
                quiet = time.time() + 0.3
            elif quiet and time.time() > quiet:
                break
        return buf.decode("ascii", "replace")
    finally:
        os.close(fd)


def decode(text, addr, size):
    """The bytes of the variable from the board's reply, None if not found."""
    m = re.search(r"mem: \[0x%08X\][^\n]*?(?:=|->) 0x([0-9A-Fa-f]{8})" % addr, text, re.I)
    if m:
        return int(m.group(1), 16).to_bytes(4, "little")
    data = b""
    for row in range((size + 15) // 16):
        m = re.search(r"0x%08X  ((?:[0-9a-f]{2} )+)" % (addr + 16 * row), text, re.I)
        if not m:
            return None
        data += bytes(int(h, 16) for h in m.group(1).split())
    return data[:size]


def show(name, addr, size, data):
    print("%s @ 0x%08X, %d byte%s" % (name, addr, size, "" if size == 1 else "s"))
    if size in (1, 2, 4):
        u = int.from_bytes(data, "little")
        s = u - (1 << (8 * size)) if u >> (8 * size - 1) else u
        print("  = %u  (0x%0*X, signed %d)" % (u, 2 * size, u, s))
    else:
        for off in range(0, len(data), 16):
            row = data[off:off + 16]
            print("  +%04x  %-48s |%s|" % (off, " ".join("%02x" % b for b in row),
                                          "".join(chr(c) if 32 <= c < 127 else "." for c in row)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 epilog=__doc__.split("\n\n", 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", help="the firmware .elf, as built by ./do")
    ap.add_argument("name", nargs="?", help="variable name")
    ap.add_argument("-p", "--port", help="console port: send the command and decode the reply")
    ap.add_argument("--set", metavar="HEX", help="write this 32-bit value (hex, no 0x)")
    ap.add_argument("--list", metavar="PREFIX", help="list data symbols starting with PREFIX")
    a = ap.parse_args()

    syms = nm_symbols(a.elf)
    if a.list is not None:
        for n, addr, size, t in sorted(s for s in syms if s[0].startswith(a.list)):
            print("  0x%08X %6d  %s" % (addr, size, n))
        return
    if not a.name:
        ap.error("a variable name, or --list PREFIX")
    if a.set is not None and not re.match(r"^[0-9A-Fa-f]{1,8}$", a.set):
        sys.exit("ERROR: --set takes up to 8 hex digits, without 0x")

    name, addr, size, _ = find(syms, a.name)
    cmd = cli_command(addr, size, a.set)
    if not a.port:
        print("%s @ 0x%08X, %d bytes - type on the console:\n  %s" % (name, addr, size, cmd))
        return
    reply = ask(a.port, cmd)
    data = decode(reply, addr, 4 if cmd.startswith("mem") else size)
    if data is None:
        sys.exit("ERROR: no reply to '%s' on %s (wrong port, or another terminal "
                 "holds it?)\n%s" % (cmd, a.port, reply.strip()))
    show(name, addr, size, data)


if __name__ == "__main__":
    main()
