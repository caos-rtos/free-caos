#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
# Coppi Angelo Operating System
# caos.rtos@protonmail.com
"""
PC counterpart of example 98: checks the CORDIC results against Python's math.

  ./examples/example_98/cordic_check.py            # runs "cordic dump" via caos_shell
  ./examples/example_98/cordic_check.py dump.txt   # or reads a saved dump

Each "CRD <func> <scale> <arg1> <arg2> <res1> <res2>" line (hex q1.31) is
recomputed in double precision and compared within the bound of the reference
manual (RM0456 table 209, 24 iterations): 2^-19 on the circular functions,
2^-18 on the hyperbolic ones and ln, plus 2^-20 of rounding; a larger SCALE
loses precision proportionally, and so does the phase of a vector shorter
than 0.5. Exit code 0 only when every result is within its bound.
"""
import math
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SHELL = os.path.join(ROOT, "test", "hw_test", "build", "caos_shell")

NAMES = ["cos", "sin", "phase", "modulus", "atan",
         "cosh", "sinh", "atanh", "ln", "sqrt"]
LINE = re.compile(r"CRD (\d+) (\d+) ([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8})")
END = re.compile(r"CRD END (\d+)")


def q31(h):
    v = int(h, 16)
    if v & 0x80000000:
        v -= 1 << 32
    return v / 2147483648.0


def angle_diff(a, b):
    """Difference of two angles in units of pi, wrapped to [-1, 1)."""
    d = (a - b + 1.0) % 2.0 - 1.0
    return d


def expected(func, n, a1, a2):
    """(res1, res2, is_angle1, is_angle2); res2 None when unused."""
    pi = math.pi
    if func == 0:
        return a2 * math.cos(a1 * pi), a2 * math.sin(a1 * pi), False, False
    if func == 1:
        return a2 * math.sin(a1 * pi), a2 * math.cos(a1 * pi), False, False
    if func == 2:
        return math.atan2(a2, a1) / pi, math.hypot(a1, a2), True, False
    if func == 3:
        return math.hypot(a1, a2), math.atan2(a2, a1) / pi, False, True
    if func == 4:
        return math.atan(a1 * 2 ** n) / (pi * 2 ** n), None, False, False
    x = a1 * 2 ** n
    if func == 5:
        return math.cosh(x) / 2 ** n, math.sinh(x) / 2 ** n, False, False
    if func == 6:
        return math.sinh(x) / 2 ** n, math.cosh(x) / 2 ** n, False, False
    if func == 7:
        return math.atanh(x) / 2 ** n, None, False, False
    if func == 8:
        return math.log(x) / 2 ** (n + 1), None, False, False
    return math.sqrt(x) / 2 ** n, None, False, False


def bound(func, n, a1, a2):
    rnd = 2.0 ** -20
    if func in (5, 6, 7):
        return 2.0 ** -18 + rnd
    if func == 8:
        return 2.0 ** -18 * 2 ** (n - 1) + rnd
    if func == 4:
        return 2.0 ** -19 * 2 ** n + rnd
    if func == 9:
        return 2.0 ** -19 * 2 ** n + rnd
    b = 2.0 ** -19 + rnd
    if func in (2, 3):
        b *= max(1.0, 0.5 / max(math.hypot(a1, a2), 1e-9))
    return b


def main():
    if len(sys.argv) > 1:
        text = open(sys.argv[1]).read()
    else:
        out = subprocess.run([SHELL, "-q", "-t", "20", "-u", "CRD END", "cordic dump"],
                             capture_output=True, text=True)
        text = out.stdout + out.stderr

    worst = {}          # func -> (err / bound, err, line)
    count = 0
    fails = 0
    announced = None
    for line in text.splitlines():
        m = END.search(line)
        if m:
            announced = int(m.group(1))
            continue
        m = LINE.search(line)
        if not m:
            continue
        func, n = int(m.group(1)), int(m.group(2))
        a1, a2, r1, r2 = (q31(m.group(i)) for i in range(3, 7))
        e1, e2, ang1, ang2 = expected(func, n, a1, a2)
        d1 = angle_diff(r1, e1) if ang1 else r1 - e1
        err = abs(d1)
        if e2 is not None:
            d2 = angle_diff(r2, e2) if ang2 else r2 - e2
            err = max(err, abs(d2))
        b = bound(func, n, a1, a2)
        count += 1
        if err > b:
            fails += 1
            print("FAIL %-7s n=%d a1=%+.6f a2=%+.6f err=%.3g bound=%.3g"
                  % (NAMES[func], n, a1, a2, err, b))
        ratio = err / b
        if func not in worst or ratio > worst[func][0]:
            worst[func] = (ratio, err)

    if count == 0:
        print("no CRD lines: is example 98 on the board? (cordic dump)")
        return 2
    if announced is not None and announced != count:
        print("lines lost: board sent %d, parsed %d" % (announced, count))
        fails += 1

    print("%-8s %12s %10s" % ("func", "worst err", "of bound"))
    for f in sorted(worst):
        ratio, err = worst[f]
        bits = -math.log2(err) if err > 0 else 99.0
        print("%-8s %9.2e (2^-%4.1f) %5.0f%%" % (NAMES[f], err, bits, ratio * 100))
    print("%d operations, %d outside the bound -> %s"
          % (count, fails, "PASS" if fails == 0 else "FAIL"))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
