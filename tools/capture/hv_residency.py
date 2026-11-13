#!/usr/bin/env python3
"""hv_frames.csv (hv_sampler.py scan output) -> guest-residency timeline for psim --guest.

  hv_residency.py RUN_DIR [--page 65536] [--max-offset N]  ->  RUN_DIR/hv_guest_res.csv  (t_ns,page,delta)

A 4 KiB data-disk block is "in guest RAM" while at least one guest frame holds its content.  Each output row changes the
number of resident 4 KiB blocks of one host cache page by +-1 at scan time t_ns (so a 64 KiB page counts 0..16).
Also prints how well residency explains the host stream: the share of requested blocks that the guest held at the
last scan before the request (should be small: a resident block is a guest page-cache hit and never reaches the host).
"""
import argparse, os
import numpy as np
import pandas as pd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--page", type=int, default=65536)
    ap.add_argument("--max-offset", type=int, default=1700790272)
    a = ap.parse_args()
    fr = pd.read_csv(os.path.join(a.run, "hv_frames.csv"))
    sc = pd.read_csv(os.path.join(a.run, "hv_scan.csv"))
    mid = (sc.t0_ns + sc.t1_ns) // 2
    if fr.t_ns.isin(mid).all() and not fr.t_ns.isin(sc.t1_ns).all():   # older captures stamped scan midpoints
        fr["t_ns"] = fr.t_ns.map(dict(zip(mid, sc.t1_ns)))
    per = a.page // 4096
    frame_blk = {}
    mult = {}                       # block -> frames holding it
    out_t, out_p, out_d = [], [], []
    for t, f, b in zip(fr.t_ns.values, fr.frame.values, fr.block.values):
        old = frame_blk.get(f, -1)
        if old == b:
            continue
        if old >= 0:
            m = mult[old] - 1
            mult[old] = m
            if m == 0:
                out_t.append(t); out_p.append(old // per); out_d.append(-1)
        if b >= 0:
            m = mult.get(b, 0) + 1
            mult[b] = m
            if m == 1:
                out_t.append(t); out_p.append(b // per); out_d.append(1)
            frame_blk[f] = b
        else:
            frame_blk.pop(f, None)
    ev = pd.DataFrame(dict(t_ns=out_t, page=out_p, delta=out_d))
    ev.to_csv(os.path.join(a.run, "hv_guest_res.csv"), index=False)

    # validation: requested 4 KiB blocks that the guest held at the last scan before the request
    req = pd.read_csv(os.path.join(a.run, "features.csv"))
    req = req[(req.type == 0) & (req.offset < a.max_offset)]
    blk_ev = []                     # rebuild at block level for the check
    frame_blk.clear(); mult.clear()
    res = set()
    scans = np.sort(fr.t_ns.unique())
    by_t = fr.groupby("t_ns")
    held = tot = 0
    ri = 0
    rt, ro, rl = req.arrive_ns.values, req.offset.values, req.length.values
    for ts in scans:
        while ri < len(rt) and rt[ri] < ts:
            for blk in range(ro[ri] // 4096, (ro[ri] + rl[ri] - 1) // 4096 + 1):
                held += blk in res; tot += 1
            ri += 1
        g = by_t.get_group(ts)
        for f, b in zip(g.frame.values, g.block.values):
            old = frame_blk.get(f, -1)
            if old == b: continue
            if old >= 0:
                mult[old] -= 1
                if mult[old] == 0: res.discard(old)
            if b >= 0:
                mult[b] = mult.get(b, 0) + 1; res.add(b); frame_blk[f] = b
            else:
                frame_blk.pop(f, None)
    print(f"{len(ev)} residency events over {len(scans)} scans; peak resident blocks {max(1, len(res))}")
    print(f"requested 4 KiB blocks the guest held at the previous scan: {100 * held / max(1, tot):.1f}% of {tot}")


if __name__ == "__main__":
    main()
