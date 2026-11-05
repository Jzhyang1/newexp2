"""Replay a condensed Thesios filename/offset/length trace as raw reads.

The image builder bakes the condensed trace and its metadata under
/opt/workload/thesios_config.json. Thesios offsets are relative to a file, so
each file is placed at a granule-aligned spot past `region_start_bytes` (the
guest's filesystem) given by a stable hash of (seed, filename), so no per-file
state is kept. Distinct files may overlap and share cache; in expectation the hit
rate is close to a real disk's. Reads that would run off the device end are
folded back into the usable region.
Trace writes are skipped because the running guest's writes are intentionally
discarded by nbd_server.
"""
import csv
import hashlib
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
region_start = cfg.get("region_start_bytes", 0)
granule = cfg.get("granule_bytes", 2 << 20)
seed = cfg.get("seed", 0)

print(
    f"THESIOS_BENCH starting: up to {max_requests} requests from {trace_path} "
    f"against {device_path} direct_io={int(direct_io)}",
    flush=True,
)



def trace_rows():
    """Yield (file, offset, size) per trace row, None for a malformed row.

    Stops after max_requests valid rows, so every pass sees the same prefix.
    """
    valid = 0
    with open(trace_path, newline="") as trace:
        rows = csv.reader(trace)
        header = next(rows, [])
        try:
            name_col = header.index("filename")
            offset_col = header.index("offset")
            size_col = header.index("length")
        except ValueError:
            raise RuntimeError(f"{trace_path} needs filename, offset, length columns")
        for row in rows:
            if max_requests and valid >= max_requests:
                return
            try:
                name = row[name_col]
                offset = int(row[offset_col])
                size = int(row[size_col])
            except (IndexError, ValueError):
                yield None
                continue
            if not name or offset < 0 or size <= 0:
                yield None
                continue
            valid += 1
            yield name, offset, size


class Layout:
    """Maps (file, offset) to a device offset with a stable hash; no per-file state.

    Each file's base is a granule-aligned spot in the usable region derived from
    hash(seed, name). Files may overlap (and so share cache) and an extent may run
    past the device end, in which case the read is folded back into the region.
    """

    def __init__(self, device_size):
        self.first = -(-region_start // granule)  # first usable granule
        self.count = device_size // granule - self.first
        if self.count <= 0:
            raise RuntimeError("device has no space past region_start_bytes")
        self.span = self.count * granule  # usable bytes
        self.base_cache = None  # (name, base): trace rows come in file runs

    def base(self, name):
        if self.base_cache is None or self.base_cache[0] != name:
            digest = hashlib.blake2b(f"{seed}:{name}".encode(), digest_size=8).digest()
            granule_index = int.from_bytes(digest, "little") % self.count
            self.base_cache = (name, granule_index * granule)
        return self.base_cache[1]

    def locate(self, name, file_offset, size):
        if size > self.span:
            raise RuntimeError(f"read of {size} bytes exceeds usable {self.span} bytes")
        rel = (self.base(name) + file_offset) % self.span
        if rel + size > self.span:
            rel %= self.span - size + 1
        return self.first * granule + rel


read_count = 0
skipped_count = 0
byte_count = 0
start = time.time()
open_flags = os.O_RDONLY | (getattr(os, "O_DIRECT", 0) if direct_io else 0)
if direct_io and not getattr(os, "O_DIRECT", 0):
    raise RuntimeError("THESIOS_DIRECT_IO requires Linux O_DIRECT support")
device_fd = os.open(device_path, open_flags)
layout = Layout(os.lseek(device_fd, 0, os.SEEK_END))
print(
    f"THESIOS_BENCH layout: hashed layout: region from {region_start} bytes, "
    f"{layout.count} granules of {granule} bytes, seed {seed}",
    flush=True,
)
direct_buffer = None
direct_buffer_size = 0
try:
    for item in trace_rows():
        if item is None:
            skipped_count += 1
            continue
        name, file_offset, size = item
        offset = layout.locate(name, file_offset, size)
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
