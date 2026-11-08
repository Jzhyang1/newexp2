#!/usr/bin/env python3
"""Builds the remote-disk write-up (one self-contained HTML page, inline SVG charts).

  python3 disk/fwd_report.py [out.html]

Inputs (all produced by the experiment scripts, nothing is typed in by hand except prose):
  logs/fwd_dc2/results.csv       host-only replay matrix  (disk/fwd_experiment.py)
  logs/fwd_dc2/calibration.json  emulated-link accuracy   (disk/fwd_calibrate.py)
  logs/fwd_dc/results.csv        the first replay matrix (before the emulator / stream-policy fixes)
  logs/fwd_vm/results.csv        VM-in-the-loop runs      (tools/capture/vm_remote_study.py)
  logs/fwd_vm/integrity.csv      result hashes across modes
  logs/capture/base/features.csv a real guest's NBD request stream (queue depth)
Every chart mark and every number quoted in the text is computed from those files.
"""
import csv
import html
import json
import math
import os
import statistics as st
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "logs/fwd_dc2/report.html")
P = lambda *a: os.path.join(ROOT, *a)


def load(path):
    rows = list(csv.DictReader(open(path)))
    for r in rows:
        for k, v in r.items():
            try:
                r[k] = float(v)
            except (ValueError, TypeError):
                pass
    return rows


R = load(P("logs/fwd_dc2/results.csv"))
OLD = load(P("logs/fwd_dc/results.csv"))
VM_EARLY = [r for r in load(P("logs/fwd_vm/results.csv")) if r["status"] == "ok"]
DRIFT = [r for r in load(P("logs/fwd_vm/drift.csv")) if r["status"] == "ok"]   # direct baselines re-measured at the end of the session
OLDLOC = [r for r in load(P("logs/fwd_vm/results_before_default_change.csv")) if r["status"] == "ok" and r["mode"] == "local"]   # local disk under the serial server
VM = VM_EARLY + DRIFT
INT = load(P("logs/fwd_vm/integrity.csv"))
DIAG = [r for r in load(P("logs/fwd_vm/depth_diag.csv")) if r["status"] == "ok"]               # final code
DIAG_OLD = [r for r in load(P("logs/fwd_vm/depth_diag_pwrite_store.csv")) if r["status"] == "ok"]  # same runs, pwrite-based store
WIN = {r["label"]: r for r in load(P("logs/fwd_dc2/window_test.csv"))}   # replay of the FIRST 6,700 reads at 60 ms
CAL = json.load(open(P("logs/fwd_dc2/calibration.json")))
mism = int(sum(r["mismatches"] for r in R))


def med(xs):
    return st.median(xs)


# ------------------------------------------------------------------ data access
def rsel(rows, sc, wl, mode, think=200):
    return [r for r in rows if r["scenario"] == sc and r["workload"] == wl and r["label"] == mode
            and (wl == "faiss" or int(r["think_us"]) == think)]


def rm(sc, wl, mode, key="io_wait_s", think=200, rows=None):
    rs = rsel(R if rows is None else rows, sc, wl, mode, think)
    return med([r[key] for r in rs]) if rs else None


def rspeed(sc, wl, mode, think=200):
    d, m = rm(sc, wl, "direct", think=think), rm(sc, wl, mode, think=think)
    return d / m if d and m else None


def ramp(sc, wl, mode, rows=None):
    rs = rsel(R if rows is None else rows, sc, wl, mode)
    if not rs:
        return None
    return (med([r["remote_demand_bytes"] for r in rs]) + med([r["remote_bulk_bytes"] for r in rs])) / med([r["bytes_read"] for r in rs])


DEPTH_MODES = ("readahead", "stream", "readahead_c1")


def vmsel(sc, mode, ra="default", n=None, depth=16):
    out = []
    for r in VM:
        if r["mode"] != mode or str(r["guest_ra_kb"]).replace(".0", "") != str(ra):
            continue
        if mode in DEPTH_MODES and r.get("readahead_pages") not in ("", None) and int(r["readahead_pages"]) != depth:
            continue
        if mode != "local" and r["scenario"] != sc:
            continue
        if n is not None and int(r["n_queries"]) != n:
            continue
        out.append(r)
    return out


def vlocal(sc, ra="default"):
    n = 2500 if sc == "region" else 5000
    return vmsel(None, "local", ra, n)


def vq(rs, key="q_mean_ms"):
    return med([r[key] for r in rs])


def vtraffic(rs):
    return (med([r["remote_demand_bytes"] for r in rs]) + med([r["remote_bulk_bytes"] for r in rs])) / 1e6


SCEN = [("zone", "Same zone"), ("az", "Cross zone"), ("region", "Cross region")]


# ------------------------------------------------------------------ svg primitives
def esc(s):
    return html.escape(str(s))


def lx(v, lo, hi, x0, x1):
    return x0 + (math.log(v) - math.log(lo)) / (math.log(hi) - math.log(lo)) * (x1 - x0)


def legend(items):
    return '<div class="legend">' + "".join(f'<span><i class="sw {c}"></i>{esc(n)}</span>' for n, c in items) + "</div>"


MODE_CLS = {"direct": "base", "direct_c1": "base2", "readahead_c1": "s1b", "cache": "s3", "readahead": "s1", "stream": "s2", "quickmine": "s4", "local": "loc"}
MODE_NAME = {"direct": "Direct", "direct_c1": "Direct, serial server", "readahead_c1": "Cache + readahead, serial server", "cache": "Cache only (LRU)", "readahead": "Cache + readahead",
             "stream": "Cache + stream readahead", "quickmine": "Cache + QuickMine", "local": "Local disk (no network)"}


def bar_panel(title, sub, items, ref=None, ref_label="local disk", unit="ms"):
    """Horizontal bars. items: (label, cls, [values over repetitions], note). ref: a dashed reference value."""
    W, left, right, rh = 760, 220, 150, 30
    H = 18 + rh * len(items) + 28
    top = max(v for _, _, vs, _ in items for v in vs)
    xmax = max(top, ref or 0) * 1.12
    x = lambda v: left + v / xmax * (W - left - right)
    o = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="{esc(title)}">']
    step = 10 ** math.floor(math.log10(xmax))
    for m in (1, 2, 5):
        if xmax / (step * m) <= 6:
            step *= m
            break
    v = 0
    while v <= xmax:
        o.append(f'<line class="grid{" base" if v == 0 else ""}" x1="{x(v):.1f}" x2="{x(v):.1f}" y1="8" y2="{H - 26}"/>')
        o.append(f'<text class="tick" x="{x(v):.1f}" y="{H - 10}" text-anchor="middle">{v:g}</text>')
        v += step
    o.append(f'<text class="axl" x="{W - right:.0f}" y="{H - 10}" text-anchor="end">{esc(unit)}</text>')
    for i, (lab, cls, vs, note) in enumerate(items):
        y = 14 + i * rh
        m = med(vs)
        o.append(f'<text class="rowl" x="{left - 10}" y="{y + 14}" text-anchor="end">{esc(lab)}</text>')
        o.append(f'<rect class="bar {cls}" x="{x(0):.1f}" y="{y}" width="{max(2, x(m) - x(0)):.1f}" height="18" rx="3"><title>{esc(lab)}: {m:.2f} {unit} (runs: {", ".join(f"{v:.2f}" for v in vs)})</title></rect>')
        if len(vs) > 1:
            o.append(f'<line class="whisk" x1="{x(min(vs)):.1f}" x2="{x(max(vs)):.1f}" y1="{y + 9}" y2="{y + 9}"/>')
            for v in vs:
                o.append(f'<circle class="rep" cx="{x(v):.1f}" cy="{y + 9}" r="2.6"/>')
        o.append(f'<text class="val" x="{x(max(vs)) + 9:.1f}" y="{y + 13}">{m:.2f}{" · " + esc(note) if note else ""}</text>')
    if ref:
        o.append(f'<line class="refline" x1="{x(ref):.1f}" x2="{x(ref):.1f}" y1="8" y2="{H - 26}"/>')
        o.append(f'<text class="sub" x="{x(ref) + 4:.1f}" y="{H - 30}">{esc(ref_label)} {ref:.2f}</text>')
    o.append("</svg>")
    return f'<p class="fig-title">{esc(title)}</p><p class="fig-sub">{esc(sub)}</p><div class="scroll">' + "\n".join(o) + "</div>"


