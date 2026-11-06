#!/usr/bin/env python3
"""Late-prefetch sensitivity for the readahead arms.

nbd_server admits prefetched pages instantly, so the next request can hit on a page prefetched by the request
just before it.  A real remote fetch takes ~300 us, but consecutive guest requests arrive a median 143 us apart
(86% < 300 us).  This re-simulates LRU + readahead on the captured stream and only lets a prefetched page count as a
hit if it was issued at least `k` requests earlier (younger ones are treated as full misses, bytes still fetched).
k=0 must reproduce the nbd_server replay.   ./bin/python tools/capture/localdisk_late.py
"""
import json
import os
from collections import OrderedDict

import pandas as pd

from localdisk_analyze import LD, ROOT, PAGE, NQ, HIT_NS, MISS_NS, BW, COMPUTE_S, OUT


def stream(rep):
    meta = json.load(open(os.path.join(ROOT, "disk/data/faiss_data.img.meta.json")))
    f = pd.read_csv(os.path.join(LD, f"ld_base_r{rep}/features.csv"))
    f = f[(f.type == 0) & (f.offset < meta["tail_offset"])].sort_values("arrive_ns")
    return list(zip(f.offset.tolist(), f.length.tolist()))


def sim(reqs, cap_pages, w, k):
    lru = OrderedDict()                     # page -> request index when admitted by prefetch (or -inf for demand)
    full = demand = pf = 0
    stall = 0.0
    for i, (off, ln) in enumerate(reqs):
        p0, p1 = off // PAGE, (off + ln - 1) // PAGE
        hit_all, miss_pages = True, 0
        for p in range(p0, p1 + 1):
            t = lru.get(p)
            if t is not None and i - t >= k:
                lru.move_to_end(p)
            else:
                hit_all = False; miss_pages += (t is None)
                if t is None:
                    lru[p] = -10**9
                    if len(lru) > cap_pages: lru.popitem(last=False)
                else:
                    lru[p] = -10**9; lru.move_to_end(p)
        if hit_all:
            full += 1; stall += HIT_NS
        else:
            stall += MISS_NS + miss_pages * PAGE / BW * 1e9; demand += miss_pages * PAGE
        for p in range(p1 + 1, p1 + 1 + w):          # readahead after the request
            if p not in lru:
                lru[p] = i; pf += PAGE
                if len(lru) > cap_pages: lru.popitem(last=False)
    stall = stall / 1e9
    stall = max(stall, (demand + pf) / BW - COMPUTE_S)
    return dict(full_hit=full / len(reqs), stall_us=stall / NQ * 1e6, total_gib=(demand + pf) / 2**30)


if __name__ == "__main__":
    rows = []
    for rep in range(3):
        reqs = stream(rep)
        for L in (256, 1024):
            for w in (2, 8, 32, 128):
                for k in (0, 2, 4):
                    r = sim(reqs, L * (1 << 20) // PAGE, w, k)
                    rows.append(dict(rep=rep, L=L, w=w, k=k, **r))
    d = pd.DataFrame(rows).groupby(["L", "w", "k"]).mean().drop(columns="rep").reset_index()
    print(d.round(3).to_string())
    d.to_csv(os.path.join(OUT, "localdisk_late_prefetch.csv"), index=False)
