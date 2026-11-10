#!/usr/bin/env python3
"""Split a capture into per-phase streams at the guest's marker reads (O_DIRECT reads at tail + 8 MiB + k pages,
written by tools/capture/workloads/rocksdb_ycsb.sh).

  split_phases.py logs/pf_streams/rk_m1g load c e     -> logs/pf_streams/rk_m1g_load, _c, _e  (features.csv + args.json)
"""
import json, os, shutil, sys


def main():
    run, names = sys.argv[1].rstrip("/"), sys.argv[2:]
    meta = json.load(open(json.load(open(os.path.join(run, "args.json")))["meta_file"]))
    base = meta["tail_offset"] + 2048 * 4096
    marks = {base + k * 4096: k for k in range(1, len(names) + 1)}
    src = open(os.path.join(run, "features.csv"))
    hdr = src.readline()
    cols = hdr.strip().split(",")
    io, it = cols.index("offset"), cols.index("type")
    outs, phase, counts = [], 0, [0] * len(names)
    for n in names:
        d = f"{run}_{n}"
        os.makedirs(d, exist_ok=True)
        shutil.copy(os.path.join(run, "args.json"), d)
        f = open(os.path.join(d, "features.csv"), "w")
        f.write(hdr)
        outs.append(f)
    for line in src:
        v = line.split(",", it + 2)
        off = int(line.split(",")[io])
        if int(v[it]) == 0 and off in marks:
            phase = marks[off]
            continue
        if phase < len(names):
            outs[phase].write(line)
            counts[phase] += 1
    for f in outs:
        f.close()
    print(run, dict(zip(names, counts)))


if __name__ == "__main__":
    main()
