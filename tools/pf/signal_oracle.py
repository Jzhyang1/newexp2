#!/usr/bin/env python3
"""Headroom of extra (non-host) signals for FAISS: what would prefetching gain if the policy were *told* X?
Writes the trace psim replays plus one prediction file per oracle (psim `file:path=...`), all in logs/pf_signals/.
Oracles (page = 64 KiB; every prediction has probability 1, so theta only needs to be <= 100):
  probe_all   all not-yet-visited lists of the query that is running when the request arrives (app's probe list)
  probe_next  only the next list in that query's probe order
  probe_all_nr  probe_all minus lists whose pages were all requested in the last 256 requests (host-visible residency proxy)
usage: signal_oracle.py [run_dir=logs/capture/base]"""
import sys, os, struct, numpy as np, pandas as pd
R = "/home/jonathan-yang/newexp2/"
run = sys.argv[1] if len(sys.argv) > 1 else R + "logs/capture/base"
j = pd.read_pickle(run + "/joined.pkl")
r = j["reads"]; q = j["queries"]
r = r[(r.type == 0) & (~r.is_probe)].sort_values("arrive_ns").reset_index(drop=True)
out = R + "logs/pf_signals/"; os.makedirs(out, exist_ok=True)
r[["offset", "length", "arrive_ns", "inflight"]].to_csv(out + "trace.csv", index=False)
ls, lb = j["list_disk_start"], j["list_bytes"]
PG = 65536
def pages(l): return int(ls[l] // PG), int((ls[l] + lb[l] - 1) // PG)
qt0 = q.t0.values; qt1 = q.t1.values
qi = np.searchsorted(qt0, r.g_arrive.values, side="right") - 1
print("requests", len(r), "in a query window:", np.mean((qi >= 0) & (r.g_arrive.values <= qt1[np.clip(qi, 0, None)])))
lists_of = [np.array([int(x) for x in s.split(",")]) if isinstance(s, str) else np.array(s) for s in q.lists]
def hit_lists(i):  # lists a request touches
    a, b = r.offset[i] // PG, (r.offset[i] + r.length[i] - 1) // PG
    return a, b
def write(name, fn):
    with open(out + name + ".bin", "wb") as f:
        for i in range(len(r)):
            for pg in fn(i):
                f.write(struct.pack("<QQf", i, pg, 1.0))
visited = {}
def probe_all(i, nr=False, nxt=False):
    k = qi[i]
    if k < 0: return []
    a, b = hit_lists(i)
    seen = visited.setdefault(k, set())
    cur = [l for l in lists_of[k] if pages(l)[0] <= b and pages(l)[1] >= a]
    seen.update(cur)
    todo = [l for l in lists_of[k] if l not in seen]
    if nxt:
        # next list in probe order after the current one
        order = list(lists_of[k]); idx = max([order.index(l) for l in cur], default=-1)
        todo = [order[idx + 1]] if idx + 1 < len(order) and order[idx + 1] not in seen else []
    return [p for l in todo for p in range(pages(l)[0], pages(l)[1] + 1)]
write("probe_all", lambda i: probe_all(i)); visited.clear()
write("probe_next", lambda i: probe_all(i, nxt=True)); visited.clear()
print("wrote", os.listdir(out))

# probe_all_res: also told which of those lists the guest will actually miss on (i.e. will reach the host) = probe + guest residency
req_pages = [(r.offset[i] // PG, (r.offset[i] + r.length[i] - 1) // PG) for i in range(len(r))]
need = {}   # query -> set of lists with at least one host request inside the query window
for i in range(len(r)):
    k = qi[i]
    if k < 0: continue
    a, b = req_pages[i]
    for l in lists_of[k]:
        if pages(l)[0] <= b and pages(l)[1] >= a: need.setdefault(k, set()).add(l)
visited.clear()
def probe_res(i):
    k = qi[i]
    if k < 0: return []
    a, b = hit_lists(i); seen = visited.setdefault(k, set())
    seen.update(l for l in lists_of[k] if pages(l)[0] <= b and pages(l)[1] >= a)
    return [p for l in need.get(k, ()) if l not in seen for p in range(pages(l)[0], pages(l)[1] + 1)]
write("probe_res", probe_res)

# probe_all_nr: probe lists minus lists the host saw requested within the last N host requests (a guest probably still holds them)
for N in (300, 1000, 3000):
    visited.clear(); last_seen = {}
    def fn(i, N=N):
        k = qi[i]; a, b = req_pages[i]
        for l in range(0):
            pass
        if k < 0: return []
        seen = visited.setdefault(k, set())
        cur = [l for l in lists_of[k] if pages(l)[0] <= b and pages(l)[1] >= a]
        seen.update(cur)
        for l in cur: last_seen[l] = i
        return [p for l in lists_of[k] if l not in seen and i - last_seen.get(l, -10**9) > N for p in range(pages(l)[0], pages(l)[1] + 1)]
    write(f"probe_nr{N}", fn)

# probe_resid{X}: probe lists minus lists whose guest page-cache residency at query start (reconstructed from the traced
# filemap add/delete events of the index file, the real guest signal #30/#32) is >= X of their pages.  Needs the traced run.
import json
meta = json.load(open(R + "disk/data/faiss_data.img.meta.json"))
L = np.array(meta["list_offset_size_codesize"], dtype=np.int64)
lo_pg = L[:, 0] // 4096; hi_pg = (L[:, 0] + L[:, 1] * (L[:, 2] + 8) - 1) // 4096
ca = j["cache_add"]; cd = j["cache_del"]; ca = ca[ca.ino == 13]; cd = cd[cd.ino == 13]
ev = pd.concat([ca.assign(d=1), cd.assign(d=-1)]).sort_values("ts")
npages = 386720
res = np.zeros(npages + 1, np.int8)
cum = np.zeros(npages + 2, np.int64)   # recomputed lazily per query
ets = ev.ts.values; epg = ev.page.values; ed = ev.d.values; ep = 0
resid_frac = np.zeros((len(q), len(L)), np.float32)   # only for the lists each query probes
for k in range(len(q)):
    t = q.t0.values[k]
    while ep < len(ets) and ets[ep] <= t:
        res[epg[ep]] = max(0, res[epg[ep]] + ed[ep]) if ed[ep] < 0 else 1
        ep += 1
    ls_k = lists_of[k]
    c = np.concatenate([[0], np.cumsum(res[:npages])])
    for l in ls_k:
        resid_frac[k, l] = (c[hi_pg[l] + 1] - c[lo_pg[l]]) / max(1, hi_pg[l] - lo_pg[l] + 1)
for X in (0.5, 0.8):
    visited.clear()
    def fn(i, X=X):
        k = qi[i]
        if k < 0: return []
        a, b = req_pages[i]; seen = visited.setdefault(k, set())
        seen.update(l for l in lists_of[k] if pages(l)[0] <= b and pages(l)[1] >= a)
        return [p for l in lists_of[k] if l not in seen and resid_frac[k, l] < X for p in range(pages(l)[0], pages(l)[1] + 1)]
    write(f"probe_resid{int(X*100)}", fn)
# how well does reconstructed residency separate lists the guest misses on from lists it hits?
miss = np.array([[l in need.get(k, ()) for l in lists_of[k]] for k in range(len(q))])
fr = np.array([[resid_frac[k, l] for l in lists_of[k]] for k in range(len(q))])
from sklearn.metrics import roc_auc_score
print("AUC(1-residency -> list reaches host):", roc_auc_score(miss.ravel(), 1 - fr.ravel()))

# probe_rec{X}: probe lists minus lists probed in the last X *queries* (an LRU model of the guest cache built only from the probe lists
# themselves: no guest tracing, no host feedback)
lastq = {}
for X in (50, 100, 200, 400, 800):
    visited.clear(); lastq.clear()
    def fn(i, X=X):
        k = qi[i]
        if k < 0: return []
        a, b = req_pages[i]; seen = visited.setdefault(k, set())
        seen.update(l for l in lists_of[k] if pages(l)[0] <= b and pages(l)[1] >= a)
        out_ = [p for l in lists_of[k] if l not in seen and k - lastq.get(l, -10**9) > X for p in range(pages(l)[0], pages(l)[1] + 1)]
        if i == 0 or qi[i - 1] != k:
            pass
        return out_
    # mark probes after the query's first request has been served (so a query never filters its own lists)
    seen_q = set()
    def fn2(i, X=X):
        k = qi[i]
        if k >= 0 and k not in seen_q:
            seen_q.add(k)
            res_ = fn(i)
            for l in lists_of[k]: lastq[l] = k
            return res_
        return fn(i)
    seen_q.clear(); write(f"probe_rec{X}", fn2)

# probe_pop{M}: skip lists probed >= M times so far (popular lists live in the guest cache); counts come from the probe lists only
for M in (3, 6, 12, 24):
    visited.clear(); cnt = {}; seen_q = set()
    def fnp(i, M=M):
        k = qi[i]
        if k < 0: return []
        a, b = req_pages[i]; seen = visited.setdefault(k, set())
        seen.update(l for l in lists_of[k] if pages(l)[0] <= b and pages(l)[1] >= a)
        out_ = [p for l in lists_of[k] if l not in seen and cnt.get(l, 0) < M for p in range(pages(l)[0], pages(l)[1] + 1)]
        if k not in seen_q:
            seen_q.add(k)
            for l in lists_of[k]: cnt[l] = cnt.get(l, 0) + 1
        return out_
    write(f"probe_pop{M}", fnp)

# --- hint files for psim --hints: one hint per probed list at the first host request of its query (the app knows the lists at q_begin)
def write_hints(name, with_aux):
    first_i = {}
    for i in range(len(r)):
        k = qi[i]
        if k >= 0 and k not in first_i: first_i[k] = i
    with open(out + name + ".bin", "wb") as f:
        for k, i in sorted(first_i.items(), key=lambda x: x[1]):
            for l in lists_of[k]:
                a, b = pages(l)
                aux = int(round(float(resid_frac[k, l]) * 15)) if with_aux else 255
                f.write(struct.pack("<QQQII", i, a, b - a + 1, aux, 0))
write_hints("hints_faiss", True); write_hints("hints_faiss_noaux", False)

# ---------------- no application hint: predict the probe lists from what a guest agent can see without touching the app ----------------
# Observations: the sequence of lists the application touches (visible to a guest agent from page-fault / mincore tracing, NOT from an
# app hook), query boundaries (idle gaps) and per-list page-cache residency (mincore, app independent).  Prediction = the lists
# that co-occurred with the lists seen so far in earlier queries ("topic"), plus the previous queries' lists (session continuity),
# minus lists the guest already holds.  Delivered when the query starts (m=0) and after its first list (m=1).
from collections import defaultdict
co = defaultdict(lambda: defaultdict(float))
first_i = {}
for i in range(len(r)):
    k = qi[i]
    if k >= 0 and k not in first_i: first_i[k] = i
def predict(k, seen, K, rf):
    sc = defaultdict(float)
    for back, w in ((1, 1.0), (2, .6), (3, .4), (4, .3)):
        if k - back >= 0:
            for l in lists_of[k - back]: sc[l] += w * 0.3
    for a in seen:
        for b, c in co[a].items(): sc[b] += c
    for a in seen: sc.pop(a, None)
    cand = [l for l in sorted(sc, key=sc.get, reverse=True) if rf(k, l)]
    return cand[:K]
def build_noapp(name, K, m_stages, rf, decay=1.0):
    co.clear()
    with open(out + name + ".bin", "wb") as f:
        for k in range(len(q)):
            if k in first_i:
                i0 = first_i[k]
                for m in m_stages:
                    cands = predict(k, list(lists_of[k][:m]), K, rf)
                    for l in cands:
                        a, b = pages(l)
                        f.write(struct.pack("<QQQII", i0, a, b - a + 1, int(round(float(resid_frac[k, l]) * 14)), 0))
            for a in lists_of[k]:
                for b in lists_of[k]:
                    if a != b: co[a][b] += 1.0
for K in (4, 8, 16):
    build_noapp(f"noapp_k{K}_m01", K, (0, 1), lambda k, l: resid_frac[k, l] < 0.5)
build_noapp("noapp_k8_m01_nofilter", 8, (0, 1), lambda k, l: True)

# ceiling of predicting the *host-visible* lists of a query (the ones that actually need a prefetch) from earlier host-visible lists only
hv = [sorted(need.get(k, ()), key=lambda l: pages(l)[0]) for k in range(len(q))]
co2 = defaultdict(lambda: defaultdict(float)); rec = {K: [] for K in (4, 8, 16, 32)}; nh = []
for k in range(len(q)):
    if k > 2000 and len(hv[k]) >= 2:
        first = hv[k][0]; rest = set(hv[k][1:])
        sc = defaultdict(float)
        for b, c in co2[first].items(): sc[b] += c
        for back in range(1, 6):
            for l in hv[k - back]: sc[l] += 0.2 / back
        sc.pop(first, None)
        ranked = sorted(sc, key=sc.get, reverse=True)
        for K in rec: rec[K].append(len(set(ranked[:K]) & rest) / len(rest))
        nh.append(len(rest))
    for a in hv[k]:
        for b in hv[k]:
            if a != b: co2[a][b] += 1
print("host-visible lists per query (when >=2): mean remaining", np.mean(nh), " recall of the remaining host-visible lists given the first one, K =", {K: round(float(np.mean(v)), 3) for K, v in rec.items()})

# ---------- no app hook, but domain knowledge: IVF list centroids live in index.faiss (read it from the image) ----------
# Lists a query probes are the centroids nearest to the query vector, so once the guest agent has seen the FIRST list the query touches
# (page fault on the mmap'd index: no app hook), the other probed lists are among that centroid's neighbours.  Rank candidates by
# distance to the mean of the centroids observed so far; drop lists the guest already holds (mincore); deliver as hints.
SC = "/tmp/claude-1000/-home-jonathan-yang-newexp2/0e888e7a-7f13-410b-94ae-3a8bcb0cf58f/scratchpad/"
cent = np.load(SC + "cent.npy"); c2 = (cent ** 2).sum(1)
def geo_rank(obs, K, k, resid_thr):
    m = cent[obs].mean(0)
    d = c2 - 2 * cent @ m
    cand = np.argsort(d)[:K * 6]
    out_ = [int(l) for l in cand if l not in obs and (resid_thr is None or resid_frac[k, l] < resid_thr)]
    return out_[:K]
def build_geo(name, K, m_obs, resid_thr, delay=1):
    with open(out + name + ".bin", "wb") as f:
        for k in range(len(q)):
            if k not in first_i: continue
            i0 = first_i[k] + delay
            for l in geo_rank(list(lists_of[k][:m_obs]), K, k, resid_thr):
                a, b = pages(l)
                f.write(struct.pack("<QQQII", i0, a, b - a + 1, int(round(float(resid_frac[k, l]) * 14)), 0))
for K in (8, 16, 24):
    build_geo(f"geo_k{K}", K, 1, 0.5)
build_geo("geo_k16_m2", 16, 2, 0.5)
# recall of what actually needs the host
tot = hit = 0
for k in range(len(q)):
    cands = set(geo_rank(list(lists_of[k][:1]), 16, k, 0.5)); nd = set(need.get(k, ())) - set(lists_of[k][:1])
    tot += len(nd); hit += len(nd & cands)
print("geo K=16, m=1, residency-filtered: recall of the host-visible lists beyond the first:", hit / max(1, tot))

import pickle
pickle.dump(dict(resid_frac=resid_frac, need=need, lists_of=lists_of, first_i=first_i, list_pages=[pages(l) for l in range(len(L))],
                 qi=qi, ntrace=len(r)), open(out + "cache.pkl", "wb"))
