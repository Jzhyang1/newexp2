#!/usr/bin/env python3
"""Summarise the local-disk study (tools/capture/localdisk_study.py) -> capture_results/localdisk_*.png/json.

  ./bin/python tools/capture/localdisk_analyze.py
"""
import glob
import json
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LD = os.path.join(ROOT, "logs/localdisk")
OUT = os.path.join(ROOT, "capture_results")
PAGE, NQ = 65536, 20000
HIT_NS, MISS_NS, BW = 30e3, 300e3, 1e9          # default latency model: local hit, remote request, remote bytes/s
SIZES = [128, 256, 512, 1024]
WINDOWS = [2, 8, 32, 128, 512]

SURF, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e6e5e1"
BLUE, ORANGE, AQUA, YELLOW, MAGENTA, VIOLET = "#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#6250d6"
plt.rcParams.update({
    "figure.facecolor": SURF, "axes.facecolor": SURF, "savefig.facecolor": SURF, "axes.edgecolor": GRID,
    "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2, "text.color": INK, "axes.titlecolor": INK,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "axes.spines.top": False, "axes.spines.right": False,
    "font.size": 9.5, "axes.titlesize": 10.5, "axes.titleweight": "bold", "axes.titlelocation": "left",
    "legend.frameon": False, "figure.dpi": 130,
})


def reads(run):
    f = pd.read_csv(os.path.join(run, "features.csv"))
    meta = json.load(open(os.path.join(ROOT, "disk/data/faiss_data.img.meta.json")))
    return f[(f.type == 0) & (f.offset < meta["tail_offset"])]


def metrics(run, exact_bytes, nocache=False):
    """Per-run counters.  exact_bytes: no page cache at all, so remote reads are exactly the guest's request sizes."""
    f = reads(run)
    full = (f.hits == f.total).to_numpy() & (not nocache)
    if exact_bytes:
        miss_bytes = f.length.to_numpy().astype(float)
    else:
        miss_bytes = ((f.total - f.hits) * PAGE).to_numpy().astype(float)
    pf_bytes = 0.0
    p = os.path.join(run, "prefetch.csv")
    if os.path.exists(p) and os.path.getsize(p) > 50:
        pf = pd.read_csv(p)
        pf_bytes = float(((pf.total - pf.hits) * PAGE).sum())
    return dict(reads=len(f), full_hit=full.mean(), compute_s=None, guest_bytes=float(f.length.sum()),
                demand_bytes=float(miss_bytes[~full].sum()), pf_bytes=pf_bytes,
                _full=full, _miss_bytes=miss_bytes)


COMPUTE_S = 16.0   # guest compute for 20k queries with free I/O; quiet single-VM runs gave 15-26 s (noisy), 15-17.8 s for the cleanest


def compute_for(arm, L):
    return COMPUTE_S


def stall_s(m, hit=HIT_NS, miss=MISS_NS, bw=BW, C=None):
    """Modeled I/O stall (s): a fully-local read costs `hit`; any read with a miss costs `miss` + transfer of the missing bytes."""
    full, mb = m["_full"], m["_miss_bytes"]
    t = float((full.sum() * hit) + ((~full).sum() * miss) + mb[~full].sum() / bw * 1e9) / 1e9
    if C is not None:
        # the remote link must carry demand + prefetch bytes within the run: if that takes longer than compute + stall, the link is the bottleneck
        t = max(t, (m["demand_bytes"] + m["pf_bytes"]) / bw - C)
    return t


def compute_s(name):
    p = os.path.join(LD, name, "out/query_latency.json")
    return float(np.sum(json.load(open(p))["lat_ms"])) / 1e3 if os.path.exists(p) else float("nan")


def collect():
    rows = []
    for rep in range(3):
        m = metrics(os.path.join(LD, f"ld_base_r{rep}"), True, nocache=True)
        rows.append(dict(arm="none", L=0, rep=rep, m=m))
        for L in SIZES:
            rows.append(dict(arm="ext", L=L, rep=rep, m=metrics(os.path.join(LD, f"ld_ext_L{L}_r{rep}"), True)))
            for arm in ["lru", "quickmine", "oracle"] + [f"ra{w}" for w in WINDOWS]:
                rows.append(dict(arm=arm, L=L, rep=rep, m=metrics(os.path.join(LD, f"ld_rp_{arm}_L{L}_r{rep}"), False)))
    return rows


