#!/usr/bin/env python3
"""Per-feature heatmaps: what pages do the *next k guest misses* touch, given a signal at the current miss?

  ./bin/python tools/capture/signal_heatmaps.py --run logs/capture/ycsb_main --out capture_results/ycsb --k 4 16 64

Model of the host (see the README discussion): it keeps a bounded buffer of recent per-request signals; when
the guest misses, it reads a suffix of that buffer and decides what to prefetch.  Only guest-missed requests
(= NBD reads on the data disk; hits never leave the guest) are rows here, and "the next k accesses" means the
next k such misses, from the same captured stream.

For every miss i and every j in 1..k the pair (feature value at i, page of miss i+j) is counted.
  * delta figures : y = page(i+j) - page(i), in 64 KiB pages, signed log2 bins (0 | 1 | 2-3 | 4-7 | ...)
  * abs   figures : y = page(i+j) itself, equal-width bins over the file (where in the database)
Each column (a bin of the feature) is normalised to sum to 1, so a column is P(next page | feature bin) and
a feature carries information exactly when its columns differ from each other and from the first panel
("all misses", the unconditional distribution).  The mutual information (bits, minus the same quantity with
the feature shuffled across rows, which is the estimator's bias) is printed in each title and written to
heatmap_mi.json.
"""
import argparse
import json
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap, LogNorm
import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_lib as C

SURF, INK, INK2, GRID, BLUE = "#fcfcfb", "#0b0b0b", "#52514e", "#e6e5e1", "#2a78d6"
SEQ = LinearSegmentedColormap.from_list("seq_blue", [SURF, "#cfe0f7", "#7fb0ea", BLUE, "#143f78"])
plt.rcParams.update({
    "figure.facecolor": SURF, "axes.facecolor": SURF, "savefig.facecolor": SURF, "axes.edgecolor": GRID,
    "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2, "text.color": INK, "axes.titlecolor": INK,
    "font.size": 8.5, "axes.titlesize": 9, "axes.titleweight": "bold", "axes.titlelocation": "left", "figure.dpi": 130,
})
PAGE = 65536
NXB = 12          # quantile bins for continuous features
NAB = 48          # equal-width bins for the absolute-page axis


# --------------------------------------------------------------------------------------------------
def load(run):
    r = C.join(run, verbose=True)
    d = r["reads"]
    d = d[(~d.is_probe) & (d.file != "raw-tail")].sort_values("arrive_ns").reset_index(drop=True)
    d = d[d.file == d.file.mode().iloc[0]].reset_index(drop=True)   # the database file's reads (drops fs metadata)
    app = C.load_app(run)
    sysc = app["sys"]
    enter = sysc[sysc.ret.isna()].sort_values("ts")
    samples = pd.read_csv(os.path.join(run, "out/samples.csv"))
    return d, enter, samples, r["meta"]


def build_features(d, enter, samples):
    """One row per miss; every column is something known at the moment this miss is observed."""
    F = pd.DataFrame(index=d.index)
    prev_end = (d.offset + d.length).shift(1)
    F["request length (KiB)"] = d.length / 1024
    F["stride from previous miss (pages)"] = ((d.offset - prev_end) / PAGE)
    F["continues previous miss (0/1)"] = (d.offset == prev_end).astype(int)
    seq = (d.offset == prev_end).astype(int).values
    run = np.zeros(len(d), dtype=int)
    for i in range(1, len(d)):
        run[i] = run[i - 1] + 1 if seq[i] else 0
    F["sequential run length"] = run
    F["inter-arrival (us)"] = d.arrive_ns.diff() / 1e3
    F["host think time (us)"] = d.think_ns / 1e3
    F["requests in flight (other conns)"] = d.inflight
    F["bytes queued behind this request"] = d.queued_bytes
    F["cache hit fraction of request"] = d.hits / d.total.clip(lower=1)
    F["position in file (GiB)"] = d.file_off / 2**30
    # --- guest-side (exact join to the guest block request) ---
    F["guest block flags (rwbs)"] = d.rwbs_flags.astype(str)
    pid_rank = d.g_pid.map(d.g_pid.value_counts().rank(ascending=False, method="first"))
    F["issuing thread (rank by volume)"] = pid_rank
    F["readahead kind (sync/async)"] = d.ra_kind
    F["readahead window (pages)"] = d.ra_win_size
    F["readahead async part (pages)"] = d.ra_win_async
    F["mmap_miss counter"] = d.ra_mmap_miss
    F["guest->host latency (us)"] = d.lat_guest_to_host_us
    # triggering syscall: last file-I/O syscall entered by the issuing thread before the block request
    nr = np.full(len(d), np.nan)
    cnt = np.full(len(d), np.nan)
    for pid, grp in enter.groupby("pid"):
        idx = np.flatnonzero((d.g_pid == pid).values)
        if not len(idx):
            continue
        ts = grp.ts.values
        pos = np.searchsorted(ts, d.g_ts.values[idx], side="right") - 1
        ok = pos >= 0
        nr[idx[ok]] = grp.nr.values[pos[ok]]
        cnt[idx[ok]] = grp.a2.values[pos[ok]].astype(float)
    F["triggering syscall nr"] = nr
    cnt_arr = np.where(np.isin(nr, (0, 17, 19, 295)), cnt, np.nan)  # byte count only exists for the read family
    F["triggering read size (bytes)"] = cnt_arr
    ts = samples.mono_ns.values
    p = np.clip(np.searchsorted(ts, d.g_arrive.values) - 1, 0, len(ts) - 1)
    F["guest page cache (MiB)"] = samples.cached_kb.values[p] / 1024
    F["guest io pressure (psi avg10)"] = pd.to_numeric(samples.io_some_avg10, errors="coerce").values[p]
    F["time since start (s)"] = (d.arrive_ns - d.arrive_ns.iloc[0]) / 1e9
    return F