def vm_main_panels():
    out = []
    for sc, name in SCEN:
        loc = vlocal(sc)
        rtt = {"zone": "0.4 ms RTT, 10 Gbps", "az": "1 ms RTT, 10 Gbps", "region": "60 ms RTT, 1 Gbps"}[sc]
        nq = int(vmsel(sc, "direct")[0]["n_queries"])
        d = vq(vmsel(sc, "direct"))
        items = []
        for m in ("direct_c1", "direct", "cache", "readahead", "stream", "quickmine"):
            rs = vmsel(sc, m)
            if not rs:
                continue
            note = "" if m == "direct" else f"{d / vq(rs):.2f}×"
            items.append((MODE_NAME[m], MODE_CLS[m], [r["q_mean_ms"] for r in rs], note))
        out.append(bar_panel(f"{name}: guest query latency", f"{rtt} · {nq:,} queries per run · bars are medians, dots are the {len(vmsel(sc, 'direct'))} repetitions · right-hand figure = speedup over direct",
                             items, ref=vq(loc)))
    return out


def penalty_chart():
    """Share of the remote penalty (direct minus local-disk time) that each policy removes."""
    groups = [("Same zone", "zone", "default"), ("Cross zone", "az", "default"), ("Cross region", "region", "default"),
              ("Cross zone, guest readahead off", "az", "0"), ("Cross zone, guest readahead 1 MiB", "az", "1024")]
    series = [("readahead", "s1"), ("stream", "s2"), ("cache", "s3"), ("quickmine", "s4")]
    W, left, right, bh, gap = 760, 230, 70, 14, 4
    lo, hi = -120, 100
    x = lambda p: left + (p - lo) / (hi - lo) * (W - left - right)
    gh = 4 * (bh + gap) + 30
    H = len(groups) * gh + 52
    o = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="Share of the remote penalty removed by each policy">']
    for t in (-120, -80, -40, 0, 40, 80):
        o.append(f'<line class="grid{" base" if t == 0 else ""}" x1="{x(t):.1f}" x2="{x(t):.1f}" y1="4" y2="{H - 32}"/><text class="tick" x="{x(t):.1f}" y="{H - 18}" text-anchor="middle">{t}%</text>')
    o.append(f'<text class="axl" x="{(left + W - right) / 2:.0f}" y="{H - 2}" text-anchor="middle">share of the remote penalty removed (0% = as slow as direct, 100% = as fast as local)</text>')
    for gi, (gname, sc, ra) in enumerate(groups):
        y0 = 8 + gi * gh
        d = vq(vmsel(sc, "direct", ra if ra != "default" else "default"))
        loc = vq(vlocal(sc, ra))
        o.append(f'<text class="grp" x="0" y="{y0 + 12}">{esc(gname)} <tspan class="sub">· direct {d:.2f} ms, local {loc:.2f} ms</tspan></text>')
        k = 0
        for mode, cls in series:
            rs = vmsel(sc, mode, ra)
            if not rs:
                continue
            m = vq(rs)
            pen = 100 * (d - m) / (d - loc)
            y = y0 + 22 + k * (bh + gap)
            xa, xb = sorted((x(0), x(max(lo, min(hi, pen)))))
            o.append(f'<rect class="bar {cls}" x="{xa:.1f}" y="{y}" width="{max(1.5, xb - xa):.1f}" height="{bh}" rx="2"><title>{esc(MODE_NAME[mode])}: {m:.2f} ms/query, removes {pen:.0f}% of the remote penalty</title></rect>')
            tx = xb + 6 if pen >= 0 else xa - 6
            o.append(f'<text class="val" x="{tx:.1f}" y="{y + 11}" text-anchor="{"start" if pen >= 0 else "end"}">{pen:.0f}%</text>')
            k += 1
    o.append("</svg>")
    return "\n".join(o)


def ablation_chart():
    W, left, right, rh = 760, 200, 110, 24
    groups = [("0", "Guest readahead off"), ("128", "Guest readahead 128 KiB (default)"), ("1024", "Guest readahead 1 MiB")]
    H = 14 + len(groups) * (3 * rh + 22) + 36
    lo, hi = 0.7, 60
    x = lambda v: lx(v, lo, hi, left, W - right)
    o = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="Guest query latency by guest readahead setting">']
    for t in (1, 2, 5, 10, 20, 50):
        o.append(f'<line class="grid" x1="{x(t):.1f}" x2="{x(t):.1f}" y1="4" y2="{H - 30}"/><text class="tick" x="{x(t):.1f}" y="{H - 14}" text-anchor="middle">{t}</text>')
    o.append(f'<text class="axl" x="{(left + W - right) / 2:.0f}" y="{H - 1}" text-anchor="middle">mean guest query latency, ms (log scale; further left is faster)</text>')
    for gi, (ra, gname) in enumerate(groups):
        y0 = 12 + gi * (3 * rh + 22)
        o.append(f'<text class="grp" x="0" y="{y0 + 12}">{esc(gname)}</text>')
        loc = vlocal("az", "default" if ra == "128" else ra)
        dr = vmsel("az", "direct", ra if ra != "128" else "128")
        rr = vmsel("az", "readahead", ra if ra != "128" else "128")
        for k, (lab, cls, rs) in enumerate((("Local disk", "loc", loc), ("Direct", "base", dr), ("Cache + readahead", "s1", rr))):
            y = y0 + 22 + k * rh
            vs = [r["q_mean_ms"] for r in rs]
            o.append(f'<text class="rowl" x="{left - 10}" y="{y + 4}" text-anchor="end">{lab}</text>')
            for v in vs:
                o.append(f'<circle class="dot {cls}" cx="{x(v):.1f}" cy="{y}" r="6"><title>{lab}: {v:.2f} ms</title></circle>')
            tag = ""
            if k == 2:
                tag = f' · {vq(dr) / vq(rs):.2f}× vs direct'
            o.append(f'<text class="val" x="{x(max(vs)) + 12:.1f}" y="{y + 4}">{vq(rs):.2f}{tag}</text>')
    o.append("</svg>")
    return "\n".join(o)


