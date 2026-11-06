"""Load + join the artifacts of one `run_capture.py` run (logs/capture/<name>/).

Host side   : features.csv (nbd_server per-request log), cache.csv (admit/evict log)
Guest side  : out/trace_blk.txt, trace_app.txt, trace_stk.txt, samples.csv, proc_snapshot_*.json, sync.json
Static      : the data image's meta.json (byte range of every inverted list) and its ext4 extent map (debugfs)

`join(run)` returns one row per NBD read request on the data disk, carrying
every signal that could be attached to it (host facts, matched guest block
request, readahead state, file/list identity, the application's query).
"""
import json
import os
import re
import subprocess
from functools import lru_cache

import numpy as np
import pandas as pd

LINE = re.compile(r"^\s*(?P<comm>.+?)-(?P<pid>\d+)\s+\[(?P<cpu>\d+)\]\s+\S+\s+(?P<ts>\d+\.\d+):\s+(?P<ev>\w+):\s?(?P<rest>.*)$")
RQ = re.compile(r"(\d+),(\d+) (\S+) (\d+) \(\) (\d+) \+ (\d+) \[(.*)\]")
RQC = re.compile(r"(\d+),(\d+) (\S+) \(\) (\d+) \+ (\d+) \[(\-?\d+)\]")
BIO = re.compile(r"(\d+),(\d+) (\S+) (\d+) \+ (\d+) \[(.*)\]")
ADD = re.compile(r"dev (\d+):(\d+) ino ([0-9a-f]+) pfn=(\S+) ofs=(\d+) order=(\d+)")
KV = re.compile(r"(\w+)=(\S+)")
SYS = re.compile(r"NR (\d+) \(([^)]*)\)")
SYSX = re.compile(r"NR (\d+) = (-?\d+)")


def ts_ns(s):
    a, b = s.split(".")
    return int(a) * 10**9 + int(b.ljust(9, "0")[:9])


def parse_trace(path):
    """Generic ftrace text -> list of (ts_ns, pid, cpu, comm, event, rest, stack-lines)."""
    out = []
    cur = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = LINE.match(line)
            if m:
                ev = m.group("ev")
                rest = m.group("rest")
                if rest.startswith("<user stack trace>"):
                    cur = out[-1] if out else None
                    continue
                cur = [ts_ns(m.group("ts")), int(m.group("pid")), int(m.group("cpu")), m.group("comm"), ev, rest, []]
                out.append(cur)
            elif line.startswith(" =>") and cur is not None:
                cur[6].append(line[3:].strip())
    return out


