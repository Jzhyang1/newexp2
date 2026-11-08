#!/usr/bin/env python3
"""Remote-disk experiment: guest NBD attached directly vs through a local cache.

For every network scenario this starts a `fwd_sim serve` process (the physically
remote disk: real TCP, RTT + bandwidth enforced with real sleeps) and then runs
`fwd_sim bench` for each workload x mode x worker count x think time, appending
one CSV row per run. Finally it prints/writes a markdown comparison.

Modes
  direct       every guest read is forwarded to the remote disk
  cache        local disk cache (LRU), no prefetch  -> shows what reuse alone buys
  readahead    cache + blind sequential readahead
  stream       cache + stream-detecting readahead (readahead_cxt_aware)

"Total latency" is the sum over all guest reads of (time the guest waited for
that read); wall time is the whole run including guest think time.

    python3 disk/fwd_experiment.py                  # full matrix
    python3 disk/fwd_experiment.py --quick          # ~1/3 the requests
    python3 disk/fwd_experiment.py --scenarios wan --workloads seq,trace --think-us 0,200,2000
"""
import argparse
import csv
import os
import re
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (rtt_us, bandwidth_mbps, remote disk service us)
SCENARIOS = {
    "lan": dict(rtt_us=200, bw_mbps=10000, disk_us=80),     # same rack / datacenter
    "wan": dict(rtt_us=2000, bw_mbps=1000, disk_us=100),    # cross-zone / metro
    "far": dict(rtt_us=20000, bw_mbps=200, disk_us=150),    # cross-region / constrained uplink
    # Data-center numbers (RTT). Measured medians: same zone ~0.36-0.5 ms, cross-zone ~0.4-1 ms
    # (up to ~2 ms), US cross-region ~40-80 ms. Remote disk = a networked SSD (~100 us).
    "zone": dict(rtt_us=400, bw_mbps=10000, disk_us=100),     # same availability zone
    "az": dict(rtt_us=1000, bw_mbps=10000, disk_us=100),      # across availability zones, same region
    "region": dict(rtt_us=60000, bw_mbps=1000, disk_us=100),  # across regions (US east<->west)
}
# Scenarios whose runs are so slow that they get fewer requests (direct mode pays RTT per read).
SLOW_SCENARIO_REQUEST_FRACTION = {"region": 0.3, "far": 0.5}

# workload -> bench args (request size follows what that guest pattern looks like)
WORKLOADS = {
    "seq": ["--req-bytes", "131072"],                       # streaming read, 128 KiB I/Os
    "seqjump": ["--req-bytes", "65536", "--run-len", "64"], # file-like: sequential runs, random jumps
    "stride": ["--req-bytes", "4096", "--stride", "8"],     # 4 KiB read every 32 KiB
    "random": ["--req-bytes", "4096"],                      # uniform 4 KiB (database-ish, no locality)
    "zipf": ["--req-bytes", "4096"],                        # skewed hot set: reuse, little sequentiality
    "trace": [],                                            # replay of a real VM's nbd_access.csv
    # Real FAISS IVF-Flat search (kvm/guest/workloads, tools/capture): the captured guest read
    # stream replayed verbatim, with its recorded think times, against the real 1.6 GiB index
    # image. 64 KiB pages / 256 MiB cache / 1 MiB readahead, as the capture harness used.
    "faiss": ["--features", "logs/capture/base/features.csv", "--trace-skip", "3000",
              "--page-size", "65536", "--capacity", "4096", "--readahead-pages", "16",
              "--verify-file", "disk/data/faiss_data.img"],
}
# Workloads that need their own remote disk image instead of the generated pattern one.
WORKLOAD_DISK = {"faiss": "disk/data/faiss_data.img"}

MODES = {
    "direct": ["--mode", "direct"],
    "cache": ["--mode", "cache", "--prefetch", "none"],
    "readahead": ["--mode", "cache", "--prefetch", "readahead"],
    "stream": ["--mode", "cache", "--prefetch", "readahead_cxt_aware"],
    "quickmine": ["--mode", "cache", "--prefetch", "quickmine"],   # mined page associations, not sequentiality
}
DEFAULT_MODES = ["direct", "cache", "readahead", "stream"]


def csv_list(s):
    return [x for x in s.split(",") if x]


