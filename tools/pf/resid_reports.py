#!/usr/bin/env python3
"""Residency-report hint file for `ivfgeo` (no application involved): a guest agent scans mincore of the index every `period` queries and
reports the lists whose resident fraction changed by >= 2/14 since its last report.  The scan sees the guest cache as of `lag` queries ago.
usage: resid_reports.py name period lag"""
import pickle, struct, sys, numpy as np
R = "/home/jonathan-yang/newexp2/"
name, period, lag = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
C = pickle.load(open(R + "logs/pf_signals/cache.pkl", "rb"))
L, first_i, LP = C["lists_of"], C["first_i"], C["list_pages"]
RA = np.load(R + "logs/pf_signals/resid_all.npy")
last = {}; n = 0
with open(R + f"logs/pf_signals/{name}.bin", "wb") as f:
    for k in range(len(L)):
        if k % period or k not in first_i: continue
        kk = max(0, k - lag)
        for l in range(RA.shape[1]):
            v = int(RA[kk, l])
            if abs(v - last.get(l, -9)) >= 2:
                lo, hi = LP[l]; last[l] = v; n += 1
                f.write(struct.pack("<QQQII", first_i[k], lo, hi - lo + 1, v, 0))
print(name, "reports", n)
