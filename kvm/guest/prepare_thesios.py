#!/usr/bin/env python3
"""Download Thesios shards and emit a compact READ filename/offset/length trace."""
import argparse
import csv
import hashlib
import os
import shutil
import tempfile
import urllib.request


# Bump when the condensed row format changes so stale per-shard caches are skipped.
CACHE_FORMAT = "v2"
# Thesios file names are 64 hex chars; 16 (64 bits) keeps the trace small and
# is far more than enough to tell a day's few hundred thousand files apart.
FILENAME_CHARS = 16


def condense_shard(url, output, trace_dir=None):
    """Write one shard's READ (filename, offset, length) rows to output.

    Uses the raw shard from trace_dir (same basename as the URL, as laid down
    by `make thesios-download`) if present, else downloads it.
    """
    stats = [0, 0, 0]  # source rows, reads, logical bytes
    local = os.path.join(trace_dir, os.path.basename(url)) if trace_dir else None
    with open(output, "w", newline="") as condensed:
        writer = csv.writer(condensed)
        with tempfile.NamedTemporaryFile() as downloaded:
            if local and os.path.isfile(local):
                print(f"using local {local}", flush=True)
                source = local
            else:
                print(f"downloading {url}", flush=True)
                with urllib.request.urlopen(url) as response:
                    while True:
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break
                        downloaded.write(chunk)
                downloaded.flush()
                source = downloaded.name
            with open(source, newline="") as trace:
                for row in csv.DictReader(trace):
                    stats[0] += 1
                    if row.get("op_type", "").strip().upper() != "READ":
                        continue
                    try:
                        filename = row["filename"][:FILENAME_CHARS]
                        offset = int(row["file_offset"])
                        length = int(row["request_io_size_bytes"])
                    except (KeyError, TypeError, ValueError):
                        continue
                    if not filename or offset < 0 or length <= 0:
                        continue
                    writer.writerow((filename, offset, length))
                    stats[1] += 1
                    stats[2] += length
    return stats


def prepare(urls, output, cache_dir=None, trace_dir=None):
    """Concatenate per-shard condensed reads into output.

    With cache_dir, each shard's condensed rows are kept there (keyed by URL)
    so later runs -- including ones that only add shards -- skip the download.
    """
    if cache_dir:
        os.makedirs(cache_dir, exist_ok=True)
    with open(output, "w", newline="") as out:
        out.write("filename,offset,length\n")
        for url in urls:
            cached = None
            if cache_dir:
                key = hashlib.sha256(f"{CACHE_FORMAT}:{url}".encode()).hexdigest()[:16]
                cached = os.path.join(cache_dir, f"{key}.reads.csv")
            if cached and os.path.exists(cached):
                print(f"cached {url}", flush=True)
            else:
                part = (cached or output) + ".part"
                stats = condense_shard(url, part, trace_dir)
                print(f"condensed {url}: {stats[0]} rows, {stats[1]} reads, {stats[2]} bytes", flush=True)
                if cached:
                    os.replace(part, cached)
                else:
                    cached = part
            with open(cached) as shard:
                shutil.copyfileobj(shard, out)
            if not cache_dir:
                os.remove(cached)
    print(f"wrote {len(urls)} shard(s) -> {output}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", action="append", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--cache-dir", help="reuse/store per-shard condensed traces here")
    parser.add_argument("--trace-dir", help="directory of pre-downloaded raw shards (see `make thesios-download`)")
    args = parser.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    prepare(args.url, args.output, args.cache_dir, args.trace_dir)


if __name__ == "__main__":
    main()
