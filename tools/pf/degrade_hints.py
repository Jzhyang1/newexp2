#!/usr/bin/env python3
"""Degrade an oracle prediction file (records {u64 request idx, u64 page, f32 p}) the ways a real hint channel would:
  delay D      the hint reaches the host D requests later than the oracle's (channel latency, batching)
  noise F      a fraction F of hint groups (one per request idx) point at the right-sized range in the wrong place
  keep F       only a fraction F of groups exist (not every request/query carries a hint)
usage: degrade_hints.py in.bin out.bin [--delay D] [--noise F] [--keep F] [--seed N]"""
import argparse, random, struct, collections
ap = argparse.ArgumentParser(); ap.add_argument("inp"); ap.add_argument("out")
ap.add_argument("--delay", type=int, default=0); ap.add_argument("--noise", type=float, default=0); ap.add_argument("--keep", type=float, default=1)
ap.add_argument("--seed", type=int, default=1); a = ap.parse_args()
rnd = random.Random(a.seed)
src = open(a.inp, "rb").read()
groups = collections.OrderedDict()
for off in range(0, len(src), 20):
    i, pg, p = struct.unpack_from("<QQf", src, off); groups.setdefault(i, []).append(pg)
maxpg = max(max(v) for v in groups.values()); minpg = min(min(v) for v in groups.values())
recs = []
for i, pgs in groups.items():
    if rnd.random() > a.keep: continue
    if rnd.random() < a.noise:
        shift = rnd.randrange(minpg, maxpg) - min(pgs); pgs = [pg + shift for pg in pgs]
    for pg in pgs: recs.append((i + a.delay, pg))
recs.sort()
with open(a.out, "wb") as f:
    for i, pg in recs: f.write(struct.pack("<QQf", i, pg, 1.0))
