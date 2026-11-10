#!/usr/bin/env python3
"""Run bin/psim over (trace x policy-spec) grids in parallel; print full-hit % against traffic (relative to
a no-prefetch run of the same trace) so policies can be compared on one frontier.

  tools/pf/sweep.py --traces faiss,thesios --specs 'adapt:theta=30' 'adapt:theta=50' [--delay 0] [--out f.csv]
"""
import argparse, csv, io, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TRACES = {
    "faiss": ["--trace", f"{ROOT}/logs/capture/base/features.csv", "--max-offset", "1700790272"],
    "faiss_nt": ["--trace", f"{ROOT}/logs/capture/notrace/features.csv", "--max-offset", "1700790272"],
    "thesios": ["--trace", f"{ROOT}/logs/oracle_thesios.csv", "--limit", "1000000"],
    "thesios3": ["--trace", f"{ROOT}/logs/oracle_thesios.csv", "--limit", "3000000"],
    "ycsb": ["--trace", f"{ROOT}/logs/capture/ycsb_p200k/features.csv", "--max-offset", "14554234880"],
    "gups": ["--trace", f"{ROOT}/logs/oracle_gups.csv", "--limit", "1000000"],
    "ycsb_vm": ["--trace", f"{ROOT}/logs/oracle_ycsb.csv"],
    "th_vm": ["--trace", f"{ROOT}/logs/capture/pf_th_none/features.csv", "--max-offset", str((8 << 40) - (16 << 20))],
    "thesios_late": ["--trace", f"{ROOT}/logs/oracle_thesios.csv", "--skip", "4000000", "--limit", "1000000"],
}
import glob
def _limit(run_dir, name):
    """end of the data disk's file system (drops the clock-sync / marker reads in the raw tail)"""
    try:
        import json
        meta = json.load(open(json.load(open(os.path.join(run_dir, "args.json")))["meta_file"]))
        return str(meta["tail_offset"])
    except (OSError, KeyError, ValueError):
        return {"f": "1700790272", "t": str((8 << 40) - (16 << 20)), "y": "14537457664"}[name[0]]
for _d in sorted(glob.glob(f"{ROOT}/logs/pf_streams/*/features.csv")):
    _n = os.path.basename(os.path.dirname(_d))
    TRACES["s_" + _n] = ["--trace", _d, "--max-offset", _limit(os.path.dirname(_d), _n)]
# root-disk streams of the app-launch captures (tools/capture/workloads/app_launch.sh): the guest's own system disk
for _d in sorted(glob.glob(f"{ROOT}/logs/pf_streams/al_*/root_access.csv")):
    TRACES["s_" + os.path.basename(os.path.dirname(_d)) + "_root"] = ["--trace", _d]
HDR = "label,policy,evict,page,cap_pages,delay,reqs,full_hit_pct,page_hit_pct,pf_pages,pf_used,pf_useful_pct,traffic_ratio,demand_miss_pages,late_pages,wall_s,futile_pages".split(",")


def run(trace, spec, extra):
    cmd = [f"{ROOT}/bin/psim", *TRACES.get(trace, ["--trace", trace]), "--policy", spec, "--csv", "--label", trace, *extra]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError(out.stderr)
    row = dict(zip(HDR, next(csv.reader(io.StringIO(out.stdout)))))
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--traces", default="faiss,thesios")
    ap.add_argument("--specs", nargs="+", required=True)
    ap.add_argument("--delay", default="0")
    ap.add_argument("--extra", default="", help="extra psim args, e.g. '--cap 1024'")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out")
    a = ap.parse_args()
    extra = ["--delay", a.delay, *a.extra.split()]
    traces = a.traces.split(",")
    with ThreadPoolExecutor(a.jobs) as ex:
        base = dict(zip(traces, ex.map(lambda t: run(t, "none", extra), traces)))
        jobs = [(t, s) for s in a.specs for t in traces]
        res = list(ex.map(lambda j: run(j[0], j[1], extra), jobs))
    bt = {t: float(r["demand_miss_pages"]) for t, r in base.items()}
    print(f"{'policy':44s}" + "".join(f" | {t:>8s} hit  traf  use% futl%" for t in traces))
    by = {}
    for (t, s), r in zip(jobs, res):
        by.setdefault(s, {})[t] = r
    for s in a.specs:
        line = f"{s:44s}"
        for t in traces:
            r = by[s][t]
            line += f" | {float(r['full_hit_pct']):13.1f} {float(r['traffic_ratio']) / bt[t]:5.2f} {float(r['pf_useful_pct']):5.0f} {100*float(r['futile_pages'])/max(1,float(r['pf_pages'])):5.0f}"
        hs = [float(by[s][t]["full_hit_pct"]) for t in traces]
        ts = [float(by[s][t]["traffic_ratio"]) / bt[t] for t in traces]
        line += f" || mean hit {sum(hs)/len(hs):5.1f}  traffic {sum(ts)/len(ts):5.2f}"
        print(line)
    if a.out:
        with open(a.out, "w", newline="") as f:
            w = csv.writer(f); w.writerow(HDR + ["traffic_rel"])
            for (t, s), r in zip(jobs, res):
                w.writerow([r[h] for h in HDR] + [float(r["traffic_ratio"]) / bt[t]])


if __name__ == "__main__":
    main()
