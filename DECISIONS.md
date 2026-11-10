# DECISIONS.md

Lightweight ADR log. Newest first. Add an entry whenever a choice is made
that a future reader would otherwise have to reverse-engineer from a diff.

## Template

```
## YYYY-MM-DD — Short title

**Decision:** what was chosen.
**Why:** the constraint/observation that drove it.
**Alternative(s) considered:** what else was on the table, and why not.
**Reference:** commit hash / file:line.
```

---

## 2026-11-09 — Adversarial guest workloads: learned loses to block-correlation mining on repeated cold app launches

**Decision:** keep `learned` as the default. Record repeated cold application launches as its one clean loss, and
`learned+cminer` as the direction to pursue for that case (not yet a default). New guest workloads go through
`run_capture.py --workload cmd` (generic shell workload, optional read-write data disk) and are always captured from a
real guest, never replayed directly from an application trace.
**Why:** 11 new guest workloads were tested at matched traffic against every published port and the older storage
prefetchers: the Linux 6.6 tree with grep/tar/parallel md5sum/mixed, YCSB on RocksDB (load/C/E, 2 GiB and 1.5 GiB
guests), YCSB A/B on SQLite, and 20 rounds of cold launches from the guest's Debian root disk.
On the app-launch root-disk stream, with a per-VM host cache (16–128 MiB) smaller than the 156 MiB launch working
set, the repo's simplified C-Miner-style miner gets 87.7% full hit at 1.02× traffic against `learned`'s 84.2% at
1.98× (32 MiB cache, delay 0). 9% of requests jump more than 16 MiB, beyond `learned`'s reach, and 94% of those
jumps recur. The union gets 92–97% at 2×. At delay 2 the miner only wins below about 1.5×.
RocksDB YCSB-C depends on the metric: the miner leads on full hit, `learned` on page hit, and the union leads on both
by 6–14 points.
Everything else was a tie, a `learned` win, or the known traffic floor. The Linux-tree "losses" were an artifact of
the theta range: sweep `learned` down to theta=1.
**Alternative(s) considered:** making `learned+cminer` the default. It costs 0.1–1.2 points at 2× and 5–14 points at
≤1.5× on FAISS/Thesios, so it needs the miner restricted to long jumps or a shared traffic budget first. TPC-H on
SQLite was not run: building `dbgen` from source was blocked by the session's permission rules.
**Reference:** `capture_results/ADVERSARIAL_WORKLOADS.md`, `tools/capture/{build_dir_disk.py,new_streams.sh,split_phases.py,workloads/}`,
`tools/pf/adv_report.py`; `run_capture.py` now puts the NBD write overlay in the run directory, because a RocksDB
load filled the `/tmp` tmpfs.

## 2026-11-09 — Published prefetchers ported to the disk setting: none beats `learned` above ~1.5× traffic

**Decision:** added block-level ports of BOP, SPP, SPP+PPF, Bingo, Pythia and Leap (`policies/policy_lit.cpp`) plus
a PPF-style filter hybrid (`learnedppf`), and keep `learned` unchanged.
**Why:** at 2× traffic (mean of 11 FAISS + Thesios streams) `learned` scores 77.6 at delay 0 and 73.4 at delay 2,
against readahead 74.6 / 69.1, BOP 73.9 / 65.3, Bingo 67.3 / 61.4, SPP and PPF 60.4 / 51.5, Leap 59.8 / 53.6 and
Pythia 36.3 / 33.8. The ranking holds with a 64 MiB cache.
The hardware designs are accurate but don't reach far enough ahead: one line at a time, region-bounded, no PC. BOP
converges to offset 1, which is readahead.
Unions and filters added nothing beyond noise: the best is `learned+bingo` at +0.5, and the PPF-style filter costs
3–4 points.
Below about 1.5× traffic, readahead of 2–4 pages wins, because `learned` can't go below about 1.4× even at theta=99
(nextreq runs at theta/4).
**Alternative(s) considered:** degree>1 BOP (an extension, not the published design); SPP without region crossing;
Pythia with 10× α and high-bandwidth rewards; address-free PPF features. All are reported in the write-up.
**Reference:** `capture_results/PUBLISHED_PREFETCHERS.md`, `tools/pf/lit_eval.py`, `tools/pf/lit_report.py`.

