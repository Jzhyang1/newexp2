#!/usr/bin/env python3
"""Published prefetchers (policies/policy_lit.cpp) vs `learned` and readahead on the stream suite.

Every policy family is swept over its own aggressiveness knob so families can be compared at matched traffic.
Writes one CSV row per (family, spec, trace):  logs/pf_lit/<name>.csv

  tools/pf/lit_eval.py [--name grid] [--delay 0] [--evict lru_seg] [--families readahead,learned,...] [--jobs 12]
"""
import argparse, csv, os, sys
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sweep

STREAMS = "f_m1g f_m512 f_m2g f_ra0 f_ra32 f_ra512 f_np16 th_m1g th_m4g th_ra32 th_ra512 y_nt".split()

FAMILIES = {
    "readahead": [f"readahead:pages={p}" for p in (1, 2, 4, 8, 16, 32, 64)],
    "learned":   [f"learned:theta={t}" for t in (15, 20, 25, 30, 40, 50, 60, 70)],
    "bop":       [f"bop:degree={d}" for d in (1, 2, 4, 8, 16)],
    "bop_lat":   [f"bop:lat=2,degree={d}" for d in (1, 2, 4, 8, 16)],   # RR fed 2 requests late: for --delay 2 runs
    "spp":       [f"spp:tp={t}" for t in (60, 40, 25, 15, 10, 5)],
    "spp_x":     [f"spp:cross=1,tp={t}" for t in (60, 40, 25, 15, 10, 5)],
    "ppf":       [f"ppf:cross=1,tau={t}" for t in (115, 110, 105, 100, 95, 85, 70)],   # tau - 100 = threshold
    "bingo":     [f"bingo:vote={v}" for v in (100, 80, 60, 40, 20, 10, 5)],
    "bingo_nopc": [f"bingo:pc=0,vote={v}" for v in (100, 60, 20, 5)],
    "pythia":    ["pythia", "pythia:bw=1", "pythia:alpha=650", "pythia:alpha=650,bw=1"],
    "leap":      [f"leap:pwmax={p}" for p in (2, 4, 8, 16, 32, 64, 128)],
    # storage-cache prefetchers already in policies/ (association / temporal mining, per-stream readahead)
    "cminer":    ["cminer"],
    "quickmine": ["quickmine"],
    "mithril":   ["mithril"],
    "ra_cxt":    [f"readahead_cxt_aware:pages={p}" for p in (2, 4, 8, 16, 32, 64)],
    # unions with learned: does a published prefetcher add hits learned misses?
    "learned+bingo": [f"learned:theta={t}+bingo" for t in (25, 40, 60)],
    "learned+ppf":   [f"learned:theta={t}+ppf:cross=1" for t in (25, 40, 60)],
    "learned+spp":   [f"learned:theta={t}+spp:cross=1" for t in (25, 40, 60)],
    "learned+leap":  [f"learned:theta={t}+leap" for t in (25, 40, 60)],
    "learned+bop":   [f"learned:theta={t}+bop" for t in (25, 40, 60)],
    "learned+pythia": [f"learned:theta={t}+pythia" for t in (25, 40, 60)],
    "learned+cminer": [f"learned:theta={t}+cminer" for t in (5, 15, 25, 40, 60, 99)],
    # follow-ups
    "learned_hi":    [f"learned:theta={t}" for t in (80, 90, 95, 99)],
    "learned_lo":    [f"learned:theta={t}" for t in (1, 2, 5, 10)],
    "learned+ra2":   [f"learned:theta={t}+readahead:pages=2" for t in (25, 40, 60, 80, 95)],
    "learned+ra4":   [f"learned:theta={t}+readahead:pages=4" for t in (25, 40, 60, 80, 95)],
    "learned+bingo60": [f"learned:theta={t}+bingo:vote=60" for t in (25, 40, 60, 80)],
    "learned+bingo100": [f"learned:theta={t}+bingo:vote=100" for t in (25, 40, 60, 80)],
    "learnedppf":    [f"learnedppf:theta={t},tau={u}" for t in (10, 15, 25, 40) for u in (90, 100, 105, 110)],
    "learnedppf_shape": [f"learnedppf:addr=0,theta={t},tau={u}" for t in (10, 15, 25, 40) for u in (90, 100, 105, 110)],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="grid")
    ap.add_argument("--delay", default="0")
    ap.add_argument("--evict", default="lru_seg")
    ap.add_argument("--families", default=",".join(FAMILIES))
    ap.add_argument("--streams", default=",".join(STREAMS))
    ap.add_argument("--extra", default="")
    ap.add_argument("--jobs", type=int, default=12)
    a = ap.parse_args()
    extra = ["--delay", a.delay, "--evict", a.evict, *a.extra.split()]
    traces = ["s_" + s for s in a.streams.split(",")]
    fams = a.families.split(",")
    jobs = [(f, s, t) for f in fams for s in FAMILIES[f] for t in traces]
    with ThreadPoolExecutor(a.jobs) as ex:
        base = dict(zip(traces, ex.map(lambda t: sweep.run(t, "none", extra), traces)))
        res = list(ex.map(lambda j: sweep.run(j[2], j[1], extra), jobs))
    out = os.path.join(sweep.ROOT, "logs", "pf_lit", a.name + ".csv")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["family", "spec", "stream", "full_hit", "page_hit", "traffic_rel", "pf_useful_pct", "futile_pct",
                    "none_full_hit", "none_page_hit", "policy_ns_per_req", "delay", "evict"])
        for t, r in base.items():
            w.writerow(["none", "none", t[2:], r["full_hit_pct"], r["page_hit_pct"], 1.0, 0, 0,
                        r["full_hit_pct"], r["page_hit_pct"], 0, a.delay, a.evict])
        for (fam, spec, t), r in zip(jobs, res):
            b = base[t]
            pf = max(1.0, float(r["pf_pages"]))
            w.writerow([fam, spec, t[2:], r["full_hit_pct"], r["page_hit_pct"],
                        float(r["traffic_ratio"]) / float(b["demand_miss_pages"]), r["pf_useful_pct"],
                        100 * float(r["futile_pages"]) / pf, b["full_hit_pct"], b["page_hit_pct"],
                        r.get("policy_ns_per_req", ""), a.delay, a.evict])
    print("wrote", out, len(res), "runs")


if __name__ == "__main__":
    main()
