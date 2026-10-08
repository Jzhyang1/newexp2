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
    heatmap.py --max-accesses 0              # include all accesses

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


def load_rows(path, worker_id, max_accesses):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if worker_id is not None:
        rows = [r for r in rows if r["worker_id"] == str(worker_id)]
    if max_accesses:
        rows = rows[:max_accesses]
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


def build_length_heatmap(rows, time_bins, length_bins):
    lengths = [int(r["length"]) for r in rows]
    min_length = min(lengths)
    max_length = max(lengths)
    length_span = max(1, max_length - min_length)
    n = len(rows)

    grid = np.zeros((length_bins, time_bins), dtype=np.int64)
    for i, length in enumerate(lengths):
        t_bin = min(time_bins - 1, i * time_bins // n)
        l_bin = min(length_bins - 1, (length - min_length) * length_bins // length_span)
        grid[l_bin, t_bin] += 1
    return grid, min_length, max_length


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
    ap.add_argument("--max-accesses", type=int, default=50000,
                     help="number of accesses to include (default: 50000; 0: all)")
    args = ap.parse_args()

    rows = load_rows(args.access_log, args.worker_id, args.max_accesses)
    grid, min_page, max_page = build_heatmap(rows, args.time_bins, args.page_bins)
    length_grid, min_length, max_length = build_length_heatmap(rows, args.time_bins, args.page_bins)

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

    length_out = f"{args.out.rsplit('.', 1)[0]}_length.{args.out.rsplit('.', 1)[1]}" if "." in args.out else f"{args.out}_length"
    length_fig, length_ax = plt.subplots(figsize=(10, 6))
    length_im = length_ax.imshow(length_grid, aspect="auto", origin="lower", cmap="viridis",
                                 extent=[0, len(rows), min_length, max_length])
    length_ax.set_xlabel("request sequence (time)")
    length_ax.set_ylabel("request length (bytes)")
    length_title = f"Request length heatmap -- {args.access_log}"
    if args.worker_id is not None:
        length_title += f" (worker_id={args.worker_id})"
    length_ax.set_title(length_title)
    length_fig.colorbar(length_im, ax=length_ax, label="accesses per cell")
    length_fig.tight_layout()
    length_fig.savefig(length_out, dpi=150)
    print(f"wrote {length_out} ({len(rows)} rows, length range [{min_length}, {max_length}])")


if __name__ == "__main__":
    main()
