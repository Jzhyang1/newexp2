#!/usr/bin/env python3
"""Join + analyse a traced capture and draw the figures.

  ./bin/python tools/capture/analyze.py --run logs/capture/base --sweep-prefix sw_ --out ./capture_results

Outputs (PNG + numbers.json) under --out.  Needs numpy/pandas/matplotlib (the repo venv has them).
"""
import argparse
import glob
import json
import os
import pickle
import sys
from collections import Counter, defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_lib as C

# ---- style: reference palette (categorical slots in fixed order), light surface --------------------
SURF, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e6e5e1"
BLUE, ORANGE, AQUA, YELLOW, MAGENTA, VIOLET = "#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#6250d6"
plt.rcParams.update({
    "figure.facecolor": SURF, "axes.facecolor": SURF, "savefig.facecolor": SURF, "axes.edgecolor": GRID,
    "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2, "text.color": INK, "axes.titlecolor": INK,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "axes.spines.top": False, "axes.spines.right": False,
    "font.size": 9.5, "axes.titlesize": 10.5, "axes.titleweight": "bold", "axes.titlelocation": "left",
    "legend.frameon": False, "figure.dpi": 130,
})
PAGE = 65536  # analysis page unit (64 KiB) regardless of the server's page size


def save(fig, out, name):
    fig.savefig(os.path.join(out, name), dpi=140, bbox_inches="tight")
    plt.close(fig)
    print("wrote", name)


# ============================================================================================
def load_all(run, force=False):
    cache = os.path.join(run, "joined.pkl")
    if os.path.exists(cache) and not force:
        return pickle.load(open(cache, "rb"))
    r = C.join(run)
    app = C.load_app(run)
    samples = pd.read_csv(os.path.join(run, "out/samples.csv"))
    snap = json.load(open(os.path.join(run, "out/proc_snapshot_mid.json")))
    vma = None
    for line in snap["maps"].splitlines():
        if "index.ivfdata" in line:
            a = line.split()[0].split("-")
            vma = (int(a[0], 16), int(a[1], 16))
    lib = None
    for line in snap["maps"].splitlines():
        if "libfaiss.so" in line and "r-xp" in line:
            a = line.split()[0].split("-")
            lib = (int(a[0], 16), int(a[1], 16), int(line.split()[2], 16))
    out = dict(reads=r["reads"], queries=r["queries"], meta=r["meta"], sync=r["sync"], args=r["args"],
               faults=app["faults"], sched=app["sched"], wake=app["wake"], sys=app["sys"], samples=samples, vma=vma, lib=lib,
               list_disk_start=r["layout"].list_disk_start, list_bytes=r["layout"].list_bytes,
               blk_counts={k: len(v) for k, v in r["blk"].items() if hasattr(v, "__len__")},
               cache_add=r["blk"]["cache_add"], cache_del=r["blk"]["cache_del"], ra=r["blk"]["ra"], fault_around=r["blk"]["fault_around"],
               merge=r["blk"]["merge"], complete=r["blk"]["complete"], issue=r["blk"]["issue"])
    pickle.dump(out, open(cache, "wb"))
    return out


