#!/usr/bin/env python3
"""Sweep theta for a spec template, report hit% interpolated at fixed traffic ratios.
  tools/pf/frontier.py 'kernel:window=64,theta={t}' [more templates] [--traces faiss,thesios] [--delay 0]
Prints, per template and trace, the hit% at traffic 1.25/1.5/2.0 (x the no-prefetch demand traffic) and the
area under hit(traffic) over [1.0, 2.5] (a single score, higher = better frontier)."""
import argparse, os, sys
import numpy as np
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sweep

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("templates", nargs="+")
    ap.add_argument("--traces", default="faiss,thesios")
    ap.add_argument("--thetas", default="15,20,25,30,35,40,50,60,70")
    ap.add_argument("--delay", default="0")
    ap.add_argument("--extra", default="")
    ap.add_argument("--metric", default="full_hit_pct", help="full_hit_pct | page_hit_pct")
    ap.add_argument("--jobs", type=int, default=10)
    a = ap.parse_args()
    traces = a.traces.split(","); th = a.thetas.split(",")
    extra = ["--delay", a.delay, *a.extra.split()]
    with ThreadPoolExecutor(a.jobs) as ex:
        base = {t: float(sweep.run(t, "none", extra)["demand_miss_pages"]) for t in traces}
        jobs = [(tpl, t, x) for tpl in a.templates for t in traces for x in th]
        res = list(ex.map(lambda j: sweep.run(j[1], j[0].replace("{t}", j[2]), extra), jobs))
    print(f"{'template':50s}" + "".join(f" | {t:>8s} @1.25 @1.5  @2.0  area" for t in traces))
    for tpl in a.templates:
        line = f"{tpl:50s}"
        for t in traces:
            pts = sorted((float(r["traffic_ratio"]) / base[t], float(r[a.metric]))
                         for (tp, tt, x), r in zip(jobs, res) if tp == tpl and tt == t)
            xs = np.array([p[0] for p in pts]); ys = np.array([p[1] for p in pts])
            # make monotone (best hit achievable at <= traffic) then interpolate
            ys = np.maximum.accumulate(ys)
            def at(v): return float(np.interp(v, xs, ys)) if xs.min() <= v <= xs.max() else float("nan")
            g = np.linspace(1.0, 2.5, 31)
            area = float(np.mean(np.interp(g, xs, ys, left=0 if xs.min() > 1.0 else ys[0])))
            line += f" | {at(1.25):13.1f} {at(1.5):5.1f} {at(2.0):5.1f} {area:5.1f}"
        print(line)

main()
