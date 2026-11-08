#!/usr/bin/env python3
"""Thesios headroom: what if the policy were told the application's next K reads (file identity + offset + length)?
Maps each host request of a captured VM run (th_m1g) to the application read that caused it, then writes a psim `file:`
prediction file listing the page ranges of the next K application reads.  usage: signal_oracle_th.py"""
import csv, hashlib, os, struct, numpy as np, pandas as pd
R = "/home/jonathan-yang/newexp2/"
f = pd.read_csv(R + "logs/pf_streams/th_m1g/features.csv"); f = f[(f.type == 0) & (f.offset < (8 << 40) - (16 << 20))].reset_index(drop=True)
out = R + "logs/pf_signals/"; os.makedirs(out, exist_ok=True)
f[["offset", "length", "arrive_ns", "inflight"]].to_csv(out + "th_trace.csv", index=False)
seed, granule, region = 20261007, 2 << 20, 1 << 30
size = 8 << 40; first = -(-region // granule); count = size // granule - first; span = count * granule
app = []; cache = (None, 0)
with open(R + "disk/data/thesios_trace.csv") as fh:
    rd = csv.reader(fh); next(rd)
    for n, (name, off, ln) in enumerate(rd):
        if n >= 150000: break
        off, ln = int(off), int(ln)
        if cache[0] != name:
            cache = (name, (int.from_bytes(hashlib.blake2b(f"{seed}:{name}".encode(), digest_size=8).digest(), "little") % count) * granule)
        rel = (cache[1] + off) % span
        if rel + ln > span: rel %= span - ln + 1
        app.append((first * granule + rel, ln, name))
ao = np.array([a[0] for a in app]); al = np.array([a[1] for a in app])
PG = 65536
# host request -> app read: the app read (index >= a moving pointer, else nearest) whose byte range, widened by the guest's
# readahead window, contains the request start.  Indexed by 1 MiB granule so cached app reads (no host request) are skipped.
from collections import defaultdict
G = 1 << 20; idx = defaultdict(list)
for j in range(len(app)):
    for g in range((ao[j] - G) // G, (ao[j] + al[j] + G) // G + 1): idx[g].append(j)
n_app = len(app); ptr = 0; owner = np.full(len(f), -1)
for i, o in enumerate(f.offset.values):
    c = idx.get(o // G)
    if not c: continue
    fwd = [j for j in c if j >= ptr - 20]
    j = min(fwd) if fwd else max(c)
    owner[i] = j; ptr = max(ptr, j)
print("host requests", len(f), "mapped", np.mean(owner >= 0))
for K in (1, 4, 16, 64):
    with open(out + f"th_next{K}.bin", "wb") as fo:
        last = -1
        for i in range(len(f)):
            j = owner[i]
            if j < 0 or j == last: continue       # one prediction per app read (when it first shows up on the host)
            last = j
            for k in range(j + 1, min(n_app, j + 1 + K)):
                for pg in range(ao[k] // PG, (ao[k] + al[k] - 1) // PG + 1):
                    fo.write(struct.pack("<QQf", i, pg, 1.0))

# file extents (device pages): union of each file's reads, widened by 1 MiB for the guest's readahead overshoot
ext = {}
for (o, l, name) in app:
    a, b = ext.get(name, (o, o + l)); ext[name] = (min(a, o), max(b, o + l))
iv = sorted(((a - (1 << 20)) // PG, (b + (1 << 20) - 1) // PG) for a, b in ext.values())
merged = []
for a, b in iv:   # overlapping files (hash collisions) merge: the extent is then a union, never narrower than the truth
    if merged and a <= merged[-1][1]: merged[-1] = (merged[-1][0], max(merged[-1][1], b))
    else: merged.append((a, b))
with open(out + "th_clip.bin", "wb") as fo:
    for a, b in merged: fo.write(struct.pack("<QQ", max(a, 0), b))
print("files", len(ext), "extents", len(merged))
