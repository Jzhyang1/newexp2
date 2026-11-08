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
