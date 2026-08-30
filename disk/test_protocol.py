#!/usr/bin/env python3
"""Protocol-level capture-correctness test for nbd_server.

Speaks the NBD wire protocol directly (bypassing QEMU/nbd-client entirely)
against a freshly-built nbd_server, sends a known, deterministic sequence of
reads from two separate "client identities" (two ports, i.e. two worker_ids
-- standing in for two VMs), and checks:

  1. every reply's bytes match the backing file's real content at that
     offset (the server is actually forwarding the right data), and
  2. nbd_server's per-request access_log (see disk/nbd.example.yaml) records
     exactly the requests this script sent -- same worker_id, offset, and
     length, in order, with no cross-contamination between the two workers
     -- and its aggregate CSV stats log (request_count/bytes_read/hits+
     misses) reconciles with that per-request log.

This is the fast, deterministic complement to kvm/verify_capture.py's real-
VM end-to-end check: it proves the wire-protocol/routing/logging path is
correct without needing QEMU, a guest image, or root.

Linux-only, like the rest of this repo (nbd_server needs <endian.h>).

Usage:
    make nbd                      # build bin/nbd_server first
    python3 disk/test_protocol.py [--binary bin/nbd_server]
"""

import argparse
import csv
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

SECTOR_SIZE = 512

NBD_MAGIC = 0x4E42444D41474943
IHAVEOPT = 0x49484156454F5054
OPT_EXPORT_NAME = 1
CLIENT_FLAG_FIXED_NEWSTYLE = 1 << 0
CLIENT_FLAG_NO_ZEROES = 1 << 1

REQUEST_MAGIC = 0x25609513
SIMPLE_REPLY_MAGIC = 0x67446698
CMD_READ = 0
CMD_DISC = 2


