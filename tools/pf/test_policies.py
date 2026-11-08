#!/usr/bin/env python3
"""Behavioural checks for the learned prefetch policies on synthetic streams (no VM, a few seconds).
Needs bin/psim (tools/pf/build.sh).  Exits non-zero on the first failed expectation.

  python3 tools/pf/test_policies.py
"""
import csv, os, random, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PSIM = os.path.join(ROOT, "bin/psim")
KB = 1024
failed = []


def write(rows, path, worker=False):
    with open(path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["worker", "offset", "length"] if worker else ["offset", "length"])
        w.writerows(rows)


def run(path, policy, evict="lru", extra=()):
    out = subprocess.run([PSIM, "--trace", path, "--policy", policy, "--evict", evict, "--csv", "--label", "t", *extra],
                         capture_output=True, text=True, check=True).stdout.strip().split(",")
    return dict(full_hit=float(out[7]), page_hit=float(out[8]), pf_pages=int(out[9]), traffic=float(out[12]), demand_miss=int(out[13]))


def check(name, cond, detail):
    print(("ok   " if cond else "FAIL ") + name + "  " + detail)
    if not cond:
        failed.append(name)


def interleaved_streams(n_streams, reqs_each, lens, seed=1, bursts=4):
    """n_streams sequential files read in bursts, interleaved; request lengths cycle through `lens`."""
    rng = random.Random(seed)
    pos = [rng.randrange(1 << 20) * 65536 for _ in range(n_streams)]
    i = [0] * n_streams
    rows = []
    left = [reqs_each] * n_streams
    while any(left):
        s = rng.randrange(n_streams)
        if not left[s]:
            continue
        for _ in range(min(bursts, left[s])):
            ln = lens[i[s] % len(lens)]
            rows.append((pos[s], ln))
            pos[s] += ln
            i[s] += 1
            left[s] -= 1
    return rows


def main():
    if not os.path.exists(PSIM):
        sys.exit("build bin/psim first: tools/pf/build.sh")
    d = tempfile.mkdtemp(prefix="pf_test_")

    # 1. interleaved sequential streams with a 1 MiB / 256 KiB length pattern: the next-request predictor should learn it
    p = os.path.join(d, "seq.csv")
    write(interleaved_streams(40, 120, [1024 * KB, 256 * KB]), p)
    base = run(p, "none")
    nr = run(p, "nextreq")
    ra = run(p, "readahead:pages=16")
    check("sequential: nextreq hits most reads", nr["full_hit"] > 75, f"{nr['full_hit']:.1f}%")
    ra = run(p, "readahead:pages=32")
    check("sequential: nextreq spends little beyond the demand traffic", nr["traffic"] < base["traffic"] * 1.25,
          f"{nr['traffic']:.0f} pages vs {base['traffic']:.0f} demand ({nr['traffic'] / base['traffic']:.2f}x); readahead:32 {ra['traffic'] / base['traffic']:.2f}x")
    check("sequential: learned >> none", run(p, "learned")["full_hit"] > base["full_hit"] + 50, f"{run(p, 'learned')['full_hit']:.1f}% vs {base['full_hit']:.1f}%")

    # 2. random 4 KiB reads over a huge device: a learned policy must go quiet, readahead floods the link
    rng = random.Random(7)
    p = os.path.join(d, "rand.csv")
    write([(rng.randrange(1 << 36) * 4096, 4096) for _ in range(60000)], p)
    base = run(p, "none")
    for pol in ("learned", "nextreq", "neighborhood"):
        r = run(p, pol)
        check(f"random: {pol} adds little traffic", r["traffic"] < base["traffic"] * 1.15, f"traffic {r['traffic']:.0f} vs none {base['traffic']:.0f}")
    ra = run(p, "readahead:pages=16")
    check("random: (sanity) readahead:16 does flood", ra["traffic"] > base["traffic"] * 5, f"{ra['traffic'] / base['traffic']:.1f}x")

    # 3. out-of-order read-around windows (a scan whose 128 KiB windows land centred on a point, then fill in around it)
    rng = random.Random(3)
    rows = []
    for _ in range(3000):
        start = rng.randrange(1 << 28) * 65536
        n = rng.randrange(4, 12)                       # extent of n windows
        order = [rng.randrange(n)] + []
        order = list(range(n)); rng.shuffle(order)
        order = [o for o in order]
        rows += [(start + o * 128 * KB, 128 * KB) for o in order]
    p = os.path.join(d, "ooo.csv")
    write(rows, p)
    base, nb = run(p, "none"), run(p, "neighborhood")
    check("out-of-order extents: neighborhood fills the gaps", nb["full_hit"] > base["full_hit"] + 30, f"{nb['full_hit']:.1f}% vs {base['full_hit']:.1f}%")

    # 4. multiple workers: contexts are independent (a worker's stream is not polluted by another's)
    rows = []
    a = interleaved_streams(8, 60, [256 * KB], seed=11)
    b = interleaved_streams(8, 60, [256 * KB], seed=12)
    for x, y in zip(a, b):
        rows.append((0, *x)); rows.append((1, *y))
    p = os.path.join(d, "two.csv")
    write(rows, p, worker=True)
    r = run(p, "learned")
    check("two workers: runs and prefetches", r["full_hit"] > 50, f"{r['full_hit']:.1f}%")

    # 5. segmented LRU never loses to LRU on a prefetching workload by much and keeps capacity
    p = os.path.join(d, "seq.csv")
    r1, r2 = run(p, "learned", "lru"), run(p, "learned", "lru_seg:unused=75")
    check("lru_seg is competitive with lru", r2["full_hit"] > r1["full_hit"] - 2, f"{r2['full_hit']:.1f}% vs {r1['full_hit']:.1f}%")

    # 6. huge offsets and tiny/odd requests do not crash
    rows = [(8 * (1 << 40) - 4096 * (i + 1), 512 * (1 + i % 7)) for i in range(2000)] + [(0, 512)] * 50
    p = os.path.join(d, "edge.csv")
    write(rows, p)
    for pol in ("learned", "nextreq", "neighborhood"):
        run(p, pol)
    check("edge offsets", True, "no crash")

    sys.exit(1 if failed else 0)


main()
