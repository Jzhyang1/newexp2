#!/usr/bin/python3
"""Root helper: which guest-RAM frames the guest *touched* (read or wrote) since the last tick.

The host never sees guest page-cache hits; their accessed bits are the only trace they leave.  This reads them with the
kernel's idle page tracking (Documentation/admin-guide/mm/idle_page_tracking.rst): mark every page backing guest RAM
idle, wait, and see which lost the idle flag.  For KVM guests the kernel's rmap walk also clears/tests the EPT accessed
bits through the MMU notifier, so guest accesses count.

  hv_idle --pid QEMU_PID --ram /dev/shm/hv-....ram --out RUN_DIR/hv_access.bin [--hz 10]

Needs root (PFNs in /proc/PID/pagemap, /sys/kernel/mm/page_idle/bitmap).  Pure stdlib so it runs under the system
python3; install it root-owned (not user-writable) before allowing it in sudoers, e.g.
    sudo install -o root -g root -m 755 tools/capture/hv_idle.py /usr/local/sbin/hv_idle
Output (binary, little endian), one record per tick:  u64 t_ns (CLOCK_MONOTONIC, end of the check), u32 n,
u32 frames[n] (guest frame = byte offset in the RAM file / 4096) that were accessed since the previous tick.
Stops when QEMU exits.  Writes nothing outside --out.
"""
import argparse, array, os, struct, sys, time

BITMAP = "/sys/kernel/mm/page_idle/bitmap"
PFN_MASK = (1 << 55) - 1


def find_vma(pid, ram):
    target = os.path.realpath(ram)
    with open(f"/proc/{pid}/maps") as f:
        for ln in f:
            parts = ln.split(maxsplit=5)
            if len(parts) == 6 and os.path.realpath(parts[5].strip().replace(" (deleted)", "")) == target:
                lo, hi = (int(x, 16) for x in parts[0].split("-"))
                return lo, hi, int(parts[2], 16)
    return None


def frame_pfns(pid, lo, hi):
    """pfn of every present page in [lo, hi): array index = page index in the VMA (0 = not present)"""
    n = (hi - lo) // 4096
    fd = os.open(f"/proc/{pid}/pagemap", os.O_RDONLY)
    try:
        raw = os.pread(fd, n * 8, (lo // 4096) * 8)
    finally:
        os.close(fd)
    ents = array.array("Q"); ents.frombytes(raw)
    return [(e & PFN_MASK) if (e >> 63) & 1 else 0 for e in ents]


def runs(words):
    """sorted unique word indices -> [(first, count)] contiguous runs"""
    out = []
    for w in words:
        if out and out[-1][0] + out[-1][1] == w: out[-1][1] += 1
        else: out.append([w, 1])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--ram", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--hz", type=float, default=10)
    ap.add_argument("--wait", type=float, default=60, help="seconds to wait for QEMU to map the RAM file")
    a = ap.parse_args()
    t_end = time.time() + a.wait
    vma = None
    while vma is None and time.time() < t_end and os.path.exists(f"/proc/{a.pid}"):
        vma = find_vma(a.pid, a.ram)
        if vma is None: time.sleep(0.05)
    if vma is None:
        sys.exit("guest RAM mapping not found")
    lo, hi, file_off = vma
    bm = os.open(BITMAP, os.O_RDWR)
    out = open(a.out, "wb")
    period = 1.0 / a.hz
    first = True
    while os.path.exists(f"/proc/{a.pid}"):
        t0 = time.monotonic()
        try:
            pfns = frame_pfns(a.pid, lo, hi)
        except OSError:
            break
        words = sorted({p >> 6 for p in pfns if p})
        rs = runs(words)
        # 1. read: a present page whose idle bit is clear was accessed since we set it (or is new)
        idle = {}
        for w0, cnt in rs:
            b = os.pread(bm, cnt * 8, w0 * 8)
            v = array.array("Q"); v.frombytes(b)
            for i, x in enumerate(v): idle[w0 + i] = x
        if not first:
            acc = array.array("I", (fi + file_off // 4096 for fi, p in enumerate(pfns)
                                    if p and not (idle.get(p >> 6, 0) >> (p & 63)) & 1))
            out.write(struct.pack("<QI", time.clock_gettime_ns(time.CLOCK_MONOTONIC), len(acc)))
            out.write(acc.tobytes()); out.flush()
        first = False
        # 2. mark every page of guest RAM idle again (only our words; the kernel ignores pages it cannot track)
        for w0, cnt in rs:
            os.pwrite(bm, b"\xff" * (cnt * 8), w0 * 8)
        time.sleep(max(0.0, period - (time.monotonic() - t0)))
    out.close()


if __name__ == "__main__":
    main()
