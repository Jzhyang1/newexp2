#!/usr/bin/env python3
"""Figure for capture_results/ADVERSARIAL_WORKLOADS.md: app-launch root-disk stream, hit vs traffic per policy family.
Reads logs/pf_lit/al_c<cap>_d<delay>.csv (tools/pf/lit_eval.py --streams al_fixed_root,al_shuf_root --extra '--cap N')."""
import csv, collections, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
C = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#6250d6"]
INK, INK2, GRID, SURF = "#1f1f1e", "#5f5e58", "#e4e3de", "#fcfcfb"
SHOW = [("learned", "learned (ours)", 0, "o"), ("cminer", "C-Miner-style", 1, "s"), ("readahead", "readahead", 2, "D"),
        ("bingo", "Bingo", 3, "^"), ("learned+cminer", "learned + C-Miner", 5, "P")]
fig, axes = plt.subplots(1, 2, figsize=(11, 4.4), sharey=True, facecolor=SURF)
for ax, d in zip(axes, (0, 2)):
    rows = list(csv.DictReader(open(f"{ROOT}/logs/pf_lit/al_c512_d{d}.csv")))
    pts = collections.defaultdict(list)
    for r in rows:
        if r["stream"] != "al_fixed_root": continue
        f = "learned" if r["family"] in ("learned_hi", "learned_lo") else r["family"]
        pts[f].append((float(r["traffic_rel"]), float(r["full_hit"])))
    ax.set_facecolor(SURF); ax.grid(True, color=GRID); ax.set_axisbelow(True)
    for k in ("top", "right"): ax.spines[k].set_visible(False)
    for k in ("left", "bottom"): ax.spines[k].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=9)
    nx, ny = pts["none"][0]
    ax.plot([nx], [ny], marker="x", color=INK2, markersize=8, linestyle="none")
    ax.annotate("no prefetch", (nx, ny), textcoords="offset points", xytext=(6, -12), fontsize=8, color=INK2)
    for f, lab, ci, mk in SHOW:
        p = sorted(pts[f])
        ax.plot([x for x, _ in p], [y for _, y in p], color=C[ci], linewidth=2, marker=mk, markersize=7,
                markeredgecolor=SURF, markeredgewidth=1.2, label=lab)
    ax.set_xlim(0.95, 3.0); ax.set_ylim(25, 100)
    ax.set_title(f"delay {d}" + (" (instant prefetch)" if d == 0 else " (prefetch lands 2 requests later)"), fontsize=11, color=INK, loc="left")
    ax.set_xlabel("traffic (backend pages / no-prefetch demand pages)", fontsize=9, color=INK2)
axes[0].set_ylabel("full-hit %  (root disk, 20 cold-launch rounds)", fontsize=9, color=INK2)
axes[1].legend(frameon=False, fontsize=8.5, loc="lower right")
fig.suptitle("Repeated cold app launches, 32 MiB host cache per VM", fontsize=11, color=INK, x=0.01, ha="left")
fig.tight_layout()
os.makedirs(f"{ROOT}/capture_results/lit", exist_ok=True)
fig.savefig(f"{ROOT}/capture_results/lit/app_launch.png", dpi=150, facecolor=SURF)
