"""Replay a condensed Thesios offset/length trace as raw reads.

The image builder downloads one public Thesios shard and writes its metadata
under /opt/workload/thesios_config.json. Thesios file_offset values are
replayed directly against the configured device; the device therefore needs
to be large enough for the selected cluster's offsets. Trace writes are
skipped because the running guest's writes are intentionally discarded by
nbd_server.
"""
import csv
import json
import mmap
import os
import time

with open("/opt/workload/thesios_config.json") as f:
    cfg = json.load(f)

trace_path = cfg["trace_path"]
device_path = cfg["device_path"]
max_requests = cfg["max_requests"]
direct_io = bool(cfg.get("direct_io", 0))

print(
    f"THESIOS_BENCH starting: up to {max_requests} requests from {trace_path} "
    f"against {device_path} direct_io={int(direct_io)}",
    flush=True,
)

read_count = 0
skipped_count = 0
byte_count = 0
start = time.time()
open_flags = os.O_RDONLY | (getattr(os, "O_DIRECT", 0) if direct_io else 0)
if direct_io and not getattr(os, "O_DIRECT", 0):
    raise RuntimeError("THESIOS_DIRECT_IO requires Linux O_DIRECT support")
device_fd = os.open(device_path, open_flags)
direct_buffer = None
direct_buffer_size = 0
try:
  with open(trace_path, newline="") as trace:
    for row in csv.DictReader(trace):
        if max_requests and read_count >= max_requests:
            break
        try:
            offset = int(row["offset"])
            size = int(row["length"])
        except (KeyError, TypeError, ValueError):
            skipped_count += 1
            continue
        if offset < 0 or size <= 0:
            skipped_count += 1
            continue
        if direct_io:
            sector_size = 512
            aligned_offset = offset // sector_size * sector_size
            aligned_end = (offset + size + sector_size - 1) // sector_size * sector_size
            aligned_read_size = aligned_end - aligned_offset
            page_size = os.sysconf("SC_PAGESIZE")
            buffer_size = (aligned_read_size + page_size - 1) // page_size * page_size
            if buffer_size > direct_buffer_size:
                if direct_buffer is not None:
                    direct_buffer.close()
                direct_buffer = mmap.mmap(-1, buffer_size)
                direct_buffer_size = buffer_size
            bytes_read = os.preadv(
                device_fd, [memoryview(direct_buffer)[:aligned_read_size]], aligned_offset
            )
        else:
            bytes_read = len(os.pread(device_fd, size, offset))
        expected_bytes = aligned_read_size if direct_io else size
        if bytes_read != expected_bytes:
            raise RuntimeError(
                f"short read at offset={offset} size={size}: got {bytes_read}"
            )
        read_count += 1
        byte_count += size
finally:
    if direct_buffer is not None:
        direct_buffer.close()
    os.close(device_fd)

elapsed = time.time() - start
if read_count == 0:
    raise RuntimeError("trace contained no valid READ requests")
print(
    f"THESIOS_BENCH complete: {read_count} reads, {byte_count} bytes in "
    f"{elapsed:.2f}s ({read_count / elapsed:.1f} reads/sec)",
    flush=True,
)
