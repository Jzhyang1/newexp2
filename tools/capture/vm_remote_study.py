#!/usr/bin/env python3
"""VM-in-the-loop study: a QEMU guest running the real FAISS search over a remote disk.

For each (scenario, mode, repetition) it boots a fresh guest via run_capture.py with the FAISS data
disk served by nbd_server in `source: remote` mode, and collects the guest's own query timings plus
the server's cache statistics into one CSV row.  The guest keeps its own page cache and readahead
(that is the point: this is what a real guest would feel); --guest-ra-kb varies the latter.

  python3 tools/capture/vm_remote_study.py                      # main matrix -> logs/fwd_vm/results.csv
  python3 tools/capture/vm_remote_study.py --only zone --reps 1 --n-queries 2000
  python3 tools/capture/vm_remote_study.py --ablation           # guest-readahead x host-readahead at cross-zone

Needs: make nbd fwd-sim, /dev/kvm access, the FAISS image (build_faiss_disk.py). No root.
"""
import argparse
import csv
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCEN = {"zone": 400, "az": 1000, "region": 60000}      # link RTT in microseconds (bandwidth: see below)
BW = {"zone": 10000, "az": 10000, "region": 1000}      # Mbit/s
MODES = {
    "local": None,   # reference: the same guest on a local backing file, no link at all
    "direct": ["--fwd-mode", "direct"],
    "direct_c1": ["--fwd-mode", "direct", "--read-concurrency", "1"],   # serial server: what nbd_server did before read_concurrency
    "readahead_c1": ["--fwd-mode", "cache", "--prefetch", "readahead", "--read-concurrency", "1"],   # readahead, but misses are served one at a time
    "cache": ["--fwd-mode", "cache", "--prefetch", "none"],
    "readahead": ["--fwd-mode", "cache", "--prefetch", "readahead"],
    "stream": ["--fwd-mode", "cache", "--prefetch", "readahead_cxt_aware"],
    "quickmine": ["--fwd-mode", "cache", "--prefetch", "quickmine"],
    "learned": ["--fwd-mode", "cache", "--prefetch", "learned"],                                   # policies/policy_factory.h
    "learned_seg": ["--fwd-mode", "cache", "--prefetch", "learned", "--evict", "lru_seg"],
}
COLS = ["scenario", "rtt_us", "mode", "rep", "guest_ra_kb", "readahead_pages", "n_queries", "status", "vm_wall_s", "guest_total_s", "query_sum_s",
        "q_mean_ms", "q_p50_ms", "q_p95_ms", "q_p99_ms", "nbd_reads", "remote_demand_reqs", "remote_demand_bytes",
        "remote_bulk_reqs", "remote_bulk_bytes", "hit_pages", "late_pages", "miss_pages", "pf_issued", "pf_used", "pf_wasted", "result_sha", "run_dir"]


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))]