def depth_chart(sc, name, rtt):
    depths = [1, 4, 16, 64, 256, 512]
    have = [d for d in depths if vmsel(sc, "readahead", depth=d)]
    W, H, l, r_, t, b = 760, 280, 60, 140, 18, 56
    X = lambda i: l + i * (W - l - r_) / (len(depths) - 1)
    dir_v, loc_v = vq(vmsel(sc, "direct")), vq(vlocal(sc))
    allv = [r["q_mean_ms"] for d in have for r in vmsel(sc, "readahead", depth=d)] + [dir_v, loc_v]
    ymax = max(allv) * 1.12
    Y = lambda v: H - b - v / ymax * (H - b - t)
    o = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="Guest query latency against host readahead depth, {esc(name)}">']
    step = 10 ** math.floor(math.log10(ymax))
    for m in (1, 2, 5):
        if ymax / (step * m) <= 6:
            step *= m
            break
    v = 0
    while v <= ymax:
        o.append(f'<line class="grid{" base" if v == 0 else ""}" x1="{l}" x2="{W - r_}" y1="{Y(v):.1f}" y2="{Y(v):.1f}"/><text class="tick" x="{l - 8}" y="{Y(v) + 4:.1f}" text-anchor="end">{v:g}</text>')
        v += step
    for i, d in enumerate(depths):
        kb = d * 64
        lab = f"{kb} KiB" if kb < 1024 else f"{kb // 1024} MiB"
        o.append(f'<text class="tick" x="{X(i):.1f}" y="{H - b + 16}" text-anchor="middle">{lab}</text><text class="sub" x="{X(i):.1f}" y="{H - b + 30}" text-anchor="middle">{d} pp</text>')
    o.append(f'<text class="axl" x="{(l + W - r_) / 2:.0f}" y="{H - 4}" text-anchor="middle">how far past each guest read the host prefetches (pp = 64 KiB pages; default run = 1 MiB)</text>')
    o.append(f'<text class="axl" transform="translate(14 {(t + H - b) / 2:.0f}) rotate(-90)" text-anchor="middle">ms per query</text>')
    for val, lab, cls in ((dir_v, "direct", "refline"), (loc_v, "local disk", "refline loc")):
        o.append(f'<line class="{cls}" x1="{l}" x2="{W - r_}" y1="{Y(val):.1f}" y2="{Y(val):.1f}"/>')
        o.append(f'<text class="rowl" x="{W - r_ + 8}" y="{Y(val) + 4:.1f}">{lab} {val:.2f}</text>')
    pts = []
    for i, d in enumerate(depths):
        rs = vmsel(sc, "readahead", depth=d)
        if not rs:
            continue
        pts.append((X(i), Y(vq(rs))))
        for r in rs:
            o.append(f'<circle class="rep" cx="{X(i):.1f}" cy="{Y(r["q_mean_ms"]):.1f}" r="2.6"/>')
    o.append('<polyline class="line s1" points="' + " ".join(f"{x:.1f},{y:.1f}" for x, y in pts) + '"/>')
    for i, d in enumerate(depths):
        rs = vmsel(sc, "readahead", depth=d)
        if rs:
            o.append(f'<circle class="dot s1" cx="{X(i):.1f}" cy="{Y(vq(rs)):.1f}" r="6"><title>{d} pages ({d * 64} KiB): {vq(rs):.2f} ms/query, {dir_v / vq(rs):.2f}× vs direct, {vtraffic(rs) / vtraffic(vmsel(sc, "direct")):.1f}× traffic</title></circle>')
    o.append("</svg>")
    return (f'<p class="fig-title">{esc(name)}</p><p class="fig-sub">{esc(rtt)} · line = median of repetitions, small dots = each repetition</p><div class="scroll">' + "\n".join(o) + "</div>")


def depth_charts():
    parts = []
    for sc, name, rtt in (("zone", "Same zone", "0.4 ms RTT"), ("az", "Cross zone", "1 ms RTT"), ("region", "Cross region", "60 ms RTT")):
        if vmsel(sc, "readahead", depth=512):
            parts.append("<figure>" + depth_chart(sc, name, rtt) + "</figure>")
    return "\n".join(parts)


def drift_table():
    out = ['<table><thead><tr><th>Baseline</th><th class="n">Earlier (ms/query)</th><th class="n">Re-measured at the end</th><th class="n">Change</th></tr></thead><tbody>']
    spec = (("Direct, same zone", lambda r: r["mode"] == "direct" and r["scenario"] == "zone" and r["guest_ra_kb"] == "default"),
            ("Direct, cross zone", lambda r: r["mode"] == "direct" and r["scenario"] == "az" and r["guest_ra_kb"] == "default"),
            ("Direct, cross region", lambda r: r["mode"] == "direct" and r["scenario"] == "region" and r["guest_ra_kb"] == "default"),
            ("Local disk, 5,000 queries", lambda r: r["mode"] == "local" and int(r["n_queries"]) == 5000 and r["guest_ra_kb"] == "default"),
            ("Local disk, 2,500 queries", lambda r: r["mode"] == "local" and int(r["n_queries"]) == 2500 and r["guest_ra_kb"] == "default"))
    chg = []
    for lab, f in spec:
        a, b = [r for r in VM_EARLY if f(r)], [r for r in DRIFT if f(r)]
        if not a or not b:
            continue
        c = 100 * (vq(b) / vq(a) - 1)
        chg.append(abs(c))
        out.append(f'<tr><td>{lab}</td><td class="n">{vq(a):.2f}</td><td class="n">{vq(b):.2f}</td><td class="n">{c:+.1f}%</td></tr>')
    out.append("</tbody></table>")
    return "\n".join(out), max(chg)


def diag_table():
    out = ['<table><thead><tr><th>Link</th><th>Configuration (depth 256 unless noted)</th><th class="n">Cache pages</th><th class="n">Reps</th><th class="n">ms/query</th><th class="n">Speedup vs direct</th><th class="n">Reads fully cached</th><th class="n">Demand-read lock wait</th><th class="n">Prefetch lock-hold, share of run</th></tr></thead><tbody>']
    names = {"base": "default cache and link", "bigcache": "4x cache", "nobw": "unlimited bandwidth", "bigcache_nobw": "4x cache, unlimited bandwidth",
             "d16": "depth 16, default cache (the usual setting)", "d16_bigcache": "depth 16, 4x cache"}
    caps = {"base": 4096, "bigcache": 16384, "nobw": 4096, "bigcache_nobw": 16384, "d16": 4096, "d16_bigcache": 16384}
    for sc, sname in (("az", "Cross zone"), ("region", "Cross region")):
        for tag in ("d16", "d16_bigcache", "base", "bigcache", "nobw", "bigcache_nobw"):
            rs = dsel(DIAG, sc, tag)
            if not rs:
                continue
            reads = reads_of(rs)
            lw = f"{1000 * med([r['lock_wait_ms'] for r in rs]) / reads:.0f} µs/read" if rs[0]["lock_wait_ms"] != "" else "–"
            hp = f"{holdpct(DIAG, sc, tag):.0f}%" if rs[0]["bulk_hold_ms"] != "" else "–"
            out.append(f'<tr><td>{sname}</td><td>{names[tag]}</td><td class="n">{caps[tag]:,}</td><td class="n">{len(rs)}</td><td class="n">{vq(rs):.2f}</td>'
                       f'<td class="n">{vq(vmsel(sc, "direct")) / vq(rs):.2f}×</td><td class="n">{hitpct(rs):.0f}%</td><td class="n">{lw}</td><td class="n">{hp}</td></tr>')
    out.append("</tbody></table>")
    return "\n".join(out)