def bin_feature(s):
    """-> (codes: int array with -1 = excluded, labels). Few distinct values: one bin each; else quantile bins."""
    s = s.copy()
    if not pd.api.types.is_numeric_dtype(s):
        top = s.value_counts().index[:NXB - 1]
        s = s.astype(object).where(s.isin(top), "other")
        cats = list(s.dropna().unique())
        cats.sort(key=lambda c: -int((s == c).sum()))
        m = {c: i for i, c in enumerate(cats)}
        return s.map(m).fillna(-1).astype(int).values, [str(c) for c in cats]
    v = s.astype(float)
    ok = v.notna()
    u = np.unique(v[ok])
    if len(u) <= NXB:
        m = {x: i for i, x in enumerate(u)}
        codes = v.map(m).fillna(-1).astype(int).values
        return codes, [("%g" % x) for x in u]
    edges = np.unique(np.quantile(v[ok], np.linspace(0, 1, NXB + 1)))
    codes = np.full(len(v), -1)
    codes[ok.values] = np.clip(np.searchsorted(edges, v[ok].values, side="right") - 1, 0, len(edges) - 2)
    labels = ["%.3g" % edges[k] for k in range(len(edges) - 1)]
    return codes, labels


def delta_bin(dp):
    m = np.where(dp == 0, 0, np.floor(np.log2(np.abs(dp).clip(min=1))) + 1).astype(int)
    return np.sign(dp).astype(int) * m


def mi_bits(x, y, nx, ny):
    h = np.zeros((nx, ny))
    np.add.at(h, (x, y), 1)
    p = h / h.sum()
    px = p.sum(1, keepdims=True)
    py = p.sum(0, keepdims=True)
    nz = p > 0
    return float((p[nz] * np.log2(p[nz] / (px @ py)[nz])).sum())


def future(pages, k):
    n = len(pages)
    rows, ys = [], []
    for j in range(1, k + 1):
        idx = np.arange(0, n - j)
        rows.append(idx)
        ys.append(pages[idx + j])
    return np.concatenate(rows), np.concatenate(ys)


# --------------------------------------------------------------------------------------------------
def heat(codes, nx, ybin, ny, rows):
    H = np.zeros((ny, nx))
    x = codes[rows]
    ok = x >= 0
    np.add.at(H, (ybin[ok], x[ok]), 1)
    return H