# ============================================================================================
def fig_overview(D, out, N):
    d = D["reads"]
    d = d[(~d.is_probe) & (d.file == "index.ivfdata")].sort_values("arrive_ns").reset_index(drop=True)
    t = (d.arrive_ns - d.arrive_ns.iloc[0]) / 1e9
    fig, ax = plt.subplots(2, 2, figsize=(11.5, 7.6))
    a = ax[0, 0]
    a.scatter(t, d.offset / 2**30, s=1.2, c=BLUE, alpha=0.35, lw=0, rasterized=True)
    a.set_xlabel("time (s)"); a.set_ylabel("disk offset (GiB)")
    a.set_title("NBD read requests over the whole run")
    a = ax[0, 1]
    q = D["queries"]
    lo = len(q) // 2
    sel = d[(d["query"] >= lo) & (d["query"] < lo + 40)]
    for parity, col in ((0, BLUE), (1, ORANGE)):
        s = sel[sel["query"] % 2 == parity]
        a.scatter(s.index, s.list_id, s=9, c=col, lw=0, label=f"even queries" if parity == 0 else "odd queries")
    a.set_xlabel("request index (40 consecutive queries)"); a.set_ylabel("inverted-list id")
    a.set_title("Zoom: each query jumps between a handful of lists")
    a.legend(loc="upper center", bbox_to_anchor=(0.5, -0.18), ncols=2, fontsize=8)
    a = ax[1, 0]
    a.hist(np.log2(d.length / 1024), bins=np.arange(2.5, 11.5, 0.5), color=BLUE, lw=0)
    a.set_xlabel("request length (log2 KiB)"); a.set_ylabel("requests")
    a.set_title("Request sizes: mostly guest readahead windows (128 KiB)")
    N["len_median_kib"] = float(d.length.median() / 1024)
    N["len_frac_128k"] = float((d.length == 131072).mean())
    a = ax[1, 1]
    prev_end = (d.offset + d.length).shift(1)
    delta = ((d.offset - prev_end) / PAGE).dropna()
    N["frac_sequential_exact"] = float((d.offset == prev_end).mean())
    clip = delta.clip(-300, 300)
    a.hist(clip, bins=np.arange(-300, 301, 6), color=BLUE, lw=0)
    a.set_yscale("log"); a.set_xlabel("gap to previous request's end (64 KiB pages, clipped at ±300)"); a.set_ylabel("requests (log)")
    a.set_title(f"Gaps: {100*N['frac_sequential_exact']:.0f}% are exact continuations, the rest are jumps")
    fig.tight_layout()
    save(fig, out, "fig1_stream_overview.png")


def fig_censoring(D, out, N):
    d = D["reads"]
    q = D["queries"].copy()
    lb = D["list_bytes"]
    q["demand"] = [lb[l].sum() for l in q.lists]
    nbd = d[(d["query"] >= 0)].groupby("query").length.sum()
    q["nbd"] = q.i.map(nbd).fillna(0)
    N["demand_total_gib"] = float(q.demand.sum() / 2**30)
    N["nbd_total_gib"] = float(q.nbd.sum() / 2**30)
    N["guest_absorbed_frac"] = float(1 - q.nbd.sum() / q.demand.sum())
    # per-list app probes vs NBD requests
    probes = Counter(l for ls in q.lists for l in ls)
    reqs = Counter(l for l in d[d.list_id >= 0].list_id)
    lists = sorted(probes)
    x = np.array([probes[l] for l in lists]); y = np.array([reqs.get(l, 0) for l in lists])
    fig, ax = plt.subplots(1, 3, figsize=(14, 4.2))
    a = ax[0]
    w = 200
    a.plot(q.i, (q.demand / 2**20).rolling(w).mean(), color=INK2, lw=1.6, label="app demand (lists probed)")
    a.plot(q.i, (q.nbd / 2**20).rolling(w).mean(), color=BLUE, lw=1.6, label="reaches NBD (guest-cache misses)")
    a.set_xlabel("query #"); a.set_ylabel("MiB per query (200-query mean)")
    a.set_title(f"The guest page cache hides {100*N['guest_absorbed_frac']:.0f}% of demand bytes")
    a.legend(fontsize=8)
    a = ax[1]
    a.scatter(x, y + 1, s=6, c=BLUE, alpha=0.5, lw=0)
    mx = max(x.max(), y.max())
    a.plot([1, mx], [2, mx + 1], color=INK2, lw=0.8, ls="--")
    a.set_xscale("log"); a.set_yscale("log")
    a.set_xlabel("probes of the list by the application"); a.set_ylabel("NBD requests touching the list (+1)")
    a.set_title("Per-list: application probes vs. what NBD sees")
    a.text(0.04, 0.9, "dashed: every probe reaches NBD", transform=a.transAxes, fontsize=8, color=INK2)
    N["corr_probe_vs_nbd_log"] = float(np.corrcoef(np.log1p(x), np.log1p(y))[0, 1])
    a = ax[2]
    s = D["samples"]
    ts = (s.mono_ns - s.mono_ns.iloc[0]) / 1e9
    a.plot(ts, s.cached_kb / 1024, color=BLUE, lw=1.6)
    a.set_xlabel("time since tracer start (s)"); a.set_ylabel("guest page cache (MiB)")
    a.set_title("Guest page cache occupancy (bounded by guest RAM)")
    fig.tight_layout()
    save(fig, out, "fig2_censoring.png")


