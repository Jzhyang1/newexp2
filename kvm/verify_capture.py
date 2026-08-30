#!/usr/bin/env python3
"""End-to-end verifier: did nbd_server correctly capture real VM disk I/O?

Cross-checks kvm/guest/workloads/known_pattern.py's *known* read pattern
(reconstructed from the guest's serial console output) against nbd_server's
access_log (see disk/nbd.example.yaml's `access_log` key) for /dev/vdb's
worker_id. The guest never touches /dev/vdb for anything else -- unlike
/dev/vda, which the kernel/systemd generate lots of read traffic on just
booting -- so this is an exact, ordered comparison of every event, not a
fuzzy "did some traffic happen" check.

This is the real-VM complement to disk/test_protocol.py's fast/deterministic
protocol-level test: it proves genuine QEMU virtio-blk-over-NBD traffic is
captured correctly, not just a hand-rolled client talking the wire protocol.

One-time setup -- bakes a second, dedicated guest image (kept separate from
DISK_IMG/GUEST_KERNEL/GUEST_INITRD's normal faiss_bench.py defaults so this
doesn't clobber your regular `make guest-image`):

    make guest-image DISK_IMG=disk/data/capture_test_disk.img \\
        GUEST_KERNEL=disk/data/capture_test_vmlinuz \\
        GUEST_INITRD=disk/data/capture_test_initrd.img \\
        WORKLOAD_SCRIPT=kvm/guest/workloads/known_pattern.py

Run:
    make nbd
    bin/nbd_server disk/nbd.test.yaml &
    python3 kvm/launch.py kvm/vms.test.yaml
    python3 kvm/launch.py kvm/vms.test.yaml --wait   # blocks until the guest powers off
    python3 kvm/verify_capture.py
    kill %1   # or `make nbd-stop` -- SIGTERM flushes the CSV stats log cleanly

Needs a Linux host (disk/nbd.cpp and kvm/launch.py are Linux-only -- see
CLAUDE.md) with qemu-system-x86_64 on PATH; kvm/vms.test.yaml sets `kvm:
false` so it doesn't require /dev/kvm access, at the cost of slower TCG
emulation.
"""
import argparse
import csv
import re
import sys

SECTOR = 512
READ_RE = re.compile(r"KNOWN_PATTERN READ skip=(\d+) count=(\d+)")


def expected_from_serial(serial_log_path):
    expected = []
    with open(serial_log_path, "r", errors="replace") as f:
        for line in f:
            m = READ_RE.search(line)
            if m:
                skip, count = int(m.group(1)), int(m.group(2))
                expected.append((skip * SECTOR, count * SECTOR))
    return expected


def captured_from_access_log(access_log_path, worker_id):
    with open(access_log_path, newline="") as f:
        rows = list(csv.DictReader(f))
    return [
        (int(r["offset"]), int(r["length"]), int(r["hits"]), int(r["total"]))
        for r in rows
        if r["worker_id"] == str(worker_id)
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial-log", default="/tmp/kvm-launch/vm0.serial.log")
    ap.add_argument("--access-log", default="./logs/nbd_access_test.csv")
    ap.add_argument("--worker-id", type=int, default=1,
                     help="index of /dev/vdb's port in nbd.test.yaml's ports: list")
    args = ap.parse_args()

    expected = expected_from_serial(args.serial_log)
    if not expected:
        sys.exit(
            f"no 'KNOWN_PATTERN READ ...' lines found in {args.serial_log} -- "
            f"did the VM boot the capture-test image and finish the workload? "
            f"(see this script's docstring for setup)"
        )

    captured = captured_from_access_log(args.access_log, args.worker_id)
    captured_ranges = [(o, l) for o, l, _, _ in captured]

    if captured_ranges != expected:
        print("FAIL: nbd_server's captured /dev/vdb events don't match what the guest issued",
              file=sys.stderr)
        print(f"  expected ({len(expected)}): {expected}", file=sys.stderr)
        print(f"  captured ({len(captured_ranges)}): {captured_ranges}", file=sys.stderr)
        sys.exit(1)

    print(f"[ok] {len(expected)} reads issued by the guest on /dev/vdb, all captured by "
          f"nbd_server (worker_id={args.worker_id}) in order with matching offset/length")

    # known_pattern.py's PATTERN ends with an identical repeat -- the second
    # occurrence should come back as a full cache hit.
    if len(captured) >= 2 and captured[-1][:2] == captured[-2][:2]:
        hits, total = captured[-1][2], captured[-1][3]
        if hits == total:
            print(f"[ok] repeated read came back as a full cache hit ({hits}/{total} blocks)")
        else:
            print(f"[warn] repeated read was only a partial hit ({hits}/{total} blocks) -- "
                  f"unexpected unless something else evicted those blocks in between")

    print("\nPASS: real VM disk I/O is being captured correctly.")


if __name__ == "__main__":
    main()
