#!/usr/bin/env python3
"""Hypervisor-side sampler for a running capture VM (no root, no guest agent).

Two modes, run as separate processes by run_capture.py --hv:

  qmp   ~100 Hz over the QMP socket:
          hv_regs.csv   t_ns,cpu,rip,cr3,cpl,hlt          (`info registers -a`: vCPU state)
          hv_kvm.csv    t_ns,target,<KVM binary stats>    (`query-stats`: exits, faults, halts, ... per vCPU and VM)
          hv_sched.csv  t_ns,cpu,tid,run_ns,wait_ns,slices (/proc/<qemu>/task/<vcpu tid>/schedstat)
  scan  ~1 Hz over guest RAM (QEMU started with a file-backed memory-backend in /dev/shm):
          hv_frames.csv t_ns,frame,block                  (t_ns = scan end) every guest 4 KiB frame whose content changed since the last scan,
                                                          and the disk block it now holds ((disk << 40) | block; -1 none, -2 ambiguous)
        i.e. a host-side reconstruction of the guest page cache's contents (which disk blocks are in guest RAM).

All timestamps are CLOCK_MONOTONIC ns, the clock nbd_server's features.csv uses (arrive_ns).
"""
import argparse, json, os, re, socket, sys, time
import numpy as np

mono = lambda: time.clock_gettime_ns(time.CLOCK_MONOTONIC)
FRAME = 4096
PREFIX = 64        # uint64 words hashed per frame (first 512 bytes: enough to name a disk block, 8x less memory traffic)
_W = np.random.default_rng(0x5eed).integers(1, 2**63, size=PREFIX, dtype=np.uint64) | np.uint64(1)


