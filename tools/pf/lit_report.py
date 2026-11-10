#!/usr/bin/env python3
"""Figures + tables for capture_results/PUBLISHED_PREFETCHERS.md from tools/pf/lit_eval.py output (logs/pf_lit/).

  tools/pf/lit_report.py            -> capture_results/lit/*.png, tables on stdout (markdown)
"""
import collections, csv, os
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGS = os.path.join(ROOT, "logs", "pf_lit")
OUT = os.path.join(ROOT, "capture_results", "lit")
SETS = {0: ["grid_d0", "hyb_d0", "ppfw_d0", "ppfs_d0"], 2: ["grid_d2", "hyb_d2", "ppfw_d2"]}
ALIAS = {"learned_hi": "learned"}

# reference categorical palette (light), fixed order; never cycled
C = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
INK, INK2, GRID, SURF = "#1f1f1e", "#5f5e58", "#e4e3de", "#fcfcfb"
SHOW = [  # family, label, color slot, marker
    ("learned", "learned (ours)", 0, "o"), ("readahead", "readahead", 1, "s"), ("bop", "BOP", 2, "D"),
    ("bingo", "Bingo", 3, "^"), ("spp_x", "SPP", 4, "v"), ("ppf", "SPP+PPF", 5, "P"),
    ("leap", "Leap", 6, "X"), ("pythia", "Pythia", 7, "*"),
]


def load(delay):
    rows, seen = [], set()
    for n in SETS[delay]:
        p = os.path.join(LOGS, n + ".csv")
        if not os.path.exists(p):
            continue
        for r in csv.DictReader(open(p)):
            r["family"] = ALIAS.get(r["family"], r["family"])
            k = (r["family"], r["spec"], r["stream"])
            if k in seen:
                continue
            seen.add(k); rows.append(r)
    return rows


class Frontier:
    def __init__(self, rows, metric="full_hit"):
        self.none, self.pts = {}, collections.defaultdict(list)
        for r in rows:
            if r["family"] == "none":
                self.none[r["stream"]] = float(r[metric])
            else:
                self.pts[(r["family"], r["stream"])].append((float(r["traffic_rel"]), float(r[metric])))
        self.streams = sorted(self.none)

    def at(self, fam, s, B):
        """best hit reachable at <= B x traffic, linear between frontier points, anchored at (1.0, no prefetch)"""
        pts = sorted([(1.0, self.none[s])] + self.pts[(fam, s)])
        xs, ys = [], []
        for t, h in pts:
            if not ys or h > ys[-1]:
                xs.append(t); ys.append(h)
        return ys[-1] if B >= xs[-1] else float(np.interp(B, xs, ys))

    def mean(self, fam, B, prefix=None):
        ss = [s for s in self.streams if s != "y_nt" and (prefix is None or s.startswith(prefix))]
        return float(np.mean([self.at(fam, s, B) for s in ss]))


def style(ax):
    ax.set_facecolor(SURF)
    for k in ("top", "right"):
        ax.spines[k].set_visible(False)
    for k in ("left", "bottom"):
        ax.spines[k].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=9)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def fig_frontier(F0, F2):
    Bs = np.linspace(1.0, 3.0, 41)
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.6), sharey=True, facecolor=SURF)
    for ax, F, title in ((axes[0], F0, "Instant prefetch (delay 0)"), (axes[1], F2, "Prefetch lands 2 requests later (delay 2)")):
        style(ax)
        ends = []
        for fam, lab, ci, mk in SHOW:
            ys = [F.mean(fam, B) for B in Bs]
            ax.plot(Bs, ys, color=C[ci], linewidth=2, label=lab)
            ax.plot(Bs[::8], ys[::8], linestyle="none", marker=mk, markersize=8, color=C[ci], markeredgecolor=SURF, markeredgewidth=1.5)
            ends.append([ys[-1], lab])
        ends.sort()
        for i in range(1, len(ends)):                      # nudge end labels apart
            ends[i][0] = max(ends[i][0], ends[i - 1][0] + 2.6)
        for y, lab in ends:
            ax.text(3.04, y, lab, fontsize=8.5, color=INK, va="center")
        ax.set_xlim(1.0, 3.0)
        ax.set_title(title, fontsize=11, color=INK, loc="left")
        ax.set_xlabel("traffic budget (backend pages / no-prefetch demand pages)", fontsize=9, color=INK2)
    axes[0].set_ylabel("full-hit %  (mean of 11 FAISS + Thesios streams)", fontsize=9, color=INK2)
    axes[1].legend(loc="lower right", fontsize=8, frameon=False, ncol=2)
    fig.tight_layout(w_pad=6)
    fig.savefig(os.path.join(OUT, "frontier.png"), dpi=150, facecolor=SURF)
    plt.close(fig)


def fig_bars(F2):
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.8), sharex=True, facecolor=SURF)
    for ax, (pref, title) in zip(axes, (("f_", "FAISS (7 streams)"), ("th", "Thesios (4 streams)"))):
        style(ax); ax.grid(axis="y", visible=False)
        vals = [(F2.mean(f, 2.0, pref), lab) for f, lab, _, _ in SHOW]
        nones = np.mean([F2.none[s] for s in F2.streams if s.startswith(pref)])
        vals.sort()
        y = np.arange(len(vals))
        ax.barh(y, [v for v, _ in vals], height=0.62, color=[C[0] if l.startswith("learned") else "#9e9d96" for _, l in vals])
        for yi, (v, _) in zip(y, vals):
            ax.text(v + 1, yi, f"{v:.1f}", va="center", fontsize=8.5, color=INK)
        ax.axvline(nones, color=INK2, linewidth=1, linestyle=(0, (3, 3)))
        ax.text(nones + 0.8, len(vals) - 0.45, "no prefetch", fontsize=8, color=INK2)
        ax.set_yticks(y, [l for _, l in vals], fontsize=9, color=INK)
        ax.set_xlim(0, 100)
        ax.set_title(title, fontsize=11, color=INK, loc="left")
        ax.set_xlabel("full-hit % at a 2.0x traffic budget, delay 2", fontsize=9, color=INK2)
    fig.tight_layout(w_pad=4)
    fig.savefig(os.path.join(OUT, "at2x_delay2.png"), dpi=150, facecolor=SURF)
    plt.close(fig)


