"""Guest-side tracer + workload driver (runs as PID 1's child, no packages needed).

Collects the guest signals of offline/SIGNALS.md using only the kernel's
tracefs (no bpftrace/perf in the image):

  instance `blk`  (global, every task): block-layer issue/complete/merge, page-cache add/delete,
                  readahead-window kprobes                        -> /out/trace_blk.txt
  instance `app`  (only the workload's pid + children): syscalls, user page faults (VA + PC),
                  sched switch/wakeup (blocking time)             -> /out/trace_app.txt
  instance `stk`  (workload pid): the file-I/O syscalls again, with the user call stack
                                                                  -> /out/trace_stk.txt
  sampler thread: /proc/meminfo, /proc/vmstat, PSI at 10 Hz       -> /out/samples.csv

All instances use trace_clock=mono, so every line carries CLOCK_MONOTONIC.
Clock-sync probes (O_DIRECT reads of reserved raw sectors beyond the
filesystem, which nbd_server sees at a known offset) are issued before and
after the workload so tools/capture/analyze.py can align guest and host clocks.
"""
import json
import mmap
import os
import subprocess
import sys
import threading
import time

T = "/sys/kernel/tracing"
OUT = "/tmp/out"
cfg = json.load(open("/tmp/tools/run_config.json"))
log = open(f"{OUT}/tracer.log", "w", buffering=1)


def w(path, val):
    try:
        with open(path, "w") as f:
            f.write(val)
        return True
    except OSError as e:
        log.write(f"FAIL write {val!r} -> {path}: {e}\n")
        return False


def mono_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def enable(inst, event):
    return w(f"{T}/{inst}events/{event}/enable", "1")


def setup_instance(name):
    d = f"{T}/instances/{name}"
    os.makedirs(d, exist_ok=True)
    w(f"{d}/tracing_on", "0")
    w(f"{d}/trace_clock", "mono")
    w(f"{d}/buffer_size_kb", str(cfg.get("buffer_kb", 65536)))
    return d + "/"


def kprobe(name, spec):
    return w(f"{T}/kprobe_events", f"p:{name} {spec}\n") if False else _append(f"{T}/kprobe_events", f"p:{name} {spec}")


def _append(path, line):
    try:
        fd = os.open(path, os.O_WRONLY | os.O_APPEND)
        try:
            os.write(fd, (line + "\n").encode())
        finally:
            os.close(fd)
        return True
    except OSError as e:
        log.write(f"FAIL append {line!r}: {e}\n")
        try:
            log.write("  tracefs error_log: " + open(f"{T}/error_log").read() + "\n")
        except OSError:
            pass
        return False


class Pump(threading.Thread):
    """Drain an instance's trace_pipe to a file on the writable 9p share."""
    def __init__(self, inst, outname):
        super().__init__(daemon=True)
        self.path = f"{T}/instances/{inst}/trace_pipe"
        self.out = open(f"{OUT}/{outname}", "wb", buffering=0)
        self.stop = False
        self.bytes = 0

    def run(self):
        fd = os.open(self.path, os.O_RDONLY | os.O_NONBLOCK)
        buf = bytearray()
        last = time.time()
        while True:
            try:
                chunk = os.read(fd, 1 << 20)
            except BlockingIOError:
                chunk = None
            except OSError:
                chunk = b""
            if chunk:
                buf += chunk
                self.bytes += len(chunk)
            if len(buf) >= (1 << 20) or (buf and time.time() - last > 0.5):
                self.out.write(buf)
                buf.clear()
                last = time.time()
            if not chunk:
                if self.stop:
                    break
                time.sleep(0.02)
        if buf:
            self.out.write(buf)
        self.out.close()


