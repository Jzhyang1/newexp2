"""Runs a disk-based analogue of the HPC Challenge "GUPS" (Giant Updates
Per Second) RandomAccess benchmark against the table that
kvm/guest/build_image.sh pre-wrote at image-build time (see its GUPS_ENABLE
block). The original GUPS repeatedly updates (read-XOR-write) a giant
in-memory table at pseudo-random indices; here the table lives on the
simulated disk instead, and only the read half of that update runs at boot
-- a write from a *running* guest lands on its volatile tmpfs overlay and
never reaches nbd_server at all (disk/sim.hpp), so a read-modify-write
wouldn't exercise anything a plain read doesn't already: what this
simulator cares about is the random *access pattern* hitting nbd_server's
cache/prefetch policy, not actual update semantics.

Reads /opt/workload/gups_config.json (written by build_image.sh) so the
table size/access count/block size used here always matches what was
actually baked into the image, instead of duplicating those numbers in two
places.
"""
import json
import mmap
import os
import random
import time

with open("/opt/workload/gups_config.json") as f:
    cfg = json.load(f)

table_path = cfg["table_path"]
table_size = cfg["table_size_bytes"]
block_size = cfg["block_size"]
updates = cfg["updates"]
direct_io = bool(cfg.get("direct_io", 0))

# Last full block_size-aligned slot the table can serve a read from.
last_slot = table_size // block_size - 1

print(
    f"GUPS_BENCH starting: {updates} random {block_size}-byte reads over "
    f"{table_size}-byte table {table_path}",
    flush=True,
)

start = time.time()
if direct_io:
    # mmap allocations are page-aligned, satisfying O_DIRECT's user-buffer
    # alignment requirement while keeping the existing random offsets.
    fd = os.open(table_path, os.O_RDONLY | os.O_DIRECT)
    buffer = mmap.mmap(-1, block_size)
    try:
        for _ in range(updates):
            slot = random.randint(0, last_slot)
            if os.preadv(fd, [buffer], slot * block_size) != block_size:
                raise IOError("short O_DIRECT read")
    finally:
        buffer.close()
        os.close(fd)
else:
    with open(table_path, "rb", buffering=0) as f:
        for _ in range(updates):
            slot = random.randint(0, last_slot)
            f.seek(slot * block_size)
            if len(f.read(block_size)) != block_size:
                raise IOError("short read")
elapsed = time.time() - start

gups = updates / elapsed / 1e9
print(
    f"GUPS_BENCH complete: {updates} reads in {elapsed:.2f}s "
    f"({gups:.6f} GUPS, {updates / elapsed:.1f} reads/sec)",
    flush=True,
)
