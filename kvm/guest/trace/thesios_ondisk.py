"""Guest workload: replay a condensed Thesios trace as buffered reads on the raw data disk (/dev/vdb).

Same placement as kvm/guest/workloads/thesios_bench.py: each trace file is put at a granule-aligned spot given by a
stable hash of (seed, filename) past `region_start`, Thesios offsets are file-relative.  Reads go through the guest page
cache (buffered pread), so the NBD stream the host sees is what the guest's own readahead makes of the trace.
"""
import csv
import hashlib
import json
import os
import sys
import time

cfg = json.load(open("/tmp/tools/run_config.json"))
th = cfg["thesios"]
seed, granule, region_start = th["seed"], th["granule"], th["region_start"]
max_requests = th["requests"]


def main():
    ra = cfg.get("guest_ra_kb")
    if ra is not None:  # ablation: the guest kernel's own readahead for the data disk
        with open("/sys/block/vdb/queue/read_ahead_kb", "w") as f:
            f.write(str(ra))
    with open("/sys/block/vdb/queue/read_ahead_kb") as f:
        print("GUEST_READ_AHEAD_KB", f.read().strip(), flush=True)
    fd = os.open("/dev/vdb", os.O_RDONLY)
    size = os.lseek(fd, 0, os.SEEK_END)
    first = -(-region_start // granule)
    count = size // granule - first
    span = count * granule
    sys.stdin.buffer.read(1)            # start barrier (tracing filters are installed before this returns)
    hinter = None
    if cfg.get("hints"):
        sys.path.insert(0, "/tmp/tools")
        import hints
        hinter = hints.Hinter()
    K = cfg.get("hints", {}).get("k", 4)
    t0 = time.time()
    n = nbytes = 0
    pending = []          # (device offset, length) of rows read ahead, so read n+K can be hinted when read n is issued
    base_cache = (None, 0)

    def placed():
        nonlocal base_cache
        with open("/tmp/tools/thesios_trace.csv", newline="") as f:
            rows = csv.reader(f)
            next(rows)
            for name, off, ln in rows:
                off, ln = int(off), int(ln)
                if base_cache[0] != name:
                    dg = hashlib.blake2b(f"{seed}:{name}".encode(), digest_size=8).digest()
                    base_cache = (name, (int.from_bytes(dg, "little") % count) * granule)
                rel = (base_cache[1] + off) % span
                if rel + ln > span:
                    rel %= span - ln + 1
                yield first * granule + rel, ln

    import collections
    src = placed()
    window = collections.deque()                       # the next K+1 reads; the one entering the window is hinted
    for _ in range(K + 1 if hinter else 1):
        nxt = next(src, None)
        if nxt is None:
            break
        window.append(nxt)
        if hinter:
            hinter.hint(nxt[0], nxt[1])
    while window and not (max_requests and n >= max_requests):
        dev_off, ln = window.popleft()
        nxt = next(src, None)
        if nxt is not None:
            window.append(nxt)
            if hinter:
                hinter.hint(nxt[0], nxt[1])
        got = len(os.pread(fd, ln, dev_off))
        if got != ln:
            raise RuntimeError(f"short read {got} != {ln} at {dev_off}")
        n += 1
        nbytes += ln
    dt = time.time() - t0
    print("WORKLOAD_RESULT: PASS reads=%d bytes=%d total=%.1fs" % (n, nbytes, dt), flush=True)



main()
