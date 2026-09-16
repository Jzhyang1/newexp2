"""Replay a Thesios CSV shard as raw block-device reads.

The image builder downloads one public Thesios shard and writes its metadata
under /opt/workload/thesios_config.json. Thesios file_offset values are
replayed directly against the configured device; the device therefore needs
to be large enough for the selected cluster's offsets. Trace writes are
skipped because the running guest's writes are intentionally discarded by
nbd_server.
"""
import csv
import json
import os
import time

with open("/opt/workload/thesios_config.json") as f:
    cfg = json.load(f)

trace_path = cfg["trace_path"]
device_path = cfg["device_path"]
max_requests = cfg["max_requests"]

print(
    f"THESIOS_BENCH starting: up to {max_requests} requests from {trace_path} "
    f"against {device_path}",
    flush=True,
)

read_count = 0
skipped_count = 0
byte_count = 0
start = time.time()
with open(device_path, "rb", buffering=0) as device, open(
    trace_path, newline=""
) as trace:
    for row in csv.DictReader(trace):
        if read_count + skipped_count >= max_requests:
            break
        if row.get("op_type", "").strip().upper() != "READ":
            skipped_count += 1
            continue
        try:
            offset = int(row["file_offset"])
            size = int(row["request_io_size_bytes"])
        except (KeyError, TypeError, ValueError):
            skipped_count += 1
            continue
        if offset < 0 or size <= 0:
            skipped_count += 1
            continue
        data = os.pread(device.fileno(), size, offset)
        if len(data) != size:
            raise RuntimeError(
                f"short read at offset={offset} size={size}: got {len(data)}"
            )
        read_count += 1
        byte_count += size

elapsed = time.time() - start
if read_count == 0:
    raise RuntimeError("trace contained no valid READ requests")
print(
    f"THESIOS_BENCH complete: {read_count} reads, {byte_count} bytes in "
    f"{elapsed:.2f}s ({read_count / elapsed:.1f} reads/sec)",
    flush=True,
)
