#!/usr/bin/env python3
"""Ground truth for a marked app-launch capture (tools/capture/workloads/app_launch_marked.sh).

  launch_truth.py RUN_DIR [--tail 2697986048]  ->  RUN_DIR/launch_truth.csv, RUN_DIR/root_trace.csv

launch_truth.csv: one row per launch: round, app, app_name, t_begin, t_end (host CLOCK_MONOTONIC ns, from the marker reads
on the data disk) and the root-disk request range [i0, i1) it covers.
root_trace.csv: the root-disk read stream (offset, length, arrive_ns, worker) with launch / round / app / pos (index of
the request within its launch, -1 outside any launch) for every request.  This is the psim trace for the study.
"""
import argparse, os
import numpy as np
import pandas as pd

APPS = ["py_faiss", "py_std", "java", "perl", "sqlite", "apt"]


def load(run, tail):
    f = pd.read_csv(os.path.join(run, "features.csv"))
    m = f[(f.type == 0) & (f.offset >= tail)].sort_values("arrive_ns").reset_index(drop=True)
    m["k"] = (m.offset - tail) // 4096
    # harness sync probes: runs of ids 0..7 at the very start and end; launch markers are 1 .. 2*(R*8+6)
    drop = np.zeros(len(m), bool)
    for lo, hi in ((0, 8), (len(m) - 8, len(m))):
        seg = m.k.values[lo:hi]
        if len(seg) == 8 and (np.sort(seg) == np.arange(8)).all(): drop[lo:hi] = True
    m = m[~drop & (m.k >= 1) & (m.k < 2048)]
    rows = {}
    for k, t in zip(m.k.values, m.arrive_ns.values):
        j = (k - 1) // 2; r, a = divmod(j, 8)
        rows.setdefault((r, a), {})["t_end" if (k - 1) % 2 else "t_begin"] = t
    L = pd.DataFrame([dict(round=r, app=a, app_name=APPS[a], **v) for (r, a), v in rows.items()])
    L = L.dropna().sort_values("t_begin").reset_index(drop=True)
    L["launch"] = np.arange(len(L))
    root = pd.read_csv(os.path.join(run, "root_features.csv"))
    root = root[root.type == 0].sort_values("arrive_ns").reset_index(drop=True)
    t = root.arrive_ns.values
    root["launch"] = -1
    for i, (b, e) in enumerate(zip(L.t_begin.values, L.t_end.values)):
        root.loc[(t >= b) & (t <= e), "launch"] = i
    L["i0"] = [int(np.searchsorted(t, b)) for b in L.t_begin]
    L["i1"] = [int(np.searchsorted(t, e, side="right")) for e in L.t_end]
    root["round"] = root.launch.map(dict(zip(L.launch, L["round"]))).fillna(-1).astype(int)
    root["app"] = root.launch.map(dict(zip(L.launch, L.app))).fillna(-1).astype(int)
    root["pos"] = root.groupby("launch").cumcount().where(root.launch >= 0, -1)
    return L, root


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--tail", type=int, default=2697986048)
    a = ap.parse_args()
    L, root = load(a.run, a.tail)
    L.to_csv(os.path.join(a.run, "launch_truth.csv"), index=False)
    root[["offset", "length", "arrive_ns", "worker", "launch", "round", "app", "pos"]].to_csv(os.path.join(a.run, "root_trace.csv"), index=False)
    inl = (root.launch >= 0).mean()
    print(f"{len(L)} launches, {len(root)} root reads, {100 * inl:.1f}% inside a launch window; "
          f"per launch: {root[root.launch >= 0].groupby('launch').size().median():.0f} reads (median)")
    print(L.groupby("app_name").apply(lambda g: (g.i1 - g.i0).median()).rename("median reads").to_string())


if __name__ == "__main__":
    main()