@lru_cache(maxsize=None)
def load_blk(run):
    rows = parse_trace(os.path.join(run, "out/trace_blk.txt"))
    issue, comp, bio, merge, add, dele, ra, fa, mark = [], [], [], [], [], [], [], [], []
    for ts, pid, cpu, comm, ev, rest, _ in rows:
        if ev == "block_rq_issue":
            m = RQ.match(rest)
            if m:
                issue.append((ts, pid, comm, m.group(3), int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(1)), int(m.group(2))))
        elif ev == "block_rq_complete":
            m = RQC.match(rest)
            if m:
                comp.append((ts, m.group(3), int(m.group(4)), int(m.group(5))))
        elif ev == "block_bio_queue":
            m = BIO.match(rest)
            if m:
                bio.append((ts, pid, comm, m.group(3), int(m.group(4)), int(m.group(5))))
        elif ev in ("block_bio_backmerge", "block_bio_frontmerge"):
            m = BIO.match(rest)
            if m:
                merge.append((ts, ev[10:], int(m.group(4)), int(m.group(5))))
        elif ev == "mm_filemap_add_to_page_cache":
            m = ADD.match(rest)
            if m:
                add.append((ts, pid, int(m.group(3), 16), int(m.group(5)) // 4096))
        elif ev == "mm_filemap_delete_from_page_cache":
            m = ADD.match(rest)
            if m:
                dele.append((ts, pid, int(m.group(3), 16), int(m.group(5)) // 4096))
        elif ev in ("ra_sync", "ra_async"):
            d = {k: int(v) for k, v in KV.findall(rest)}
            d.update(ts=ts, pid=pid, kind=ev[3:])
            ra.append(d)
        elif ev == "fault_around":
            d = {k: int(v) for k, v in KV.findall(rest)}
            d.update(ts=ts, pid=pid)
            fa.append(d)
        elif ev == "tracing_mark_write":
            mark.append((ts, rest))
    return dict(
        issue=pd.DataFrame(issue, columns="ts pid comm rwbs bytes sector nsect dmaj dmin".split()),
        complete=pd.DataFrame(comp, columns="ts rwbs sector nsect".split()),
        bio=pd.DataFrame(bio, columns="ts pid comm rwbs sector nsect".split()),
        merge=pd.DataFrame(merge, columns="ts kind sector nsect".split()),
        cache_add=pd.DataFrame(add, columns="ts pid ino page".split()),
        cache_del=pd.DataFrame(dele, columns="ts pid ino page".split()),
        ra=pd.DataFrame(ra), fault_around=pd.DataFrame(fa),
        marks=mark,
    )


@lru_cache(maxsize=None)
def load_app(run):
    rows = parse_trace(os.path.join(run, "out/trace_app.txt"))
    pf, sysc, sw, wk, mark = [], [], [], [], []
    for ts, pid, cpu, comm, ev, rest, _ in rows:
        if ev == "page_fault_user":
            d = dict(KV.findall(rest))
            pf.append((ts, pid, int(d["address"], 16), int(d["ip"], 16), int(d["error_code"], 16)))
        elif ev == "sys_enter":
            m = SYS.match(rest)
            if m:
                a = [int(x.strip().rstrip(","), 16) if x.strip() else 0 for x in m.group(2).split(",")]
                sysc.append((ts, pid, int(m.group(1)), *a[:6], None))
        elif ev == "sys_exit":
            m = SYSX.match(rest)
            if m:
                sysc.append((ts, pid, int(m.group(1)), 0, 0, 0, 0, 0, 0, int(m.group(2))))
        elif ev == "sched_switch":
            m = re.search(r"prev_pid=(\d+) prev_prio=\d+ prev_state=(\S+) ==> next_comm=.* next_pid=(\d+)", rest)
            if m:
                sw.append((ts, int(m.group(1)), m.group(2), int(m.group(3))))
        elif ev == "sched_wakeup":
            m = re.search(r"pid=(\d+)", rest)
            if m:
                wk.append((ts, int(m.group(1))))
        elif ev == "tracing_mark_write":
            mark.append((ts, rest))
    sc = pd.DataFrame(sysc, columns="ts pid nr a0 a1 a2 a3 a4 a5 ret".split())
    return dict(
        faults=pd.DataFrame(pf, columns="ts pid addr ip err".split()), sys=sc,
        sched=pd.DataFrame(sw, columns="ts prev_pid prev_state next_pid".split()),
        wake=pd.DataFrame(wk, columns="ts pid".split()), marks=mark)


def load_markers(run):
    """Application ground truth: one row per query (begin/end ts, probed lists in probe order)."""
    marks = load_app(run)["marks"]
    q = {}
    for ts, rest in marks:
        m = re.match(r"APP q_begin i=(\d+) lists=(\S+)", rest)
        if m:
            q[int(m.group(1))] = dict(i=int(m.group(1)), t0=ts, lists=[int(x) for x in m.group(2).split(",")])
            continue
        m = re.match(r"APP q_end i=(\d+) dt_us=(\d+)", rest)
        if m and int(m.group(1)) in q:
            q[int(m.group(1))].update(t1=ts, dt_us=int(m.group(2)))
    rows = [v for v in q.values() if "t1" in v]
    if not rows:  # workloads without per-query markers (e.g. YCSB)
        return pd.DataFrame(columns=["i", "t0", "t1", "lists", "dt_us"])
    return pd.DataFrame(rows).sort_values("t0").reset_index(drop=True)


def load_host(run):
    f = pd.read_csv(os.path.join(run, "features.csv"))
    return f


# --- clock sync -------------------------------------------------------------
def clock_offset(run, host=None, blk=None):
    """Estimate host_mono - guest_mono (ns) from the O_DIRECT sync probes, pre and post (linear in time)."""
    host = load_host(run) if host is None else host
    blk = load_blk(run) if blk is None else blk
    sync = json.load(open(os.path.join(run, "out/sync.json")))
    iss = blk["issue"]
    pts = []
    for tag, k, off, t0, t1 in sync["probes"]:
        g = iss[(iss.sector == off // 512) & (iss.dmaj == 254) & (iss.dmin == 16)]
        h = host[(host.offset == off) & (host.type == 0)]
        if len(g) and len(h):
            pts.append((float(g.ts.iloc[0]), float(h.arrive_ns.iloc[0] - g.ts.iloc[0]), tag))
    if not pts:
        raise RuntimeError("no sync probe could be matched between guest and host")
    ts = np.array([p[0] for p in pts])
    d = np.array([p[1] for p in pts])
    pre = np.median([p[1] for p in pts if p[2] == "pre"])
    post_l = [p[1] for p in pts if p[2] == "post"]
    post = np.median(post_l) if post_l else pre
    t_pre = np.median([p[0] for p in pts if p[2] == "pre"])
    t_post = np.median([p[0] for p in pts if p[2] == "post"]) if post_l else t_pre + 1
    def off_at(t):
        return pre + (post - pre) * (np.asarray(t, dtype=float) - t_pre) / max(1.0, (t_post - t_pre))
    return off_at, dict(pre_ns=float(pre), post_ns=float(post), spread_ns=float(d.max() - d.min()), n=len(pts))


# --- static layout ------------------------------------------------------------
@lru_cache(maxsize=None)
def extents(img, path):
    out = subprocess.run(["debugfs", "-R", f"stat {path}", img], capture_output=True, text=True).stdout
    ext = []
    for a, b, c, d in re.findall(r"\((\d+)-(\d+)\):(\d+)-(\d+)", out):
        ext.append((int(a), int(b), int(c), int(d)))
    ext.sort()
    return ext


def file_to_disk(ext, file_off, bs=4096):
    lb = file_off // bs
    for la, lb_end, pa, _ in ext:
        if la <= lb <= lb_end:
            return (pa + (lb - la)) * bs + file_off % bs
    return None


class Layout:
    """disk byte offset <-> (file, file offset, inverted list)."""
    def __init__(self, img, meta):
        self.img = img
        self.meta = meta
        self.files = {}
        for name in ("index.ivfdata", "index.faiss", "meta.json"):
            ext = extents(img, "/" + name)
            if ext:
                self.files[name] = ext
        # disk-extent table for reverse lookups: [disk_start, disk_end, file, file_start]
        tab = []
        for name, ext in self.files.items():
            for la, lb, pa, pb in ext:
                tab.append((pa * 4096, (pb + 1) * 4096, name, la * 4096))
        tab.sort()
        self.tab = tab
        self.tab_start = np.array([t[0] for t in tab])
        L = np.array(meta["list_offset_size_codesize"], dtype=np.int64)
        self.list_off = L[:, 0]
        self.list_bytes = L[:, 1] * (L[:, 2] + 8)
        order = np.argsort(self.list_off)
        self.list_sorted_off = self.list_off[order]
        self.list_order = order
        self.nlist = len(L)
        # disk start/end byte of each list (lists may straddle an extent boundary; use the start for placement)
        iv = self.files["index.ivfdata"]
        self.list_disk_start = np.array([file_to_disk(iv, int(o)) for o in self.list_off], dtype=np.int64)
        self.list_disk_end = self.list_disk_start + self.list_bytes

    def locate(self, disk_off):
        i = np.searchsorted(self.tab_start, disk_off, side="right") - 1
        if i < 0:
            return None
        s, e, name, fs = self.tab[i]
        if disk_off >= e:
            return None
        return name, fs + (disk_off - s)

    def list_of_file_off(self, foff):
        i = np.searchsorted(self.list_sorted_off, foff, side="right") - 1
        if i < 0:
            return -1
        lid = int(self.list_order[i])
        return lid if foff < self.list_off[lid] + self.list_bytes[lid] else -1

    def lists_overlapping(self, disk_off, length):
        """All list ids whose bytes intersect [disk_off, disk_off+length)."""
        loc = self.locate(disk_off)
        if not loc or loc[0] != "index.ivfdata":
            return []
        a = loc[1]
        b = a + length
        i = max(0, np.searchsorted(self.list_sorted_off, a, side="right") - 1)
        res = []
        while i < self.nlist and self.list_sorted_off[i] < b:
            lid = int(self.list_order[i])
            if self.list_off[lid] + self.list_bytes[lid] > a:
                res.append(lid)
            i += 1
        return res


class FileLayout:
    """disk byte offset -> (file, file offset) for a data disk that holds plain files (no FAISS list structure)."""
    def __init__(self, img, names):
        tab = []
        for name in names:
            for la, lb, pa, pb in extents(img, "/" + name):
                tab.append((pa * 4096, (pb + 1) * 4096, name, la * 4096))
        tab.sort()
        self.tab = tab
        self.tab_start = np.array([t[0] for t in tab])

    def locate(self, disk_off):
        i = np.searchsorted(self.tab_start, disk_off, side="right") - 1
        if i < 0:
            return None
        s, e, name, fs = self.tab[i]
        return (name, fs + (disk_off - s)) if disk_off < e else None


# --- the join -------------------------------------------------------------------
def join(run, verbose=True):
    args = json.load(open(os.path.join(run, "args.json")))
    meta = json.load(open(args["meta_file"]))
    host = load_host(run)
    blk = load_blk(run)
    off_at, sync_info = clock_offset(run, host, blk)
    reads = host[host.type == 0].copy().reset_index(drop=True)
    # drop the sync probes themselves
    tail = meta["tail_offset"]
    reads["is_probe"] = reads.offset >= tail
    reads["g_arrive"] = reads.arrive_ns - off_at(reads.arrive_ns.values - 0)  # guest-clock time of arrival
    faiss_like = "list_offset_size_codesize" in meta
    layout = Layout(args["data_img"], meta) if faiss_like else FileLayout(args["data_img"], ["ycsb.db"])

    # ---- match to guest block requests (same start sector + length, nearest preceding issue) ----
    iss = blk["issue"][(blk["issue"].dmaj == 254) & (blk["issue"].dmin == 16)].reset_index(drop=True)
    iss["end_sector"] = iss.sector + iss.nsect
    by_key = {}
    for idx, (s, n, t) in enumerate(zip(iss.sector.values, iss.nsect.values, iss.ts.values)):
        by_key.setdefault((s, n), []).append((t, idx))
    used = set()
    match = np.full(len(reads), -1, dtype=np.int64)
    for i, (o, l, ga) in enumerate(zip(reads.offset.values, reads.length.values, reads.g_arrive.values)):
        c = by_key.get((o // 512, l // 512), [])
        best = None
        for t, idx in c:
            if idx in used:
                continue
            dt = ga - t
            if -2e6 <= dt <= 50e6 and (best is None or dt < best[0]):
                best = (dt, idx)
        if best:
            used.add(best[1])
            match[i] = best[1]
    reads["rq"] = match
    ok = reads.rq >= 0
    if verbose:
        print(f"[join] clock offset pre={sync_info['pre_ns']/1e6:.3f} ms post={sync_info['post_ns']/1e6:.3f} ms (n={sync_info['n']}); "
              f"exact guest-rq match for {ok.sum()}/{len(reads)} NBD reads ({100*ok.mean():.1f}%)")
    g = iss.reindex(reads.rq.clip(lower=0)).reset_index(drop=True)
    for col in ("pid", "comm", "rwbs", "ts"):
        reads["g_" + col] = np.where(ok, g[col].values, None if col in ("comm", "rwbs") else np.nan)
    reads["rwbs_flags"] = reads.g_rwbs.fillna("?")
    reads["lat_guest_to_host_us"] = (reads.g_arrive - reads.g_ts) / 1e3

    # ---- readahead state in force (last ra_* kprobe by the same pid shortly before issue) ----
    ra = blk["ra"].sort_values("ts").reset_index(drop=True) if len(blk["ra"]) else pd.DataFrame(columns=["ts"])
    ra_cols = ["kind", "ra_index", "ra_start", "ra_size", "ra_async", "ra_pages", "mmap_miss", "req"]
    for c in ra_cols:
        reads["ra_" + c if not c.startswith("ra_") else c] = np.nan
    if len(ra):
        rts = ra.ts.values
        pos = np.searchsorted(rts, reads.g_ts.fillna(-1).values, side="right") - 1
        for new, c in [("ra_kind", "kind"), ("ra_idx", "ra_index"), ("ra_win_start", "ra_start"), ("ra_win_size", "ra_size"),
                       ("ra_win_async", "ra_async"), ("ra_pages_max", "ra_pages"), ("ra_mmap_miss", "mmap_miss"), ("ra_req", "req")]:
            vals = ra[c].values
            reads[new] = [vals[p] if (p >= 0 and ok_ and (gts - rts[p]) < 2e6) else np.nan
                          for p, ok_, gts in zip(pos, ok.values, reads.g_ts.fillna(0).values)]

    # ---- file / list identity of each request (from the ext4 extent map) ----
    locs = [layout.locate(int(o)) for o in reads.offset.values]
    reads["file"] = [l[0] if l else ("raw-tail" if o >= meta["fs_bytes"] else "fs-metadata") for l, o in zip(locs, reads.offset.values)]
    reads["file_off"] = [l[1] if l else -1 for l in locs]
    reads["list_id"] = [layout.list_of_file_off(f) if (faiss_like and fn == "index.ivfdata") else -1 for f, fn in zip(reads.file_off.values, reads.file.values)]

    # ---- application context: which query / list slot the request belongs to ----
    q = load_markers(run)
    reads["g_arrive_i"] = reads.g_arrive.astype("int64")
    qi = np.searchsorted(q.t0.values, reads.g_arrive.values, side="right") - 1 if len(q) else np.full(len(reads), -1)
    inq = np.array([(i >= 0 and reads.g_arrive.values[k] <= q.t1.values[i] + 1e6) for k, i in enumerate(qi)]) if len(q) else np.zeros(len(reads), bool)
    reads["query"] = np.where(inq, qi, -1)
    reads["q_rank"] = -1
    reads["q_lists"] = None
    qlists = q.lists.tolist() if len(q) else []
    ranks, lists_col = [], []
    for k, i in enumerate(reads["query"].values):
        if i < 0:
            ranks.append(-1)
            lists_col.append(None)
            continue
        ls = qlists[i]
        lid = reads.list_id.values[k]
        ranks.append(ls.index(lid) if lid in ls else -1)
        lists_col.append(ls)
    reads["q_rank"] = ranks
    reads["q_lists"] = lists_col
    return dict(reads=reads, layout=layout, meta=meta, blk=blk, queries=q, sync=sync_info, args=args)