def recv_full(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed unexpectedly")
        buf += chunk
    return buf


class NbdClient:
    """A minimal raw NBD client: handshake + NBD_CMD_READ, nothing else."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(5)
        self._handshake()
        self._handle = 0

    def _handshake(self):
        hello = recv_full(self.sock, 8 + 8 + 2)
        magic, opts_magic, flags = struct.unpack(">QQH", hello)
        assert magic == NBD_MAGIC, f"bad server magic: {magic:#x}"
        assert opts_magic == IHAVEOPT, f"bad opts magic: {opts_magic:#x}"

        client_flags = CLIENT_FLAG_FIXED_NEWSTYLE | CLIENT_FLAG_NO_ZEROES
        self.sock.sendall(struct.pack(">I", client_flags))

        # NBD_OPT_EXPORT_NAME with an empty name -- the server ignores the
        # requested name anyway (one export per listening port).
        name = b""
        self.sock.sendall(struct.pack(">QII", IHAVEOPT, OPT_EXPORT_NAME, len(name)) + name)

        info = recv_full(self.sock, 8 + 2)
        (self.size, self.trans_flags) = struct.unpack(">QH", info)
        # client_flags set NO_ZEROES, so no 124-byte pad follows.

    def read(self, offset, length):
        self._handle += 1
        handle = self._handle
        req = struct.pack(">IHHQQI", REQUEST_MAGIC, 0, CMD_READ, handle, offset, length)
        self.sock.sendall(req)

        reply = recv_full(self.sock, 4 + 4 + 8)
        magic, error, rhandle = struct.unpack(">IIQ", reply)
        assert magic == SIMPLE_REPLY_MAGIC, f"bad reply magic: {magic:#x}"
        assert rhandle == handle, f"handle mismatch: sent {handle}, got {rhandle}"
        assert error == 0, f"server returned error {error} for offset={offset} length={length}"
        return recv_full(self.sock, length)

    def close(self):
        try:
            self.sock.sendall(struct.pack(">IHHQQI", REQUEST_MAGIC, 0, CMD_DISC, self._handle, 0, 0))
        except OSError:
            pass
        self.sock.close()


def wait_for_port(host, port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            socket.create_connection((host, port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError(f"nbd_server never opened {host}:{port}")


def free_tcp_ports(n):
    socks = [socket.socket(socket.AF_INET, socket.SOCK_STREAM) for _ in range(n)]
    for s in socks:
        s.bind(("127.0.0.1", 0))
    ports = [s.getsockname()[1] for s in socks]
    for s in socks:
        s.close()
    return ports


def expected_byte(offset):
    """Deterministic backing-file content: byte at `offset` is offset & 0xFF."""
    return offset & 0xFF


def make_backing_file(path, size):
    with open(path, "wb") as f:
        # Written in chunks; expected_byte() lets the test recompute the
        # expected content independently instead of keeping a second copy.
        chunk = bytes(expected_byte(o) for o in range(65536))
        written = 0
        while written < size:
            n = min(len(chunk), size - written)
            f.write(chunk[:n])
            written += n


def gen_pattern(rng, n, max_offset):
    """n deterministic (offset, length) pairs, sector-aligned, within bounds,
    including a couple of intentional repeats to exercise cache hits."""
    pattern = []
    for _ in range(n):
        length = rng.choice([1, 2, 4, 8]) * SECTOR_SIZE
        max_block = (max_offset - length) // SECTOR_SIZE
        offset = rng.randint(0, max_block) * SECTOR_SIZE
        pattern.append((offset, length))
    # Repeat a couple of earlier entries verbatim to check that the second
    # read of the same range is recorded as a cache hit.
    if n >= 4:
        pattern.append(pattern[1])
        pattern.append(pattern[1])
    return pattern


def run_worker(host, port, pattern, results, key):
    client = NbdClient(host, port)
    try:
        got = []
        for offset, length in pattern:
            data = client.read(offset, length)
            expected = bytes(expected_byte(offset + i) for i in range(length))
            if data != expected:
                raise AssertionError(
                    f"content mismatch at offset={offset} length={length}: "
                    f"got {data[:16].hex()}... expected {expected[:16].hex()}..."
                )
            got.append((offset, length))
        results[key] = got
    finally:
        client.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="bin/nbd_server", help="path to the built nbd_server")
    ap.add_argument("--requests-per-worker", type=int, default=12)
    ap.add_argument("--file-size", type=int, default=4 * 1024 * 1024)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()

    if not os.path.isfile(args.binary):
        sys.exit(f"{args.binary} not found -- build it first with `make nbd`")

    workdir = tempfile.mkdtemp(prefix="nbd_protocol_test_")
    server_output = ""
    try:
        backing_path = os.path.join(workdir, "backing.img")
        make_backing_file(backing_path, args.file_size)

        stats_log = os.path.join(workdir, "stats.csv")
        access_log = os.path.join(workdir, "access.csv")
        config_path = os.path.join(workdir, "nbd.yaml")
        port_a, port_b = free_tcp_ports(2)

        with open(config_path, "w") as f:
            f.write(
                f"source: local\n"
                f"file: {backing_path}\n"
                f"ports: [{port_a}, {port_b}]\n"
                f"capacity: 512\n"
                f"hit_latency_ns: 0\n"
                f"miss_latency_ns: 0\n"
                f"warmup: 0\n"
                f"evict_policy: lru\n"
                f"prefetch_policy: none\n"
                f"log: {stats_log}\n"
                f"access_log: {access_log}\n"
            )

        proc = subprocess.Popen([os.path.abspath(args.binary), config_path],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True)
        try:
            wait_for_port("127.0.0.1", port_a)
            wait_for_port("127.0.0.1", port_b)

            rng_a = random.Random(args.seed)
            rng_b = random.Random(args.seed + 1)
            pattern_a = gen_pattern(rng_a, args.requests_per_worker, args.file_size)
            pattern_b = gen_pattern(rng_b, args.requests_per_worker, args.file_size)

            results = {}
            ta = threading.Thread(target=run_worker, args=("127.0.0.1", port_a, pattern_a, results, "a"))
            tb = threading.Thread(target=run_worker, args=("127.0.0.1", port_b, pattern_b, results, "b"))
            ta.start(); tb.start()
            ta.join(); tb.join()

            assert results.get("a") == pattern_a, "worker A: not all reads completed/matched"
            assert results.get("b") == pattern_b, "worker B: not all reads completed/matched"
            print(f"[ok] content check: {len(pattern_a) + len(pattern_b)} reads, "
                  f"all bytes matched the backing file")

            time.sleep(0.2)  # let the last access_log fflush() land
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

        if proc.stdout:
            server_output = proc.stdout.read()
        else:
            server_output = ""

        # --- cross-check the access_log against exactly what was sent ---
        with open(access_log, newline="") as f:
            rows = list(csv.DictReader(f))

        got_a = [(int(r["offset"]), int(r["length"])) for r in rows if r["worker_id"] == "0"]
        got_b = [(int(r["offset"]), int(r["length"])) for r in rows if r["worker_id"] == "1"]
        other = [r for r in rows if r["worker_id"] not in ("0", "1")]

        assert not other, f"access_log has entries for unexpected worker_id(s): {other}"
        assert got_a == pattern_a, (
            f"worker 0 (port {port_a}) access_log mismatch:\n  sent:      {pattern_a}\n  captured:  {got_a}"
        )
        assert got_b == pattern_b, (
            f"worker 1 (port {port_b}) access_log mismatch:\n  sent:      {pattern_b}\n  captured:  {got_b}"
        )
        print(f"[ok] access_log: {len(rows)} events captured, in order, "
              f"correctly attributed to worker_id 0 vs 1 (no cross-contamination)")

        # Repeated range (appended twice in gen_pattern) should show up as a
        # full cache hit the second time -- informational, not a hard
        # requirement of "capture correctness", so just report it.
        if len(pattern_a) >= 3 and pattern_a[-1] == pattern_a[-2]:
            last_row = rows[[i for i, r in enumerate(rows) if r["worker_id"] == "0"][-1]]
            hits, total = int(last_row["hits"]), int(last_row["total"])
            print(f"[info] repeated-range re-read: {hits}/{total} blocks were cache hits")

        # --- cross-check the aggregate stats CSV against the access_log ---
        with open(stats_log, newline="") as f:
            stats_rows = list(csv.DictReader(f))
        assert stats_rows, "stats log has no rows -- did the server shut down cleanly?"
        stats = stats_rows[-1]

        expected_requests = len(pattern_a) + len(pattern_b)
        expected_bytes = sum(l for _, l in pattern_a) + sum(l for _, l in pattern_b)
        expected_blocks = sum(int(r["total"]) for r in rows)

        assert int(stats["request_count"]) == expected_requests, (
            f"request_count: expected {expected_requests}, got {stats['request_count']}"
        )
        assert int(stats["bytes_read"]) == expected_bytes, (
            f"bytes_read: expected {expected_bytes}, got {stats['bytes_read']}"
        )
        assert int(stats["hits"]) + int(stats["misses"]) == expected_blocks, (
            f"hits+misses: expected {expected_blocks} (sum of access_log 'total' column), "
            f"got {int(stats['hits']) + int(stats['misses'])}"
        )
        print(f"[ok] aggregate stats reconcile with access_log: "
              f"request_count={stats['request_count']} bytes_read={stats['bytes_read']} "
              f"hits={stats['hits']} misses={stats['misses']}")

        print("\nPASS: nbd_server captures client/VM read events correctly.")
    except Exception:
        print("\n--- nbd_server output ---", file=sys.stderr)
        print(server_output, file=sys.stderr)
        raise
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
