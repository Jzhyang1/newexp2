#!/usr/bin/env python3
"""Build the data disk the YCSB capture workload reads (no root needed).

Produces `--out` (default disk/data/ycsb_data.img): an ext4 image holding one SQLite
database, `ycsb.db`, with YCSB's JDBC schema (table USERTABLE(YCSB_KEY PRIMARY KEY, FIELD0..FIELDn)),
plus `<out>.meta.json` (fs size + raw tail for clock-sync probes, same convention as
build_faiss_disk.py so replay.py / run_capture.py work unchanged).

The keys are exactly what YCSB's CoreWorkload generates ("user" + abs(FNV-64 hash of the
record number), `insertorder=hashed`), so `ycsb run` finds every record it asks for.
Field values are random hex (SQLite's randomblob) of `--field-length` characters: the
content is irrelevant to the I/O pattern, only the row size and the B-tree layout matter.

The guest image already has the JRE, YCSB and the sqlite-jdbc driver, but no dataset (it was
re-baked for other workloads), and a guest cannot persist writes, so the dataset lives here.
"""
import argparse
import json
import os
import shutil
import sqlite3
import subprocess
import time

import numpy as np

FNV_OFFSET = np.uint64(0xCBF29CE484222325)
FNV_PRIME = np.uint64(1099511628211)


def ycsb_hash(vals):
    """YCSB Utils.fnvhash64 (vectorised): 8 octets, little endian, xor then multiply; then Math.abs."""
    v = vals.astype(np.uint64)
    h = np.full(v.shape, FNV_OFFSET, dtype=np.uint64)
    with np.errstate(over="ignore"):
        for _ in range(8):
            h ^= v & np.uint64(0xFF)
            v = v >> np.uint64(8)
            h *= FNV_PRIME
    s = h.view(np.int64)
    return np.where(s < 0, -s, s)  # Math.abs(long) (Long.MIN_VALUE stays negative; astronomically unlikely)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="disk/data/ycsb_data.img")
    ap.add_argument("--records", type=int, default=10_000_000)
    ap.add_argument("--fields", type=int, default=10)
    ap.add_argument("--field-length", type=int, default=100)
    ap.add_argument("--tail-mib", type=int, default=16)
    ap.add_argument("--cache-mib", type=int, default=1024, help="SQLite page cache used while loading")
    args = ap.parse_args()

    stage = os.path.abspath(args.out) + ".stage"
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    dbp = os.path.join(stage, "ycsb.db")
    t0 = time.time()
    db = sqlite3.connect(dbp)
    db.execute("PRAGMA journal_mode=OFF")
    db.execute("PRAGMA synchronous=OFF")
    db.execute(f"PRAGMA cache_size=-{args.cache_mib * 1024}")
    cols = ", ".join(f"FIELD{i} TEXT" for i in range(args.fields))
    db.execute(f"CREATE TABLE USERTABLE (YCSB_KEY VARCHAR(255) PRIMARY KEY, {cols})")
    hexlen = args.field_length // 2
    sel = ", ".join(f"hex(randomblob({hexlen}))" for _ in range(args.fields))
    chunk = 250_000
    for lo in range(0, args.records, chunk):
        hi = min(args.records, lo + chunk)
        keys = ycsb_hash(np.arange(lo, hi, dtype=np.int64))
        db.execute("DROP TABLE IF EXISTS tmpkeys")
        db.execute("CREATE TEMP TABLE tmpkeys (k TEXT)")
        db.executemany("INSERT INTO tmpkeys VALUES (?)", (("user%d" % k,) for k in keys.tolist()))
        db.execute(f"INSERT INTO USERTABLE SELECT k, {sel} FROM tmpkeys")
        db.commit()
        done = hi
        el = time.time() - t0
        print(f"{done:>10,}/{args.records:,}  {el:6.0f}s  eta {el / done * (args.records - done):5.0f}s", flush=True)
    db.close()
    dbsize = os.path.getsize(dbp)
    print(f"db {dbsize / 2**30:.2f} GiB in {time.time() - t0:.0f}s; building ext4 image", flush=True)

    fs_bytes = int(dbsize * 1.03 + 64 * 2**20)
    fs_bytes = (fs_bytes + 2**20 - 1) // 2**20 * 2**20
    if os.path.exists(args.out):
        os.remove(args.out)
    subprocess.check_call(["mke2fs", "-q", "-t", "ext4", "-F", "-L", "ycsbdata", "-b", "4096",
                           "-O", "^has_journal,extent,^resize_inode", "-d", stage, args.out,
                           f"{fs_bytes // 1024}k"])
    with open(args.out, "r+b") as f:
        f.truncate(fs_bytes + args.tail_mib * 2**20)
    meta = dict(workload="ycsb", records=args.records, fields=args.fields, field_length=args.field_length,
                db_bytes=dbsize, fs_bytes=fs_bytes, tail_offset=fs_bytes, tail_bytes=args.tail_mib * 2**20)
    with open(args.out + ".meta.json", "w") as f:
        json.dump(meta, f)
    shutil.rmtree(stage)
    print(f"image {args.out}: fs {fs_bytes / 2**20:.0f} MiB + {args.tail_mib} MiB raw tail ({time.time() - t0:.0f}s total)")


if __name__ == "__main__":
    main()
