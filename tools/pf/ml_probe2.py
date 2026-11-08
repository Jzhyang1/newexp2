#!/usr/bin/env python3
"""Feature-headroom probe (fast version).  Label: candidate page (64 KiB) within +-F pages of a request is requested
by one of the next W requests.  Compares a context table (what KernelPrefetchPolicy learns) against gradient boosting
on progressively richer features, by recall at fixed page precision.  Anchors are sampled so it runs in minutes."""
import sys, numpy as np, pandas as pd
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import precision_recall_curve, average_precision_score
R = "/home/jonathan-yang/newexp2/"
W, F, PG = 32, 32, 65536

def load(kind):
    if kind == "f":
        f = pd.read_csv(R + "logs/capture/notrace/features.csv"); f = f[(f.type == 0) & (f.offset < 1700790272)].sort_values("arrive_ns")
        return f.offset.values, f.length.values, f.arrive_ns.values, f.inflight.values
    f = pd.read_csv(R + "logs/capture/pf_th_none/features.csv"); f = f[(f.type == 0)].sort_values("arrive_ns")
    return f.offset.values, f.length.values, f.arrive_ns.values, f.inflight.values

def build(s, l, t, infl, n_anchor, rng):
    n = len(s); a = s // PG; b = (s + l - 1) // PG
    idx = np.sort(rng.choice(np.arange(40, n - W - 1), size=min(n_anchor, n - W - 41), replace=False))
    rows = []
    for i in idx:
        lo, hi = a[i], b[i]
        ring = range(max(0, i - W), i)
        cov = np.zeros(2 * F + (hi - lo + 1) + 2, bool); base = lo - F
        def mark(x, y):
            x = max(x, base); y = min(y, base + len(cov) - 1)
            if x <= y: cov[x - base:y - base + 1] = True
        nclose = 0
        for k in ring:
            if b[k] + F >= lo and a[k] <= hi + F: nclose += 1; mark(a[k], b[k])
        mark(lo, hi)
        chain = 1
        if abs(a[i] - (b[i-1] + 1)) <= 1:
            chain = 2
            if abs(a[i-1] - (b[i-2] + 1)) <= 1:
                chain = 3
                if abs(a[i-2] - (b[i-3] + 1)) <= 1: chain = 4
        fut = np.zeros(len(cov), bool)
        for k in range(i + 1, i + W + 1):
            if b[k] + F >= lo and a[k] <= hi + F:
                x = max(a[k], base); y = min(b[k], base + len(cov) - 1); fut[x - base:y - base + 1] = True
        # hole class
        covidx = np.flatnonzero(cov)
        prv = np.full(len(cov), -1); nxt = np.full(len(cov), -1)
        last = -1
        for j in range(len(cov)):
            if cov[j]: last = j
            prv[j] = last
        last = -1
        for j in range(len(cov) - 1, -1, -1):
            if cov[j]: last = j
            nxt[j] = last
        ln64 = hi - lo + 1
        lenk = l[i] // 4096; endk = ((s[i] + l[i]) % PG) // 4096; stk = (s[i] % PG) // 4096
        plenk = l[i-1] // 4096; p2lenk = l[i-2] // 4096
        pd1 = np.clip(a[i] - b[i-1], -100, 100); pd2 = np.clip(a[i] - b[i-2], -100, 100)
        dtl = np.log1p((t[i] - t[i-1]) / 1e3) if t[i] else 0.0
        dtl2 = np.log1p((t[i-1] - t[i-2]) / 1e3) if t[i] else 0.0
        for side in (0, 1):
            for d in range(1, F + 1):
                pg = hi + d if side == 0 else lo - d
                j = pg - base
                if j < 0 or j >= len(cov): continue
                if cov[j]: hole = -1
                elif prv[j] >= 0 and nxt[j] >= 0: hole = min(nxt[j] - prv[j] - 1, 40)
                else: hole = 99
                rows.append((i, side, d, ln64, chain, nclose, lenk, stk, endk, plenk, p2lenk, pd1, pd2, dtl, dtl2, infl[i], hole, int(fut[j])))
    return pd.DataFrame(rows, columns="i side d ln64 chain nclose lenk stk endk plenk p2lenk pd1 pd2 dtl dtl2 infl hole y".split())

def table_pred(tr, te):
    def keys(df):
        lb = np.minimum(np.ceil(np.log2(np.maximum(df.ln64, 1))).astype(int), 5); cb = np.minimum(df.chain, 4)
        db = np.minimum(np.ceil(np.log2(df.d)).astype(int), 6)
        return ((lb * 5 + cb) * 2 + (df.nclose > 1)) * 14 + df.side * 7 + db
    g = tr.groupby(keys(tr)).y.agg(["sum", "count"]); p = ((g["sum"] + 0.1) / (g["count"] + 1)).to_dict()
    return np.array([p.get(k, tr.y.mean()) for k in keys(te)])

def rec_at(prec, y, sc):
    pr, rc, _ = precision_recall_curve(y, sc); ok = pr[:-1] >= prec
    return rc[:-1][ok].max() if ok.any() else 0.0

if __name__ == "__main__":
    rng = np.random.default_rng(1)
    for kind in sys.argv[1:] or ["f", "t"]:
        s, l, t, infl = load(kind)
        df = build(s, l, t, infl, 6000 if kind == "t" else 9000, rng)
        cut = np.quantile(df.i.unique(), 0.65); tr, te = df[df.i <= cut], df[df.i > cut]
        print(f"[{kind}] rows={len(df)} base rate {df.y.mean():.3f}", flush=True)
        sets = {"table (kernel ctx)": None,
                "gbm: kernel feats": ["side", "d", "ln64", "chain", "nclose"],
                "+hole": ["side", "d", "ln64", "chain", "nclose", "hole"],
                "+byte-exact len/offsets": ["side", "d", "ln64", "chain", "nclose", "hole", "lenk", "stk", "endk"],
                "+prev req shape": ["side", "d", "ln64", "chain", "nclose", "hole", "lenk", "stk", "endk", "plenk", "p2lenk", "pd1", "pd2"],
                "+timing/inflight": ["side", "d", "ln64", "chain", "nclose", "hole", "lenk", "stk", "endk", "plenk", "p2lenk", "pd1", "pd2", "dtl", "dtl2", "infl"]}
        for name, feats in sets.items():
            sc = table_pred(tr, te) if feats is None else HistGradientBoostingClassifier(max_iter=120, max_leaf_nodes=32, learning_rate=0.12).fit(tr[feats], tr.y).predict_proba(te[feats])[:, 1]
            print(f"   {name:26s} AP={average_precision_score(te.y, sc):.3f}  recall@prec .8/.7/.6/.5: " + " ".join(f"{rec_at(p, te.y.values, sc):.3f}" for p in (.8, .7, .6, .5)), flush=True)
