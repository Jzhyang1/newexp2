#!/usr/bin/env python3
"""Local disk as extended page cache vs. as a prefetching cache.

Equal-budget comparison: for every local-disk size L the guest gets the same total memory + disk.
  ext    : guest RAM = base + L, local cache size 0   (disk-as-extra-page-cache; ideal exclusive upper bound)  [live VMs]
  base   : guest RAM = base                           (the request stream the local cache sees)               [live VMs]
  lru    : base stream through an L-byte LRU cache, no prefetch  (inclusive disk cache; reference)           [replay]
  ra<w>  : ... + readahead of w pages (64 KiB) after every request                                          [replay]
  quickmine: ... + the repo's association-mining prefetcher (a realistic learned predictor)                  [replay]
  oracle : ... + oracle prefetch from the exact stream being served                                          [replay]

Why replay for the cache arms: in simulated-latency mode the local cache cannot change what the guest asks for,
so replaying a captured stream is exactly what a VM would see -- and it lets the oracle know that stream's real
future.  (A live oracle fed with a *previous* boot's trace only reaches ~30% hits: guest I/O streams differ
between boots, see capture_results/localdisk.md.)  The RAM arm does change the stream, so it needs live VMs.

  python3 tools/capture/localdisk_study.py --phase live   --reps 3
  python3 tools/capture/localdisk_study.py --phase replay --reps 3
"""
import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "logs/localdisk")
PAGE = 65536
BASE_MIB = 1024


def sh(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    print((r.stdout.strip().splitlines() or ["?"])[-1], "" if r.returncode == 0 else "FAILED " + r.stderr[-300:], flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", required=True, choices=["live", "replay"])
    ap.add_argument("--parallel", type=int, default=4)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--sizes", default="128,256,512,1024", help="local disk sizes, MiB")
    ap.add_argument("--windows", default="2,8,32,128,512", help="readahead windows, 64 KiB pages")
    ap.add_argument("--n-queries", type=int, default=20000)
    ap.add_argument("--latency-mode", default="simulated")
    args = ap.parse_args()
    sizes = [int(x) for x in args.sizes.split(",")]
    windows = [int(x) for x in args.windows.split(",")]
    jobs = []
    if args.phase == "live":
        for rep in range(args.reps):
            jobs.append((f"ld_base_r{rep}", dict(mem=BASE_MIB, cap=4096, prefetch="none", evict="lru")))
            for L in sizes:
                jobs.append((f"ld_ext_L{L}_r{rep}", dict(mem=BASE_MIB + L, cap=0, prefetch="none", evict="none")))
        def run(i_job):
            i, (name, s) = i_job
            if os.path.exists(os.path.join(OUT, name, "status.json")):
                return
            sh([sys.executable, os.path.join(ROOT, "tools/capture/run_capture.py"), "--name", name, "--out-root", OUT,
                "--mem", f"{s['mem']}M", "--capacity", str(s["cap"]), "--prefetch", s["prefetch"], "--evict", s["evict"],
                "--n-queries", str(args.n_queries), "--no-trace", "--latency-mode", args.latency_mode,
                "--root-port", str(20000 + 2 * i), "--data-port", str(20001 + 2 * i), "--oracle-file", os.path.join(OUT, "none.csv")])
    else:
        for rep in range(args.reps):
            for L in sizes:
                arms = [("lru", "none", 0)] + [(f"ra{w}", "readahead", w) for w in windows] + [("quickmine", "quickmine", 0), ("oracle", "oracle", 0)]
                for arm, pf, w in arms:
                    jobs.append((f"ld_rp_{arm}_L{L}_r{rep}", dict(src=f"ld_base_r{rep}", cap=L * (1 << 20) // PAGE, prefetch=pf, ra=w or 512)))
        def run(i_job):
            i, (name, s) = i_job
            if os.path.exists(os.path.join(OUT, name, "results.csv")):
                return
            sh([sys.executable, os.path.join(ROOT, "tools/capture/replay.py"), "--source", os.path.join(OUT, s["src"]), "--name", name,
                "--out-root", OUT, "--prefetch", s["prefetch"], "--capacity", str(s["cap"]), "--readahead-pages", str(s["ra"]),
                "--port", str(21000 + i % 2000)])
    with ThreadPoolExecutor(args.parallel) as ex:
        list(ex.map(run, enumerate(jobs)))


if __name__ == "__main__":
    main()