def serial_chart():
    items = []
    for sc, name in SCEN[:2]:
        pass
    parts = []
    for sc, name in (("zone", "Same zone"), ("az", "Cross zone"), ("region", "Cross region")):
        c1, c8, loc = vmsel(sc, "direct_c1"), vmsel(sc, "direct"), vlocal(sc)
        if not c1:
            continue
        r1, r8 = vmsel(sc, "readahead_c1"), vmsel(sc, "readahead")
        parts.append(bar_panel(f"{name}", "mean guest query latency; right-hand figure = how much slower than the overlapped-reads server doing the same thing",
                               [("Direct, one read at a time", "base2", [r["q_mean_ms"] for r in c1], f"{vq(c1) / vq(c8):.2f}×"),
                                ("Direct, reads overlapped", "base", [r["q_mean_ms"] for r in c8], ""),
                                ("Readahead, one read at a time", "s1b", [r["q_mean_ms"] for r in r1], f"{vq(r1) / vq(r8):.2f}×"),
                                ("Readahead, reads overlapped", "s1", [r["q_mean_ms"] for r in r8], "")], ref=vq(loc)))
    return "\n".join(f"<figure>{p}</figure>" for p in parts)


def mk_dotplot(title, sub, rows, xlo, xhi, ticks, xlabel, marks):
    """rows: (label, {mark_key: value}). marks: (key, cls, name)."""
    W, rowh, top, left, right = 760, 34, 24, 236, 40
    H = top + rowh * len(rows) + 36
    x = lambda v: lx(v, xlo, xhi, left, W - right)
    o = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="{esc(title)}">']
    for t in ticks:
        o.append(f'<line class="grid{" base" if t == 1 else ""}" x1="{x(t):.1f}" x2="{x(t):.1f}" y1="{top - 10}" y2="{H - 30}"/><text class="tick" x="{x(t):.1f}" y="{H - 14}" text-anchor="middle">{t:g}×</text>')
    o.append(f'<text class="axl" x="{(left + W - right) / 2:.0f}" y="{H - 1}" text-anchor="middle">{esc(xlabel)}</text>')
    for i, (lab, vals) in enumerate(rows):
        y = top + i * rowh + 8
        o.append(f'<text class="rowl" x="{left - 12}" y="{y + 4}" text-anchor="end">{esc(lab)}</text>')
        best = None
        for key, cls, name in reversed(marks):
            v = vals.get(key)
            if v is None:
                continue
            o.append(f'<circle class="dot {cls}" cx="{x(max(xlo * 1.02, min(v, xhi * 0.98))):.1f}" cy="{y}" r="6"><title>{esc(lab)} · {esc(name)}: {v:.2f}×</title></circle>')
            best = v if best is None else max(best, v)
        if best and best >= 1.5:
            o.append(f'<text class="val" x="{min(x(best) + 11, W - 4):.1f}" y="{y + 4}">{best:.1f}×</text>')
    o.append("</svg>")
    return f'<p class="fig-title">{esc(title)}</p><p class="fig-sub">{esc(sub)}</p><div class="scroll">' + "\n".join(o) + "</div>"


WL = [("seq", "Sequential, 128 KiB reads"), ("seqjump", "Sequential runs + jumps"), ("stride", "Strided 4 KiB / 32 KiB"),
      ("random", "Random 4 KiB"), ("zipf", "Zipf hot set, 4 KiB"), ("faiss", "FAISS IVF search (replay)")]
RMODES = [("readahead", "s1", "Cache + readahead"), ("stream", "s2", "Cache + stream readahead"), ("cache", "s3", "Cache only")]


def replay_dots():
    out = []
    for sc, name in SCEN:
        rows = [(wn, {m: rspeed(sc, wl, m) for m, _, _ in RMODES}) for wl, wn in WL]
        sub = {"zone": "0.4 ms RTT, 10 Gbps", "az": "1 ms RTT, 10 Gbps", "region": "60 ms RTT, 1 Gbps"}[sc]
        out.append("<figure>" + mk_dotplot(f"{name} (replay, one reader at a time)", sub, rows, 0.5, 30, (0.5, 1, 2, 5, 10, 20),
                                            "total read wait vs direct access (log scale; right of 1× = faster)", RMODES) + "</figure>")
    return "\n".join(out)


def faiss_compare():
    rows = []
    for sc, name in SCEN:
        v = {}
        v["replay"] = rspeed(sc, "faiss", "readahead")
        ra, d8, d1, ra1 = vmsel(sc, "readahead"), vmsel(sc, "direct"), vmsel(sc, "direct_c1"), vmsel(sc, "readahead_c1")
        v["vm8"] = vq(d8) / vq(ra)
        if d1 and ra1:
            v["vm1"] = vq(d1) / vq(ra1)
        rows.append((name, v))
    marks = [("replay", "mkA", "Replay: reads one at a time"), ("vm1", "mkC", "VM, serial server both ways"), ("vm8", "mkB", "VM, overlapped reads both ways")]
    return mk_dotplot("Readahead speedup on FAISS: replay vs a real guest", "same workload, same links, same readahead policy; only the baseline and the guest layer differ",
                      rows, 0.5, 12, (0.5, 1, 2, 4, 8), "speedup over the matching direct baseline (log scale)", marks), marks


def think_chart():
    W, H, l, r_, t, b = 760, 300, 56, 170, 16, 46
    xs = [0, 200, 2000]
    X = lambda i: l + i * (W - l - r_) / 2
    allp = [rspeed("az", wl, "readahead", th) for wl in ("seq", "seqjump") for th in xs]
    ymax = math.ceil(max(allp) * 1.08 / 5) * 5
    Y = lambda v: H - b - v / ymax * (H - b - t)
    o = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="Readahead speedup against guest think time">']
    for v in range(0, ymax + 1, 5):
        o.append(f'<line class="grid{" base" if v == 0 else ""}" x1="{l}" x2="{W - r_}" y1="{Y(v):.1f}" y2="{Y(v):.1f}"/><text class="tick" x="{l - 8}" y="{Y(v) + 4:.1f}" text-anchor="end">{v}×</text>')
    for i, v in enumerate(xs):
        o.append(f'<text class="tick" x="{X(i):.1f}" y="{H - b + 18}" text-anchor="middle">{v:,} µs</text>')
    o.append(f'<text class="axl" x="{(l + W - r_) / 2:.0f}" y="{H - 6}" text-anchor="middle">time the reader spends between reads</text>')
    ends = []
    dash = {"seq": "", "seqjump": "7 4"}
    names = {"seq": "sequential", "seqjump": "seq + jumps"}
    for wl in ("seq", "seqjump"):
        pts = [rspeed("az", wl, "readahead", th) for th in xs]
        p = " ".join(f"{X(i):.1f},{Y(v):.1f}" for i, v in enumerate(pts))
        o.append(f'<polyline class="line s1" stroke-dasharray="{dash[wl]}" points="{p}"/>')
        for i, v in enumerate(pts):
            o.append(f'<circle class="dot s1" cx="{X(i):.1f}" cy="{Y(v):.1f}" r="5"><title>{names[wl]}, think {xs[i]} µs: {v:.2f}×</title></circle>')
        ends.append((Y(pts[-1]), f"{names[wl]} {pts[-1]:.1f}×"))
    ends.sort()
    for i in range(1, len(ends)):  # keep end labels at least 16 px apart
        if ends[i][0] - ends[i - 1][0] < 16:
            ends[i] = (ends[i - 1][0] + 16, ends[i][1])
    for yy, lab in ends:
        o.append(f'<text class="rowl" x="{X(2) + 12:.1f}" y="{yy + 4:.1f}">{lab}</text>')
    o.append("</svg>")
    return "\n".join(o)


