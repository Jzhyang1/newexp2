#!/usr/bin/env python3
"""Run a grid of host-only (no guest tracing) captures in parallel: policy / page size / cache size.

Every run replays the same FAISS workload (same seed => same query stream), so the
differences are the cache + prefetch policy only.  Results: logs/capture/<name>/results.csv.

  python3 tools/capture/sweep.py --source logs/capture/base --parallel 3
"""
import argparse
import itertools
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MIB = 1 << 20


def specs(oracle_from):
    out = []
    # S1: policy comparison at the default geometry (64 KiB pages, 256 MiB cache)
    for pol in ("none", "readahead", "quickmine", "cminer", "mithril") + (("oracle",) if oracle_from else ()):
        out.append((f"pol_{pol}", dict(prefetch=pol, page=65536, cache_mib=256)))
    # S2: page size at constant cache bytes
    for page in (4096, 262144, 1 << 20):
        for pol in ("none", "quickmine"):
            out.append((f"page{page // 1024}k_{pol}", dict(prefetch=pol, page=page, cache_mib=256)))
    # S3: cache size at 64 KiB pages
    for mib in (64, 1024):
        for pol in ("none", "readahead", "quickmine"):
            out.append((f"cache{mib}m_{pol}", dict(prefetch=pol, page=65536, cache_mib=mib)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parallel", type=int, default=3)
    ap.add_argument("--n-queries", type=int, default=20000)
    ap.add_argument("--source", default="logs/capture/base", help="captured run whose request stream is replayed")
    ap.add_argument("--only", default="", help="substring filter on run names")
    ap.add_argument("--prefix", default="sw_")
    args = ap.parse_args()
    runs = [(n, s) for n, s in specs(True) if args.only in n]

    def run(i_ns):
        i, (name, s) = i_ns
        cmd = [sys.executable, os.path.join(ROOT, "tools/capture/replay.py"), "--source", args.source, "--name", args.prefix + name,
               "--prefetch", s["prefetch"], "--page-size", str(s["page"]), "--capacity", str(s["cache_mib"] * MIB // s["page"]),
               "--port", str(12000 + i)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        print((r.stdout.strip().splitlines() or ["?"])[-1], "" if r.returncode == 0 else "FAILED " + r.stderr[-300:], flush=True)

    with ThreadPoolExecutor(args.parallel) as ex:
        list(ex.map(run, enumerate(runs)))


if __name__ == "__main__":
    main()
