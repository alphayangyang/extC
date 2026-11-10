#!/usr/bin/env python3
"""Regenerate figures/roofline.pdf and data/roofline-pareto.tsv from the calibrated constants.

Constants are the calibration of docs/analysis/CALIBRATION.md (Table 6 in the paper).
Run from the paper/ directory:  python3 figures/gen_roofline.py
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np

C_T = 0.4114          # ns per live byte traced
C_S_F = 0.01404       # c_s * F_slab: sweep floor, ns per heap byte
KAPPA = 0.772         # measured retained-garbage ratio (typical)
KAPPA_CONS = 1.0      # conservative value used by the contract
TAU_T = 17.49         # ns per marked object
TAU_S = 0.458         # ns per freed slot
W_S = 8               # bitmap words swept per step
G_EDGES = 2.76        # pushes per marked object
K_EVERY = 64          # allocations per progress unit
B_DEF = 256           # default batch
L_BYTES = 2.333e6     # peak live set of the digraph shape
A_MUT = 0.335         # GB/s, mutator allocation rate
A_PEAK = 10.2         # GB/s, mutator platform
RHO = 1.0
R_EFF_OFF = 0.4       # measured offset between peak-round and effective ratio


def lam(r):
    r = np.asarray(r, dtype=float)
    return (C_T + C_S_F * r) / (r - 1.0)


def r_min(b):
    return 1.0 + G_EDGES * K_EVERY / float(b)


def r_max(m_max_mb, kappa=KAPPA):
    return (m_max_mb * 1e6 / L_BYTES - 1.0) / kappa


def tax(lam_ns_per_b, a_gb_s=A_MUT):
    x = lam_ns_per_b * a_gb_s
    return x / (1.0 + x)


def fig_plane(ax):
    lam_ridge = RHO / A_PEAK
    r = np.logspace(np.log10(1.02), np.log10(40.0), 600)
    x = lam(r)
    roof = RHO / x
    binding = roof <= A_PEAK * 1.0001
    ax.plot(x[~binding], roof[~binding], color="#1f4e9c", lw=0.9, ls=(0, (2, 2)), alpha=0.55)
    ax.plot(x[binding], roof[binding], color="#1f4e9c", lw=2.0,
            label="compute roof  $a=\\rho/\\lambda$")
    ax.axhline(A_PEAK, color="#2e7d32", lw=2.0,
               label="mutator platform  $a_{peak}=10.2$ GB/s")
    ax.axvline(lam_ridge, color="0.45", ls=":", lw=1.1)
    ax.annotate("ridge $\\lambda^\\ast=0.098$", xy=(lam_ridge, A_PEAK),
                xytext=(lam_ridge * 0.62, A_PEAK * 1.30), fontsize=7.5, color="0.35")

    lam_keep = float(lam(r_min(B_DEF) + R_EFF_OFF))
    lam_min = float(lam(r_max(7.0, KAPPA) + R_EFF_OFF))
    ax.axvspan(lam_min, lam_keep, color="#c8e6c9", alpha=0.55, lw=0, label="feasible window")
    ax.axvline(lam_min, color="#c62828", ls="--", lw=1.2)
    ax.axvline(lam_keep, color="#2e7d32", ls="--", lw=1.2)
    ax.annotate("memory wall", xy=(lam_min, 14.0), xytext=(lam_min - 0.062, 14.0),
                fontsize=7.2, color="#8e0000", va="center")
    ax.annotate("keep-up wall", xy=(lam_keep, 14.0), xytext=(lam_keep + 0.012, 14.0),
                fontsize=7.2, color="#1b5e20", va="center")

    ax.plot([0.2508], [0.309], marker="*", ms=13, color="#c62828", zorder=5,
            label="extC, chase (measured)")
    ax.annotate("$(0.251,\\ 0.309)$", xy=(0.2508, 0.309), xytext=(0.262, 0.185), fontsize=7.5)

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(0.055, 0.60)
    ax.set_ylim(0.13, 22.0)
    ax.xaxis.set_major_locator(mticker.FixedLocator([0.06, 0.1, 0.15, 0.2, 0.3, 0.4]))
    ax.set_xticklabels(["0.06", "0.1", "0.15", "0.2", "0.3", "0.4"])
    ax.set_xlabel("reclamation cost  $\\lambda$  (ns per byte reclaimed)")
    ax.set_ylabel("sustainable allocation rate  $a$  (GB/s)")
    ax.set_title("(a) roofline plane: which roof binds", fontsize=9)
    ax.grid(True, which="both", ls=":", lw=0.4, alpha=0.5)
    ax.legend(fontsize=6.8, loc="lower left", framealpha=0.93)


def fig_pareto(ax):
    mb = np.linspace(3.2, 30.0, 300)
    reff = r_max(mb, KAPPA) + R_EFF_OFF
    tx = 100.0 * tax(lam(reff))
    ax.plot(mb, tx, color="#c62828", lw=1.9)
    ax.set_xlabel("memory budget  $M_{max}$  (MB)")
    ax.set_ylabel("collection tax (% of wall clock)")
    ax.set_title("(b) what memory buys: the knee is near $M=5L$", fontsize=9)
    ax.set_xlim(3.0, 30.0)
    ax.set_ylim(0.0, 14.0)
    ax.grid(True, ls=":", lw=0.4, alpha=0.5)
    for m in (7.0, 9.0, 12.0, 21.0, 28.0):
        re = float(r_max(m, KAPPA) + R_EFF_OFF)
        t = 100.0 * tax(lam(re))
        ax.plot([m], [t], "o", ms=3.5, color="#1f4e9c")
        ax.annotate("%d MB\n%.1f%%" % (int(m), t), xy=(m, t), xytext=(m + 0.45, t + 0.6),
                    fontsize=6.6)
    ax.axvline(3 * L_BYTES / 1e6, color="0.45", ls=":", lw=1.1)
    ax.annotate("default $3L$", xy=(3 * L_BYTES / 1e6, 12.5),
                xytext=(3 * L_BYTES / 1e6 + 0.5, 12.6), fontsize=7.0, color="0.3")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10.0, 3.9))
    fig_plane(ax1)
    fig_pareto(ax2)
    fig.tight_layout()
    out = os.path.join(root, "figures", "roofline.pdf")
    fig.savefig(out, bbox_inches="tight")
    fig.savefig(out.replace(".pdf", ".png"), dpi=200, bbox_inches="tight")
    print("wrote", out)

    rows = ["# M_max_MB\tr_max_typ\tr_max_cons\tr_eff\tlambda_ns_per_B\ttax_pct_a0.335\ttax_pct_a1.0"]
    for m in (7.0, 9.0, 12.0, 21.0, 28.0):
        rt = float(r_max(m, KAPPA))
        rc = float(r_max(m, KAPPA_CONS))
        re = rt + R_EFF_OFF
        lv = float(lam(re))
        rows.append("%.2f\t%.3f\t%.3f\t%.3f\t%.4f\t%.2f\t%.2f"
                    % (m, rt, rc, re, lv, 100 * tax(lv, 0.335), 100 * tax(lv, 1.0)))
    dout = os.path.join(root, "data", "roofline-pareto.tsv")
    with open(dout, "w") as f:
        f.write("\n".join(rows) + "\n")
    print("wrote", dout)
    print("r_min(256)=%.3f lambda_keep=%.4f lambda_min(7MB)=%.4f"
          % (r_min(B_DEF), float(lam(r_min(B_DEF) + R_EFF_OFF)),
             float(lam(r_max(7.0, KAPPA) + R_EFF_OFF))))


if __name__ == "__main__":
    main()
