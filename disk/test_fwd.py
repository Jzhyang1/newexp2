#!/usr/bin/env python3
"""End-to-end check of `source: remote`: NBD client -> nbd_server -> remote disk.

Starts `fwd_sim serve` (the remote disk), then nbd_server once per fwd_mode
(direct, cache with readahead), speaks raw NBD to it and checks that

  1. reads (sequential, random, unaligned, spanning pages, and a re-read that is
     served from the local cache) return the remote file's exact bytes,
  2. a guest write (including a partial-block write that must read-modify-write
     against the *remote* disk) is read back correctly and the remote file is
     left untouched,
  3. in cache mode the sequential reads were actually served ahead of time:
     the server's shutdown stats show prefetched pages used.

Needs no QEMU, root or network. Usage:  make nbd fwd-sim && python3 disk/test_fwd.py
"""
import argparse
import os
import random
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_protocol import NbdClient, free_tcp_ports, wait_for_port, recv_full  # noqa: E402
from test_protocol import REQUEST_MAGIC, SIMPLE_REPLY_MAGIC  # noqa: E402

CMD_WRITE = 1


def nbd_write(client, offset, data):
    client._handle += 1
    client.sock.sendall(struct.pack(">IHHQQI", REQUEST_MAGIC, 0, CMD_WRITE, client._handle, offset, len(data)) + data)
    magic, error, handle = struct.unpack(">IIQ", recv_full(client.sock, 16))
    assert magic == SIMPLE_REPLY_MAGIC and handle == client._handle and error == 0, f"write failed: err={error}"


def make_disk(path, size):
    # Non-repeating-per-block content so a wrong page/offset can't alias.
    rng = random.Random(7)
    with open(path, "wb") as f:
        for _ in range(size // 65536):
            f.write(rng.randbytes(65536))


def run_mode(args, mode, remote_port, disk_bytes, conc=1):
    port = free_tcp_ports(1)[0]
    work = tempfile.mkdtemp(prefix="fwd_test_")
    cfg = os.path.join(work, "nbd.yaml")
    with open(cfg, "w") as f:
        f.write(f"""source: remote
remote_host: 127.0.0.1
remote_port: {remote_port}
fwd_mode: {mode}
ports: [{port}]
page_size: 16384
capacity: 256
evict_policy: lru
prefetch_policy: readahead
readahead_pages: 8
read_concurrency: {conc}
overlay_dir: {work}
overlay_block_size: 4096
log: {work}/results.csv
access_log: {work}/access.csv
prefetch_log: {work}/prefetch.csv
vm_log_dir: {work}
""")
    srv = subprocess.Popen([args.nbd, cfg], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        wait_for_port("127.0.0.1", port)
        c = NbdClient("127.0.0.1", port)
        assert c.size == len(disk_bytes), f"export size {c.size} != remote disk {len(disk_bytes)}"
        rng = random.Random(99)

        def check(off, n):
            got = c.read(off, n)
            assert got == disk_bytes[off:off + n], f"[{mode}] wrong data at off={off} len={n}"

        for off in range(0, 16 * 16384, 8192):           # sequential: triggers readahead
            check(off, 8192)
        for _ in range(60):                              # random, unaligned, page-spanning
            n = rng.choice([512, 4096, 20000, 70000])
            check(rng.randrange(0, len(disk_bytes) - n), n)
        check(100, 40000)                                # unaligned start
        check(len(disk_bytes) - 1000, 1000)              # tail of the disk
        check(0, 8192)                                   # re-read: served by cache in cache mode

        if conc > 1:  # pipelined reads: many requests in flight, replies may come back in any order
            reqs = {}
            for i in range(24):
                n = rng.choice([4096, 65536, 131072])
                off = rng.randrange(0, len(disk_bytes) - n)
                c._handle += 1
                reqs[c._handle] = (off, n)
                c.sock.sendall(struct.pack(">IHHQQI", REQUEST_MAGIC, 0, 0, c._handle, off, n))
            order = []
            for _ in range(len(reqs)):
                magic, error, handle = struct.unpack(">IIQ", recv_full(c.sock, 16))
                assert magic == SIMPLE_REPLY_MAGIC and error == 0 and handle in reqs, f"bad pipelined reply {handle}"
                off, n = reqs.pop(handle)
                assert recv_full(c.sock, n) == disk_bytes[off:off + n], f"[{mode}] pipelined read wrong data at {off}"
                order.append(handle)
            assert not reqs
            print(f"   pipelined: 24 reads ok, reply order {'out of order' if order != sorted(order) else 'in order'}")

        # Partial write -> read-modify-write against the remote base, then read back.
        w_off, payload = 123456, bytes(range(256)) * 3
        nbd_write(c, w_off, payload)
        expect = bytearray(disk_bytes)
        expect[w_off:w_off + len(payload)] = payload
        for off, n in ((w_off - 600, len(payload) + 1200), (w_off, len(payload)), (0, 4096)):
            assert c.read(off, n) == bytes(expect[off:off + n]), f"[{mode}] write not read back at {off}"
        c.close()
    finally:
        srv.send_signal(signal.SIGTERM)
        try:
            _, err = srv.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            srv.kill()
            _, err = srv.communicate()
    m = re.search(r"FWD mode=(\w+).*", err)
    assert m, f"[{mode}] server printed no FWD summary:\n{err[-500:]}"
    if mode == "cache":
        used = int(re.search(r"pf_used=(\d+)", err).group(1))
        issued = int(re.search(r"pf_issued=(\d+)", err).group(1))
        assert issued > 0 and used > 0, f"[{mode}] readahead never helped: {m.group(0)}"
    print(f"ok [{mode}] {m.group(0)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nbd", default="bin/nbd_server")
    ap.add_argument("--fwd", default="bin/fwd_sim")
    args = ap.parse_args()
    for b in (args.nbd, args.fwd):
        if not os.path.isfile(b):
            sys.exit(f"{b} not found -- run `make nbd fwd-sim`")

    work = tempfile.mkdtemp(prefix="fwd_remote_")
    disk = os.path.join(work, "remote.img")
    make_disk(disk, 4 << 20)
    before = open(disk, "rb").read()
    rport = free_tcp_ports(1)[0]
    remote = subprocess.Popen([args.fwd, "serve", "--file", disk, "--port", str(rport), "--rtt-us", "500",
                               "--bw-mbps", "2000"], stdout=subprocess.PIPE, text=True)
    try:
        assert remote.stdout.readline().startswith("listening")
        for mode, conc in (("direct", 1), ("cache", 1), ("direct", 8), ("cache", 8)):
            run_mode(args, mode, rport, before, conc)
    finally:
        remote.send_signal(signal.SIGTERM)
        remote.wait(5)
    assert open(disk, "rb").read() == before, "remote disk was modified by a guest write"
    print("PASS")


if __name__ == "__main__":
    main()
