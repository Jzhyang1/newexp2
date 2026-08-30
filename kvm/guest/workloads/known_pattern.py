#!/usr/bin/env python3
"""Deterministic disk-read workload for verifying VM I/O capture end-to-end.

Baked into a guest image via `make guest-image WORKLOAD_SCRIPT=kvm/guest/
workloads/known_pattern.py ...` (see kvm/verify_capture.py's docstring for
the full recipe). Reads a fixed, printed sequence of (skip, count) sector
ranges from /dev/vdb -- a *second* disk, wired to its own nbd_server port,
that this workload is the only thing ever touching (unlike /dev/vda, the
boot disk, which the kernel/systemd generate lots of read traffic on before
this even runs). That means every event nbd_server captures for /dev/vdb's
worker_id is attributable to exactly this script, in exactly this order --
no need to correlate timestamps or filter out boot noise.

`iflag=direct` makes dd bypass the guest page cache so every read actually
reaches virtio-blk (and therefore nbd_server) instead of being served from
a prior read still sitting in guest RAM.

Each line printed here goes to the serial console, i.e.
<log_dir>/<vm_name>.serial.log on the host (kvm/launch.py).
"""
import subprocess

DEVICE = "/dev/vdb"
SECTOR = 512

# (skip_sectors, count_sectors). The last pair is deliberately repeated to
# also exercise a cache hit -- see kvm/verify_capture.py's --expect-hit-on-repeat.
PATTERN = [
    (0, 8),
    (100, 4),
    (500, 16),
    (1000, 1),
    (1000, 1),
]

for skip, count in PATTERN:
    print(f"KNOWN_PATTERN READ skip={skip} count={count}", flush=True)
    subprocess.run(
        [
            "dd",
            f"if={DEVICE}",
            "of=/dev/null",
            f"bs={SECTOR}",
            f"skip={skip}",
            f"count={count}",
            "iflag=direct",
        ],
        check=True,
    )
    print(f"KNOWN_PATTERN DONE skip={skip} count={count}", flush=True)

print("KNOWN_PATTERN COMPLETE", flush=True)
