"""Guest workload: online nearest-neighbour search against a disk-resident FAISS IVF index.

The index's inverted lists live in /data/index.ivfdata and are mmap'd by
faiss, so every search touches `nprobe` contiguous list extents (one page
fault stream per list) -- the access pattern the host-side prefetcher sees as
NBD reads.  Queries come in topical sessions (see tools/capture/build_faiss_disk.py).

Application-level ground truth (query id + the inverted lists it is about to
read, i.e. signal #50, "app phase marker") is written to ftrace's
trace_marker so it lands on the same clock as the kernel events.
"""
import hashlib
import json
import os
import sys
import time

import numpy as np
import faiss

cfg = json.load(open("/tmp/tools/run_config.json"))
MARK_FDS = []


def open_markers():
    for inst in cfg["marker_instances"]:
        path = f"/sys/kernel/tracing/{inst}trace_marker"
        try:
            MARK_FDS.append(os.open(path, os.O_WRONLY))
        except OSError:
            pass


def mark(msg):
    b = msg.encode()
    for fd in MARK_FDS:
        os.write(fd, b)


def snapshot(tag):
    """Dump what /proc knows about this process: VMAs (VA -> file, to symbolize PCs/faults) and the fd table."""
    try:
        with open("/proc/self/maps") as f:
            maps = f.read()
        fds = {}
        for fd in os.listdir("/proc/self/fd"):
            try:
                fds[fd] = os.readlink(f"/proc/self/fd/{fd}")
            except OSError:
                pass
        with open(f"/tmp/out/proc_snapshot_{tag}.json", "w") as f:
            json.dump({"pid": os.getpid(), "maps": maps, "fds": fds, "mono_ns": time.clock_gettime_ns(time.CLOCK_MONOTONIC)}, f)
    except Exception as e:  # never let diagnostics kill the workload
        print("snapshot failed", e, file=sys.stderr)


def main():
    ra = cfg.get("guest_ra_kb")
    if ra is not None:  # ablation: change the guest kernel's own readahead for the data disk
        with open("/sys/block/vdb/queue/read_ahead_kb", "w") as f:
            f.write(str(ra))
    with open("/sys/block/vdb/queue/read_ahead_kb") as f:
        print("GUEST_READ_AHEAD_KB", f.read().strip(), flush=True)
    faiss.omp_set_num_threads(1)   # one search thread: reproducible order, one pid for the event filter
    q = np.load("/tmp/tools/queries.npy")[: cfg["n_queries"]]
    # barrier: guest_main adds our pid to the tracing filters, then writes one byte
    sys.stdin.buffer.read(1)
    open_markers()
    t0 = time.time()
    index = faiss.read_index("/tmp/data/index.faiss", faiss.IO_FLAG_ONDISK_SAME_DIR | faiss.IO_FLAG_READ_ONLY)
    index.nprobe = cfg["nprobe"]
    mark(f"APP load_done dt={time.time()-t0:.3f} nlist={index.nlist} ntotal={index.ntotal}")
    snapshot("start")
    nq = len(q)
    hinter = mincore = lists_dev = None
    if cfg.get("hints"):
        sys.path.insert(0, "/tmp/tools")
        import hints
        hinter = hints.Hinter()
        lists_dev = json.load(open("/tmp/tools/lists_dev.json"))
        if cfg["hints"]["mode"] == "aux":
            mincore = hints.Mincore("/tmp/data/index.ivfdata")
    lat = []
    digest = hashlib.sha256()   # over every query's returned ids+distances: proves the disk path returned the right bytes
    for i in range(nq):
        qi = q[i:i + 1]
        _, lists = index.quantizer.search(qi, cfg["nprobe"])     # which lists this query will read
        if hinter:           # the query's probe lists are known before any list is read
            for l in lists[0]:
                l = int(l)
                aux = 15
                if mincore:
                    fr = mincore.resident_fraction(lists_dev["file_off"][l], lists_dev["nbytes"][l])
                    aux = 15 if fr is None else min(14, int(fr * 14 + 0.5))
                hinter.hint(lists_dev["start"][l], lists_dev["nbytes"][l], aux)
        mark("APP q_begin i=%d lists=%s" % (i, ",".join(str(int(x)) for x in lists[0])))
        t1 = time.perf_counter()
        dist, ids = index.search(qi, cfg["k"])
        dt = time.perf_counter() - t1
        digest.update(ids.tobytes()); digest.update(dist.tobytes())
        lat.append(dt)
        mark("APP q_end i=%d dt_us=%d" % (i, int(dt * 1e6)))
        if i == nq // 2:
            snapshot("mid")
        if cfg.get("think_us"):
            time.sleep(cfg["think_us"] / 1e6)
    snapshot("end")
    print("RESULT_SHA256", digest.hexdigest(), flush=True)
    lat = np.array(lat) * 1e3
    print("WORKLOAD_RESULT: PASS nq=%d p50=%.3fms p99=%.3fms mean=%.3fms total=%.1fs" % (
        nq, np.percentile(lat, 50), np.percentile(lat, 99), lat.mean(), time.time() - t0), flush=True)
    json.dump({"lat_ms": lat.tolist()}, open("/tmp/out/query_latency.json", "w"))


main()