def fig_join(D, out, N):
    d = D["reads"]
    d = d[~d.is_probe]
    ok = d.rq >= 0
    N["join_match_rate"] = float(ok.mean())
    fig, ax = plt.subplots(1, 3, figsize=(14, 3.8))
    a = ax[0]
    a.hist(d.lat_guest_to_host_us.dropna().clip(-100, 4000), bins=60, color=BLUE, lw=0)
    a.set_xlabel("guest block issue -> NBD arrival (µs)"); a.set_ylabel("requests")
    a.set_title("Time-aligned join: guest request to host request")
    N["g2h_us_median"] = float(d.lat_guest_to_host_us.median())
    a = ax[1]
    bins = [0, 8192, 65536, 131073, 262145, 2**30]
    names = ["≤8K", "≤64K", "≤128K", "≤256K", ">256K"]
    cls = pd.cut(d.length, bins=bins, labels=names, right=False)
    rate = ok.groupby(cls, observed=False).mean() * 100
    cnt = ok.groupby(cls, observed=False).size()
    a.bar(range(len(rate)), rate.values, color=BLUE, width=0.6)
    a.set_xticks(range(len(rate))); a.set_xticklabels([f"{n}\n(n={c})" for n, c in zip(names, cnt.values)], fontsize=8)
    a.set_ylabel("% matched to one guest request"); a.set_ylim(0, 105)
    a.set_title("Unmatched = QEMU merged guest requests")
    a = ax[2]
    unm = d[~ok]
    comp = [("exact match", ok.sum()), ("unmatched", (~ok).sum())]
    a.barh(["exact", "no 1:1 guest request"], [ok.sum(), (~ok).sum()], color=[BLUE, ORANGE], height=0.5)
    a.set_xlabel("NBD read requests"); a.set_title("Join coverage")
    fig.tight_layout()
    save(fig, out, "fig3_join_validation.png")


# ============================================================================================
def build_visits(D):
    """Collapse the NBD read stream into *list visits*: maximal runs of requests that fall in the same
    inverted list. The first request of a visit is where a prefetcher must decide what comes next."""
    d = D["reads"]
    d = d[(~d.is_probe) & (d.file == "index.ivfdata") & (d.list_id >= 0)].sort_values("arrive_ns").reset_index(drop=True)
    d["gap_us"] = d.arrive_ns.diff().fillna(1e9) / 1e3
    d["new_visit"] = d.list_id != d.list_id.shift(1)
    return d, d[d.new_visit].reset_index(drop=True)


def evaluate(ev, ctx_chain_fn, ks=(1, 4, 8), warm=0.3, oracle_fn=None):
    """Prequential (predict, then learn) next-visit prediction in list space."""
    lid = ev.list_id.values
    n = len(ev)
    tables = defaultdict(Counter)
    start = int(n * warm)
    hits = {k: 0 for k in ks}
    tot = 0
    kmax = max(ks)
    for t in range(n - 1):
        y = lid[t + 1]
        chain = ctx_chain_fn(t) if ctx_chain_fn else []
        cands = list(oracle_fn(t)) if oracle_fn is not None else []
        for c in chain:
            if len(cands) >= kmax:
                break
            if c in tables:
                for p, _ in tables[c].most_common(kmax):
                    if p not in cands:
                        cands.append(p)
        if t >= start:
            tot += 1
            for k in ks:
                # +-1 list tolerance: a guest readahead window is aligned to 128 KiB, so a request for list L
                # often starts in the tail of list L-1 (36% of visits)
                if any(abs(c - y) <= 1 for c in cands[:k]):
                    hits[k] += 1
        for c in chain:
            tables[c][y] += 1
    return [hits[k] / tot for k in ks]


