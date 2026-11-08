#!/usr/bin/env python3
"""Conditional signal sweep (FAISS): predict, for every (query, probed list), whether the list reaches the host (the guest
does not hold it), from groups of candidate signals added one at a time and leave-one-out on top of all of them.
Metric: AUC and recall at 90% precision on the later 35% of queries.  Residency features are reconstructed from the
traced guest page-cache events; the rest from the joined capture (logs/capture/base)."""
import json, numpy as np, pandas as pd
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import roc_auc_score, precision_recall_curve
R = "/home/jonathan-yang/newexp2/"
j = pd.read_pickle(R + "logs/capture/base/joined.pkl")
q = j["queries"]; r = j["reads"]; smp = j["samples"]
meta = json.load(open(R + "disk/data/faiss_data.img.meta.json")); L = np.array(meta["list_offset_size_codesize"], dtype=np.int64)
lo_pg = L[:, 0] // 4096; hi_pg = (L[:, 0] + L[:, 1] * (L[:, 2] + 8) - 1) // 4096
lists_of = [np.array([int(x) for x in s.split(",")]) if isinstance(s, str) else np.array(s) for s in q.lists]
ca, cd = j["cache_add"], j["cache_del"]; ca = ca[ca.ino == 13]; cd = cd[cd.ino == 13]
ev = pd.concat([ca.assign(d=1), cd.assign(d=-1)]).sort_values("ts"); ets, epg, ed = ev.ts.values, ev.page.values, ev.d.values
res = np.zeros(386721, np.int8); ep = 0
r = r[(r.type == 0) & (~r.is_probe)].sort_values("arrive_ns"); PG = 65536
ls, lb = j["list_disk_start"], j["list_bytes"]
req_t = r.g_arrive.values; req_a = r.offset.values // PG; req_b = (r.offset.values + r.length.values - 1) // PG
list_a = ls // PG; list_b = (ls + lb - 1) // PG
rows = []; last_host = np.full(len(L), -1e18); visits = np.zeros(len(L)); last_q = np.full(len(L), -1.0)
sm_t = smp.mono_ns.values
for k in range(len(q)):
    t0, t1 = q.t0.values[k], q.t1.values[k]
    while ep < len(ets) and ets[ep] <= t0:
        if ed[ep] > 0: res[epg[ep]] = 1
        else: res[epg[ep]] = 0
        ep += 1
    c = np.concatenate([[0], np.cumsum(res[:386720])])
    m = (req_t >= t0) & (req_t <= t1)
    ra, rb = req_a[m], req_b[m]
    si = np.searchsorted(sm_t, t0) - 1
    cached_kb = smp.cached_kb.values[max(si, 0)] if len(smp) else 0
    for pos, l in enumerate(lists_of[k]):
        reaches = bool(np.any((ra <= list_b[l]) & (rb >= list_a[l])))
        n = hi_pg[l] - lo_pg[l] + 1
        rows.append(dict(k=k, y=int(reaches),
            resid=(c[hi_pg[l] + 1] - c[lo_pg[l]]) / n, resid_head=(c[min(hi_pg[l] + 1, lo_pg[l] + 16)] - c[lo_pg[l]]) / min(16, n),
            pos=pos, size_pg=n, visits=visits[l], since_q=k - last_q[l] if last_q[l] >= 0 else 1e5,
            since_host=(t0 - last_host[l]) / 1e9 if last_host[l] > -1e17 else 1e3,
            cached_mb=cached_kb / 1024, qdur_prev=(q.t1.values[k - 1] - q.t0.values[k - 1]) / 1e6 if k else 0,
            nlist_dup=len(set(lists_of[k])), ))
        visits[l] += 1; last_q[l] = k
        if reaches: last_host[l] = t0
df = pd.DataFrame(rows)
groups = {"residency": ["resid", "resid_head"], "probe order": ["pos"], "list size": ["size_pg"],
          "popularity (query visits)": ["visits", "since_q"], "host recency": ["since_host"], "guest memory level": ["cached_mb"],
          "previous query duration": ["qdur_prev"]}
cut = df.k.quantile(0.65); tr, te = df[df.k <= cut], df[df.k > cut]
def score(cols):
    m = HistGradientBoostingClassifier(max_iter=150, max_leaf_nodes=24, learning_rate=0.1).fit(tr[cols], tr.y)
    p = m.predict_proba(te[cols])[:, 1]; pr, rc, _ = precision_recall_curve(te.y, p)
    ok = pr[:-1] >= 0.9
    return roc_auc_score(te.y, p), (rc[:-1][ok].max() if ok.any() else 0.0)
allc = sum(groups.values(), [])
print(f"base rate (list reaches host) {df.y.mean():.3f}, rows {len(df)}")
out = ["| signal group | alone: AUC | recall@p90 | added to all others: dAUC (leave-one-out) |", "|---|---|---|---|"]
full = score(allc)
for g, cols in groups.items():
    a, rc = score(cols); lo = score([c for c in allc if c not in cols])
    out.append(f"| {g} | {a:.3f} | {rc:.2f} | {full[0] - lo[0]:+.3f} |")
out.append(f"| **all** | {full[0]:.3f} | {full[1]:.2f} | |")
print("\n".join(out))