def draw(F, d, k, mode, out, rng, numbers):
    pages = (d.file_off.values // PAGE).astype(np.int64)
    rows, fut = future(pages, k)
    if mode == "delta":
        dp = fut - pages[rows]
        yb = delta_bin(dp)
        lo = yb.min()
        yb = yb - lo
        ny = int(yb.max()) + 1
        tick_vals = np.arange(ny) + lo
        ylab = lambda v: "0" if v == 0 else ("+" if v > 0 else "-") + ("%d" % 2 ** (abs(v) - 1))
        ylabel = "page of a future miss minus page of this miss (64 KiB pages, signed log2 bins)"
    else:
        npg = pages.max() + 1
        yb = np.minimum((fut * NAB // npg).astype(int), NAB - 1)
        ny = NAB
        tick_vals = (np.arange(ny) * npg / NAB * PAGE / 2**30)
        ylab = lambda v: "%.1f" % v
        ylabel = "page of a future miss: position in the database file (GiB)"
    feats = list(F.columns)
    cols = 4
    nrow = int(np.ceil((len(feats) + 1) / cols))
    fig, axes = plt.subplots(nrow, cols, figsize=(4.1 * cols, 3.3 * nrow + 0.9), squeeze=False)
    norm = LogNorm(vmin=1e-3, vmax=1.0, clip=True)
    ytick_pos = np.arange(0, ny, max(1, ny // 8))
    # panel 0: unconditional
    marg = np.bincount(yb, minlength=ny).astype(float)[:, None]
    panels = [("all misses (no feature)", marg / marg.sum(), [""], None)]
    mi_rec = {}
    for f in feats:
        codes, labels = bin_feature(F[f])
        nx = len(labels)
        H = heat(codes, nx, yb, ny, rows)
        n_rows = np.bincount(codes[codes >= 0], minlength=nx)
        colsum = H.sum(0, keepdims=True)
        P = np.divide(H, colsum, out=np.zeros_like(H), where=colsum > 0)
        P[:, n_rows < 20] = 0  # too few misses to say anything
        sel = codes[rows] >= 0
        mi = mi_bits(codes[rows][sel], yb[sel], nx, ny)
        sh = np.mean([mi_bits(rng.permutation(codes)[rows][sel], yb[sel], nx, ny) for _ in range(3)])
        mi_rec[f] = round(mi - sh, 5)
        panels.append((f, P, labels, mi - sh))
    for ax in axes.ravel():
        ax.axis("off")
    im = None
    for a, (title, P, labels, mi) in zip(axes.ravel(), panels):
        a.axis("on")
        im = a.imshow(np.where(P > 0, P, np.nan), origin="lower", aspect="auto", cmap=SEQ, norm=norm, interpolation="nearest")
        a.set_title(title if mi is None else f"{title}   [{mi:.3f} bit]", loc="left", fontsize=8.2)
        a.set_xticks(range(len(labels)))
        a.set_xticklabels(labels, rotation=60 if len(labels) > 6 else 0, ha="right" if len(labels) > 6 else "center", fontsize=6.8)
        a.set_yticks(ytick_pos)
        a.set_yticklabels([ylab(tick_vals[i]) for i in ytick_pos], fontsize=6.8)
        for sp in a.spines.values():
            sp.set_visible(False)
        a.tick_params(length=2)
    for r in range(nrow):
        axes[r, 0].set_ylabel(ylabel if r == nrow // 2 else "", fontsize=7.5)
    fig.suptitle(f"{'Relative' if mode == 'delta' else 'Absolute'} page of the next {k} guest misses, by signal at the current miss "
                 f"({len(d):,} misses; columns sum to 1; [..] = information about the next page, bits)", x=0.01, ha="left", fontsize=10.5, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 0.93, 0.97))
    cax = fig.add_axes([0.945, 0.15, 0.012, 0.65])
    cb = fig.colorbar(im, cax=cax)
    cb.set_label("P(page bin | feature bin), log", fontsize=8)
    cb.outline.set_visible(False)
    name = f"hm_{mode}_k{k}.png"
    fig.savefig(os.path.join(out, name), dpi=130, bbox_inches="tight")
    plt.close(fig)
    numbers[f"{mode}_k{k}"] = mi_rec
    print("wrote", name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--k", type=int, nargs="+", default=[4, 16, 64])
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    d, enter, samples, meta = load(args.run)
    F = build_features(d, enter, samples)
    F = F.loc[:, [c for c in F.columns if F[c].notna().sum() > 0.5 * len(F) and F[c].nunique() > 1]]
    print(f"{len(d)} misses, {len(F.columns)} features")
    rng = np.random.default_rng(0)
    numbers = {"n_misses": int(len(d)), "features": list(F.columns)}
    for k in args.k:
        for mode in ("delta", "abs"):
            draw(F, d, k, mode, args.out, rng, numbers)
    json.dump(numbers, open(os.path.join(args.out, "heatmap_mi.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
