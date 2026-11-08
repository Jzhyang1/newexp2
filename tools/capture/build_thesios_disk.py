#!/usr/bin/env python3
"""Thesios workload for the capture harness (no root, no guest-image rebuild).

Creates  disk/data/thesios_data.img      sparse 8 TiB zero device (vdb in the guest; Thesios offsets are file-relative,
                                         the guest workload places each file at a stable hash of (seed, filename))
         disk/data/thesios_data.img.meta.json
         disk/data/thesios_trace.csv     the first --shards condensed Thesios shards (filename,offset,length), in
                                         shard order, from the cache under disk/data/thesios_cache
Both are gitignored (disk/data).  Same shard order and file placement as kvm/guest/workloads/thesios_bench.py.
"""
import argparse, hashlib, json, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASE = "https://storage.googleapis.com/thesios-io-traces/cluster1_16TB/20240115"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shards", type=int, default=12)
    ap.add_argument("--first-shard", type=int, default=0)
    ap.add_argument("--size", type=int, default=8 << 40)
    ap.add_argument("--out", default=os.path.join(ROOT, "disk/data/thesios_data.img"))
    ap.add_argument("--trace", default=os.path.join(ROOT, "disk/data/thesios_trace.csv"))
    ap.add_argument("--cache", default=os.path.join(ROOT, "disk/data/thesios_cache"))
    a = ap.parse_args()
    if not os.path.exists(a.out):
        with open(a.out, "wb") as f:
            f.truncate(a.size)
    tail = 16 << 20
    json.dump(dict(workload="thesios", size=a.size, tail_offset=a.size - tail, tail_bytes=tail), open(a.out + ".meta.json", "w"))
    rows = 0
    with open(a.trace, "w") as out:
        out.write("filename,offset,length\n")
        for i in range(a.first_shard, a.first_shard + a.shards):
            key = hashlib.sha256(f"v2:{BASE}/data-{i:05d}-of-00100".encode()).hexdigest()[:16]
            with open(os.path.join(a.cache, key + ".reads.csv")) as f:
                for line in f:
                    out.write(line); rows += 1
    print(f"{a.out} ({a.size >> 30} GiB sparse), {a.trace}: {rows} reads from shards {a.first_shard}..{a.first_shard + a.shards - 1}")


if __name__ == "__main__":
    main()