def table(rows, **kw):
    out = []
    for (arm, L), g in pd.DataFrame([dict(arm=r["arm"], L=r["L"], rep=r["rep"],
                                          reads=r["m"]["reads"], full_hit=r["m"]["full_hit"],
                                          remote_reqs=r["m"]["reads"] * (1 - r["m"]["full_hit"]),
                                          demand_gib=r["m"]["demand_bytes"] / 2**30, pf_gib=r["m"]["pf_bytes"] / 2**30,
                                          stall_per_q_us=stall_s(r["m"], C=compute_for(r["arm"], r["L"]), **kw) / NQ * 1e6,
                                          stall_nolink_us=stall_s(r["m"], **kw) / NQ * 1e6) for r in rows]).groupby(["arm", "L"]):
        d = dict(arm=arm, L=L, n=len(g))
        for c in ["reads", "full_hit", "remote_reqs", "demand_gib", "pf_gib", "stall_per_q_us", "stall_nolink_us"]:
            d[c] = g[c].mean()
            d[c + "_min"], d[c + "_max"] = g[c].min(), g[c].max()
        d["total_gib"] = d["demand_gib"] + d["pf_gib"]
        out.append(d)
    return pd.DataFrame(out)


def main():
    os.makedirs(OUT, exist_ok=True)
    rows = collect()
    T = table(rows)
    T.to_csv(os.path.join(OUT, "localdisk_table.csv"), index=False)
    pd.set_option("display.width", 250, "display.max_columns", 30)
    print(T[["arm", "L", "reads", "full_hit", "remote_reqs", "demand_gib", "pf_gib", "stall_nolink_us", "stall_per_q_us", "stall_per_q_us_min", "stall_per_q_us_max"]].round(3).to_string())

    # ---------------- fig 1: stall per query vs local size
    def series(arm):
        t = T[T.arm == arm].sort_values("L")
        return t.L.to_numpy(), t.stall_per_q_us.to_numpy(), t.stall_per_q_us_min.to_numpy(), t.stall_per_q_us_max.to_numpy()
    none = T[T.arm == "none"].stall_per_q_us.iloc[0]
    best_ra = {L: min(WINDOWS, key=lambda w: T[(T.arm == f"ra{w}") & (T.L == L)].stall_per_q_us.iloc[0]) for L in SIZES}
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    spec = [("ext", "more guest RAM (disk-as-page-cache)", BLUE, "o"), ("lru", "local LRU cache, no prefetch", INK2, "s"),
            ("readahead", "local cache + readahead (best window per size)", ORANGE, "^"),
            ("quickmine", "local cache + quickmine", VIOLET, "D"), ("oracle", "local cache + oracle", AQUA, "v")]
    for key, label, col, mk in spec:
        if key == "readahead":
            x = np.array(SIZES)
            y = np.array([T[(T.arm == f"ra{best_ra[L]}") & (T.L == L)].stall_per_q_us.iloc[0] for L in SIZES])
            lo = y; hi = y
        else:
            x, y, lo, hi = series(key)
        ax[0].errorbar(x, y, yerr=[y - lo, hi - y], color=col, marker=mk, ms=5, lw=1.8, capsize=2, label=label)
    ax[0].axhline(none, color=GRID, lw=1.2); ax[0].text(SIZES[0], none * 1.02, "no local cache, 1 GiB RAM", ha="left", color=INK2, fontsize=8.5)
    ax[0].set_xscale("log", base=2); ax[0].set_xticks(SIZES); ax[0].set_xticklabels([f"{s} MiB" for s in SIZES])
    ax[0].set_xlabel("local disk budget L (same MiB added as RAM in the 'more RAM' arm)"); ax[0].set_ylabel("modeled I/O stall per query (µs)")
    ax[0].set_title("Modeled I/O stall per query (300 µs remote, 1 GB/s)"); ax[0].legend(fontsize=8, loc="upper center", bbox_to_anchor=(0.5, -0.2), ncol=2)
    # panel 2: readahead windows
    for L, col in zip(SIZES, [YELLOW, ORANGE, MAGENTA, VIOLET]):
        t = [T[(T.arm == f"ra{w}") & (T.L == L)].stall_per_q_us.iloc[0] for w in WINDOWS]
        ax[1].plot([w * 64 for w in WINDOWS], t, color=col, marker="o", ms=4, lw=1.6, label=f"L = {L} MiB")
    ax[1].set_yscale("log"); ax[1].set_xscale("log", base=2); ax[1].set_xlabel("readahead window (KiB after every request)"); ax[1].set_ylabel("modeled I/O stall per query (µs)")
    ax[1].set_title("Readahead window sweep"); ax[1].legend(fontsize=8)
    fig.tight_layout(); fig.savefig(os.path.join(OUT, "ld_fig1_stall_vs_size.png"), bbox_inches="tight"); plt.close(fig)

    # ---------------- fig 2: remote requests per query vs remote traffic at L=256
    L = 256
    fig, ax = plt.subplots(figsize=(6.6, 4.3))
    for _, r in T[T.L == L].iterrows():
        col = {"ext": BLUE, "lru": INK2, "quickmine": VIOLET, "oracle": AQUA}.get(r.arm, ORANGE)
        y = r.remote_reqs / NQ
        ax.scatter(r.total_gib, y, s=48, color=col, zorder=3)
        if r.arm.startswith("ra"):
            kib = int(r.arm[2:]) * 64
            lab = f"RA {kib} KiB" if kib < 1024 else f"RA {kib // 1024} MiB"
        else:
            lab = {"ext": "more RAM", "lru": "LRU", "quickmine": "quickmine", "oracle": "oracle"}[r.arm]
        ax.annotate(lab, (r.total_gib, y), textcoords="offset points", xytext=(6, 4), fontsize=8, color=INK2)
    ax.set_xscale("log"); ax.set_xlabel("bytes pulled from remote storage (GiB, demand + prefetch)")
    ax.set_ylabel("remote requests per query (demand misses)"); ax.set_title(f"What each arm asks of remote storage (L = {L} MiB)")
    fig.tight_layout(); fig.savefig(os.path.join(OUT, "ld_fig2_hit_vs_traffic.png"), bbox_inches="tight"); plt.close(fig)

    # ---------------- fig 3: sensitivity (ratio best realistic prefetch / more RAM) over latency x bandwidth
    lats = [100e3, 300e3, 1e6, 3e6]; bws = [100e6, 250e6, 1e9, 4e9]
    fig, axs = plt.subplots(1, 3, figsize=(12.5, 3.8))
    for ax, (title, pick) in zip(axs, [("best readahead window", "ra"), ("quickmine", "quickmine"), ("oracle", "oracle")]):
        M = np.zeros((len(bws), len(lats)))
        for i, bw in enumerate(bws):
            for j, lt in enumerate(lats):
                Tt = table(rows, miss=lt, bw=bw)
                e = Tt[(Tt.arm == "ext") & (Tt.L == 256)].stall_per_q_us.iloc[0]
                if pick == "ra":
                    p = min(Tt[(Tt.arm == f"ra{w}") & (Tt.L == 256)].stall_per_q_us.iloc[0] for w in WINDOWS)
                else:
                    p = Tt[(Tt.arm == pick) & (Tt.L == 256)].stall_per_q_us.iloc[0]
                M[i, j] = p / e
        im = ax.imshow(np.log2(M), cmap="RdBu_r", vmin=-3, vmax=3, origin="lower", aspect="auto")
        for i in range(len(bws)):
            for j in range(len(lats)):
                ax.text(j, i, f"{M[i, j]:.2f}", ha="center", va="center", fontsize=8.5, color="white" if abs(np.log2(M[i, j])) > 1.6 else INK)
        ax.set_xticks(range(len(lats))); ax.set_xticklabels(["100 µs", "300 µs", "1 ms", "3 ms"]); ax.set_yticks(range(len(bws)))
        ax.set_yticklabels(["100 MB/s", "250 MB/s", "1 GB/s", "4 GB/s"]); ax.grid(False)
        ax.set_xlabel("remote request latency"); ax.set_title(f"{title}: stall / more-RAM stall")
    axs[0].set_ylabel("remote bandwidth")
    fig.tight_layout(); fig.savefig(os.path.join(OUT, "ld_fig3_sensitivity.png"), bbox_inches="tight"); plt.close(fig)
    json.dump(json.loads(T.to_json(orient="records")), open(os.path.join(OUT, "localdisk_numbers.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