def ladder():
    items = [("Same rack (not modeled)", 0.05, "fabric, 5–200 µs one way", "dim"), ("Same zone", 0.4, "median ≈ 0.36 ms", "s1"),
             ("Cross zone", 1.0, "≈ 0.4–2 ms", "s1"), ("Cross region (US)", 60, "≈ 40–80 ms", "s1")]
    W, left, right = 760, 190, 200
    H = 30 + 42 * len(items) + 36
    lo, hi = 0.03, 150
    x = lambda v: lx(v, lo, hi, left, W - right)
    o = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="Round-trip latency by distance">']
    for v in (0.1, 1, 10, 100):
        o.append(f'<line class="grid" x1="{x(v):.1f}" x2="{x(v):.1f}" y1="6" y2="{H - 34}"/><text class="tick" x="{x(v):.1f}" y="{H - 18}" text-anchor="middle">{v:g} ms</text>')
    o.append(f'<text class="axl" x="{(left + W - right) / 2:.0f}" y="{H - 2}" text-anchor="middle">round-trip time, log scale</text>')
    for i, (n, v, note, cls) in enumerate(items):
        y = 18 + i * 42
        o.append(f'<text class="rowl" x="{left - 12}" y="{y + 12}" text-anchor="end">{esc(n)}</text>')
        o.append(f'<rect class="bar {cls}" x="{left}" y="{y}" width="{x(v) - left:.1f}" height="18" rx="3"/>')
        lab = f"{v * 1000:g} µs" if v < 0.1 else f"{v:g} ms"
        o.append(f'<text class="val" x="{x(v) + 8:.1f}" y="{y + 13}">{"used: " if cls != "dim" else ""}{lab} · <tspan class="sub">{esc(note)}</tspan></text>')
    o.append("</svg>")
    return "\n".join(o)


def diagram():
    return '''<svg viewBox="0 0 760 330" role="img" aria-label="The layers between the application and the remote disk">
<defs><marker id="ar" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" markerHeight="7" orient="auto"><path d="M0 0L8 4L0 8z" class="arrowhead"/></marker></defs>
<text class="grp" x="0" y="16">A · Direct</text>
<rect class="box guest" x="0" y="26" width="270" height="44" rx="6"/><text class="boxt" x="135" y="46" text-anchor="middle">Guest VM</text><text class="sub" x="135" y="62" text-anchor="middle">page cache + its own readahead</text>
<rect class="box" x="320" y="26" width="120" height="44" rx="6"/><text class="boxt" x="380" y="53" text-anchor="middle">nbd_server</text>
<rect class="box remote" x="600" y="26" width="160" height="44" rx="6"/><text class="boxt" x="680" y="53" text-anchor="middle">Remote disk</text>
<line class="arrow" x1="270" y1="48" x2="318" y2="48" marker-end="url(#ar)"/>
<line class="arrow link" x1="440" y1="48" x2="598" y2="48" marker-end="url(#ar)"/>
<text class="sub" x="519" y="40" text-anchor="middle">every guest read</text>
<text class="grp" x="0" y="118">B · Local disk cache + readahead in front of the remote disk</text>
<rect class="box guest" x="0" y="128" width="270" height="44" rx="6"/><text class="boxt" x="135" y="148" text-anchor="middle">Guest VM</text><text class="sub" x="135" y="164" text-anchor="middle">page cache + its own readahead</text>
<rect class="box" x="320" y="128" width="120" height="44" rx="6"/><text class="boxt" x="380" y="155" text-anchor="middle">nbd_server</text>
<rect class="box cache" x="470" y="128" width="110" height="44" rx="6"/><text class="boxt" x="525" y="148" text-anchor="middle">Local cache</text><text class="sub" x="525" y="164" text-anchor="middle">LRU + policy</text>
<rect class="box remote" x="620" y="128" width="140" height="44" rx="6"/><text class="boxt" x="690" y="155" text-anchor="middle">Remote disk</text>
<line class="arrow" x1="270" y1="150" x2="318" y2="150" marker-end="url(#ar)"/>
<line class="arrow" x1="440" y1="150" x2="468" y2="150" marker-end="url(#ar)"/>
<line class="arrow link" x1="580" y1="140" x2="618" y2="140" marker-end="url(#ar)"/>
<line class="arrow link dashed" x1="580" y1="162" x2="618" y2="162" marker-end="url(#ar)"/>
<text class="sub" x="600" y="124" text-anchor="middle">miss</text><text class="sub" x="600" y="186" text-anchor="middle">prefetch</text>
<text class="grp" x="0" y="232">Evidence from two kinds of experiment</text>
<rect class="box" x="0" y="246" width="360" height="70" rx="6"/><text class="boxt" x="180" y="268" text-anchor="middle">Replay (no VM)</text><text class="sub" x="180" y="286" text-anchor="middle">a host process issues the guest's recorded reads</text><text class="sub" x="180" y="302" text-anchor="middle">straight to the reader; one read at a time</text>
<rect class="box guest" x="400" y="246" width="360" height="70" rx="6"/><text class="boxt" x="580" y="268" text-anchor="middle">VM in the loop</text><text class="sub" x="580" y="286" text-anchor="middle">a QEMU guest runs the real FAISS search; its page cache,</text><text class="sub" x="580" y="302" text-anchor="middle">readahead and pipelined reads all take part</text>
</svg>'''


# ------------------------------------------------------------------ tables
def table_replay():
    out = ['<table><thead><tr><th>Link</th><th>Workload</th><th>Mode</th><th class="n">Reps</th><th class="n">Total wait (s), median</th><th class="n">min–max</th><th class="n">Speedup</th><th class="n">Link traffic ÷ read</th><th class="n">Reads with no wait</th></tr></thead><tbody>']
    for sc, sname in SCEN:
        for wl, wn in WL:
            d = rm(sc, wl, "direct")
            for m in ("direct", "cache", "readahead", "stream", "quickmine"):
                rs = rsel(R, sc, wl, m)
                if not rs:
                    continue
                vs = [r["io_wait_s"] for r in rs]
                hit = "–" if m == "direct" else f"{100 * med([r['hit_requests'] for r in rs]) / rs[0]['requests']:.0f}%"
                out.append(f'<tr><td>{esc(sname)}</td><td>{esc(wn)}</td><td>{m}</td><td class="n">{len(rs)}</td><td class="n">{med(vs):.2f}</td><td class="n">{min(vs):.2f}–{max(vs):.2f}</td>'
                           f'<td class="n">{d / med(vs):.2f}×</td><td class="n">{ramp(sc, wl, m):.1f}×</td><td class="n">{hit}</td></tr>')
    out.append("</tbody></table>")
    return "\n".join(out)


def dstr(r):
    v = r.get("readahead_pages", "")
    return "" if v in ("", None) else str(int(float(v)))