def one(args, scen, mode, rep, ra_kb, n_queries, depth=None, extra=None, tag=None):
    depth_pages = depth if depth is not None else args.readahead_pages
    name = f"{scen}_{mode}_ra{ra_kb if ra_kb is not None else 'def'}" + (f"_d{depth}" if depth is not None else "") + (f"_{tag}" if tag else "") + f"_r{rep}"

    cmd = [sys.executable, os.path.join(ROOT, "tools/capture/run_capture.py"), "--name", name, "--out-root", args.out_root,
           "--no-trace", "--n-queries", str(n_queries), "--readahead-pages", str(depth_pages),
           "--timeout", str(args.timeout), "--local-read-us", "50"]
    if ra_kb is not None:
        cmd += ["--guest-ra-kb", str(ra_kb)]
    if MODES[mode] is not None:
        cmd += ["--remote-rtt-us", str(SCEN[scen]), "--remote-bw-mbps", str(BW[scen])] + MODES[mode]
    if mode == "local":
        cmd += ["--lean-local"]
    if extra:
        cmd += extra          # later flags win in run_capture's parser: lets a diagnostic override capacity / bandwidth
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    run = os.path.join(args.out_root, name)
    row = dict(tag=tag or "", scenario=scen if MODES[mode] is not None else "-", rtt_us=SCEN[scen] if MODES[mode] is not None else 0, mode=mode, rep=rep,
               guest_ra_kb=ra_kb if ra_kb is not None else "default", readahead_pages=depth_pages if mode in ("readahead", "stream", "readahead_c1") else "", n_queries=n_queries, status="ok" if p.returncode == 0 else "fail",
               vm_wall_s=round(time.time() - t0, 1), run_dir=run)
    try:
        lat = json.load(open(os.path.join(run, "out/query_latency.json")))["lat_ms"]
        row.update(query_sum_s=round(sum(lat) / 1e3, 3), q_mean_ms=round(sum(lat) / len(lat), 3), q_p50_ms=round(pct(lat, .5), 3),
                   q_p95_ms=round(pct(lat, .95), 3), q_p99_ms=round(pct(lat, .99), 3))
        m = re.search(r"WORKLOAD_RESULT: PASS .*total=([\d.]+)s", open(os.path.join(run, "serial.log")).read())
        row["guest_total_s"] = float(m.group(1)) if m else ""
        m = re.search(r"RESULT_SHA256 ([0-9a-f]{64})", open(os.path.join(run, "serial.log")).read())
        row["result_sha"] = m.group(1)[:16] if m else ""
    except Exception as e:  # a failed run still gets a row
        row["status"] = f"fail:{type(e).__name__}"
    log = open(os.path.join(run, "nbd_data.log")).read() if os.path.exists(os.path.join(run, "nbd_data.log")) else ""
    m = re.search(r"STATS reads=(\d+)", log)
    row["nbd_reads"] = int(m.group(1)) if m else ""
    for k, key in (("remote_demand_reqs", "remote_demand_reqs"), ("remote_demand_bytes", "remote_demand_bytes"),
                   ("remote_bulk_reqs", "remote_bulk_reqs"), ("remote_bulk_bytes", "remote_bulk_bytes"), ("hit_pages", "hit_pages"),
                   ("late_pages", "late_pages"), ("miss_pages", "miss_pages"), ("pf_issued", "pf_issued"), ("pf_used", "pf_used"),
                   ("pf_wasted", "pf_wasted")):
        m = re.search(rf"\b{key}=(\d+)", log)
        row[k] = int(m.group(1)) if m else ""
    for k in ("lock_wait_ms", "bulk_hold_ms"):
        m = re.search(rf"\b{k}=(\d+)", log)
        row[k] = int(m.group(1)) if m else ""
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-root", default=os.path.join(ROOT, "logs/fwd_vm/runs"))
    ap.add_argument("--csv", default=os.path.join(ROOT, "logs/fwd_vm/results.csv"))
    ap.add_argument("--scenarios", default="zone,az,region")
    ap.add_argument("--modes", default="local,direct,direct_c1,cache,readahead,stream,quickmine")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--region-reps", type=int, default=2)
    ap.add_argument("--n-queries", type=int, default=5000)
    ap.add_argument("--region-queries", type=int, default=2500, help="cross-region runs are ~60x slower per read; fewer queries")
    ap.add_argument("--readahead-pages", type=int, default=16, help="64 KiB pages: 16 = 1 MiB")
    ap.add_argument("--timeout", type=int, default=1500)
    ap.add_argument("--ablation", action="store_true", help="guest readahead (0/128/1024 KiB) x {direct, readahead} at cross-zone")
    ap.add_argument("--ra-kb", default="0,128,1024")
    ap.add_argument("--ablation-modes", default="direct,readahead")
    ap.add_argument("--only", default="", help="run only this scenario")
    ap.add_argument("--include-slow", action="store_true", help="also run direct_c1 at cross-region (about 7 minutes per run)")
    ap.add_argument("--depth-sweep", action="store_true", help="host readahead depth sweep (readahead_pages; 16 is the main matrix)")
    ap.add_argument("--depths", default="1,4,64,256,512")
    ap.add_argument("--sweep-mode", default="readahead", help="policy to sweep: readahead or stream")
    ap.add_argument("--diag", action="store_true", help="why deep readahead loses: bigger cache / unlimited link at one deep depth")
    ap.add_argument("--diag-depth", type=int, default=256)
    ap.add_argument("--diag-tags", default="", help="comma list of diagnostic configs to run (default all)")
    ap.add_argument("--local-refs", action="store_true", help="local-disk reference runs matching every query count / guest readahead used above")
    args = ap.parse_args()
    os.makedirs(os.path.dirname(args.csv), exist_ok=True)

    cells = []
    diag = args.diag
    if diag:
        for scen in args.scenarios.split(","):
            nq = args.region_queries if scen == "region" else args.n_queries
            reps = args.region_reps if scen == "region" else args.reps
            for tag, extra, d in (("base", [], args.diag_depth), ("bigcache", ["--capacity", "16384"], args.diag_depth),
                                  ("nobw", ["--remote-bw-mbps", "0"], args.diag_depth),
                                  ("bigcache_nobw", ["--capacity", "16384", "--remote-bw-mbps", "0"], args.diag_depth),
                                  ("d16_bigcache", ["--capacity", "16384"], 16), ("d16", [], 16)):
                if args.diag_tags and tag not in args.diag_tags.split(","):
                    continue
                for rep in range(reps):
                    cells.append((scen, "readahead", rep, None, nq, d, extra, tag))
    elif args.depth_sweep:
        for scen in args.scenarios.split(","):
            for d in map(int, args.depths.split(",")):
                for rep in range(args.region_reps if scen == "region" else args.reps):
                    cells.append((scen, args.sweep_mode, rep, None, args.region_queries if scen == "region" else args.n_queries, d, None, None))
    elif args.local_refs:  # run names embed the query count so they never collide with the main matrix
        for ra, nq in ((None, args.region_queries), (0, args.n_queries), (1024, args.n_queries)):
            for rep in range(args.reps):
                cells.append((f"q{nq}", "local", rep, ra, nq, None, None, None))
    elif args.ablation:
        for ra in map(int, args.ra_kb.split(",")):
            for mode in args.ablation_modes.split(","):
                for rep in range(args.reps):
                    cells.append(("az", mode, rep, ra, args.n_queries, None, None, None))
    else:
        for scen in args.scenarios.split(","):
            if args.only and scen != args.only:
                continue
            for rep in range(args.region_reps if scen == "region" else args.reps):
                for mode in args.modes.split(","):
                    if mode == "local" and scen != "zone":
                        continue          # the local reference has no link; run it once per rep alongside the first scenario
                    if mode in ("direct_c1", "readahead_c1") and scen == "region" and not args.include_slow and mode == "direct_c1":
                        continue          # serial reads at 60 ms RTT take ~7 minutes per run
                    cells.append((scen, mode, rep, None, args.region_queries if scen == "region" else args.n_queries, None, None, None))
    if diag and args.csv.endswith("results.csv"):
        args.csv = os.path.join(os.path.dirname(args.csv), "depth_diag.csv")
    cols = COLS + (["tag", "lock_wait_ms", "bulk_hold_ms"] if diag else [])
    fresh = not os.path.exists(args.csv)
    with open(args.csv, "a", newline="") as fh:
        w = csv.DictWriter(fh, cols)
        if fresh:
            w.writeheader()
        for scen, mode, rep, ra, nq, depth, extra, tag in cells:
            row = one(args, scen, mode, rep, ra, nq, depth, extra, tag)
            w.writerow({c: row.get(c, "") for c in cols})
            fh.flush()
            print(f"[{scen:6s} {mode:10s} ra={ra if ra is not None else 'def':>4} depth={row['readahead_pages'] if row['readahead_pages'] != '' else '-':>3} rep{rep}] {row['status']:5s} "
                  f"guest_total={row.get('guest_total_s', '')}s query_mean={row.get('q_mean_ms', '')}ms "
                  f"remote={row.get('remote_demand_bytes', 0) or 0:>11} demand B, {row.get('remote_bulk_bytes', 0) or 0:>11} bulk B", flush=True)


if __name__ == "__main__":
    main()
