"""Guest workload: online nearest-neighbour search against a disk-resident FAISS IVF index.

The index's inverted lists live in /data/index.ivfdata and are mmap'd by
faiss, so every search touches `nprobe` contiguous list extents (one page
fault stream per list) -- the access pattern the host-side prefetcher sees as
NBD reads.  Queries come in topical sessions (see tools/capture/build_faiss_disk.py).

Application-level ground truth (query id + the inverted lists it is about to
read, i.e. signal #50, "app phase marker") is written to ftrace's
trace_marker so it lands on the same clock as the kernel events.
"""
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
    lat = []
    for i in range(nq):
        qi = q[i:i + 1]
        _, lists = index.quantizer.search(qi, cfg["nprobe"])     # which lists this query will read
        mark("APP q_begin i=%d lists=%s" % (i, ",".join(str(int(x)) for x in lists[0])))
        t1 = time.perf_counter()
        index.search(qi, cfg["k"])
        dt = time.perf_counter() - t1
        lat.append(dt)
        mark("APP q_end i=%d dt_us=%d" % (i, int(dt * 1e6)))
        if i == nq // 2:
            snapshot("mid")
        if cfg.get("think_us"):
            time.sleep(cfg["think_us"] / 1e6)
    snapshot("end")
    lat = np.array(lat) * 1e3
    print("WORKLOAD_RESULT: PASS nq=%d p50=%.3fms p99=%.3fms mean=%.3fms total=%.1fs" % (
        nq, np.percentile(lat, 50), np.percentile(lat, 99), lat.mean(), time.time() - t0), flush=True)
    json.dump({"lat_ms": lat.tolist()}, open("/tmp/out/query_latency.json", "w"))


main()