def table_vm():
    out = ['<table><thead><tr><th>Link</th><th>Guest RA</th><th>Mode</th><th class="n">RA pages</th><th class="n">Queries</th><th class="n">Reps</th><th class="n">ms/query, median</th><th class="n">min–max</th><th class="n">p99 ms</th><th class="n">Speedup vs direct</th><th class="n">Link MB</th><th class="n">Prefetch used</th></tr></thead><tbody>']
    seen = []
    for r in VM:
        key = (r["scenario"], str(r["guest_ra_kb"]), r["mode"], int(r["n_queries"]), dstr(r))
        if key not in seen:
            seen.append(key)
    order = {m: i for i, m in enumerate(("local", "direct_c1", "direct", "cache", "readahead_c1", "readahead", "stream", "quickmine"))}
    scn = {"-": 0, "zone": 1, "az": 2, "region": 3}
    seen.sort(key=lambda k: (k[1] != "default", {"default": 0, "0": 1, "128": 2, "1024": 3}.get(k[1], 9), scn.get(k[0], 9), order[k[2]], int(k[4] or 0)))
    for sc, ra, m, nq, dp in seen:
        rs = [r for r in VM if (r["scenario"], str(r["guest_ra_kb"]), r["mode"], int(r["n_queries"]), dstr(r)) == (sc, ra, m, nq, dp)]
        vs = [r["q_mean_ms"] for r in rs]
        if m == "local":
            link, sp, tr, used = "none", "–", "0", "–"
        else:
            d = vmsel(sc, "direct", ra, nq)
            link, sp, tr = sc, f"{vq(d) / med(vs):.2f}×" if d else "–", f"{vtraffic(rs):.0f}"
            iss = med([r["pf_issued"] or 0 for r in rs])
            used = f"{100 * med([r['pf_used'] or 0 for r in rs]) / iss:.0f}%" if iss else "–"
        out.append(f'<tr><td>{link}</td><td class="n">{ra}</td><td>{m}</td><td class="n">{dp or "–"}</td><td class="n">{nq:,}</td><td class="n">{len(rs)}</td><td class="n">{med(vs):.2f}</td><td class="n">{min(vs):.2f}–{max(vs):.2f}</td>'
                   f'<td class="n">{med([r["q_p99_ms"] for r in rs]):.1f}</td><td class="n">{sp}</td><td class="n">{tr}</td><td class="n">{used}</td></tr>')
    out.append("</tbody></table>")
    return "\n".join(out)


def table_calib():
    out = ['<table><thead><tr><th>Link</th><th class="n">Model 4 KiB</th><th class="n">Measured</th><th class="n">Model 128 KiB</th><th class="n">Measured</th></tr></thead><tbody>']
    names = {"zero": "No delay at all (software cost only)", "zone": "Same zone", "az": "Cross zone", "region": "Cross region"}
    for k, v in CAL["links"].items():
        out.append(f'<tr><td>{names[k]}</td><td class="n">{v["4K"]["model_us"]:,.0f} µs</td><td class="n">{v["4K"]["measured_us"]:,.0f} µs</td>'
                   f'<td class="n">{v["128K"]["model_us"]:,.0f} µs</td><td class="n">{v["128K"]["measured_us"]:,.0f} µs</td></tr>')
    out.append("</tbody></table>")
    return "\n".join(out)


# ------------------------------------------------------------------ numbers quoted in the prose
def pen(sc, mode, ra="default"):
    d, m, loc = vq(vmsel(sc, "direct", ra)), vq(vmsel(sc, mode, ra)), vq(vlocal(sc, ra))
    return 100 * (d - m) / (d - loc)


def sp(sc, mode, ra="default", base="direct"):
    return vq(vmsel(sc, base, ra)) / vq(vmsel(sc, mode, ra))


DEPTHS = [1, 4, 16, 64, 256, 512]
LINKBW = {"zone": 10000, "az": 10000, "region": 1000}
RTTMS = {"zone": 0.4, "az": 1.0, "region": 60.0}


def dep(sc, d):
    return vmsel(sc, "readahead", depth=d)


def dsel(rows, sc, tag):
    return [r for r in rows if r["scenario"] == sc and r["tag"] == tag]


def dsp(sc, tag, rows=None):
    rs = dsel(DIAG if rows is None else rows, sc, tag)
    return vq(vmsel(sc, "direct")) / vq(rs) if rs else float("nan")


def dstat(rows, sc, tag, key, per="read"):
    rs = dsel(rows, sc, tag)
    v = med([r[key] for r in rs])
    return v


def reads_of(rs):
    """Median NBD read count of a diagnostic group. One run lost its STATS line, so blanks are skipped."""
    return med([r["nbd_reads"] for r in rs if r["nbd_reads"] != ""])


def lockwait_us(rows, sc, tag):
    rs = dsel(rows, sc, tag)
    return 1000 * med([r["lock_wait_ms"] for r in rs]) / reads_of(rs)


def holdpct(rows, sc, tag):
    rs = dsel(rows, sc, tag)
    return 100 * med([r["bulk_hold_ms"] for r in rs]) / (1000 * med([r["query_sum_s"] for r in rs]))


def hitpct(rs, key="hit_pages"):
    return 100 * med([r[key] for r in rs]) / med([r["hit_pages"] + r["late_pages"] + r["miss_pages"] for r in rs])


def linkutil(sc, rs):
    mb = (med([r["remote_demand_bytes"] for r in rs]) + med([r["remote_bulk_bytes"] for r in rs])) / 1e6
    return 100 * mb * 8 / med([r["query_sum_s"] for r in rs]) / LINKBW[sc]


def need_mib(sc):
    n = 2500 if sc == "region" else 5000
    d = vmsel(sc, "direct")
    kib_per_query = med([r["remote_demand_bytes"] for r in d]) / n / 1024
    rate = kib_per_query / vq(vlocal(sc))          # KiB/ms the guest consumes at local-disk speed
    return rate * (RTTMS[sc] + 0.1) / 1024


def best_depth(sc):
    return min([d for d in DEPTHS if dep(sc, d)], key=lambda d: vq(dep(sc, d)))


# guest's NBD request stream: how many reads arrive with another already queued
feat = [r for r in csv.DictReader(open(P("logs/capture/base/features.csv"))) if r["type"] == "0" and int(r["req"]) >= 0]
queued_pct = 100 * sum(1 for r in feat if int(r["queued_bytes"]) > 0) / len(feat)

rnd_az = rsel(R, "az", "random", "readahead")
rnd_hit = 100 * med([r["hit_requests"] for r in rnd_az]) / rnd_az[0]["requests"]
cover = 100 * rnd_az[0]["capacity"] * rnd_az[0]["page_size"] / (1 << 30)
fa_vm_ra = vmsel("az", "readahead")
fa_iss = med([r["pf_issued"] for r in fa_vm_ra])
abl_dir0, abl_ra0 = vmsel("az", "direct", "0"), vmsel("az", "readahead", "0")
abl_dir1k = vmsel("az", "direct", "1024")
abl_dir128 = vmsel("az", "direct", "128")
old_stream_amp = ramp("az", "faiss", "stream", rows=OLD)
old_stream_sp = (rm("az", "faiss", "direct", rows=OLD) / rm("az", "faiss", "stream", rows=OLD))
cache_seq_extra = (rm("zone", "seq", "cache", "mean_us") - rm("zone", "seq", "direct", "mean_us"))
integ = {r["mode"]: r["result_sha"] for r in INT}
qm_synth = [r["pf_issued_pages"] for r in R if r["label"] == "quickmine" and r["workload"] != "faiss"]
qm_faiss_az = rsel(R, "az", "faiss", "quickmine")
old_zone_128 = rm("zone", "seq", "direct", "mean_us", rows=OLD)
new_zone_128 = rm("zone", "seq", "direct", "mean_us")
tot_pages = lambda rs: med([r["hit_pages"] + r["late_pages"] + r["miss_pages"] for r in rs])

