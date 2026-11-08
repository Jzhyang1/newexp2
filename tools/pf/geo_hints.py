#!/usr/bin/env python3
"""Write psim hint files for the no-app-hook FAISS predictor: for each query, the K lists nearest (centroid distance) to the first list it
touches, not resident in the guest, each hinted with aux = residency | confidence(rank) << 4.  usage: geo_hints.py K name [delay]"""
import pickle, struct, sys, numpy as np
R = "/home/jonathan-yang/newexp2/"; SC = "/tmp/claude-1000/-home-jonathan-yang-newexp2/0e888e7a-7f13-410b-94ae-3a8bcb0cf58f/scratchpad/"
K = int(sys.argv[1]); name = sys.argv[2]; delay = int(sys.argv[3]) if len(sys.argv) > 3 else 1
use_conf = len(sys.argv) <= 4 or sys.argv[4] != "noconf"
C = pickle.load(open(R + "logs/pf_signals/cache.pkl", "rb")); cent = np.load(SC + "cent.npy"); c2 = (cent ** 2).sum(1)
L, first_i, LP = C["lists_of"], C["first_i"], C["list_pages"]
RA = np.load(R + "logs/pf_signals/resid_all.npy")        # residency of every list at each query start (0..14); the old resid_frac only covered probed lists
with open(R + f"logs/pf_signals/{name}.bin", "wb") as f:
    for k in range(len(L)):
        if k not in first_i: continue
        a = int(L[k][0]); d = c2 - 2 * cent @ cent[a]
        cand = [int(l) for l in np.argsort(d)[:K * 8] if l != a and RA[k, l] < 7][:K]
        for rank, l in enumerate(cand):
            lo, hi = LP[l]
            aux = int(RA[k, l]) | ((min(14, rank // 2) if use_conf else 15) << 4)
            f.write(struct.pack("<QQQII", first_i[k] + delay, lo, hi - lo + 1, aux, 0))
