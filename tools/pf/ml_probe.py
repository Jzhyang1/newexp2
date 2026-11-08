#!/usr/bin/env python3
"""How much does richer context buy over the table-kernel? Offline probe: predict, for each request (anchor) and
candidate page near it, whether the page is requested in the next W requests; compare a gradient-boosted model on
rich features against the (len, chain, near, side, distance-bin) count table the C++ KernelPrefetchPolicy uses.
Reports recall at fixed precision (page level), which is the frontier that matters (hit vs wasted traffic)."""
import sys, numpy as np, pandas as pd
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import precision_recall_curve
R = "/home/jonathan-yang/newexp2/"
W, F = 32, 64

def load(kind, n=None):
    if kind == "f":
        f = pd.read_csv(R + "logs/capture/base/features.csv"); f = f[(f.type == 0) & (f.offset < 1700790272)].sort_values("arrive_ns")
        return f.offset.values // 65536, (f.offset.values + f.length.values - 1) // 65536, f.arrive_ns.values
    d = pd.read_csv(R + "logs/oracle_thesios.csv", nrows=n or 300000)
    return d.offset.values // 65536, (d.offset.values + d.length.values - 1) // 65536, np.zeros(len(d), dtype=np.int64)

def build(a, b, t):
    n = len(a); rows = []
    seen = {}
    for i in range(n):
        lo, hi = a[i], b[i]
        # context
        chain = 1; near = 0; nclose = 0
        prev_delta = prev_len = prev2 = 0
        cover_recent = {}
        for k in range(max(0, i - W), i):
            if b[k] + F >= lo and a[k] <= hi + F:
                nclose += 1
            if a[k] <= hi and b[k] >= lo: pass
        if i:
            prev_delta = int(np.clip(a[i] - b[i-1], -200, 200)); prev_len = b[i-1] - a[i-1] + 1
            if abs(a[i] - (b[i-1] + 1)) <= 1: chain = 2
        if i > 1:
            prev2 = int(np.clip(a[i] - b[i-2], -200, 200))
            if chain == 2 and abs(a[i-1] - (b[i-2] + 1)) <= 1: chain = 3
        dt = (t[i] - t[i-1]) / 1e3 if i else 0
        ln = hi - lo + 1
        fut = set()
        for k in range(i + 1, min(n, i + W + 1)):
            if b[k] >= lo - F and a[k] <= hi + F:
                fut.update(range(max(a[k], lo - F), min(b[k], hi + F) + 1))
        for side in (0, 1):
            for d in range(1, F + 1):
                pg = hi + d if side == 0 else lo - d
                if pg < 0: continue
                nseen = seen.get(pg, 0)
                rows.append((i, side, d, ln, min(chain, 10), min(nclose, 20), prev_delta, prev_len, prev2, lo % 4, dt, min(nseen, 3), int(pg in fut)))
        for pg in range(lo, hi + 1): seen[pg] = seen.get(pg, 0) + 1
    return pd.DataFrame(rows, columns="i side d ln chain nclose prev_delta prev_len prev2 mod4 dt nseen y".split())

def table_model(tr, te):
    def keys(df):
        lb = np.minimum(np.ceil(np.log2(np.maximum(df.ln, 1))).astype(int), 5)
        cb = np.minimum(df.chain, 6)
        db = np.minimum(np.ceil(np.log2(df.d)).astype(int), 6)
        return lb * 1000 + cb * 100 + (df.nclose > 1).astype(int) * 10 * 7 + df.side * 7 + db
    kt, ke = keys(tr), keys(te)
    g = tr.groupby(kt).y.agg(["sum", "count"])
    p = ((g["sum"] + 0.1) / (g["count"] + 1)).to_dict()
    return np.array([p.get(k, tr.y.mean()) for k in ke])

def recall_at(prec_target, y, s):
    pr, rc, th = precision_recall_curve(y, s)
    ok = pr[:-1] >= prec_target
    return rc[:-1][ok].max() if ok.any() else 0.0

for kind in sys.argv[1:] or ["f", "t"]:
    a, b, t = load(kind, 120000 if kind == "t" else None)
    df = build(a, b, t)
    cut = df.i.max() * 0.65
    tr, te = df[df.i <= cut], df[df.i > cut]
    print(f"[{kind}] anchors={df.i.max()+1} rows={len(df)} base rate={df.y.mean():.3f}")
    feats = ["side", "d", "ln", "chain", "nclose", "prev_delta", "prev_len", "prev2", "mod4", "nseen"] + (["dt"] if kind == "f" else [])
    m = HistGradientBoostingClassifier(max_iter=250, learning_rate=0.1, max_leaf_nodes=48).fit(tr[feats], tr.y)
    s_gb = m.predict_proba(te[feats])[:, 1]
    s_tb = table_model(tr, te)
    for name, s in (("table-kernel", s_tb), ("gbm rich", s_gb)):
        print(f"   {name:14s} recall @ precision 0.8/0.7/0.6/0.5: " + " ".join(f"{recall_at(p, te.y.values, s):.3f}" for p in (0.8, 0.7, 0.6, 0.5)))
    imp = {}
    for f in feats:
        te2 = te[feats].copy(); te2[f] = np.random.permutation(te2[f].values)
        from sklearn.metrics import average_precision_score
        imp[f] = average_precision_score(te.y, s_gb) - average_precision_score(te.y, m.predict_proba(te2)[:, 1])
    print("   permutation importance (drop in AP):", {k: round(v, 3) for k, v in sorted(imp.items(), key=lambda x: -x[1])})