N = dict(
    n_vm=len(VM), n_replay=len(R),
    loc_z=vq(vlocal("zone")), d_z=vq(vmsel("zone", "direct")), d_a=vq(vmsel("az", "direct")), d_r=vq(vmsel("region", "direct")),
    loc_a=vq(vlocal("az")), loc_r=vq(vlocal("region")),
    ra_z=vq(vmsel("zone", "readahead")), ra_a=vq(vmsel("az", "readahead")), ra_r=vq(vmsel("region", "readahead")),
    sp_ra_z=sp("zone", "readahead"), sp_ra_a=sp("az", "readahead"), sp_ra_r=sp("region", "readahead"),
    sp_ca_z=sp("zone", "cache"), sp_ca_a=sp("az", "cache"), sp_ca_r=sp("region", "cache"),
    sp_st_z=sp("zone", "stream"), sp_st_a=sp("az", "stream"), sp_st_r=sp("region", "stream"),
    pen_ra_z=pen("zone", "readahead"), pen_ra_a=pen("az", "readahead"), pen_ra_r=pen("region", "readahead"),
    gap_z=vq(vmsel("zone", "direct")) / vq(vlocal("zone")), gap_a=vq(vmsel("az", "direct")) / vq(vlocal("az")),
    c1_z=vq(vmsel("zone", "direct_c1")), c1_a=vq(vmsel("az", "direct_c1")),
    c1_ratio_z=vq(vmsel("zone", "direct_c1")) / vq(vmsel("zone", "direct")), c1_ratio_a=vq(vmsel("az", "direct_c1")) / vq(vmsel("az", "direct")),
    queued_pct=queued_pct,
    amp_ra=vtraffic(vmsel("az", "readahead")) / vtraffic(vmsel("az", "direct")),
    amp_st=vtraffic(vmsel("az", "stream")) / vtraffic(vmsel("az", "direct")),
    used_ra=100 * med([r["pf_used"] for r in fa_vm_ra]) / fa_iss,
    used_st=100 * med([r["pf_used"] for r in vmsel("az", "stream")]) / med([r["pf_issued"] for r in vmsel("az", "stream")]),
    abl_dir0=vq(abl_dir0), abl_ra0=vq(abl_ra0), abl_loc0=vq(vlocal("az", "0")), abl_sp0=vq(abl_dir0) / vq(abl_ra0),
    abl_pen0=pen("az", "readahead", "0"),
    abl_dir1k=vq(abl_dir1k), abl_ra1k=vq(vmsel("az", "readahead", "1024")), abl_sp1k=sp("az", "readahead", "1024"), abl_loc1k=vq(vlocal("az", "1024")),
    abl_sp128=sp("az", "readahead", "128"),
    abl_reads0=med([r["nbd_reads"] / r["n_queries"] for r in abl_dir0]),
    abl_reads128=med([r["nbd_reads"] / r["n_queries"] for r in abl_dir128]),
    abl_reads1k=med([r["nbd_reads"] / r["n_queries"] for r in abl_dir1k]),
    abl_mb128=vtraffic(abl_dir128), abl_mb1k=vtraffic(abl_dir1k),
    rep_f_z=rspeed("zone", "faiss", "readahead"), rep_f_a=rspeed("az", "faiss", "readahead"), rep_f_r=rspeed("region", "faiss", "readahead"),
    vm1_z=vq(vmsel("zone", "direct_c1")) / vq(vmsel("zone", "readahead_c1")), vm1_a=vq(vmsel("az", "direct_c1")) / vq(vmsel("az", "readahead_c1")),
    vm1_r=vq(vmsel("region", "direct_c1")) / vq(vmsel("region", "readahead_c1")),
    c1_ratio_r=vq(vmsel("region", "direct_c1")) / vq(vmsel("region", "direct")),
    ra_c1_ratio_r=vq(vmsel("region", "readahead_c1")) / vq(vmsel("region", "readahead")),
    win_d=WIN["direct"]["io_wait_s"], win_r=WIN["readahead"]["io_wait_s"], win_sp=WIN["direct"]["io_wait_s"] / WIN["readahead"]["io_wait_s"],
    vm_c1_d_sum=vq(vmsel("region", "direct_c1"), "query_sum_s"), vm_c1_r_sum=vq(vmsel("region", "readahead_c1"), "query_sum_s"),
    vm_c8_r_sum=vq(vmsel("region", "readahead"), "query_sum_s"), vm_c8_d_sum=vq(vmsel("region", "direct"), "query_sum_s"),
    mid_sp=rspeed("region", "faiss", "readahead"),
    gap_lo=100 * (1 - vq(vmsel("zone", "direct_c1")) / vq(vmsel("zone", "readahead_c1")) / rspeed("zone", "faiss", "readahead")),
    gap_hi=100 * (1 - vq(vmsel("az", "direct_c1")) / vq(vmsel("az", "readahead_c1")) / rspeed("az", "faiss", "readahead")),
    seq_z=rspeed("zone", "seq", "readahead"), seq_a=rspeed("az", "seq", "readahead"), seq_r=rspeed("region", "seq", "readahead"),
    rnd_z=rspeed("zone", "random", "readahead"), rnd_amp=ramp("az", "random", "readahead"),
    rnd_st_z=rspeed("zone", "random", "stream"), rnd_st_amp=ramp("az", "random", "stream"),
    rnd_hit=rnd_hit, cover=cover,
    stream_amp_new=ramp("az", "faiss", "stream"), stream_amp_old=old_stream_amp, stream_sp_new=rspeed("az", "faiss", "stream"), stream_sp_old=old_stream_sp,
    cache_extra=cache_seq_extra, pwrite=CAL["pwrite_128k_us"],
    old_z=old_zone_128, new_z=new_zone_128,
    qm_synth_max=int(max(qm_synth)), qm_f_iss=int(med([r["pf_issued_pages"] for r in qm_faiss_az])), qm_f_used=int(med([r["pf_used_pages"] for r in qm_faiss_az])),
    qm_vm_iss=int(med([r["pf_issued"] for r in vmsel("az", "quickmine")])), qm_vm_used=int(med([r["pf_used"] for r in vmsel("az", "quickmine")])),
    qm_vm_sp=sp("az", "quickmine"),
    integ_n=len(integ), integ_distinct=len(set(integ.values())),
    mism=mism,
    loc0_serial=med([r["q_mean_ms"] for r in OLDLOC if str(r["guest_ra_kb"]).replace(".0", "") == "0"]), loc0_conc=vq(vlocal("az", "0")),
    locd_serial=med([r["q_mean_ms"] for r in OLDLOC if r["guest_ra_kb"] == "default" and int(r["n_queries"]) == 5000]), locd_conc=vq(vlocal("zone")),
    rep_over_vm1_z=100 * (rspeed("zone", "faiss", "readahead") / (vq(vmsel("zone", "direct_c1")) / vq(vmsel("zone", "readahead_c1"))) - 1),
    rep_over_vm1_a=100 * (rspeed("az", "faiss", "readahead") / (vq(vmsel("az", "direct_c1")) / vq(vmsel("az", "readahead_c1"))) - 1),
    drift_max=drift_table()[1],
    dc_min=min(sp(sc, m) for sc in ("zone", "az") for m in ("cache", "readahead", "stream", "quickmine")),
    dc_max=max(sp(sc, m) for sc in ("zone", "az") for m in ("cache", "readahead", "stream", "quickmine")),
    used16_region_need=round(100 * 1.0 / need_mib("region")),
    rep_over_vm1_r=100 * (rspeed("region", "faiss", "readahead") / (vq(vmsel("region", "direct_c1")) / vq(vmsel("region", "readahead_c1"))) - 1),
    **{f"best_d_{sc}": best_depth(sc) for sc in ("zone", "az", "region")},
    **{f"best_sp_{sc}": vq(vmsel(sc, "direct")) / vq(dep(sc, best_depth(sc))) for sc in ("zone", "az", "region")},
    **{f"sp_d{d}_{sc}": vq(vmsel(sc, "direct")) / vq(dep(sc, d)) for sc in ("zone", "az", "region") for d in DEPTHS},
    **{f"need_{sc}": need_mib(sc) for sc in ("zone", "az", "region")},
    **{f"hit16_{sc}": hitpct(dep(sc, 16)) for sc in ("zone", "az", "region")},
    **{f"hit256_{sc}": hitpct(dep(sc, 256)) for sc in ("zone", "az", "region")},
    **{f"miss16_{sc}": hitpct(dep(sc, 16), "miss_pages") for sc in ("zone", "az", "region")},
    **{f"miss256_{sc}": hitpct(dep(sc, 256), "miss_pages") for sc in ("zone", "az", "region")},
    **{f"util16_{sc}": linkutil(sc, dep(sc, 16)) for sc in ("zone", "az", "region")},
    **{f"util256_{sc}": linkutil(sc, dep(sc, 256)) for sc in ("zone", "az", "region")},
    **{f"used16_{sc}": 100 * med([r["pf_used"] for r in dep(sc, 16)]) / med([r["pf_issued"] for r in dep(sc, 16)]) for sc in ("zone", "az", "region")},
    **{f"used256_{sc}": 100 * med([r["pf_used"] for r in dep(sc, 256)]) / med([r["pf_issued"] for r in dep(sc, 256)]) for sc in ("zone", "az", "region")},
    **{f"wasted256_{sc}": 100 * med([r["pf_wasted"] for r in dep(sc, 256)]) / med([r["pf_issued"] for r in dep(sc, 256)]) for sc in ("zone", "az", "region")},
    **{f"amp256_{sc}": vtraffic(dep(sc, 256)) / vtraffic(vmsel(sc, "direct")) for sc in ("zone", "az", "region")},
    **{f"amp16_{sc}": vtraffic(dep(sc, 16)) / vtraffic(vmsel(sc, "direct")) for sc in ("zone", "az", "region")},
    dg_big_az=dsp("az", "bigcache"), dg_nobw_az=dsp("az", "nobw"), dg_bn_az=dsp("az", "bigcache_nobw"), dg_base_az=dsp("az", "base"), dg_d16big_az=dsp("az", "d16_bigcache"),
    dg_big_region=dsp("region", "bigcache"), dg_nobw_region=dsp("region", "nobw"), dg_bn_region=dsp("region", "bigcache_nobw"), dg_base_region=dsp("region", "base"), dg_d16big_region=dsp("region", "d16_bigcache"),
    dg_hit_base_az=hitpct(dsel(DIAG, "az", "base")), dg_hit_big_az=hitpct(dsel(DIAG, "az", "bigcache")),
    dg_hit_base_region=hitpct(dsel(DIAG, "region", "base")), dg_hit_big_region=hitpct(dsel(DIAG, "region", "bigcache")),
    lw16=lockwait_us(DIAG, "az", "d16"), lw256=lockwait_us(DIAG, "az", "base"), hold16=holdpct(DIAG, "az", "d16"), hold256=holdpct(DIAG, "az", "base"),
    lw16_old=lockwait_us(DIAG_OLD, "az", "d16"), lw256_old=lockwait_us(DIAG_OLD, "az", "base"), hold16_old=holdpct(DIAG_OLD, "az", "d16"), hold256_old=holdpct(DIAG_OLD, "az", "base"),
    lwq256=lockwait_us(DIAG, "az", "base") * reads_of(dsel(DIAG, "az", "base")) / 5000 / 1000,
    q256_az=vq(dsel(DIAG, "az", "base")), q16_az=vq(dsel(DIAG, "az", "d16")),
    ovr_us=CAL["sleep_overshoot_us"]["100"]["median"],
    cal_z4=CAL["links"]["zone"]["4K"]["measured_us"], cal_z4m=CAL["links"]["zone"]["4K"]["model_us"],
    cal_z128=CAL["links"]["zone"]["128K"]["measured_us"], cal_z128m=CAL["links"]["zone"]["128K"]["model_us"],
    cal_0=CAL["links"]["zero"]["4K"]["measured_us"], cal_0b=CAL["links"]["zero"]["128K"]["measured_us"],
    cal_r4=CAL["links"]["region"]["4K"]["measured_us"], cal_r4m=CAL["links"]["region"]["4K"]["model_us"],
    think0=rspeed("az", "seq", "readahead", 0), think2k=rspeed("az", "seq", "readahead", 2000),
    vm_q_reads=med([r["nbd_reads"] / r["n_queries"] for r in vmsel("az", "direct")]),
    vm_mb_per_read=vtraffic(vmsel("az", "direct")) / med([r["nbd_reads"] for r in vmsel("az", "direct")]) * 1000,
)

