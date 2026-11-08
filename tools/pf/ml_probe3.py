#!/usr/bin/env python3
"""Turn the offline GBM into a prefetch policy to see whether its precision gain survives contact with the cache:
train on the first 65% of anchors, emit (request idx, page, prob) for the rest, and let psim's `file` policy prefetch
them (psim --policy file:path=...,theta=...).  usage: ml_probe3.py trace.csv out.bin"""
import sys, numpy as np, pandas as pd, struct
sys.path.insert(0, "/home/jonathan-yang/newexp2/tools/pf")
from sklearn.ensemble import HistGradientBoostingClassifier
import ml_probe2 as m
trace, out = sys.argv[1], sys.argv[2]
f = pd.read_csv(trace)
s, l, t, infl = f.offset.values, f.length.values, f.arrive_ns.values, f.inflight.values
n = len(s)
rng = np.random.default_rng(1)
cut = int(n * 0.65)
# train on a sample of anchors from the first part, predict every anchor of the rest
def build_idx(idx):
    # reuse ml_probe2.build with an explicit anchor list by monkey-patching the sampler
    class R:  # fake rng returning the requested anchors
        def choice(self, a, size, replace): return np.array(idx)
    return m.build(s, l, t, infl, len(idx), R())
tr = build_idx(list(range(60, cut, 2)))
feats = ["side", "d", "ln64", "chain", "nclose", "hole", "lenk", "stk", "endk"]
gb = HistGradientBoostingClassifier(max_iter=150, max_leaf_nodes=32, learning_rate=0.12).fit(tr[feats], tr.y)
te = build_idx(list(range(cut, n - m.W - 2)))
p = gb.predict_proba(te[feats])[:, 1]
PG = m.PG
hi = (s + l - 1) // PG; lo = s // PG
with open(out, "wb") as fo:
    keep = p >= 0.15
    sub = te[keep]; pp = p[keep]
    for i, side, d, prob in zip(sub.i.values, sub.side.values, sub.d.values, pp):
        pg = hi[i] + d if side == 0 else lo[i] - d
        fo.write(struct.pack("<QQf", int(i), int(pg), float(prob)))
print("rows", len(te), "kept", keep.sum(), "cut", cut)
