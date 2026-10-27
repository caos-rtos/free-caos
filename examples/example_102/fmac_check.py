#!/usr/bin/env python3
# Proprietary — All rights reserved
# Copyright (c) 2026 Angelo Coppi
# Coppi Angelo Operating System
# caos.rtos@protonmail.com
"""
PC counterpart of example 102: the FMAC results against numpy and scipy.

  ./examples/example_102/fmac_check.py                 # "fmac dump" via caos_shell
  ./examples/example_102/fmac_check.py dump.txt        # or a saved dump
  ./examples/example_102/fmac_check.py --sweep [name]  # measured response vs design
  ./examples/example_102/fmac_check.py --sweep name sweep.txt   # or a saved sweep
  ./examples/example_102/fmac_check.py --tables        # C tables of the filters

Dump: each case (filter, input, FMAC output) is recomputed in integers with
the four arithmetic models of the board's "fmac test" - written again here,
from the reference manual, not from the C - and must match one of them on
every sample; the float filter with the same q1.15 coefficients shows how far
the fixed point is from the ideal one.

Sweep: "fmac sweep" measures the gain at each tone through DAC -> ADC ->
FMAC; it must stay within 0.5 dB of the design (freqz of the quantized
coefficients) where the design is above -40 dB, and below -35 dB elsewhere,
where the 12-bit channel's own floor takes over.

Exit code 0 only when everything is within bounds.
"""
import os
import re
import subprocess
import sys

import numpy as np
from scipy import signal

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SHELL = os.path.join(ROOT, "test", "hw_test", "build", "caos_shell")
FS = 16000

MODELS = ["products whole, output truncated", "products truncated, output truncated",
          "products whole, output rounded", "products truncated, output rounded"]


def design():
    """The filters of ca_os_ex102.c: name -> (b q15, a q15 or None, gain)."""
    def q15(c):
        return np.clip(np.round(np.asarray(c) * 32768), -32768, 32767).astype(int)
    f = {"lp1k": (q15(signal.firwin(63, 1000, fs=FS)), None, 0),
         "bp1k": (q15(signal.firwin(127, [800, 1250], pass_zero=False, fs=FS)), None, 0)}
    b, a = signal.butter(2, 100, "highpass", fs=FS)
    f["hp100"] = (q15(b / 2), q15(-a[1:] / 2), 1)
    b, a = signal.butter(4, 1000, "lowpass", fs=FS)
    f["lp1k_iir"] = (q15(b / 4), q15(-a[1:] / 4), 2)
    return f


def tables():
    for name, (b, a, _) in design().items():
        for tag, v in (("b", b), ("a", a)):
            if v is None:
                continue
            print("static const i16 s_%s_%s[%d] = {" % (name, tag, len(v)))
            for i in range(0, len(v), 10):
                print("    " + ", ".join("%6d" % x for x in v[i:i + 10]) + ",")
            print("};")
    return 0


def wrap(v, bits):
    m = 1 << bits
    v &= m - 1
    return v - m if v >= m >> 1 else v


def model(b, a, gain, clip, x, m):
    """RM0456 26.3.7: q1.15 products into a q4.22 accumulator, gain 2^R,
    output q1.15 clipped or truncated. The variants say what the manual
    leaves open: products truncated to 22 bits or not, output truncated or
    rounded."""
    frac, drop = (22, 8) if m & 1 else (30, 0)
    shift = frac - 15
    y = []
    for n in range(len(x)):
        acc = 0
        for k in range(min(len(b), n + 1)):
            acc += (b[k] * x[n - k]) >> drop
        for k in range(1, min(len(a), n) + 1):
            acc += (a[k - 1] * y[n - k]) >> drop
        acc = wrap(acc, frac + 4)
        v = acc << gain
        if m & 2:
            v += 1 << (shift - 1)
        v >>= shift
        v = max(-32768, min(32767, v)) if clip else wrap(v, 16)
        y.append(v)
    return y


def s16(h):
    v = int(h, 16)
    return v - 65536 if v & 0x8000 else v


def shell(*cmds, until, timeout):
    text = ""
    for i, c in enumerate(cmds):
        args = [SHELL, "-q", "-t", str(timeout)]
        if i == len(cmds) - 1:
            args += ["-u", until]
        out = subprocess.run(args + [c], capture_output=True, text=True)
        text += out.stdout + out.stderr
    return text