def hash_frames(buf):
    """64-bit hash of the first 512 bytes of each 4 KiB frame (multiply-add over uint64 words, wraps).
    buf: bytes, or a uint64 array of shape (n, 512) (e.g. an mmap of guest RAM)."""
    a = buf if isinstance(buf, np.ndarray) else np.frombuffer(buf, dtype=np.uint64).reshape(-1, FRAME // 8)
    with np.errstate(over="ignore"):
        return (a[:, :PREFIX] * _W).sum(axis=1, dtype=np.uint64)


def _data_extents(fd, size):
    """allocated [start, end) byte ranges of a (possibly sparse) file, via SEEK_DATA / SEEK_HOLE"""
    out, pos = [], 0
    while pos < size:
        try:
            d = os.lseek(fd, pos, os.SEEK_DATA)
        except OSError:
            break
        h = os.lseek(fd, d, os.SEEK_HOLE)
        out.append((d, h)); pos = h
    return out


def disk_index(img, cache_dir, disk_id=0):
    """sorted block hashes of a disk image + the block of each (disk_id << 40 | 4 KiB block number); blocks whose
    content occurs more than once (zero / repeated blocks) are marked -2.  Only allocated extents are read."""
    st = os.stat(img)
    f = os.path.join(cache_dir, f"{os.path.basename(img)}.{st.st_size}.{int(st.st_mtime)}.p{PREFIX}.blkhash.npz")
    if os.path.exists(f):
        z = np.load(f)
        h, b = z["h"], z["b"]
    else:
        os.makedirs(cache_dir, exist_ok=True)
        hs, bl = [], []
        fd = os.open(img, os.O_RDONLY)
        step = 16384
        for s0, s1 in _data_extents(fd, st.st_size):
            s0 -= s0 % FRAME
            for o in range(s0, s1, step * FRAME):
                buf = os.pread(fd, min(step * FRAME, s1 - o), o)
                buf = buf[:len(buf) - len(buf) % FRAME]
                if not buf: continue
                hs.append(hash_frames(buf)); bl.append(np.arange(o // FRAME, o // FRAME + len(buf) // FRAME, dtype=np.int64))
        os.close(fd)
        hs = np.concatenate(hs); bl = np.concatenate(bl)
        order = np.argsort(hs, kind="stable")
        h = hs[order]; b = bl[order]
        dup = np.zeros(len(h), bool); dup[1:] |= h[1:] == h[:-1]; dup[:-1] |= h[1:] == h[:-1]
        b[dup] = -2
        keep = np.ones(len(h), bool); keep[1:] = h[1:] != h[:-1]
        h, b = h[keep], b[keep]
        np.savez(f, h=h, b=b)
    b = np.where(b >= 0, b | (np.int64(disk_id) << 40), b)
    return h, b


def multi_index(disks, cache_dir):
    """one lookup table over several disks (block ids carry the disk index in bits 40+); a hash present on more
    than one disk is ambiguous (-2)"""
    hs, bs = zip(*[disk_index(d, cache_dir, i) for i, d in enumerate(disks)])
    h = np.concatenate(hs); b = np.concatenate(bs)
    order = np.argsort(h, kind="stable"); h = h[order]; b = b[order]
    dup = np.zeros(len(h), bool); dup[1:] |= h[1:] == h[:-1]; dup[:-1] |= h[1:] == h[:-1]
    b = np.where(dup, -2, b)
    keep = np.ones(len(h), bool); keep[1:] = h[1:] != h[:-1]
    return h[keep], b[keep]


def lookup(h_sorted, b_sorted, q):
    i = np.searchsorted(h_sorted, q).clip(0, len(h_sorted) - 1)
    return np.where(h_sorted[i] == q, b_sorted[i], -1)


def run_scan(a):
    hs, bs = multi_index(a.disk, a.cache_dir)
    zero_h = hash_frames(bytes(FRAME))[0]
    while not os.path.exists(a.ram):
        time.sleep(0.05)
    while os.path.getsize(a.ram) == 0:
        time.sleep(0.05)
    ram = np.memmap(a.ram, dtype=np.uint64, mode="r").reshape(-1, FRAME // 8)
    nfr = ram.shape[0]
    prev = np.zeros(nfr, dtype=np.uint64)
    out = open(os.path.join(a.out, "hv_frames.csv"), "w")
    out.write("t_ns,frame,block\n")
    meta = open(os.path.join(a.out, "hv_scan.csv"), "w")
    meta.write("t0_ns,t1_ns,changed,disk_frames\n")
    period = 1.0 / a.scan_hz
    step = 32768
    while a.pid == 0 or os.path.exists(f"/proc/{a.pid}"):
        t0 = mono()
        cur = np.empty(nfr, dtype=np.uint64)
        for i in range(0, nfr, step):
            k = min(step, nfr - i)
            cur[i:i + k] = hash_frames(ram[i:i + k])
        t1 = mono()
        ch = np.nonzero(cur != prev)[0]
        blk = np.where(cur[ch] == zero_h, -1, lookup(hs, bs, cur[ch]))
        tm = t1                                   # a change is only certain once the scan has finished
        out.write("".join(f"{tm},{f},{b}\n" for f, b in zip(ch.tolist(), blk.tolist())))
        out.flush()
        prev = cur
        nd = int(np.count_nonzero(lookup(hs, bs, cur) >= 0))
        meta.write(f"{t0},{t1},{len(ch)},{nd}\n"); meta.flush()
        time.sleep(max(0.0, period - (mono() - t0) / 1e9))


class Qmp:
    def __init__(self, path):
        for _ in range(400):
            try:
                self.s = socket.socket(socket.AF_UNIX); self.s.connect(path); break
            except OSError:
                time.sleep(0.05)
        else:
            sys.exit(f"no QMP socket at {path}")
        self.f = self.s.makefile("rwb")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}).encode() + b"\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r: return r["return"]
            if "error" in r: raise RuntimeError(r["error"])
            # asynchronous events are skipped


RE_CPU = re.compile(r"^CPU#(\d+)", re.M)
RE_RIP = re.compile(r"RIP=([0-9a-f]+)")
RE_CR3 = re.compile(r"CR3=([0-9a-f]+)")
RE_CPL = re.compile(r"CPL=(\d)")
RE_HLT = re.compile(r"HLT=(\d)")


def run_qmp(a):
    q = Qmp(a.qmp)
    cpus = q.cmd("query-cpus-fast")
    tids = {c["cpu-index"]: c["thread-id"] for c in cpus}
    regs = open(os.path.join(a.out, "hv_regs.csv"), "w"); regs.write("t_ns,cpu,rip,cr3,cpl,hlt\n")
    sched = open(os.path.join(a.out, "hv_sched.csv"), "w"); sched.write("t_ns,cpu,tid,run_ns,wait_ns,slices\n")
    kvm = open(os.path.join(a.out, "hv_kvm.csv"), "w")
    names = None
    period = 1.0 / a.hz
    n = 0
    while True:
        t0 = mono()
        try:
            txt = q.cmd("human-monitor-command", **{"command-line": "info registers -a"})
            t1 = mono()
            kv = q.cmd("query-stats", target="vcpu") + q.cmd("query-stats", target="vm")
        except (OSError, ValueError, json.JSONDecodeError):
            break                                   # QEMU exited
        tm = (t0 + t1) // 2
        parts = RE_CPU.split(txt)                   # ['', '0', body0, '1', body1, ...]
        for i in range(1, len(parts) - 1, 2):
            body = parts[i + 1]
            rip, cr3, cpl, hlt = RE_RIP.search(body), RE_CR3.search(body), RE_CPL.search(body), RE_HLT.search(body)
            regs.write(f"{tm},{parts[i]},{int(rip.group(1), 16) if rip else -1},{int(cr3.group(1), 16) if cr3 else -1},"
                       f"{cpl.group(1) if cpl else -1},{hlt.group(1) if hlt else -1}\n")
        for c, tid in tids.items():
            try:
                r, w, s = open(f"/proc/{a.pid}/task/{tid}/schedstat").read().split()
                sched.write(f"{tm},{c},{tid},{r},{w},{s}\n")
            except OSError:
                pass
        rows = []
        nv = 0
        for ent in kv:                              # vCPU entries (they carry a qom-path) come first, in cpu order
            if "qom-path" in ent: tgt = f"cpu{nv}"; nv += 1
            else: tgt = "vm"
            vals = {s["name"]: s["value"] for s in ent["stats"] if isinstance(s["value"], (int, bool))}
            rows.append((tgt, vals))
        if names is None:
            names = sorted({k for _, v in rows for k in v})
            kvm.write("t_ns,target," + ",".join(names) + "\n")
        for tgt, v in rows:
            kvm.write(f"{tm},{tgt}," + ",".join(str(int(v.get(k, 0))) for k in names) + "\n")
        n += 1
        if n % 50 == 0:
            for fh in (regs, sched, kvm): fh.flush()
        time.sleep(max(0.0, period - (mono() - t0) / 1e9))
    for fh in (regs, sched, kvm): fh.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["qmp", "scan", "index"])
    ap.add_argument("--out", default=".")
    ap.add_argument("--pid", type=int, default=0, help="QEMU pid (stop when it exits)")
    ap.add_argument("--qmp", default="")
    ap.add_argument("--hz", type=float, default=100)
    ap.add_argument("--ram", default="", help="file backing guest RAM")
    ap.add_argument("--scan-hz", type=float, default=1)
    ap.add_argument("--disk", action="append", default=[], help="disk image(s) to name the blocks found in guest RAM; block ids "
                    "are (disk index << 40) | 4 KiB block, in --disk order")
    ap.add_argument("--cache-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../logs/hv_cache"))
    a = ap.parse_args()
    if a.mode == "qmp": run_qmp(a)
    elif a.mode == "scan": run_scan(a)
    else:
        t = time.time(); h, _ = multi_index(a.disk, a.cache_dir); print(f"{len(h)} distinct block hashes in {time.time() - t:.1f}s")


if __name__ == "__main__":
    main()
