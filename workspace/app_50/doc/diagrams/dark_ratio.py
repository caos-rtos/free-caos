#!/usr/bin/env python3
"""Why dividing by the dark channel destroys the measurement.

Two panels, one argument:

  (a) the ratio CLEAR/D at CONSTANT light. The whole measurement is the gap
      between the curve and the asymptote at 1 - and that gap collapses as the
      dark current grows. The shading is the signal; it vanishes.

  (b) what a +10% change in light does to each estimator, log scale. The
      difference answers with 10% at any temperature; the ratio answers with
      5% cold and 0.02% hot.

Model: dark current doubles every 100/9 degrees, so it grows exactly x512 over
-20..+80 - the most FAVOURABLE case quoted in par. 1.7 of the guide. Light is
taken constant and equal to the dark at -20, i.e. L/D = 1 at the cold end.

Run by the Makefile:  python3 dark_ratio.py <output.pdf>
"""

import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

T_MIN, T_MAX = -20.0, 80.0
DOUBLE_EVERY = (T_MAX - T_MIN) / 9.0     # 100/9 degrees -> x512 over the span
STEP = 0.10                              # the +10% light step of panel (b)

# Palette: readable in grey as well, so line style carries the meaning too.
C_RATIO = "#c0392b"
C_DIFF = "#1f6f8b"
C_GREY = "#8a8a8a"


def dark(t):
    """Dark current, normalised to 1 at T_MIN."""
    return 2.0 ** ((t - T_MIN) / DOUBLE_EVERY)


def main(out):
    t = np.linspace(T_MIN, T_MAX, 400)
    d = dark(t)
    light = 1.0                    # constant light, equal to the dark at -20 C
    ratio = light / d + 1.0        # CLEAR/D = L/D + 1

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.4, 3.1))

    # -- (a) the ratio collapses onto its own asymptote --------------------
    ax1.plot(t, ratio, color=C_RATIO, lw=2.0, label="rapporto  CLEAR / D")
    ax1.axhline(1.0, color=C_GREY, lw=1.0, ls="--",
                label="asintoto 1: nessuna informazione")
    ax1.fill_between(t, 1.0, ratio, color=C_RATIO, alpha=0.13)
    ax1.legend(fontsize=7.2, loc="upper right", framealpha=0.9)
    ax1.annotate("tutta la misura\ne' questo scarto",
                 xy=(-8, 1.55), xytext=(14, 1.72),
                 fontsize=7.5, color=C_RATIO,
                 arrowprops=dict(arrowstyle="->", color=C_RATIO, lw=0.9))
    ax1.set_xlim(T_MIN, T_MAX)
    ax1.set_ylim(0.97, 2.35)
    ax1.set_xlabel("temperatura [$^\\circ$C]", fontsize=8)
    ax1.set_ylabel("lettura del rapporto", fontsize=8)
    ax1.set_title("(a) rapporto CLEAR/D, luce COSTANTE", fontsize=8.5)
    ax1.tick_params(labelsize=7.5)
    ax1.grid(alpha=0.25, lw=0.6)
    for x, txt in ((T_MIN, "2,000"), (T_MAX, "1,002")):
        ax1.plot([x], [light / dark(x) + 1.0], "o", color=C_RATIO, ms=4)
        ax1.annotate(txt, xy=(x, light / dark(x) + 1.0),
                     xytext=(6 if x == T_MIN else -4, 6 if x == T_MIN else 11),
                     textcoords="offset points", fontsize=7.5,
                     color=C_RATIO, ha="left" if x == T_MIN else "right")

    # -- (b) sensitivity to a +10% change in the light ---------------------
    # ratio:      ((L(1+s)/D + 1) - (L/D + 1)) / (L/D + 1)
    # difference: ((L(1+s) + D) - D - L) / L = s, flat by construction
    resp_ratio = (light * STEP / d) / ratio * 100.0
    resp_diff = np.full_like(t, STEP * 100.0)

    ax2.semilogy(t, resp_diff, color=C_DIFF, lw=2.0,
                 label="differenza  CLEAR $-$ D")
    ax2.semilogy(t, resp_ratio, color=C_RATIO, lw=2.0, ls="--",
                 label="rapporto  CLEAR / D")
    ax2.set_xlim(T_MIN, T_MAX)
    ax2.set_ylim(5e-3, 60)
    ax2.set_xlabel("temperatura [$^\\circ$C]", fontsize=8)
    ax2.set_ylabel("variazione della lettura [%]", fontsize=8)
    ax2.set_title("(b) risposta a una luce che sale del 10%", fontsize=8.5)
    ax2.tick_params(labelsize=7.5)
    ax2.grid(alpha=0.25, lw=0.6, which="both")
    ax2.legend(fontsize=7.2, loc="lower left", framealpha=0.9)
    # The vertical gap at the hot end is 1 + D/L = 513, NOT the x512 by which
    # the dark itself grew: the ratio starts out a factor 2 down already.
    ax2.annotate("x513", xy=(T_MAX - 1, 0.35), fontsize=7.5, color=C_GREY,
                 ha="right")
    ax2.annotate("", xy=(T_MAX - 3, 10), xytext=(T_MAX - 3, 0.0195),
                 arrowprops=dict(arrowstyle="<->", color=C_GREY, lw=0.9))

    fig.tight_layout()
    fig.savefig(out)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("uso: dark_ratio.py <output.pdf>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
