#!/usr/bin/env python3
"""Replay a captured NBD read stream into nbd_server (no VM): capture once, evaluate many policies.

Takes the data-disk read requests recorded in <source>/features.csv (in arrival order) and issues the
identical (offset, length) sequence over the NBD protocol to a fresh nbd_server running the policy /
page size / cache size under test.  Because the cache simulation never changes what the guest asks
for, this is exactly what a VM would have produced, but identical across policies and ~10x cheaper.

Outputs the same files as a VM capture (features.csv, cache.csv, access.csv, results.csv).
"""
import argparse
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time

import pandas as pd

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def handshake(sock):
    hello = b""
    while len(hello) < 18:
        hello += sock.recv(18 - len(hello))
    sock.sendall(struct.pack(">I", 3))                                   # FIXED_NEWSTYLE | NO_ZEROES
    sock.sendall(struct.pack(">QII", 0x49484156454F5054, 1, 0))          # IHAVEOPT, EXPORT_NAME, empty name
    info = b""
    while len(info) < 10:
        info += sock.recv(10 - len(info))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="run dir whose features.csv holds the request stream")
    ap.add_argument("--name", required=True)
    ap.add_argument("--out-root", default=os.path.join(ROOT, "logs/capture"))
    ap.add_argument("--data-img", default=os.path.join(ROOT, "disk/data/faiss_data.img"))
    ap.add_argument("--port", type=int, default=12000)
    ap.add_argument("--prefetch", default="none")
    ap.add_argument("--evict", default="lru")
    ap.add_argument("--page-size", type=int, default=65536)
    ap.add_argument("--capacity", type=int, default=4096)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    meta = json.load(open(args.data_img + ".meta.json"))
    f = pd.read_csv(os.path.join(args.source, "features.csv"))
    f = f[(f.type == 0) & (f.offset < meta["tail_offset"])].sort_values("arrive_ns")
    reqs = list(zip(f.offset.tolist(), f.length.tolist()))
    if args.limit:
        reqs = reqs[: args.limit]

    run = os.path.join(args.out_root, args.name)
    os.makedirs(run, exist_ok=True)
    oracle = os.path.join(run, "oracle_in.csv")
    with open(oracle, "w") as o:
        o.write("worker_id,offset,length,hits,total\n")
        for off, ln in reqs:
            o.write(f"0,{off},{ln},0,0\n")
    cfg = os.path.join(run, "nbd_data.yaml")
    with open(cfg, "w") as c:
        for k, v in dict(source="local", file=args.data_img, ports=f"[{args.port}]", capacity=args.capacity,
                         page_size=args.page_size, hit_latency_ns=30000, miss_latency_ns=300000, latency_mode="simulated",
                         evict_policy=args.evict, prefetch_policy=args.prefetch, oracle_file=oracle,
                         log=os.path.join(run, "results.csv"), access_log=os.path.join(run, "access.csv"),
                         prefetch_log=os.path.join(run, "prefetch.csv"), feature_log=os.path.join(run, "features.csv"),
                         cache_log=os.path.join(run, "cache.csv"), vm_log_dir=run).items():
            c.write(f"{k}: {v}\n")
    json.dump(dict(vars(args), source_requests=len(reqs), meta_file=args.data_img + ".meta.json", replay=True),
              open(os.path.join(run, "args.json"), "w"), indent=1)
    srv = subprocess.Popen([os.path.join(ROOT, "bin/nbd_server"), cfg], stderr=open(os.path.join(run, "nbd_data.log"), "w"), cwd=ROOT)
    t0 = time.time()
    try:
        for _ in range(100):
            try:
                s = socket.create_connection(("127.0.0.1", args.port), timeout=60)
                break
            except OSError:
                time.sleep(0.1)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        handshake(s)
        buf = bytearray(8 << 20)
        mv = memoryview(buf)
        for i, (off, ln) in enumerate(reqs):
            s.sendall(struct.pack(">IHHQQI", 0x25609513, 0, 0, i + 1, off, ln))
            hdr = b""
            while len(hdr) < 16:
                hdr += s.recv(16 - len(hdr))
            got = 0
            while got < ln:
                n = s.recv_into(mv[: min(len(buf), ln - got)])
                if n == 0:
                    raise ConnectionError("server closed")
                got += n
        s.sendall(struct.pack(">IHHQQI", 0x25609513, 0, 2, 0, 0, 0))
        s.close()
        time.sleep(0.3)
    finally:
        srv.send_signal(signal.SIGTERM)
        try:
            srv.wait(timeout=30)
        except subprocess.TimeoutExpired:
            srv.kill()
    print(f"{args.name}: replayed {len(reqs)} requests in {time.time()-t0:.0f}s -> {run}")


if __name__ == "__main__":
    main()