TEMPLATE = open(P("disk/fwd_report.template.html")).read()
fa_fig, _ = faiss_compare()
subs = {
    "{{DIAGRAM}}": diagram(), "{{LADDER}}": ladder(), "{{PENALTY}}": penalty_chart(), "{{ABLATION}}": ablation_chart(),
    "{{VMPANELS}}": "".join(f"<figure>{p}</figure>" for p in vm_main_panels()), "{{SERIAL}}": serial_chart(), "{{DEPTH}}": depth_charts(), "{{DIAGTABLE}}": diag_table(), "{{DRIFTTABLE}}": drift_table()[0],
    "{{REPLAYDOTS}}": replay_dots(), "{{FAISSCMP}}": fa_fig, "{{THINK}}": think_chart(),
    "{{TABLE_REPLAY}}": table_replay(), "{{TABLE_VM}}": table_vm(), "{{TABLE_CAL}}": table_calib(),
    "{{LEG_MODES}}": legend([("Direct", "base"), ("Direct, serial server", "base2"), ("Cache only", "s3"), ("Cache + readahead", "s1"), ("Cache + stream readahead", "s2"), ("Cache + QuickMine", "s4")]),
    "{{LEG_PEN}}": legend([("Cache + readahead", "s1"), ("Cache + stream readahead", "s2"), ("Cache only", "s3"), ("Cache + QuickMine", "s4")]),
    "{{LEG_ABL}}": legend([("Local disk", "loc"), ("Direct", "base"), ("Cache + readahead", "s1")]),
    "{{LEG_REPLAY}}": legend([("Cache + readahead", "s1"), ("Cache + stream readahead", "s2"), ("Cache only", "s3")]),
    "{{LEG_CMP}}": '<div class="legend"><span><i class="sw mkA"></i>Replay (one read at a time)</span><span><i class="sw mkC"></i>VM, serial server both ways</span><span><i class="sw mkB"></i>VM, overlapped reads both ways</span></div>',
}
html_out = TEMPLATE
for k, v in subs.items():
    html_out = html_out.replace(k, v)
for k, v in N.items():
    html_out = html_out.replace("{{" + k + "}}", f"{v:.2f}" if isinstance(v, float) and abs(v) < 10 else (f"{v:.1f}" if isinstance(v, float) else str(v)))
left = [s for s in html_out.split("{{")[1:2]]
assert not left, left
open(OUT, "w").write(html_out)
print("wrote", OUT, len(html_out) // 1024, "KiB")
