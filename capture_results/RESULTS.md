# Preliminary results: FAISS on-disk IVF workload

One workload, one seed, one 20,000-query run (`logs/capture/base`), so treat every number as a first look, not a
conclusion. Regenerate everything with `tools/capture/analyze.py` (see `tools/capture/README.md`).

**Setup.** A 6M-vector, 64-d IVF-Flat index (4096 inverted lists, 1.5 GiB, memory-mapped by faiss) lives on an ext4
data disk served by `nbd_server`. The guest has 1 GiB of RAM. Queries come in topical sessions (Zipf topics),
8 lists probed per query. Cache: 64 KiB pages, 256 MiB, LRU.

## What was captured (fig0)

38 of the 50 features in `SIGNALS.md` are collected, 4 partially/by proxy, 8 not (cgroup, ioprio, file size, refault,
dirty/writeback, HW counters, access-bit scan, EPT exits; the last three need host privileges). The "page-cache hit"
signal (#31) has no tracepoint in the 6.1 guest kernel, so it is approximated from mmap faults, fault-around events
and syscalls. The join to NBD requests is exact for 96% of reads (fig3); the rest are 256 KiB requests QEMU built by
merging two guest requests.

![inventory](fig0_signal_inventory.png)

## Findings

**1. The guest page cache hides almost everything the host would want to learn from (fig2).** The application
requested 75 GiB of list data; 2.2 GiB (2.9%) reached NBD. Fewer than one list visit per query arrives at the host
(15.8k visits / 20k queries), and the hottest lists are the least visible. This is the central difficulty: the host
sees a heavily filtered sample of the real access stream. The mmap page-fault stream (60k faults on the index
file, fig5) is the less-filtered view that exists inside the guest.

![censoring](fig2_censoring.png)

**2. Request shapes (fig1).** 53% of requests are exactly 128 KiB (guest readahead windows); only 21.5% continue
exactly where the previous request ended; the rest are jumps between lists. 36% of list visits begin in the list
*adjacent* to the one being probed, because readahead windows are aligned to 128 KiB and start in the tail of the
previous list. Any list/page-level predictor (and any evaluation of one) has to tolerate this.

![overview](fig1_stream_overview.png)

**3. Policy comparison on the identical replayed stream (fig6).** Percent of NBD reads fully served from the
64 KiB-page / 256 MiB cache, and data admitted (backend traffic):

| policy | reads fully cached | admitted |
|---|---|---|
| none (LRU) | 16.9% | 2.4 GiB |
| quickmine = cminer = mithril | 28.9% | 2.8 GiB |
| readahead | 71.3% | 131.7 GiB |
| oracle (replays the trace) | 100% | 2.4 GiB |

The three mining policies give identical results here: with a single client, cminer and quickmine differ only in
per-client partitioning, and mithril happens to coincide too. Readahead buys hits with 55x the traffic (1% of what
it prefetches is used); the oracle shows what accurate prefetching could do with no extra traffic.

**4. Page size and cache size (fig6).** At a fixed 256 MiB, hit rate with no prefetching goes 0.5% (4 KiB pages),
16.9% (64 KiB), 55% (256 KiB), 75% (1 MiB), while admitted data grows (2.2 → 2.4 → 3.1 → 5.9 GiB). At 4 KiB there is almost no exact-page
reuse in the NBD stream: the hits at larger page sizes come mostly from adjacent requests sharing a page (spatial
locality), not temporal reuse. Reuse only appears once the cache is large (1 GiB: 51%, since reuse distances exceed
the guest's ~700 MiB cache). Larger pages are therefore partly a prefetcher in disguise.

![policies](fig6_policy_sweeps.png)

**5. Which signals predict the next list visit? (fig4, fig7).** Prequential next-visit prediction, ±1 list tolerance,
host-only context first:

| context | top-1 | top-8 |
|---|---|---|
| last list (Markov-1) | 8.2% | 26.8% |
| + inter-arrival gap / request length / hit-miss | 8.5 / 9.5 / 9.4% | 26.9 / 26.9 / 26.8% |
| + guest rq flags + readahead kind (guest tracing) | 8.2% | 26.9% |
| last 2 lists (Markov-2) | 11.3% | 26.8% |
| + position in query (app marker) | 14.0% | 27.0% |
| current query's probe list known (oracle) | 16.6% | 47.2% |

Per-request host features (gap, length, flags, readahead state) add about nothing over knowing the last list. The
only signal that moves top-8 is the application's own probe list (+20 points). The two guest-cache-state variants did
**not** help ("frequent lists not resident": 1.2% top-8; "upcoming probes not resident": 22.3%, below the plain probe-order oracle's 47.2%). The reconstructed residency itself is right (it
tracks the guest's reported Cached memory within 1-3%), so I don't yet understand why it fails to predict which
probed lists miss; likely causes are partial residency and readahead pulling in lists that were never probed. Treat
this as an open question, not a negative result about the signal.

![signals](fig4_signal_predictability.png)
![lookahead](fig7_lookahead_value.png)

**6. Guest-only signals that exist (fig5).** Readahead windows are fixed at 32 pages (the guest default, no
adaptation seen), faulting PCs concentrate in a handful of libfaiss code sites (54 distinct 64 B blocks), and the
median I/O wait is 169 µs.

![guest](fig5_guest_signals.png)
![join](fig3_join_validation.png)

## Caveats

* **Tracing changes the workload.** Ftrace buffers and the 9p cache are guest RAM. With 64 MB buffers the guest
  issued 3x the NBD requests of an untraced run; at the 2 MB buffers used here it is +16% (22.6k vs 19.5k) with no dropped
  events. Policy sweeps replay the traced stream rather than rerunning VMs, so they are mutually consistent but
  reflect the traced guest's cache behaviour.
* One workload (and one query stream); the application-marker signal is an upper bound (a real host cannot see
  which lists a query will probe).
* Page-hit rates at different page sizes are not comparable as "hit rate per byte"; compare reads fully cached and
  the admitted bytes together.
* The `model-prefetcher` path still assumes 512 B blocks (`offline/serve.py`).

**Follow-up:** local disk as extended page cache vs. prefetching cache: see [localdisk.md](localdisk.md).