def sampler(stop_evt):
    f = open(f"{OUT}/samples.csv", "w", buffering=1)
    f.write("mono_ns,mem_free_kb,cached_kb,active_file_kb,inactive_file_kb,pgmajfault,pgfault,io_some_avg10,mem_some_avg10,pgpgin\n")
    while not stop_evt.is_set():
        try:
            mi = {}
            for line in open("/proc/meminfo"):
                k, v = line.split(":", 1)
                mi[k] = int(v.split()[0])
            vm = {}
            for line in open("/proc/vmstat"):
                k, v = line.split()
                vm[k] = int(v)
            def psi(p):
                try:
                    return open(p).readline().split("avg10=")[1].split()[0]
                except Exception:
                    return ""
            f.write("%d,%d,%d,%d,%d,%d,%d,%s,%s,%d\n" % (
                mono_ns(), mi["MemFree"], mi["Cached"], mi.get("Active(file)", 0), mi.get("Inactive(file)", 0),
                vm.get("pgmajfault", 0), vm.get("pgfault", 0), psi("/proc/pressure/io"), psi("/proc/pressure/memory"),
                vm.get("pgpgin", 0)))
        except Exception as e:
            log.write(f"sampler: {e}\n")
        stop_evt.wait(0.1)


def sync_probes(tag, n=8):
    """O_DIRECT reads of reserved raw sectors; returns [(probe_id, guest_mono_ns)]."""
    tail = cfg["sync_tail_offset"]
    fd = os.open("/dev/vdb", os.O_RDONLY | os.O_DIRECT)
    buf = mmap.mmap(-1, 4096)
    res = []
    for k in range(n):
        off = tail + (cfg["sync_base"] + k) * 4096
        t0 = mono_ns()
        os.preadv(fd, [buf], off)
        t1 = mono_ns()
        res.append((tag, k, off, t0, t1))
        time.sleep(0.01)
    os.close(fd)
    return res


def launch_workload(env=None):
    """Start the workload with its stdin as a start barrier (it must not do I/O before its pid is in the
    tracing filters); returns the Popen. Whatever it is exec's in place, so proc.pid is the traced pid."""
    if cfg.get("workload", "faiss") == "ycsb":
        y = cfg["ycsb"]
        cp = ":".join(["/opt/ycsb/jdbc-binding/conf", "/opt/ycsb/conf", "/opt/ycsb/lib/*", "/opt/ycsb/jdbc-binding/lib/*"])
        cmd = ["java", "-Xmx256m", "-Djava.io.tmpdir=/tmp", "-cp", cp, "site.ycsb.Client", "-t",
               "-db", "site.ycsb.db.JdbcDBClient", "-P", "/opt/ycsb/workloads/" + y["workload"],
               "-p", "db.driver=org.sqlite.JDBC", "-p", "db.url=jdbc:sqlite:/tmp/data/ycsb.db?open_mode=1",
               "-p", f"recordcount={y['records']}", "-p", f"operationcount={y['ops']}",
               "-p", f"fieldcount={y['fields']}", "-p", f"fieldlength={y['field_length']}",
               "-p", f"requestdistribution={y['distribution']}", "-threads", "1", "-s"]
        # `read` blocks until the barrier byte; exec keeps the pid
        return subprocess.Popen(["/bin/sh", "-c", 'read x; cd /opt/ycsb; exec "$@"', "sh"] + cmd,
                                stdin=subprocess.PIPE, env=env, stdout=open(f"{OUT}/workload.out", "w"), stderr=subprocess.STDOUT)
    return subprocess.Popen(["python3", "/tmp/tools/faiss_ondisk.py"], stdin=subprocess.PIPE, env=env)


