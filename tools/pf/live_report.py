#!/usr/bin/env python3
"""Summarize logs/pf_live/*: hit ratio (nbd_server's own stat), reads fully cached, prefetch traffic and use, policy time."""
import glob, os, sys
import pandas as pd
R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
rows = []
for d in sorted(glob.glob(f"{R}/logs/pf_live/*/")):
    try:
        r = pd.read_csv(d + "results.csv").iloc[-1]
        a = pd.read_csv(d + "access.csv")
        c = None
    except Exception:
        continue
    pf = pd.read_csv(d + "prefetch.csv")          # one row per prefetched range: hits = pages already cached, total = pages
    pf_adm = int((pf.total - pf.hits).sum())
    adm_n = int(r.blk_misses) + pf_adm            # demand-miss pages + prefetched pages = pages fetched from the backend
    rows.append(dict(run=os.path.basename(d.rstrip("/")), evict=r.evict_policy, policy=r.prefetch_policy, reads=len(a),
                     hit_ratio=100 * r.hit_ratio, full_hit=100 * (a.hits == a.total).mean(),
                     admitted_pages=adm_n, pf_pages=pf_adm,
                     policy_us_per_read=1e6 * r.policy_s / max(1, r.requests)))
df = pd.DataFrame(rows)
for w in ("faiss", "thesios"):
    x = df[df.run.str.startswith(w)].copy()
    if x.empty: continue
    base = x[x.run == f"{w}_none"].admitted_pages
    x["traffic_x"] = x.admitted_pages / float(base.iloc[0]) if len(base) else float("nan")
    print(f"\n== {w}  (hit_ratio = nbd_server page hit ratio; traffic_x = pages admitted / policy none)")
    print(x[["run", "evict", "policy", "reads", "hit_ratio", "full_hit", "traffic_x", "policy_us_per_read"]].round(2).to_string(index=False))
