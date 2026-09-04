#!/usr/bin/env python3
"""Render a heatmap of page-access density over time from nbd_server's
access_log CSV (columns: worker_id,offset,length,hits,total -- see
disk/nbd.example.yaml's `access_log` key and disk/sim.hpp's SimReadClass,
which writes one row per read() call, in the order it was processed).

The log has no timestamp column, so "time" here is that request sequence
(row order), grouped into --time-bins buckets. Each row's byte range
[offset, offset+length) is converted to a page range (page = byte //
SECTOR_SIZE) and every page it touches increments that (time bin, page
bin) cell's count -- so a darker cell is a page range that was read more
often during that stretch of the run. All workers are combined into one
heatmap; pass --worker-id to isolate one.

Usage:
    heatmap.py                              # reads ./logs/nbd_access.csv
    heatmap.py path/to/access_log.csv
    heatmap.py --out heatmap.png --time-bins 300 --page-bins 300
    heatmap.py --worker-id 1

Requires matplotlib: pip install matplotlib
"""
import argparse
import csv
import sys

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    sys.exit("heatmap.py requires matplotlib: pip install matplotlib")

SECTOR_SIZE = 512
DEFAULT_ACCESS_LOG = "./logs/nbd_access.csv"


def load_rows(path, worker_id):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if worker_id is not None:
        rows = [r for r in rows if r["worker_id"] == str(worker_id)]
    if not rows:
        sys.exit(f"{path}: no matching rows found")
    return rows


def build_heatmap(rows, time_bins, page_bins):
    starts = [int(r["offset"]) // SECTOR_SIZE for r in rows]
    counts = [max(1, int(r["length"]) // SECTOR_SIZE) for r in rows]
    ends = [s + c for s, c in zip(starts, counts)]

    min_page = min(starts)
    max_page = max(ends)
    page_span = max(1, max_page - min_page)
    n = len(rows)

    grid = np.zeros((page_bins, time_bins), dtype=np.int64)
    for i, (start, end) in enumerate(zip(starts, ends)):
        t_bin = min(time_bins - 1, i * time_bins // n)
        p_lo = min(page_bins - 1, (start - min_page) * page_bins // page_span)
        p_hi = min(page_bins - 1, max(p_lo, (end - 1 - min_page) * page_bins // page_span))
        grid[p_lo:p_hi + 1, t_bin] += 1
    return grid, min_page, max_page


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("access_log", nargs="?", default=DEFAULT_ACCESS_LOG,
                     help=f"path to the access_log CSV (default: {DEFAULT_ACCESS_LOG})")
    ap.add_argument("--out", default="heatmap.png",
                     help="output image path (default: heatmap.png)")
    ap.add_argument("--time-bins", type=int, default=200,
                     help="number of buckets along the time axis (default: 200)")
    ap.add_argument("--page-bins", type=int, default=200,
                     help="number of buckets along the page axis (default: 200)")
    ap.add_argument("--worker-id", type=int, default=None,
                     help="restrict to one worker_id (default: combine all workers)")
    args = ap.parse_args()

    rows = load_rows(args.access_log, args.worker_id)
    grid, min_page, max_page = build_heatmap(rows, args.time_bins, args.page_bins)

    fig, ax = plt.subplots(figsize=(10, 6))
    im = ax.imshow(grid, aspect="auto", origin="lower", cmap="viridis",
                    extent=[0, len(rows), min_page, max_page])
    ax.set_xlabel("request sequence (time)")
    ax.set_ylabel(f"page number ({SECTOR_SIZE}B sectors)")
    title = f"Page access heatmap -- {args.access_log}"
    if args.worker_id is not None:
        title += f" (worker_id={args.worker_id})"
    ax.set_title(title)
    fig.colorbar(im, ax=ax, label="accesses per cell")
    fig.tight_layout()
    fig.savefig(args.out, dpi=150)
    print(f"wrote {args.out} ({len(rows)} rows, page range [{min_page}, {max_page}))")


if __name__ == "__main__":
    main()