class Remote:
    """A running `fwd_sim serve` for one scenario."""

    def __init__(self, binary, disk, cfg, prio):
        self.proc = subprocess.Popen(
            [binary, "serve", "--file", disk, "--port", "0",
             "--rtt-us", str(cfg["rtt_us"]), "--bw-mbps", str(cfg["bw_mbps"]),
             "--disk-us", str(cfg["disk_us"]), "--prio", "1" if prio else "0"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        line = self.proc.stdout.readline()
        m = re.match(r"listening (\d+)", line)
        if not m:
            self.stop()
            sys.exit(f"remote disk failed to start: {line!r}")
        self.port = int(m.group(1))

    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def f(row, k):
    return float(row[k])


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2


def aggregate(rows):
    """Collapse repetitions: one row per (scenario, workload, label, workers, think), numeric columns = median."""
    groups = {}
    for r in rows:
        groups.setdefault((r["scenario"], r["workload"], r["label"], r["workers"], r["think_us"]), []).append(r)
    out = []
    for rs in groups.values():
        agg = dict(rs[0])
        for k in rs[0]:
            try:
                agg[k] = str(median([float(r[k]) for r in rs]))
            except ValueError:
                pass
        agg["n_reps"] = str(len(rs))
        agg["wait_min"] = str(min(float(r["io_wait_s"]) for r in rs))
        agg["wait_max"] = str(max(float(r["io_wait_s"]) for r in rs))
        out.append(agg)
    return out


def summarize(rows, out):
    """rows: dicts from the CSV (medians over repetitions). Markdown: per scenario/workers/think, one table."""
    rows = aggregate(rows)
    keyf = lambda r: (r["scenario"], int(float(r["workers"])), int(float(r["think_us"])))
    groups = {}
    for r in rows:
        groups.setdefault(keyf(r), []).append(r)
    lines = []
    for (scen, workers, think), rs in sorted(groups.items()):
        lines.append(f"### {scen} — {workers} worker(s), think {think} µs\n")
        lines.append("Total latency = sum of every read's wait (s), median of the repetitions; wall = whole run incl. think time. "
                     "Speedup is direct ÷ mode. Remote MB / bytes read = network amplification.\n")
        lines.append("| workload | mode | total latency (s) | min–max | speedup | wall (s) | mean µs | p99 µs | remote MB | amp | "
                     "hit req % | prefetch used % |")
        lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for wl in WORKLOADS:
            wrs = {r["label"]: r for r in rs if r["workload"] == wl}
            if "direct" not in wrs:
                continue
            base = f(wrs["direct"], "io_wait_s")
            for mode in MODES:
                r = wrs.get(mode)
                if not r:
                    continue
                reqs = f(r, "requests") * workers
                remote_mb = (f(r, "remote_demand_bytes") + f(r, "remote_bulk_bytes")) / 1e6
                amp = remote_mb * 1e6 / max(1.0, f(r, "bytes_read"))
                hitp = "-" if mode == "direct" else f"{100 * f(r, 'hit_requests') / reqs:.0f}"
                issued = f(r, "pf_issued_pages")
                usedp = "-" if issued == 0 else f"{100 * f(r, 'pf_used_pages') / issued:.0f}"
                lines.append(f"| {wl} | {mode} | {f(r, 'io_wait_s'):.2f} | {f(r, 'wait_min'):.2f}–{f(r, 'wait_max'):.2f} | "
                             f"{base / max(f(r, 'io_wait_s'), 1e-9):.2f}x | "
                             f"{f(r, 'wall_s'):.2f} | {f(r, 'mean_us'):.0f} | {f(r, 'p99_us'):.0f} | {remote_mb:.0f} | "
                             f"{amp:.1f} | {hitp} | {usedp} |")
        lines.append("")
    text = "\n".join(lines)
    with open(out, "w") as fh:
        fh.write(text)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=os.path.join(ROOT, "bin/fwd_sim"))
    ap.add_argument("--disk", default=os.path.join(ROOT, "disk/data/fwd_remote.img"))
    ap.add_argument("--disk-mb", type=int, default=1024)
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "logs/fwd"))
    ap.add_argument("--scenarios", default="zone,az,region")
    ap.add_argument("--workloads", default=",".join(WORKLOADS))
    ap.add_argument("--modes", default=",".join(DEFAULT_MODES))
    ap.add_argument("--workers", default="1")
    ap.add_argument("--think-us", default="200")
    ap.add_argument("--requests", type=int, default=1000, help="reads per worker")
    ap.add_argument("--reps", type=int, default=3, help="repetitions of every cell (medians are reported)")
    ap.add_argument("--region-reps", type=int, default=2, help="repetitions for the 60 ms scenario (slow)")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--page-size", type=int, default=16384)
    ap.add_argument("--capacity", type=int, default=16384, help="cache pages (default 16384 x 16 KiB = 256 MiB)")
    ap.add_argument("--readahead-pages", type=int, default=32)
    ap.add_argument("--local-read-us", type=int, default=50, help="modeled local cache device latency per request")
    ap.add_argument("--no-prio", action="store_true", help="link does not prioritize demand over prefetch traffic")
    ap.add_argument("--trace", default=os.path.join(ROOT, "logs/nbd_access.csv"))
    ap.add_argument("--keep", action="store_true", help="append to an existing results.csv instead of replacing it")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        sys.exit(f"{args.binary} missing -- run `make fwd-sim`")
    if not os.path.exists(args.disk):
        os.makedirs(os.path.dirname(args.disk), exist_ok=True)
        print(f"creating {args.disk_mb} MiB verifiable remote disk image")
        subprocess.run([args.binary, "mkdisk", "--file", args.disk, "--size-mb", str(args.disk_mb)], check=True)

    os.makedirs(args.out_dir, exist_ok=True)
    results = os.path.join(args.out_dir, "results.csv")
    if os.path.exists(results) and not args.keep:
        os.remove(results)
    base_requests = max(100, args.requests // 3) if args.quick else args.requests

    scenarios, workloads, modes = csv_list(args.scenarios), csv_list(args.workloads), csv_list(args.modes)
    for name, allowed in (("scenario", SCENARIOS), ("workload", WORKLOADS), ("mode", MODES)):
        for x in {"scenario": scenarios, "workload": workloads, "mode": modes}[name]:
            if x not in allowed:
                sys.exit(f"unknown {name} {x!r}; choose from {', '.join(allowed)}")

    failures = 0
    for scen in scenarios:
        requests = max(50, int(base_requests * SLOW_SCENARIO_REQUEST_FRACTION.get(scen, 1.0)))
        remotes = {}  # disk path -> running remote disk for this scenario
        try:
            for workers in map(int, csv_list(args.workers)):
                for think in csv_list(args.think_us):
                    for wl in workloads:
                        disk = WORKLOAD_DISK.get(wl, args.disk)
                        if not os.path.isabs(disk):
                            disk = os.path.join(ROOT, disk)
                        if wl == "faiss" and workers != 1:
                            continue  # one recorded VM
                        if wl == "faiss" and think != csv_list(args.think_us)[0]:
                            continue  # think time comes from the trace, not --think-us
                        if disk not in remotes:
                            if not os.path.exists(disk):
                                print(f"skip {wl}: {disk} not found")
                                remotes[disk] = None
                            else:
                                remotes[disk] = Remote(args.binary, disk, SCENARIOS[scen], not args.no_prio)
                        remote = remotes[disk]
                        if remote is None:
                            continue
                        nreps = args.region_reps if scen == "region" else args.reps
                        for rep, mode in ((r, m) for r in range(nreps) for m in modes):
                            if wl == "trace" and not os.path.exists(args.trace):
                                print(f"skip trace: {args.trace} not found")
                                continue
                            cmd = [args.binary, "bench", "--port", str(remote.port), "--scenario", scen,
                                   "--label", mode, "--workload", wl, "--workers", str(workers),
                                   "--requests", str(requests), "--think-us", think,
                                   "--page-size", str(args.page_size), "--capacity", str(args.capacity),
                                   "--readahead-pages", str(args.readahead_pages),
                                   "--local-read-us", str(args.local_read_us),
                                   "--trace", args.trace, "--csv", results, "--rep", str(rep)] + MODES[mode]
                            # workload flags last so e.g. faiss's page size overrides the default
                            cmd += WORKLOADS[wl]
                            t0 = time.time()
                            p = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
                            tag = f"[{scen} w{workers} think{think} {wl:7s} {mode:9s} r{rep}]"
                            if p.returncode:
                                failures += 1
                                print(tag, "FAILED", p.returncode, (p.stderr or p.stdout).strip()[-300:])
                            else:
                                m = re.search(r"io_wait_s=(\S+) mean_us=(\S+)", p.stdout)
                                print(f"{tag} io_wait {m.group(1)}s mean {m.group(2)}us ({time.time() - t0:.1f}s)", flush=True)
        finally:
            for r in remotes.values():
                if r:
                    r.stop()

    if not os.path.exists(results):
        sys.exit("no results produced")
    with open(results) as fh:
        rows = list(csv.DictReader(fh))
    print()
    print(summarize(rows, os.path.join(args.out_dir, "summary.md")))
    print(f"raw rows: {results}\nsummary:  {os.path.join(args.out_dir, 'summary.md')}")
    bad = sum(int(r["mismatches"]) for r in rows)
    if bad or failures:
        sys.exit(f"{failures} failed run(s), {bad} data mismatch(es)")


if __name__ == "__main__":
    main()