def main():
    info = {"cmdline": open("/proc/cmdline").read(), "uname": os.uname()._asdict() if hasattr(os.uname(), "_asdict") else list(os.uname())}
    info["ra_kb"] = {d: open(f"/sys/block/{d}/queue/read_ahead_kb").read().strip() for d in os.listdir("/sys/block") if d.startswith("vd")}
    info["queue"] = {d: {k: open(f"/sys/block/{d}/queue/{k}").read().strip() for k in ("max_sectors_kb", "nr_requests", "scheduler", "rotational")} for d in os.listdir("/sys/block") if d.startswith("vd")}
    info["devs"] = {d: open(f"/sys/block/{d}/dev").read().strip() for d in os.listdir("/sys/block") if d.startswith("vd")}
    try:
        info["avail_events"] = len(open(f"{T}/available_events").read().split())
    except OSError:
        info["avail_events"] = -1
    try:
        syms = [l for l in open("/proc/kallsyms") if any(k in l for k in ("page_cache_sync_ra", "page_cache_async_ra", "filemap_map_pages", "page_cache_ra_order", "do_sync_mmap_readahead"))]
        info["kallsyms"] = [l.strip() for l in syms]
    except OSError as e:
        info["kallsyms_err"] = str(e)
    json.dump(info, open(f"{OUT}/guest_info.json", "w"))

    if cfg.get("probe_debug"):
        for i, spec in enumerate(["p:dbg0 page_cache_sync_ra", "p:dbg1 page_cache_sync_ra a=%di:u64", "p:dbg2 page_cache_sync_ra a=+24(%di):u64",
                                  "p:dbg3 page_cache_sync_ra a=+0(+16(%di)):u64", "p:dbg4 page_cache_sync_ra a=+24(%di):u32", "p:dbg5 page_cache_sync_ra a=+24(%di)",
                                  "p:dbg6 page_cache_sync_ra a=+24(%di):x64", "p:dbg7 filemap_map_pages a=%si:u64", "p:dbg8 filemap_map_pages a=%si"]):
            log.write(f"dbg {spec}: {_append(f'{T}/kprobe_events', spec)}\n")
        w(f"{T}/kprobe_events", "")
        for cmd in ("echo 'p:dbgs0 page_cache_sync_ra' >> /sys/kernel/tracing/kprobe_events",
                    "echo 'p:dbgs1 page_cache_sync_ra a=%di:u64' >> /sys/kernel/tracing/kprobe_events",
                    "echo 'p:dbgs2 page_cache_sync_ra a=+24(%di):u64' >> /sys/kernel/tracing/kprobe_events",
                    "echo 'p:dbgs3 vfs_read' >> /sys/kernel/tracing/kprobe_events",
                    "cat /sys/kernel/tracing/kprobe_events; ls /sys/kernel/tracing | head -80 | tr '\\n' ' '; cat /sys/kernel/tracing/error_log",
                    "echo > /sys/kernel/tracing/kprobe_events"):
            r = subprocess.run(["sh", "-c", cmd], capture_output=True, text=True)
            log.write(f"sh: {cmd[:60]} -> rc={r.returncode} {r.stdout[:600]!r} {r.stderr[:300]!r}\n")
    if not cfg.get("trace", True):
        # host-only run (policy sweeps): no tracefs at all, just sync probes + workload
        probes = sync_probes("pre")
        proc = launch_workload()
        proc.stdin.write(b"g\n")
        proc.stdin.flush()
        rc = proc.wait()
        probes += sync_probes("post")
        json.dump({"probes": probes, "rc": rc}, open(f"{OUT}/sync.json", "w"))
        w("/proc/sys/kernel/sysrq", "1")
        w("/proc/sysrq-trigger", "o")
        time.sleep(5)
        return
    # ---- instances -------------------------------------------------------
    blk = setup_instance("blk")
    app = setup_instance("app")
    stk = setup_instance("stk")
    for ev in ("block/block_rq_issue", "block/block_rq_complete", "block/block_bio_queue",
               "block/block_bio_backmerge", "block/block_bio_frontmerge",
               "filemap/mm_filemap_add_to_page_cache", "filemap/mm_filemap_delete_from_page_cache"):
        enable("instances/blk/", ev)
    # only the data disk (vdb = 254:16): boot-time library reads from vda would otherwise dominate the trace
    VDB = (254 << 20) | 16
    for ev in ("block/block_rq_issue", "block/block_rq_complete", "block/block_bio_queue",
               "block/block_bio_backmerge", "block/block_bio_frontmerge"):
        w(f"{T}/instances/blk/events/{ev}/filter", f"dev == {VDB}")
    for ev in ("filemap/mm_filemap_add_to_page_cache", "filemap/mm_filemap_delete_from_page_cache"):
        w(f"{T}/instances/blk/events/{ev}/filter", f"s_dev == {VDB}")
    # readahead window state (struct readahead_control / file_ra_state offsets are stable across 6.x):
    #   ractl: file@0 mapping@8 ra@16 _index@24 _nr_pages@32 ; ra: start@0 size@8 async_size@12 ra_pages@16 mmap_miss@20
    ra_fields = ("ra_index=+24(%di):u64 ra_nr=+32(%di):u32 ra_start=+0(+16(%di)):u64 ra_size=+8(+16(%di)):u32 "
                 "ra_async=+12(+16(%di)):u32 ra_pages=+16(+16(%di)):u32 mmap_miss=+20(+16(%di)):u32")
    if kprobe("ra_sync", f"page_cache_sync_ra {ra_fields} req=%si:u64"):
        enable("instances/blk/", "kprobes/ra_sync")
    if kprobe("ra_async", f"page_cache_async_ra {ra_fields} req=%dx:u64"):
        enable("instances/blk/", "kprobes/ra_async")
    # reads that were satisfied from the page cache never reach the block layer: count them per mapping via fault-around
    # (filemap_map_pages) -- start/end pgoff of the mapped window; vmf->pgoff@16 for the faulting page
    if kprobe("fault_around", "filemap_map_pages vm_pgoff=+16(%di):u64 start_pgoff=%si:u64 end_pgoff=%dx:u64"):
        enable("instances/blk/", "kprobes/fault_around")

    for ev in ("exceptions/page_fault_user", "sched/sched_switch", "sched/sched_wakeup",
               "raw_syscalls/sys_enter", "raw_syscalls/sys_exit"):
        enable("instances/app/", ev)
    # file-I/O syscalls only: read 0, pread64 17, readv 19, preadv 295, mmap 9, munmap 11, openat 257, close 3, madvise 28, fadvise64 221, lseek 8
    ids = (0, 17, 19, 295, 9, 11, 257, 3, 28, 221, 8)
    flt = " || ".join(f"id == {i}" for i in ids)
    w(f"{T}/instances/app/events/raw_syscalls/sys_enter/filter", flt)
    w(f"{T}/instances/app/events/raw_syscalls/sys_exit/filter", flt)
    enable("instances/stk/", "raw_syscalls/sys_enter")
    w(f"{T}/instances/stk/events/raw_syscalls/sys_enter/filter", flt)
    w(f"{stk}options/userstacktrace", "1")
    w(f"{stk}options/sym-userobj", "1")
    for d in (app, stk):
        w(f"{d}options/event-fork", "1")

    pumps = [Pump("blk", "trace_blk.txt"), Pump("app", "trace_app.txt"), Pump("stk", "trace_stk.txt")]
    for p in pumps:
        p.start()
    stop_evt = threading.Event()
    samp = threading.Thread(target=sampler, args=(stop_evt,), daemon=True)
    samp.start()
    for d in (blk, app, stk):
        w(f"{d}tracing_on", "1")
    # the tracer's own writes (9p) must not pollute the app instance
    for d in (app, stk):
        w(f"{d}set_event_pid", "99999")   # placeholder (matches nothing); replaced by the workload pid below

    # ---- sync probes, workload, sync probes ------------------------------
    probes = sync_probes("pre")
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    proc = launch_workload(env)
    for d in (app, stk):
        w(f"{d}set_event_pid", str(proc.pid))
    proc.stdin.write(b"g\n")
    proc.stdin.flush()
    t_start = mono_ns()
    rc = proc.wait()
    t_end = mono_ns()
    probes += sync_probes("post")
    time.sleep(0.5)

    for d in (blk, app, stk):
        w(f"{d}tracing_on", "0")
    time.sleep(0.3)
    for p in pumps:
        p.stop = True
    for p in pumps:
        p.join(timeout=30)
    stop_evt.set()
    # lost events? (overrun counters)
    lost = {}
    for inst in ("blk", "app", "stk"):
        for cpu in sorted(os.listdir(f"{T}/instances/{inst}/per_cpu")):
            try:
                lost[f"{inst}/{cpu}"] = open(f"{T}/instances/{inst}/per_cpu/{cpu}/stats").read()
            except OSError:
                pass
    json.dump({"probes": probes, "workload_pid": proc.pid, "rc": rc, "t_start": t_start, "t_end": t_end,
               "pump_bytes": {p.path: p.bytes for p in pumps}, "cpu0_stats": lost}, open(f"{OUT}/sync.json", "w"))
    log.write("done\n")
    log.flush()
    os.sync() if False else None
    # power off (init=/bin/sh: no systemd to do it for us)
    w("/proc/sys/kernel/sysrq", "1")
    w("/proc/sysrq-trigger", "o")
    time.sleep(5)


try:
    main()
except Exception as e:
    import traceback
    log.write(traceback.format_exc())
    log.flush()
    w("/proc/sysrq-trigger", "o")
    time.sleep(5)
