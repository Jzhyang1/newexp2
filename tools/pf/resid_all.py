#!/usr/bin/env python3
"""Per-query guest residency of EVERY inverted list (0..14 fourteenths), reconstructed from the traced filemap add/delete events of the
index file in logs/capture/base: what a guest agent scanning mincore of the whole index at each query start would see. -> logs/pf_signals/resid_all.npy"""
import json, numpy as np, pandas as pd
R = "/home/jonathan-yang/newexp2/"
j = pd.read_pickle(R + "logs/capture/base/joined.pkl"); q = j["queries"]
meta = json.load(open(R + "disk/data/faiss_data.img.meta.json")); L = np.array(meta["list_offset_size_codesize"], dtype=np.int64)
lo = L[:, 0] // 4096; hi = (L[:, 0] + L[:, 1] * (L[:, 2] + 8) - 1) // 4096
ca, cd = j["cache_add"], j["cache_del"]; ca = ca[ca.ino == 13]; cd = cd[cd.ino == 13]
ev = pd.concat([ca.assign(d=1), cd.assign(d=0)]).sort_values("ts"); ets, epg, ed = ev.ts.values, ev.page.values, ev.d.values
res = np.zeros(386721, np.int8); ep = 0
out = np.zeros((len(q), len(L)), np.uint8)
for k in range(len(q)):
    t = q.t0.values[k]
    while ep < len(ets) and ets[ep] <= t:
        res[epg[ep]] = ed[ep]; ep += 1
    c = np.concatenate([[0], np.cumsum(res[:386720], dtype=np.int64)])
    out[k] = np.rint(14 * (c[hi + 1] - c[lo]) / np.maximum(1, hi - lo + 1)).astype(np.uint8)
np.save(R + "logs/pf_signals/resid_all.npy", out)
print(out.shape, "mean resident fraction", out.mean() / 14)