---

## 2026-11-07 — `learned` prefetcher (nextreq + neighborhood), developed by replaying captured VM streams

**Decision:** added `prefetch_policy: learned[:theta=T]` = `nextreq` (per-stream next-request predictor, byte-exact
lengths/gaps via the new `CachePolicy::on_request` hook) + `neighborhood` (learned P(page beside a request is requested
soon), conditioned on request shape, distance, "hole" class and last-seen footprint), plus `lru_seg` eviction. Policy
values are now specs (`name:k=v`, `a+b`). Evaluation uses `bin/psim` replaying real VM streams (`tools/pf/`).
**Why:** live VM, 20k FAISS queries / 150k Thesios reads, nbd_server page `hit_ratio` (traffic = pages fetched vs no
prefetch): FAISS none 30.2%, readahead(512) 69.7% at 53x, quickmine 38.0%, learned+lru_seg 74.3% at 1.7x
(`theta=30`: 75.6% at 1.8x); Thesios none 8.6%, readahead 70.5% at 24x, quickmine 9.1%, learned+lru_seg 81.1% at 1.8x
(`theta=30`: 81.4% at 2.0x). Over an emulated 1 ms link the guest's mean query time was 1.77 ms (direct 2.13,
readahead 1.94) with ~37% less bulk traffic. On random streams (YCSB, GUPS) it adds <=2% traffic (readahead: 16x+).
Policy cost is ~3-4 us/read on an idle machine (the 100+ us in `policy_s` of the live runs was CPU contention).
**Alternatives considered (all measured on the stream suite, all dead ends or no better than the above):** fixed
readahead at any depth (tuned depth is on the frontier for Thesios but floods random traffic and collapses when
prefetches are late: `--delay 2`); stream tracker with geometric ramp; learned-extent stream tracker (survival curve);
one big count table; context mixing (7 tables + logistic mixer); burst-aware stream tracker; association mining on top
(+0.4 pt for +6% traffic); frequency-aware eviction (`lru_freq`, no gain); longer label windows (worse); per-page "seen
before" memory (no gain). An offline gradient-boosted model with all features gained only 2-3 pts over the online
tables, so the remaining gap is not modelling: with local information the ceiling is ~80% (FAISS ~85%) because the
first request of every list visit / new Thesios stream is unpredictable. Belady eviction would add ~11 pts on FAISS but
reuse distances far exceed the cache. Caveats: nbd_server admits prefetches instantly (real links need `--delay`-style
lead; learned held up best there); single seed per run.
**Reference:** `policies/policy_nextreq.cpp`, `policy_neighborhood.cpp`, `policy_lru_seg.cpp`, `policy_factory.h`,
`tools/pf/`, `logs/pf_live/` (gitignored).

---

## 2026-08-29 — Delete the synthetic-workload pipeline; NBD/VM path is now the whole project

**Decision:** Removed `app.cpp`, `dat.cpp`, `runner.cpp`, `ldat.cpp`,
`lhook.cpp`, `lrunner.cpp`, `uffd_protocol.h`, `pipe_pair.hpp`, and (later)
`workloads/`, `configs/`, `traces/`, `tools/verify_against_myycsb/` —
everything belonging to the old synthetic-workload-over-pipes simulator.
**Why:** project's purpose narrowed to exactly one workflow: create disk →
serve it over NBD → attach real QEMU VMs → test → log measurements. The old
pipeline (synthetic app processes talking to a cache daemon over pipes) was
a different, now-unwanted approach to the same underlying cache/policy
question.
**Alternative(s) considered:** keep both pipelines side by side — rejected
as maintenance burden with no active use.
**Reference:** commits `99cf8bf` "cleanup", and this session's deletion of
`workloads`/`configs`/`traces`/`tools`.

## 2026-08-29 — NBD server option-reply magic bug