def check_dump(text):
    cases, cur, announced = [], None, None
    for line in text.splitlines():
        m = re.search(r"FMC END (\d+)", line)
        if m:
            announced = int(m.group(1))
            continue
        m = re.search(r"FMC F (\S+) (\d+) (\d+) (\d+) (\d+) (\d+)", line)
        if m:
            cur = {"name": m.group(1), "gain": int(m.group(4)), "clip": int(m.group(5)) != 0,
                   "B": [], "A": [], "X": [], "Y": []}
            cases.append(cur)
            continue
        m = re.search(r"FMC ([BAXY])((?: [0-9A-F]{4})+)\s*$", line)
        if m and cur is not None:
            cur[m.group(1)] += [s16(h) for h in m.group(2).split()]

    if not cases:
        print("no FMC lines: is example 102 on the board? (fmac dump)")
        return 2
    fails = 0
    if announced is not None and announced != len(cases):
        print("cases lost: board sent %d, parsed %d" % (announced, len(cases)))
        fails += 1

    total = [0] * len(MODELS)
    print("%-9s %4s %4s %2s %2s  %-23s %s" % ("filter", "nb", "na", "R", "cl",
                                              "differing samples m0..3", "float err (LSB)"))
    for c in cases:
        b, a, x, y = c["B"], c["A"], c["X"], c["Y"]
        if len(y) != len(x):
            print("%s: %d inputs, %d outputs" % (c["name"], len(x), len(y)))
            fails += 1
            continue
        diff = []
        for m in range(len(MODELS)):
            ref = model(b, a, c["gain"], c["clip"], x, m)
            diff.append(sum(1 for p, q in zip(ref, y) if p != q))
            total[m] += diff[-1]
        # ideal filter, same coefficients, float: only for the non-overflowing cases
        bb = np.array(b) / 32768 * 2 ** c["gain"]
        aa = np.concatenate([[1.0], -np.array(a) / 32768 * 2 ** c["gain"]]) if a else [1.0]
        yf = signal.lfilter(bb, aa, np.array(x) / 32768) * 32768
        err = "%.1f" % np.max(np.abs(yf - np.array(y))) if np.max(np.abs(yf)) < 32767 else "overflow"
        print("%-9s %4d %4d %2d %2d  %-23s %s" % (c["name"], len(b), len(a), c["gain"],
                                                  int(c["clip"]), " ".join(map(str, diff)), err))

    match = [m for m in range(len(MODELS)) if total[m] == 0]
    if match:
        print("FMAC arithmetic: %s (model %d)" % (MODELS[match[0]], match[0]))
    else:
        print("no model matches every sample")
        fails += 1
    print("%d cases -> %s" % (len(cases), "PASS" if fails == 0 else "FAIL"))
    return 0 if fails == 0 else 1


def check_sweep(name, path=None):
    filters = design()
    if name not in filters:
        print("filters: " + " ".join(filters))
        return 2
    if path:
        text = open(path).read()
    else:
        text = shell("fmac filter " + name, "fmac sweep 100 4000 100", until="FMS END", timeout=120)
    if "no signal" in text:
        print("no signal on the ADC: is the jumper in place?")
        return 2
    rows = re.findall(r"FMS\s+(\d+)\.(\d{3})\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)", text)
    if not rows:
        print("no FMS lines: is example 102 on the board?")
        return 2
    b, a, gain = filters[name]
    bb = b / 32768 * 2 ** gain
    aa = np.concatenate([[1.0], -a / 32768 * 2 ** gain]) if a is not None else [1.0]
    fails = 0
    print("%10s %9s %9s %8s %7s" % ("Hz", "level", "measured", "design", "error"))
    for hz_i, hz_f, lin, _lout, g in rows:
        hz = int(hz_i) + int(hz_f) / 1000
        _, h = signal.freqz(bb, aa, worN=[hz], fs=FS)
        d = 20 * np.log10(max(abs(h[0]), 1e-12))
        meas = float(g)
        if d >= -40:
            ok = abs(meas - d) <= 0.5
        else:
            ok = meas <= -35
        fails += 0 if ok else 1
        print("%10.3f %9s %9.2f %8.2f %7.2f %s" % (hz, lin, meas, d, meas - d, "" if ok else "<- FAIL"))
    print("%s: %d tones, %d out of bounds -> %s"
          % (name, len(rows), fails, "PASS" if fails == 0 else "FAIL"))
    return 0 if fails == 0 else 1


def main():
    args = sys.argv[1:]
    if args and args[0] == "--tables":
        return tables()
    if args and args[0] == "--sweep":
        return check_sweep(args[1] if len(args) > 1 else "lp1k", args[2] if len(args) > 2 else None)
    if args:
        text = open(args[0]).read()
    else:
        text = shell("fmac dump", until="FMC END", timeout=30)
    return check_dump(text)


if __name__ == "__main__":
    sys.exit(main())
