#!/usr/bin/env python3
"""Download Thesios shards and emit a compact READ offset/length trace."""
import argparse
import csv
import os
import tempfile
import urllib.request


def prepare(urls, output):
    read_count = 0
    byte_count = 0
    row_count = 0
    with open(output, "w", newline="") as condensed:
        writer = csv.writer(condensed)
        writer.writerow(("offset", "length"))
        for url in urls:
            with tempfile.NamedTemporaryFile() as downloaded:
                print(f"downloading {url}", flush=True)
                with urllib.request.urlopen(url) as response:
                    while True:
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break
                        downloaded.write(chunk)
                downloaded.flush()
                downloaded.seek(0)
                with open(downloaded.name, newline="") as trace:
                    for row in csv.DictReader(trace):
                        row_count += 1
                        if row.get("op_type", "").strip().upper() != "READ":
                            continue
                        try:
                            offset = int(row["file_offset"])
                            length = int(row["request_io_size_bytes"])
                        except (KeyError, TypeError, ValueError):
                            continue
                        if offset < 0 or length <= 0:
                            continue
                        writer.writerow((offset, length))
                        read_count += 1
                        byte_count += length
    print(
        f"condensed {len(urls)} shard(s): {row_count} source rows, "
        f"{read_count} reads, {byte_count} logical bytes -> {output}",
        flush=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", action="append", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    prepare(args.url, args.output)


if __name__ == "__main__":
    main()
