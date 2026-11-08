#!/usr/bin/env python3
"""FAISS without an app hook: predict a query's host-visible lists from the first list it touches + the IVF centroid geometry (index.faiss).
Recall@K (of the host-visible lists beyond the first, among not-resident candidates) for different estimators. Needs logs/pf_signals/cache.pkl
(written by signal_oracle.py) and the scratch centroids (cent.npy, from geo.py: faiss reads index.faiss extracted from the image)."""
import pickle, sys, numpy as np
from collections import defaultdict
R = "/home/jonathan-yang/newexp2/"; SC = "/tmp/claude-1000/-home-jonathan-yang-newexp2/0e888e7a-7f13-410b-94ae-3a8bcb0cf58f/scratchpad/"
C = pickle.load(open(R + "logs/pf_signals/cache.pkl", "rb")); cent = np.load(SC + "cent.npy"); c2 = (cent ** 2).sum(1)
rf, need, L = C["resid_frac"], C["need"], C["lists_of"]; nq = len(L)
G = c2[:, None] + c2[None, :] - 2 * cent @ cent.T
def ev(est, K=16, start=2000):
    tot = hit = nc = 0
    for k in range(start, nq):
        cand = est(k, K)
        nd = set(need.get(k, ())) - {L[k][0]}
        tot += len(nd); hit += len(nd & set(cand)); nc += len(cand)
    return hit / max(1, tot), nc / (nq - start)
def ranked(x, k, first, K, thr=0.5):
    d = c2 - 2 * cent @ x
    return [int(l) for l in np.argsort(d)[:K * 8] if l not in set(L[k][:1]) and rf[k, l] < thr][:K]
co = defaultdict(lambda: defaultdict(float))
for k in range(nq):
    pass
for K in (8, 16, 24):
    print("K=%2d  c1 only: recall %.3f (cands %.1f)" % ((K,) + ev(lambda k, K: ranked(cent[L[k][0]], k, 0, K), K)))
for beta in (0.1, 0.2, 0.3):
    print("K=16  c1+%.1f(c2-c1) [needs 2 observed]: recall %.3f (cands %.1f)" % ((beta,) + ev(lambda k, K: ranked(cent[L[k][0]] + beta * (cent[L[k][1]] - cent[L[k][0]]), k, 0, K), 16)))
# history prior: pair counts of (first list, host-visible list) from earlier queries, combined with geometry rank
pair = defaultdict(lambda: defaultdict(float)); built = [0]
def with_hist(lam, K):
    def f(k, K_):
        while built[0] < k:
            kk = built[0]; a = L[kk][0]
            for b in need.get(kk, ()):
                if b != a: pair[a][b] += 1
            built[0] += 1
        a = L[k][0]; d = G[a].copy()
        sc = -np.argsort(np.argsort(d)).astype(float)       # geometry: -rank among the neighbours of a
        for b, c in pair[a].items(): sc[b] += lam * c
        order = np.argsort(-sc)
        return [int(l) for l in order[:K * 8] if l != a and rf[k, l] < 0.5][:K]
    return f
for lam in (0, 5, 20, 100):
    built[0] = 0; pair.clear()
    print("K=16  geometry rank + %3d * pair-history: recall %.3f (cands %.1f)" % ((lam,) + ev(with_hist(lam, 16), 16)))

# --- anchor = first HOST-VISIBLE list of the query (what the host itself sees), not the first list the app touches
print("\nanchor = first host-visible list (probe order):")
def anchor(k):
    for l in L[k]:
        if l in need.get(k, ()): return int(l)
    return None
def ev_h(K, thr=0.5, filt=True):
    tot = hit = nc = n = 0
    for k in range(2000, nq):
        a = anchor(k)
        if a is None: continue
        d = c2 - 2 * cent @ cent[a]
        cand = [int(l) for l in np.argsort(d)[:K * 8] if l != a and (not filt or rf[k, l] < thr)][:K]
        nd = set(need.get(k, ())) - {a}
        tot += len(nd); hit += len(nd & set(cand)); nc += len(cand); n += 1
    return hit / max(1, tot), nc / max(1, n)
for K in (8, 16, 24):
    print("K=%2d  resident lists filtered (mincore): recall %.3f   unfiltered: recall %.3f" % (K, ev_h(K)[0], ev_h(K, filt=False)[0]))