**Decision:** Server-to-client option replies (`NBD_OPT_GO` fallback,
`NBD_OPT_ABORT` ack, catch-all unsupported) must use `NBD_REP_MAGIC`
(`0x3e889045565a9`), not the client's `IHAVEOPT` request magic.
**Why:** the code had reused `IHAVEOPT` for both directions since it was
first written. `nbd-client` tolerated this silently; QEMU's stricter NBD
client rejected it with "Unexpected option reply magic," which is how the
bug was actually found (real VM attach failure, not a spec read).
**Alternative(s) considered:** none — this was a straightforward protocol
conformance bug, not a design choice.
**Reference:** `disk/nbd.cpp:61-62` (`kRepMagic`), commit `08102b0`
"updated magics".

## 2026-08-29 — Serial console to a log file, not `mon:stdio`

**Decision:** `kvm/launch.py` routes `display: none`'s serial output to
`<log_dir>/<name>.serial.log` via `-serial file:...` instead of
`-serial mon:stdio`.
**Why:** `mon:stdio` is incompatible with `-daemonize` (QEMU detaches from
the controlling terminal before the chardev would attach) — real failure:
`cannot use stdio with -daemonize`. A file-backed chardev works identically
whether or not the VM is daemonized, and persists the console for later
inspection either way.
**Alternative(s) considered:** default `daemonize: false` instead (a live
terminal would make `mon:stdio` work) — rejected because it ties QEMU's
process lifetime to the launching shell/process instead of letting it fully
detach.
**Reference:** `kvm/launch.py`'s `build_command`, commit `aa132d9`
"display-none".

## 2026-08-29 — `nbd_server` stats logging modeled on deleted `dat.cpp`/`runner.cpp`

**Decision:** On clean shutdown (`SIGINT`/`SIGTERM` via `sigwait`),
`nbd_server` prints a `STATS key=value ...` line and appends one CSV row
(header-on-first-write) — reusing the exact column set and log-file
conventions the now-deleted `dat.cpp`/`runner.cpp` used, rather than
designing a new schema from scratch.
**Why:** `nbd_server` is long-running (no fixed request count to run out,
unlike `dat.cpp`), so shutdown-triggered logging (matching the also-
long-running, now-deleted `ldat.cpp`'s `sigwait` pattern) was the natural
trigger point. Reusing the old column names/semantics kept results
comparable to historical runs from the deleted pipeline.
**Alternative(s) considered:** periodic time-based log flushing while the
server runs — considered for the "watch progress during a long VM test"
case, not implemented; shutdown-only was judged sufficient and closer to
what the deleted programs already did.
**Reference:** `disk/nbd.cpp`'s `AppendLog`/`PrintStatsLine`; originals
recoverable via `git show HEAD~N:dat.cpp` / `:runner.cpp` / `:ldat.cpp`.

## 2026-08-29 — Ports identify clients, not exports

**Decision:** `nbd_server`'s NBD handshake ignores the requested export
name; every port in a config's `ports:` list serves the same backing file,
and exists solely to give `SimReadClass` a distinct `worker_id` per VM disk.
**Why:** the simulator's per-client virtual-time and cache-warmup tracking
needs a stable client identity; TCP port-per-client was simpler to
implement than parsing/dispatching on NBD export names, and QEMU's
`driver=nbd` config already names a port per disk naturally.
**Alternative(s) considered:** implement real per-export routing (distinct
backing file per export name on one port) — not done; documented as a known
limitation instead (see `CLAUDE.md` gotchas).
**Reference:** `disk/nbd.cpp`'s `DoHandshake`/`AcceptLoop`.

## 2026-08-29 — `pread` instead of `fseek`+`fread` for backing-file reads

**Decision:** `SimReadClass::read` uses `pread(fileno(f), buf, length,
offset)` instead of seeking then reading on the shared `FILE*`.
**Why:** the backing `FILE*` is shared across every client-handling thread;
`fseek`+`fread` would race on the file's shared position under concurrent
reads from different VMs. `pread`'s explicit offset argument makes each
read atomic and lock-free.
**Alternative(s) considered:** a mutex around seek+read — rejected as
unnecessary overhead when `pread` does the same thing for free.
**Reference:** `disk/sim.hpp`.