def fig_signals(D, out, N):
    d, J = build_visits(D)
    N["n_requests"] = int(len(d)); N["n_visits"] = int(len(J)); N["requests_per_visit"] = float(len(d) / len(J))
    lid = J.list_id.values
    gap_b = np.clip(np.log2(J.gap_us.clip(lower=1).values) // 2, 0, 8).astype(int)
    len_b = np.clip(np.log2(J.length.values / 4096), 0, 8).astype(int)
    hitf = (J.hits == J.total).values
    rw = J.rwbs_flags.values
    rak = J.ra_kind.fillna("-").values
    def near_rank(l, ls):
        if ls is None:
            return -1
        best = (9, -1)
        for i_, x in enumerate(ls):
            dd = abs(x - l)
            if dd <= 1 and dd < best[0]:
                best = (dd, i_)
        return best[1]
    rank = np.array([near_rank(l, ls) for l, ls in zip(lid, J.q_lists.values)])
    N["frac_visits_in_probe_set_pm1"] = float((rank >= 0).mean())
    prev1 = np.r_[-10, lid[:-1]]
    chains = {
        "Popularity only (no context)": ("H", lambda t: [()]),
        "Last list (Markov-1)": ("H", lambda t: [(lid[t],), ()]),
        "Last 2 lists (Markov-2)": ("H", lambda t: [(prev1[t], lid[t]), (lid[t],), ()]),
        "M1 + inter-arrival gap": ("H", lambda t: [(lid[t], gap_b[t]), (lid[t],), ()]),
        "M1 + request length": ("H", lambda t: [(lid[t], len_b[t]), (lid[t],), ()]),
        "M1 + cache hit/miss of the request": ("H", lambda t: [(lid[t], hitf[t]), (lid[t],), ()]),
        "M1 + guest rq flags + readahead kind": ("G", lambda t: [(lid[t], rw[t], rak[t]), (lid[t],), ()]),
        "M1 + position in query (app marker)": ("App", lambda t: [(lid[t], rank[t]), (lid[t],), ()]),
        "M2 + position in query (app marker)": ("App", lambda t: [(prev1[t], lid[t], rank[t]), (prev1[t], lid[t]), (lid[t],), ()]),
    }
    qlists = J.q_lists.values
    def orc(t):
        ls, r = qlists[t], rank[t]
        if ls is None or r < 0:
            return []
        return [int(x) for x in ls[r + 1:r + 9]]
    # ---- guest page-cache residency per list, replayed from the mm_filemap add/delete events (signals #30/#32) ----
    meta = D["meta"]
    L = np.array(meta["list_offset_size_codesize"], dtype=np.int64)
    loff, lbytes = L[:, 0], L[:, 1] * (L[:, 2] + 8)
    order = np.argsort(loff)
    sorted_off = loff[order]
    npages = np.maximum(1, (lbytes + 4095) // 4096)
    ca, cd = D["cache_add"], D["cache_del"]
    ino = ca.ino.value_counts().index[0]            # the ivfdata inode dominates the page-cache traffic
    def page_lists(df):
        df = df[df.ino == ino]
        foff = df.page.values.astype(np.int64) * 4096
        idx = np.searchsorted(sorted_off, foff, side="right") - 1
        lid_ = order[np.clip(idx, 0, len(order) - 1)]
        ok_ = (idx >= 0) & (foff < loff[lid_] + lbytes[lid_])
        return df.ts.values[ok_], lid_[ok_]
    ts_a, l_a = page_lists(ca)
    ts_d, l_d = page_lists(cd)
    ev_ts = np.r_[ts_a, ts_d]; ev_l = np.r_[l_a, l_d]; ev_s = np.r_[np.ones(len(ts_a), np.int32), -np.ones(len(ts_d), np.int32)]
    o = np.argsort(ev_ts, kind="stable"); ev_ts, ev_l, ev_s = ev_ts[o], ev_l[o], ev_s[o]
    resident = np.zeros(len(L), dtype=np.int64)
    state = {"p": 0}
    vts = J.g_arrive.values
    def advance(t):
        p = state["p"]
        end = np.searchsorted(ev_ts, vts[t], side="right")
        if end > p:
            np.add.at(resident, ev_l[p:end], ev_s[p:end])
            state["p"] = end
    def cached(l):
        return resident[l] >= npages[l] - 2
    qlist_all = D["queries"].lists.tolist()
    qid = J["query"].values
    def orc_cache(t):
        advance(t)
        ls, r, q = qlists[t], rank[t], qid[t]
        if ls is None or r < 0:
            return []
        cand = list(ls[r + 1:])
        for qq in range(q + 1, min(q + 4, len(qlist_all))):
            cand += qlist_all[qq]
        out_ = []
        for l in cand:
            if not cached(l) and l not in out_:
                out_.append(int(l))
            if len(out_) >= 8:
                break
        return out_
    visits_cnt = Counter()
    popcache = {"t": -1, "top": []}
    def pop_uncached(t):
        advance(t)
        visits_cnt[lid[t]] += 1
        if t - popcache["t"] >= 25:
            popcache.update(t=t, top=[l for l, _ in visits_cnt.most_common(400)])
        out_ = [int(l) for l in popcache["top"] if not cached(l)]
        return out_[:8]
    rows = []
    for name, (tier, fn) in chains.items():
        rows.append((name, tier, *evaluate(J, fn)))
    rows.append(("Current query's probe list known (oracle) + M2", "App", *evaluate(J, lambda t: [(prev1[t], lid[t]), (lid[t],), ()], oracle_fn=orc)))
    state["p"] = 0; resident[:] = 0
    rows.append(("Frequent lists NOT resident in guest page cache", "G", *evaluate(J, lambda t: [()], oracle_fn=pop_uncached)))
    state["p"] = 0; resident[:] = 0
    rows.append(("Upcoming probes (this + 3 queries) NOT resident in guest cache (oracle)", "App+G", *evaluate(J, lambda t: [()], oracle_fn=orc_cache)))
    # ---- how far ahead must the application be known?  (lookahead horizon in queries, with / without cache state) ----
    def make_orc(K, use_cache):
        def f(t):
            advance(t)
            ls, r, q = qlists[t], rank[t], qid[t]
            if ls is None or r < 0:
                return []
            cand = list(ls[r + 1:])
            for qq in range(q + 1, min(q + 1 + K, len(qlist_all))):
                cand += qlist_all[qq]
            out_ = []
            for l in cand:
                if (not use_cache or not cached(l)) and l not in out_:
                    out_.append(int(l))
                    if len(out_) >= 8:
                        break
            return out_
        return f
    look = []
    for K in (0, 1, 2, 4, 8, 16, 32):
        for use_cache in (False, True):
            state["p"] = 0; resident[:] = 0
            a1, a4, a8 = evaluate(J, lambda t: [()], oracle_fn=make_orc(K, use_cache))
            look.append(dict(K=K, cache_state=use_cache, top1=a1, top4=a4, top8=a8))
    L2 = pd.DataFrame(look)
    N["lookahead"] = L2.round(4).to_dict("records")
    figL, aL = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True)
    for a_, key in zip(aL, ("top1", "top8")):
        for uc, c, lab in ((False, INK2, "probe lists only"), (True, MAGENTA, "probe lists + guest page-cache residency")):
            g = L2[L2.cache_state == uc]
            a_.plot(np.arange(len(g)), g[key] * 100, marker="o", color=c, lw=1.8, label=lab)
        a_.set_xticks(np.arange(len(g))); a_.set_xticklabels([str(k) for k in g.K], fontsize=8)
        a_.set_xlabel("queries of lookahead known to the host"); a_.set_title(f"{key.replace('top', 'Top-')} accuracy of the next list visit")
    aL[0].set_ylabel("% of list visits predicted"); aL[0].legend(fontsize=8.5, loc="upper left")
    figL.tight_layout()
    save(figL, out, "fig7_lookahead_value.png")
    res = pd.DataFrame(rows, columns=["context", "tier", "top1", "top4", "top8"])
    N["signals"] = res.round(4).to_dict("records")
    col = {"H": BLUE, "G": ORANGE, "App": AQUA, "App+G": MAGENTA}
    fig, ax = plt.subplots(1, 3, figsize=(16.5, 5.4), sharey=True)
    ys = np.arange(len(res))[::-1]
    for a, key, ttl in ((ax[0], "top1", "Top-1"), (ax[1], "top4", "Top-4"), (ax[2], "top8", "Top-8")):
        a.barh(ys, res[key] * 100, color=[col[t] for t in res.tier], height=0.62)
        for y_, v in zip(ys, res[key]):
            a.text(v * 100 + 0.5, y_, f"{100*v:.1f}%", va="center", fontsize=8.5, color=INK)
        a.set_title(f"{ttl}: next visited list is among the {ttl[4:]} candidate(s)", fontsize=9.5)
        a.set_xlabel("% of list visits predicted")
        a.set_xlim(0, max(res.top8.max() * 100 * 1.15, 10))
        a.grid(axis="y", visible=False)
    ax[0].set_yticks(ys); ax[0].set_yticklabels(res.context, fontsize=8.5)
    from matplotlib.patches import Patch
    ax[2].legend(handles=[Patch(color=col["H"], label="host-observable"), Patch(color=col["G"], label="needs guest tracing"),
                          Patch(color=col["App"], label="application-level marker"), Patch(color=col["App+G"], label="app marker + guest cache state")], loc="upper right", fontsize=8.5)
    fig.suptitle(f"Which list does the NBD stream visit next?  ({len(J):,} visits from {len(d):,} requests; prequential, last 70% scored)",
                 x=0.01, ha="left", fontsize=11, fontweight="bold")
    fig.tight_layout()
    save(fig, out, "fig4_signal_predictability.png")


def fig_guest_signals(D, out, N):
    """Some guest-only signals: readahead windows, fault stream (VA/PC), blocking time."""
    f = D["faults"]
    vma = D["vma"]
    fig, ax = plt.subplots(2, 2, figsize=(11.5, 7.6))
    a = ax[0, 0]
    ra = D["ra"]
    if len(ra):
        g = ra[ra.kind == "async"]
        a.hist(g.ra_size.clip(0, 80), bins=np.arange(0, 82, 2), color=BLUE, lw=0)
    a.set_xlabel("readahead window size (4 KiB pages) at async trigger"); a.set_ylabel("events")
    a.set_title("Guest readahead state (kprobe on page_cache_async_ra)")
    a = ax[0, 1]
    if vma:
        m = f[(f.addr >= vma[0]) & (f.addr < vma[1])]
        N["faults_on_ivfdata"] = int(len(m)); N["faults_total"] = int(len(f))
        off = (m.addr - vma[0]) / 2**30
        tt = (m.ts - m.ts.min()) / 1e9
        a.scatter(tt, off, s=0.5, c=BLUE, alpha=0.25, lw=0, rasterized=True)
        a.set_xlabel("guest time (s)"); a.set_ylabel("file offset of faulting VA (GiB)")
        a.set_title(f"mmap page-fault stream ({len(m):,} faults) – before guest cache filtering")
    a = ax[1, 0]
    if D["lib"]:
        lb = D["lib"]
        m = f[(f.ip >= lb[0]) & (f.ip < lb[1])]
        pc = (m.ip - lb[0] + lb[2])
        top = Counter((pc // 64).tolist()).most_common(12)
        N["fault_pcs_in_libfaiss"] = int(len(m)); N["fault_distinct_pc_64B"] = int(len(set((pc // 64).tolist())))
        a.bar(range(len(top)), [c for _, c in top], color=BLUE, width=0.7)
        a.set_xticks(range(len(top))); a.set_xticklabels([f"+{p*64:#x}" for p, _ in top], rotation=60, fontsize=7.5)
        a.set_ylabel("faults"); a.set_title("Faulting PCs inside libfaiss (top 12 code sites)")
    a = ax[1, 1]
    sw, wk = D["sched"], D["wake"]
    pid = D["sync_pid"] if "sync_pid" in D else None
    # blocking time: D-state switch-out -> next wakeup of that pid
    dd = sw[sw.prev_state.str.startswith("D")]
    wkd = {p: g.ts.values for p, g in wk.groupby("pid")}
    blk = []
    for t, p in zip(dd.ts.values, dd.prev_pid.values):
        w = wkd.get(p)
        if w is None:
            continue
        i = np.searchsorted(w, t)
        if i < len(w):
            blk.append((w[i] - t) / 1e3)
    blk = np.array(blk)
    if len(blk):
        a.hist(np.log10(blk.clip(1, 1e5)), bins=50, color=BLUE, lw=0)
        a.set_xlabel("log10 blocked time in D state (µs)"); a.set_ylabel("events")
        a.set_title(f"Blocking time per I/O wait ({len(blk):,} waits, median {np.median(blk):.0f} µs)")
        N["blocked_median_us"] = float(np.median(blk))
    fig.tight_layout()
    save(fig, out, "fig5_guest_signals.png")


def fig_policies(out, prefix, N, base):
    rows = []
    for d in sorted(glob.glob(os.path.join(os.path.dirname(base), prefix + "*"))):
        name = os.path.basename(d)[len(prefix):]
        try:
            f = pd.read_csv(os.path.join(d, "features.csv"))
            a = json.load(open(os.path.join(d, "args.json")))
            cache = pd.read_csv(os.path.join(d, "cache.csv"))
        except Exception:
            continue
        r = f[(f.type == 0) & (f.offset < a_tail(a))]
        admits = cache[cache.event == "admit"]
        res = pd.read_csv(os.path.join(d, "results.csv")).iloc[-1]
        rows.append(dict(name=name, prefetch=a["prefetch"], page=a["page_size"], cache_mib=a["capacity"] * a["page_size"] / 2**20,
                         req_hit=float((r.hits == r.total).mean()), page_hit=float(r.hits.sum() / r.total.sum()),
                         backend_gib=float(len(admits) * a["page_size"] / 2**30),
                         pf_admits=int(admits.prefetched.sum()),
                         pf_used=int(r.pf_hits.sum()),
                         pf_acc=float(r.pf_hits.sum() / max(1, admits.prefetched.sum())),
                         miss_s=float(res["miss_s"]), n=len(r), policy_s=float(res["policy_s"])))
    if not rows:
        return
    S = pd.DataFrame(rows)
    N["sweep"] = S.round(4).to_dict("records")
    fig, ax = plt.subplots(2, 2, figsize=(12, 8.2))
    pol_col = {"none": INK2, "readahead": BLUE, "quickmine": ORANGE, "cminer": AQUA, "mithril": MAGENTA, "oracle": YELLOW}
    a = ax[0, 0]
    P = S[S.name.str.startswith("pol_")].copy()
    order = [p for p in ("none", "readahead", "quickmine", "cminer", "mithril", "oracle") if p in set(P.prefetch)]
    P = P.set_index("prefetch").loc[order]
    a.bar(range(len(P)), P.req_hit * 100, color=[pol_col[p] for p in P.index], width=0.62)
    for i, v in enumerate(P.req_hit):
        a.text(i, v * 100 + 1, f"{100*v:.1f}%", ha="center", fontsize=8.5)
    a.set_xticks(range(len(P))); a.set_xticklabels(P.index)
    a.set_ylabel("% of NBD reads fully served from cache"); a.set_title("Policy comparison (64 KiB pages, 256 MiB cache)")
    a.set_ylim(0, 100)
    a = ax[0, 1]
    a.scatter(P.backend_gib, P.req_hit * 100, s=70, c=[pol_col[p] for p in P.index], zorder=3)
    groups = {}
    for p, r in P.iterrows():
        groups.setdefault((round(r.backend_gib, 2), round(r.req_hit, 3)), []).append(p)
    for (bx, hy), names in groups.items():
        a.annotate(" = ".join(names), (bx, hy * 100), xytext=(6, 4), textcoords="offset points", fontsize=8.5)
    a.set_xlabel("data admitted to the cache (GiB) = backend traffic"); a.set_ylabel("% reads fully cached")
    a.set_title("Hit rate vs. backend traffic: prefetch buys hits with bytes")
    a = ax[1, 0]
    G = S[S.name.str.startswith("page")].copy()
    for pol, c in (("none", INK2), ("quickmine", ORANGE)):
        g = G[G.prefetch == pol].sort_values("page")
        # include the 64 KiB point from the policy sweep
        base_pt = S[(S.name == f"pol_{pol}")]
        g = pd.concat([g, base_pt]).sort_values("page")
        a.plot(np.log2(g.page / 1024), g.req_hit * 100, marker="o", color=c, lw=1.8, label=pol)
    a.set_xticks([2, 4, 6, 8, 10]); a.set_xticklabels(["4K", "16K", "64K", "256K", "1M"])
    a.set_xlabel("cache page size (cache bytes fixed at 256 MiB)"); a.set_ylabel("% reads fully cached")
    a.set_title("Page size"); a.legend(fontsize=8.5)
    a = ax[1, 1]
    for pol, c in (("none", INK2), ("readahead", BLUE), ("quickmine", ORANGE)):
        g = pd.concat([S[(S.name.str.startswith("cache")) & (S.prefetch == pol)], S[S.name == f"pol_{pol}"]]).sort_values("cache_mib")
        a.plot(g.cache_mib, g.req_hit * 100, marker="o", color=c, lw=1.8, label=pol)
    a.set_xscale("log", base=2)
    a.set_xlabel("cache size (MiB, 64 KiB pages)"); a.set_ylabel("% reads fully cached")
    a.set_title("Cache size"); a.legend(fontsize=8.5)
    fig.tight_layout()
    save(fig, out, "fig6_policy_sweeps.png")
    return S


def a_tail(a):
    return json.load(open(a["meta_file"]))["tail_offset"]


def fig_inventory(out):
    # status of each of the 50 features of offline/SIGNALS.md ('y' collected, 'p' partial/proxy, 'n' not collected)
    items = [
        ("1 offset", "y"), ("2 length", "y"), ("3 cmd type", "y"), ("4 cmd flags", "y"), ("5 arrival ts", "y"), ("6 service latency", "y"),
        ("7 NBD handle", "y"), ("8 worker/VM id", "y"), ("9 queue depth", "p"), ("10 cache hit/miss", "y"), ("11 prefetched flag", "y"), ("12 eviction event", "y"),
        ("13 guest rq sector", "y"), ("14 guest rq length", "y"), ("15 rq flags", "y"), ("16 pid/tid/comm", "y"), ("17 cgroup id", "n"), ("18 merge events", "y"),
        ("19 bio queue time", "y"), ("20 rq completion", "y"), ("21 io priority", "n"), ("22 readahead window", "y"), ("23 readahead trigger", "y"),
        ("24 inode", "y"), ("25 file path", "p"), ("26 file page index", "y"), ("27 file size", "n"), ("28 open flags", "p"), ("29 fadvise/madvise", "y"),
        ("30 page-cache add", "y"), ("31 page-cache hit", "p"), ("32 page-cache evict", "y"), ("33 refault/shadow", "n"), ("34 dirty/writeback", "n"),
        ("35 extent map", "y"), ("36 metadata vs data", "y"), ("37 syscall nr", "y"), ("38 syscall args", "y"), ("39 syscall ret", "y"), ("40 syscall ts", "y"),
        ("41 user PC", "y"), ("42 call stack", "y"), ("43 fault VA/type", "y"), ("44 CPU id", "y"), ("45 sched/blocking", "y"), ("46 mem pressure", "y"),
        ("47 HW counters", "n"), ("48 access-bit scan", "n"), ("49 EPT/VM-exit", "n"), ("50 app marker", "y"),
    ]
    col = {"y": BLUE, "p": YELLOW, "n": "#d9d8d3"}
    fig, ax = plt.subplots(figsize=(12, 4.6))
    for i, (nm, st) in enumerate(items):
        r, c = divmod(i, 10)
        ax.add_patch(plt.Rectangle((c * 1.0, -r), 0.94, 0.86, color=col[st], lw=0))
        ax.text(c + 0.47, -r + 0.43, nm, ha="center", va="center", fontsize=7.6, color="#ffffff" if st == "y" else INK)
    ax.set_xlim(-0.05, 10); ax.set_ylim(-4.15, 1); ax.axis("off")
    n = Counter(s for _, s in items)
    ax.set_title(f"Signal inventory: {n['y']} collected, {n['p']} partial/proxy, {n['n']} not collected (of 50)", loc="left")
    from matplotlib.patches import Patch
    ax.legend(handles=[Patch(color=BLUE, label="collected"), Patch(color=YELLOW, label="partial / proxy"), Patch(color="#d9d8d3", label="not collected")],
              loc="lower center", ncols=3, bbox_to_anchor=(0.5, -0.12))
    save(fig, out, "fig0_signal_inventory.png")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", default="logs/capture/base")
    ap.add_argument("--sweep-prefix", default="sw_")
    ap.add_argument("--out", default="capture_results")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    N = {}
    D = load_all(args.run, args.force)
    N["blk_counts"] = D["blk_counts"]
    fig_inventory(args.out)
    fig_overview(D, args.out, N)
    fig_censoring(D, args.out, N)
    fig_join(D, args.out, N)
    fig_signals(D, args.out, N)
    fig_guest_signals(D, args.out, N)
    fig_policies(args.out, args.sweep_prefix, N, args.run)
    json.dump(N, open(os.path.join(args.out, "numbers.json"), "w"), indent=1, default=float)
    print(json.dumps({k: v for k, v in N.items() if k not in ("signals", "sweep")}, indent=1, default=float))


if __name__ == "__main__":
    main()
