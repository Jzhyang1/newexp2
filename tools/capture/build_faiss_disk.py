#!/usr/bin/env python3
"""Build the data disk the FAISS capture workload searches (no root needed).

Produces `--out` (default disk/data/faiss_data.img): an ext4 image holding a
disk-resident FAISS IVF-Flat index (`index.faiss` + `index.ivfdata`, the
inverted lists, memory-mapped by faiss at search time) plus `meta.json` with the
parameters, the exact byte range of every inverted list inside the index data
file and a query set (`queries.npy`, written next to the image, NOT inside it,
so reading it never touches the NBD disk).

Data model (so the access stream has structure worth predicting, like a RAG /
semantic-search service rather than uniform noise):
  * vectors come from a Gaussian mixture with `--modes` topics of Zipf-skewed size;
  * queries come in "sessions" (a user/topic issues a burst of related
    queries), topics drawn Zipf(`--zipf`).  Related queries probe overlapping
    inverted lists, which is the temporal locality a prefetcher can learn.

Requires `pip install faiss-cpu` (host only; the guest image already has it)
and e2fsprogs (`mke2fs -d` populates an ext4 image from a directory).
"""
import argparse
import json
import os
import shutil
import subprocess
import time

import numpy as np
import faiss


def zipf_weights(n, s):
    w = 1.0 / np.arange(1, n + 1) ** s
    return w / w.sum()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="disk/data/faiss_data.img")
    ap.add_argument("--queries-out", default="disk/data/faiss_queries.npy")
    ap.add_argument("--n", type=int, default=6_000_000)
    ap.add_argument("--d", type=int, default=64)
    ap.add_argument("--nlist", type=int, default=4096)
    ap.add_argument("--modes", type=int, default=1500)
    ap.add_argument("--mode-skew", type=float, default=0.6)
    ap.add_argument("--zipf", type=float, default=1.05, help="query topic popularity skew")
    ap.add_argument("--n-queries", type=int, default=20000)
    ap.add_argument("--session-min", type=int, default=5)
    ap.add_argument("--session-max", type=int, default=40)
    ap.add_argument("--tail-mib", type=int, default=16, help="raw bytes after the fs, used for clock-sync probes")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    d, n = args.d, args.n
    centers = (rng.standard_normal((args.modes, d)) * 3.0).astype("float32")
    mode_w = zipf_weights(args.modes, args.mode_skew)
    sigma = 0.6

    def sample_points(m):
        a = rng.choice(args.modes, size=m, p=mode_w)
        return (centers[a] + rng.standard_normal((m, d), dtype="float32") * sigma).astype("float32")

    stage = os.path.abspath(args.out) + ".stage"
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    cwd = os.getcwd()

    t0 = time.time()
    quant = faiss.IndexFlatL2(d)
    index = faiss.IndexIVFFlat(quant, d, args.nlist)
    index.train(sample_points(max(40 * args.nlist, 200_000)))
    print(f"trained in {time.time()-t0:.1f}s", flush=True)

    # in-memory populate, then rewrite as a compact on-disk invlists file
    chunk = 500_000
    for i in range(0, n, chunk):
        index.add(sample_points(min(chunk, n - i)))
    print(f"added {index.ntotal} vectors in {time.time()-t0:.1f}s", flush=True)

    os.chdir(stage)
    try:
        ondisk = faiss.OnDiskInvertedLists(index.nlist, index.code_size, "index.ivfdata")
        ivf_vector = faiss.InvertedListsPtrVector()
        ivf_vector.push_back(index.invlists)
        ntotal = ondisk.merge_from_multiple(ivf_vector.data(), ivf_vector.size(), False, False)
        index.ntotal = ntotal
        index.replace_invlists(ondisk, False)
        faiss.write_index(index, "index.faiss")
        lists = []
        for i in range(index.nlist):
            lst = ondisk.lists.at(i)
            lists.append([int(lst.offset), int(ondisk.list_size(i)), int(index.code_size)])
        meta = dict(n=n, d=d, nlist=index.nlist, code_size=index.code_size, seed=args.seed,
                    list_offset_size_codesize=lists, ivfdata_bytes=os.path.getsize("index.ivfdata"),
                    queries_file=os.path.basename(args.queries_out))
        with open("meta.json", "w") as f:
            json.dump(meta, f)
    finally:
        os.chdir(cwd)
    print(f"index written in {time.time()-t0:.1f}s; ivfdata={meta['ivfdata_bytes']/2**20:.0f} MiB", flush=True)

    # query stream: sessions of related queries
    qs = []
    while len(qs) < args.n_queries:
        topic = rng.choice(args.modes, p=zipf_weights(args.modes, args.zipf))
        for _ in range(rng.integers(args.session_min, args.session_max + 1)):
            qs.append(centers[topic] + rng.standard_normal(d, dtype="float32") * sigma * 1.5)
    qs = np.stack(qs[: args.n_queries]).astype("float32")
    np.save(args.queries_out, qs)

    total = sum(os.path.getsize(os.path.join(stage, f)) for f in os.listdir(stage))
    fs_bytes = int(total * 1.03 + 64 * 2**20)
    fs_bytes = (fs_bytes + 2**20 - 1) // 2**20 * 2**20
    if os.path.exists(args.out):
        os.remove(args.out)
    subprocess.check_call(["mke2fs", "-q", "-t", "ext4", "-F", "-L", "faissdata", "-b", "4096",
                           "-O", "^has_journal,extent,^resize_inode", "-d", stage, args.out,
                           f"{fs_bytes // 1024}k"])
    with open(args.out, "r+b") as f:
        f.truncate(fs_bytes + args.tail_mib * 2**20)
    meta["fs_bytes"] = fs_bytes
    meta["tail_offset"] = fs_bytes
    meta["tail_bytes"] = args.tail_mib * 2**20
    with open(args.out + ".meta.json", "w") as f:
        json.dump(meta, f)
    shutil.rmtree(stage)
    print(f"image {args.out}: fs {fs_bytes/2**20:.0f} MiB + {args.tail_mib} MiB raw tail; "
          f"queries {qs.shape} -> {args.queries_out}  (total {time.time()-t0:.0f}s)")


if __name__ == "__main__":
    main()