HYB = [("learned+bingo", "+ Bingo (union)"), ("learned+bop", "+ BOP (union)"), ("learned+leap", "+ Leap (union)"),
       ("learned+pythia", "+ Pythia (union)"), ("learned+spp", "+ SPP (union)"), ("learned+ppf", "+ SPP+PPF (union)"),
       ("learned+ra2", "+ 2-page readahead floor"), ("learned+bingo60", "+ Bingo, vote 60%"),
       ("learnedppf", "PPF filter on learned"), ("learnedppf_shape", "PPF filter, shape features only")]


def fig_hybrids(F0, F2):
    fig, ax = plt.subplots(figsize=(8.5, 4.4), facecolor=SURF)
    style(ax); ax.grid(axis="y", visible=False)
    labs = [l for _, l in HYB]
    y = np.arange(len(HYB))
    for k, (F, name, ci) in enumerate(((F0, "delay 0", 0), (F2, "delay 2", 1))):
        base = F.mean("learned", 2.0)
        d = [F.mean(f, 2.0) - base if F.pts.get((f, "f_m1g")) else np.nan for f, _ in HYB]
        ax.barh(y + (k - 0.5) * 0.36, d, height=0.34, color=C[ci], label=name)
    ax.axvline(0, color=INK, linewidth=1)
    ax.set_yticks(y, labs, fontsize=9, color=INK)
    ax.invert_yaxis()
    ax.set_xlabel("change in mean full-hit % vs learned alone, at a 2.0x traffic budget (points)", fontsize=9, color=INK2)
    ax.set_title("Adding published ideas to learned", fontsize=11, color=INK, loc="left")
    ax.legend(frameon=False, fontsize=8.5, loc="upper left")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "hybrids.png"), dpi=150, facecolor=SURF)
    plt.close(fig)


def tables(F0, F2, rows0):
    print("### Mean full-hit % at a traffic budget (11 FAISS + Thesios streams)\n")
    print("| policy | d0 @1.25x | d0 @1.5x | d0 @2x | d0 @3x | d2 @1.5x | d2 @2x | d2 @3x |")
    print("|---|---|---|---|---|---|---|---|")
    for f, lab, _, _ in SHOW:
        v = [F0.mean(f, b) for b in (1.25, 1.5, 2, 3)] + [F2.mean(f, b) for b in (1.5, 2, 3)]
        print(f"| {lab} | " + " | ".join(f"{x:.1f}" for x in v) + " |")
    print("\n### Per workload at 2x (full hit %, d0 / d2)\n")
    print("| policy | FAISS d0 | FAISS d2 | Thesios d0 | Thesios d2 |")
    print("|---|---|---|---|---|")
    for f, lab, _, _ in SHOW:
        print(f"| {lab} | {F0.mean(f, 2, 'f_'):.1f} | {F2.mean(f, 2, 'f_'):.1f} | {F0.mean(f, 2, 'th'):.1f} | {F2.mean(f, 2, 'th'):.1f} |")
    print("\n### Hybrids: change vs learned at 2x (points; FAISS / Thesios / all)\n")
    print("| variant | d0 all | d0 FAISS | d0 Thesios | d2 all | d2 FAISS | d2 Thesios |")
    print("|---|---|---|---|---|---|---|")
    for f, lab in HYB:
        out = []
        for F in (F0, F2):
            if not F.pts.get((f, "f_m1g")):
                out += ["-"] * 3; continue
            for p in (None, "f_", "th"):
                out.append(f"{F.mean(f, 2, p) - F.mean('learned', 2, p):+.1f}")
        print(f"| {lab} | " + " | ".join(out) + " |")
    print("\n### Random-access control (YCSB): traffic at each policy's default spec\n")
    dflt = {"learned": "learned:theta=40", "readahead": "readahead:pages=8", "bop": "bop:degree=1", "bingo": "bingo:vote=20",
            "spp_x": "spp:cross=1,tp=25", "ppf": "ppf:cross=1,tau=95", "leap": "leap:pwmax=8", "pythia": "pythia"}
    print("| policy | spec | YCSB traffic | YCSB full hit |")
    print("|---|---|---|---|")
    for f, lab, _, _ in SHOW:
        for r in rows0:
            if r["family"] == f and r["spec"] == dflt[f] and r["stream"] == "y_nt":
                print(f"| {lab} | `{r['spec']}` | {float(r['traffic_rel']):.2f}x | {float(r['full_hit']):.1f} |")


def main():
    os.makedirs(OUT, exist_ok=True)
    r0, r2 = load(0), load(2)
    F0, F2 = Frontier(r0), Frontier(r2)
    fig_frontier(F0, F2); fig_bars(F2); fig_hybrids(F0, F2)
    tables(F0, F2, r0)


if __name__ == "__main__":
    main()
