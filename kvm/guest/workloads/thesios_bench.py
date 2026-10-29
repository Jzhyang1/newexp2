"""Replay a condensed Thesios filename/offset/length trace as raw reads.

The image builder bakes the condensed trace and its metadata under
/opt/workload/thesios_config.json. Thesios offsets are relative to a file, so
each file is given its own extent of the (sparse) device the first time the
trace touches it: a random, granule-aligned spot past `region_start_bytes`
(the guest's filesystem) that no other file has been given. Extent sizes come
from a first pass over the trace (the furthest byte each file is read to); the
random choices are seeded, so a given trace always lays out the same way.
Trace writes are skipped because the running guest's writes are intentionally
discarded by nbd_server.
"""
import csv
import json
import mmap
import os
import random
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
        for row in csv.DictReader(trace):
            if max_requests and valid >= max_requests:
                return
            try:
                name = row["filename"]
                offset = int(row["offset"])
                size = int(row["length"])
            except (KeyError, TypeError, ValueError):
                yield None
                continue
            if not name or offset < 0 or size <= 0:
                yield None
                continue
            valid += 1
            yield name, offset, size


class Layout:
    """Hands each file a random free extent of granules, once, on first visit."""

    MAX_TRIES = 1000

    def __init__(self, device_size, extents):
        self.extents = extents
        self.first = -(-region_start // granule)  # first usable granule
        self.count = device_size // granule - self.first
        self.rng = random.Random(seed)
        self.bases = {}  # file -> byte offset of its extent
        self.provisioned = set()  # every granule index handed out so far

    def base(self, name):
        if name not in self.bases:
            self.bases[name] = self._place(name)
        return self.bases[name]

    def _place(self, name):
        need = -(-self.extents[name] // granule)
        if need > self.count:
            raise RuntimeError(f"{name} needs {need} granules; device has {self.count}")
        for _ in range(self.MAX_TRIES):
            start = self.rng.randrange(self.count - need + 1)
            granules = range(start, start + need)
            if self.provisioned.isdisjoint(granules):
                self.provisioned.update(granules)
                return (self.first + start) * granule
        raise RuntimeError(
            f"no free {need}-granule extent for {name} after {self.MAX_TRIES} tries "
            f"({len(self.provisioned)}/{self.count} granules provisioned); "
            "use a larger device"
        )


extents = {}
for item in trace_rows():
    if item is not None:
        name, offset, size = item
        extents[name] = max(extents.get(name, 0), offset + size)

read_count = 0
skipped_count = 0
byte_count = 0
start = time.time()
open_flags = os.O_RDONLY | (getattr(os, "O_DIRECT", 0) if direct_io else 0)
if direct_io and not getattr(os, "O_DIRECT", 0):
    raise RuntimeError("THESIOS_DIRECT_IO requires Linux O_DIRECT support")
device_fd = os.open(device_path, open_flags)
layout = Layout(os.lseek(device_fd, 0, os.SEEK_END), extents)
print(
    f"THESIOS_BENCH layout: {len(extents)} files, region from {region_start} bytes, "
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
        offset = layout.base(name) + file_offset
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
