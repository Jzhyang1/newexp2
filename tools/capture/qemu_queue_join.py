#!/usr/bin/env python3
"""Attach the issuing virtio queue (= guest vCPU under blk-mq's per-CPU queue mapping) to every NBD request.

  qemu_queue_join.py RUN_DIR [--data-img-bytes N]  ->  RUN_DIR/features_q.csv  (features.csv + column `queue`)

Needs a capture with --qemu-trace virtio_queue_notify,virtio_blk_handle_read,nbd_send_request.  In QEMU's log a guest
kick (virtio_queue_notify vdev, n) is followed by the reads it popped from that queue (virtio_blk_handle_read vdev, req,
sector, nsectors) and then by the NBD requests they became (nbd_send_request .from, .len, .cookie): each NBD request
gets the queue of the pending read that starts at its offset.  The NBD cookie is the handle nbd_server logs, so the join
to features.csv is exact (cookie + offset).
"""
import argparse, collections, os, re
import pandas as pd

RE_N = re.compile(r"virtio_queue_notify vdev (0x[0-9a-f]+) n (\d+)")
RE_R = re.compile(r"virtio_blk_handle_read vdev (0x[0-9a-f]+) req (0x[0-9a-f]+) sector (\d+) nsectors (\d+)")
RE_S = re.compile(r"nbd_send_request .*\.from = (\d+), \.len = (\d+), \.cookie = (\d+), .*\.type = 0")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    a = ap.parse_args()
    last_q = {}
    pend = collections.defaultdict(collections.deque)   # byte offset -> deque of (vdev, queue)
    sends = []
    nq = collections.Counter()
    with open(os.path.join(a.run, "qemu_trace.log")) as f:
        for ln in f:
            if ln.startswith("virtio_queue_notify"):
                m = RE_N.search(ln)
                if m: last_q[m.group(1)] = int(m.group(2)); nq[(m.group(1), int(m.group(2)))] += 1
            elif ln.startswith("virtio_blk_handle_read"):
                m = RE_R.search(ln)
                if m: pend[int(m.group(3)) * 512].append((m.group(1), last_q.get(m.group(1), -1)))
            elif ln.startswith("nbd_send_request"):
                m = RE_S.search(ln)
                if m:
                    off = int(m.group(1)); d = pend.get(off)
                    v, q = d.popleft() if d else ("?", -1)
                    sends.append((off, int(m.group(2)), int(m.group(3)), v, q))
    S = pd.DataFrame(sends, columns=["offset", "length", "handle", "vdev", "queue"])
    print("kicks per (device, queue):", dict(nq))
    f = pd.read_csv(os.path.join(a.run, "features.csv"))
    # one NBD connection per disk: pick the vdev whose sends match the data disk's (handle, offset) pairs best
    best = None
    for v in S.vdev.unique():
        s = S[S.vdev == v].drop_duplicates(["handle", "offset"], keep="last")
        j = f.merge(s[["handle", "offset", "queue"]], on=["handle", "offset"], how="left")
        cov = j.queue.notna().mean()
        if best is None or cov > best[0]: best = (cov, v, j)
    cov, v, j = best
    j["queue"] = j.queue.fillna(-1).astype(int)
    j.to_csv(os.path.join(a.run, "features_q.csv"), index=False)
    rd = j[j.type == 0]
    print(f"data disk = vdev {v}: queue attributed to {100 * (rd.queue >= 0).mean():.1f}% of reads; share per queue:",
          (rd.queue.value_counts(normalize=True).sort_index() * 100).round(1).to_dict())


if __name__ == "__main__":
    main()
