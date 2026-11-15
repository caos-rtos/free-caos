#!/usr/bin/env python3
"""Summarise the accesses to addresses with nothing behind them in Renode.

run.sh keeps the system bus at warning level, so every read/write to an
address that no model covers lands in the Renode log. On silicon that address
is a peripheral (or a BusFault): each line is either a peripheral the platform
still lacks, or a firmware bug. This groups them by address, with the function
that made the first access (arm-none-eabi-addr2line, when installed).

usage: simu/unmapped.py <renode.log> [<elf>]
"""
import re
import shutil
import subprocess
import sys

RX = re.compile(r'\[cpu: (0x[0-9A-Fa-f]+)\][^\n]*?(Read|Write)\w* (?:from|to) non existing '
                r'peripheral at (0x[0-9A-Fa-f]+)(?:[^\n]*?\((\d+)\))?')


def symbols(elf, pcs):
    tool = shutil.which('arm-none-eabi-addr2line')
    if not elf or not tool or not pcs:
        return {}
    out = subprocess.run([tool, '-f', '-e', elf] + pcs, capture_output=True, text=True).stdout
    names = out.splitlines()[0::2]
    return dict(zip(pcs, names))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip())
    text = open(sys.argv[1], errors='replace').read()
    errors = text.count('[ERROR]')
    if errors:
        print("Renode errors in the log: %d (see %s)" % (errors, sys.argv[1]))
    stats = {}                                  # addr -> [reads, writes, first pc]
    for m in RX.finditer(text):
        pc, kind, addr, rep = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        n = int(rep) if rep else 1              # Renode folds repeats into "(N)"
        s = stats.setdefault(addr, [0, 0, pc])
        s[0 if kind == 'Read' else 1] += n
    if not stats:
        print("unmapped accesses: none (every address the firmware touched has a model)")
        return
    names = symbols(sys.argv[2] if len(sys.argv) > 2 else None,
                    sorted({s[2] for s in stats.values()}))
    print("unmapped accesses (no Renode model at these addresses):")
    print("  %-10s %8s %8s  %s" % ("address", "reads", "writes", "first access in"))
    for addr in sorted(stats):
        r, w, pc = stats[addr]
        print("  0x%08X %8d %8d  %s (%s)" % (addr, r, w, names.get(pc, '?'), pc))


if __name__ == '__main__':
    main()
