# Candidate signals for page-access prediction

Goal: a long list of features to extract and test for correlation with the *next* page(s) accessed at the NBD server. Justification is optional. Not every signal needs to be available to a real host.

**Access tiers** (where the signal can be collected):
- **H**: host NBD server already sees it.
- **H+**: host-side, but needs new instrumentation (QEMU, KVM, eBPF on the host).
- **G**: needs a guest agent or guest kernel tracing (eBPF, ftrace, /proc).

**Targets to correlate against** (define these once, reuse for every signal):
next-offset delta (signed, in pages), "next access within K pages of the current one", "page P re-accessed within N requests/seconds", and request-length class. Sweep the lookahead horizon (1, 2, 4, 16, 64 requests).

## Top 50 direct features to extract

Only raw, directly observable values; deltas, histories, counts and other derived quantities are built later from these (sections 1-7 list those). Extraction assumes a running QEMU/KVM guest (Linux, ext4 or similar) attached to `nbd_server`.

**Common plumbing (applies to every row):**
- **Clock:** log every event with `CLOCK_MONOTONIC` ns (`bpf_ktime_get_ns()` in eBPF, `clock_gettime` in the server). Guest and host monotonic clocks differ, so run kvm-clock/TSC-based offset calibration (one guest-to-host ping via virtio-vsock at start and every ~10 s) and store the offset.
- **Transport:** guest tracer writes ring-buffer records to a file on a virtio-serial or vsock channel, so the host collects it with no guest disk write (writes are discarded anyway). Host features go to `logs/` as CSV/binary beside `nbd_access.csv`.
- **Join keys:** (a) time, (b) guest sector, which maps to NBD offset by `offset = sector*512 + partition_start` (known from `fdisk -l` on the image), (c) guest `(dev, inode, page index)` to guest sector via the `ext4_*` / `filemap` tracepoints and `fiemap`.
- **Tooling:** `bpftrace`/`libbpf` programs on tracepoints and kprobes in the guest; `perf` and QEMU tracing on the host. Overhead target is under 5%.

| # | Feature | Tier | How to extract in a running system |
|---|---|---|---|
| **NBD server (host)** | | | |
| 1 | Offset | H | Already parsed in `DoHandshake`/request loop of `disk/nbd.cpp` |
| 2 | Length | H | Same request header |
| 3 | Command type (read/write/flush/trim/write-zeroes) | H | NBD request `type` field; log writes before discarding |
| 4 | Command flags (FUA, NO_HOLE, etc.) | H | NBD request `flags` field |
| 5 | Arrival timestamp | H | `clock_gettime` right after header read |
| 6 | Completion timestamp / service latency | H | Timestamp before/after `pread` and the reply `write` |
| 7 | NBD handle (request ID) | H | Request `handle` field; lets us match to QEMU-side trace |
| 8 | Worker/VM ID (port) | H | Already `worker_id` |
| 9 | In-flight requests on the connection | H | Counter incremented on header read, decremented on reply (needs multi-threaded handling or `FIONREAD` on the socket for pending bytes) |
| 10 | Cache hit/miss per page | H | `policy::Cache` lookup result in `SimReadClass` |
| 11 | Page resident because of a prefetch (flag) | H | Tag on insert in `policies/cache.cpp` |
| 12 | Eviction event (page, time, was it ever hit) | H | Hook in `Cache` evict path |
| **Guest block layer** | | | |
| 13 | Guest request start sector | G | tracepoint `block:block_rq_issue` (`sector`) |
| 14 | Guest request length (sectors) | G | same (`nr_sector`) |
| 15 | Request flags (RAHEAD, SYNC, META, FUA, PRIO) | G | `block_rq_issue` `rwbs` string; or `req->cmd_flags` via kprobe |
| 16 | Issuing PID/TID and comm | G | `block_rq_issue` carries `pid`, `comm`; with bpftrace `pid`, `tid`, `comm` builtins |
| 17 | cgroup ID of the issuer | G | `bpf_get_current_cgroup_id()` |
| 18 | Merge events (bio merged into existing request) | G | `block:block_bio_backmerge`, `block_bio_frontmerge` |
| 19 | Bio submit time (before queue/merge) | G | `block:block_bio_queue` |
| 20 | Request completion time | G | `block:block_rq_complete` |
| 21 | I/O priority | G | `bio->bi_ioprio` via kprobe on `submit_bio`, or `block_rq_issue` |
| **Guest readahead** | | | |
| 22 | Readahead window (start, size, async_size) | G | kprobes/fentry on `page_cache_sync_ra` / `page_cache_async_ra`; read `struct file_ra_state` |
| 23 | Readahead trigger kind (sync miss, async PG_readahead hit, mmap fault-around) | G | Which of the functions above fired; `do_fault_around` kprobe for mmap |
| **Guest filesystem / page cache** | | | |
| 24 | File identity (dev, inode) | G | `mm_filemap_add_to_page_cache` (`s_dev`, `i_ino`); or `file->f_inode` in a `vfs_read` fentry |
| 25 | File path | G | `bpf_d_path()` on `vfs_read`/`filemap_fault` fentry; cache inode-to-path map in userspace |
| 26 | Logical file page index | G | `mm_filemap_add_to_page_cache` (`index`); `vfs_read` `pos` |
| 27 | File size | G | `inode->i_size` read in the same fentry |
| 28 | Open flags (O_DIRECT, O_SYNC, ...) | G | `file->f_flags` in `vfs_read`, or `openat` tracepoint args |
| 29 | `fadvise`/`madvise` hints | G | `syscalls:sys_enter_fadvise64`, `sys_enter_madvise` |
| 30 | Page cache add (page inserted) | G | `filemap:mm_filemap_add_to_page_cache` |
| 31 | Page cache hit (page found without I/O) | G | fentry `filemap_get_pages` returning with no read; or `mark_page_accessed` kprobe. **This is the censored stream** (never reaches NBD) |
| 32 | Page cache eviction/delete | G | `filemap:mm_filemap_delete_from_page_cache` |
| 33 | Refault with shadow entry (workingset distance) | G | kprobe `workingset_refault`; `mm_vmscan_*` also useful |
| 34 | Dirty and writeback events | G | `writeback:writeback_dirty_page`, `writeback_start` |
| 35 | File extent map (logical to physical blocks) | G | `ioctl(FS_IOC_FIEMAP)` / `filefrag -v` at file open or periodically, or `ext4:ext4_map_blocks_exit` trace |
| 36 | Metadata vs. data block class | G | `ext4:ext4_*` tracepoints for inode/bitmap/extent reads, or `REQ_META` in #15 |
| **Guest syscalls** | | | |
| 37 | Syscall number | G | `raw_syscalls:sys_enter` |
| 38 | Syscall args (fd, buf, count, offset; for mmap addr, len, flags) | G | `sys_enter_read/pread64/readv/mmap/io_uring_enter` args |
| 39 | Return value (bytes) | G | `raw_syscalls:sys_exit` |
| 40 | Syscall entry timestamp | G | same tracepoint, `bpf_ktime_get_ns()` |
| **Guest CPU / memory** | | | |
| 41 | User-space PC (RIP) at syscall/fault | G | `PT_REGS_IP` of `task_pt_regs()` in the above probes (or `perf` sample `ip`) |
| 42 | User call stack (a few frames) | G | `bpf_get_stack(..., BPF_F_USER_STACK)`; needs frame pointers or unwinding info |
| 43 | Page fault address (virtual) and fault type (major/minor) | G | `exceptions:page_fault_user` tracepoint, `filemap_fault` fentry |
| 44 | vCPU/CPU number | G | `bpf_get_smp_processor_id()` in every probe |
| 45 | Scheduler state and timestamps (blocked/uninterruptible, wake) | G | `sched:sched_switch` (`prev_state`), `sched_wakeup`; the delta is blocking time (derived) |
| 46 | Free memory / memory pressure (PSI) | G | `/proc/pressure/{memory,io}`, `/proc/meminfo`, sampled at ~10 Hz |
| 47 | Hardware counters (LLC misses, instructions, cycles) | H+/G | `perf stat -e` on the guest's PMU if exposed, else host-side `perf kvm stat` |
| 48 | Virtual access bit scan (which guest pages were touched recently) | H+ | Host-side `/sys/kernel/mm/page_idle` / `damon` on the QEMU process's guest-memory mapping, or KVM dirty-log |
| 49 | EPT violation / VM-exit events with guest physical address | H+ | `kvm:kvm_page_fault`, `kvm_exit`, `kvm_mmio` tracepoints on the host |
| 50 | Application phase marker | G | uprobe on a known function in the app (e.g., `rocksdb::DBImpl::*`), or an app-written log line tailed into the trace |

Rows 24-26 together are the most valuable join: they turn a block-level miss into a file/offset event, and #31 recovers the hidden hits.

## 1. Request-stream signals (H, mostly already in hand)

| Signal | Notes |
|---|---|
| Page number, offset, length of each request | baseline |
| Deltas and delta-of-deltas (signed, page units) | stride detection |
| Last-k delta history as a context vector, k = 1..16 | Markov / n-gram on deltas |
| Request length, and length-vs-previous-length | merged/readahead requests are large |
| Inter-arrival time between requests (log-scaled) | burst vs. idle |
| Burst length, and requests-per-ms over several windows | sequential runs arrive in bursts |
| Run length of the current sequential/strided run | has the run lasted n requests |
| Reuse distance and time since last access, per page and per extent | temporal locality |
| Access count and frequency of the page/extent (decayed) | LFU-like |
| Extent/region hotness (counts per 1 MB, 2 MB, 64 MB bucket) | spatial locality at several scales |
| Position in the file/device, normalized | head/tail of device behaves differently |
| Read/write flag, flush/trim flag, FUA flag | writes are discarded by our server but the guest still issues them |
| Queue depth and number of in-flight requests | NBD connection state |
| Worker/VM ID, and which VMs touched the page recently | cross-VM sharing |
| Cache state: was the page a hit, was it a prefetched page, how long was it resident, how many hits per prefetch | feedback on the policy's own prior decisions |
| Prefetch outcome history (useful / evicted-unused) per prefetch source | online trust in each predictor |
| Time of day, wall-clock phase, request index since start | warmup, periodic jobs |

## 2. Guest block layer (G / H+)

| Signal | Notes |
|---|---|
| Request merge events (front/back merge), merge count | reveals how many guest requests became one NBD request |
| I/O scheduler queue state, plug/unplug events | batching boundaries |
| Guest readahead window size and state (`ra->size`, async-size, miss/hit flags) | the guest's own prefetcher; our prefetch should be complementary or complete it |
| Readahead trigger type: sync miss vs. `PG_readahead` marker hit vs. mmap fault-around | tells us whether more is coming |
| Request issuer: PID, TID, comm, cgroup | per-application streams get separated |
| I/O priority class, REQ_RAHEAD / REQ_SYNC / REQ_META flags | hints already set by the kernel |
| Block-layer latency per request, and D2C latency | guest-perceived stall |
| virtio-blk / virtio queue depth, kick and interrupt timing | |
| `blk_mq` hardware context / CPU that submitted the request | |

## 3. Guest filesystem and page cache (G)

| Signal | Notes |
|---|---|
| File identity (inode, path, extent map) behind each block | **block to file reverse map**: most important for sequential detection across fragmented files |
| File offset (logical) vs. block offset (physical) | logical sequentiality survives fragmentation |
| File size, file type, open flags (O_DIRECT, O_SYNC), fadvise/madvise hints | explicit app hints (SEQUENTIAL, WILLNEED, DONTNEED) |
| Page cache events: add, hit, miss, evict, writeback, dirty, mark-accessed, activate | `mm_filemap_*`, `mm_vmscan_*` tracepoints |
| Page cache hit ratio, and cache occupancy over time | the NBD stream is only the *misses*; hits are hidden |
| Page cache hits that the host never sees (their file offsets) | **the censored data**: the host sees a stream filtered by the guest cache |
| LRU list position, active/inactive state, refault distance | working-set model |
| Free memory, memory pressure (PSI), reclaim events, kswapd activity | eviction waves cause re-read bursts |
| Dirty/writeback counts, journal commits | |
| Syscalls: read/pread/readv/mmap/sendfile/io_uring + args (fd, offset, len) | the true application request, pre-batching |
| Syscall sequence n-grams, syscall inter-arrival time | |
| Directory lookups, `stat`/`open`/`getdents` events | metadata precedes data reads |
| Filesystem metadata reads (inode table, bitmap, journal, extent tree blocks) | distinct pages; classify "metadata vs. data" per request |

## 4. Guest memory and CPU (G / H+)

| Signal | Notes |
|---|---|
| Virtual PC / RIP of the faulting or syscalling instruction | code location identifies the access site |
| Call stack (a few frames) at the I/O, or its hash | stronger than PC alone; like PC-based prefetchers (SHiP, Hawkeye, IP-stride) |
| Virtual memory addresses touched (load/store), at page granularity | for mmap'd files, the VA stream is the real access pattern |
| Page faults: major/minor, VA, fault address, fault-around range | mmap file reads arrive as faults |
| Page-table events: accessed/dirty bits scanned, TLB misses (EPT violations) | H+: EPT faults are visible to KVM |
| VA to file-offset mapping (VMA lookup) | |
| Heap/stack/mmap region classification of the VA | |
| Thread ID, CPU number, scheduler run-queue state, context-switch events | which thread is running |
| Hardware counters: LLC misses, branch mispredicts, IPC, cache-miss PC (via PMU in guest or host perf) | proxy for compute phase vs. I/O phase |
| Virtual time spent blocked on I/O, per thread (`D` state), and virtual blocking times | how much a prefetch would help (latency-critical or not) |
| Compute time between I/O completions per thread ("think time") | predicts when the next request fires |
| Thread wakeup chains (who woke whom, after which I/O) | dependency graph of requests |

## 5. Application-level signals (G)

| Signal | Notes |
|---|---|
| Application name, command line, job/task ID | |
| Phase/stage of the application (e.g., compaction, scan, point lookup, checkpoint) | phase labels from logs or uprobes |
| Query/operation type and key (for DB-like workloads: Get, Scan, Compaction input) | scans are sequential; gets are random |
| Data-structure traversal state (B-tree level, SSTable block index, LSM level) | an index read implies a data block read next |
| Library-level calls: `fread`, `mmap`, RocksDB/LevelDB iterator calls, protobuf parse | uprobes |
| Application-level cache state (block cache hit/miss) | like the guest page cache: hides requests |
| Heap allocation / free events | |
| Network RX/TX events that precede I/O (an RPC arrival triggers reads) | request-driven workloads |
| Thread pool identity and queue lengths | |

## 6. Host / hypervisor / environment (H+)

| Signal | Notes |
|---|---|
| Host page-cache state of the backing file | our software cache vs. the real one |
| Backing-device latency and queue depth | |
| NUMA node, vCPU pinning, host CPU contention, steal time | timing jitter |
| QEMU virtio-blk and NBD-client batching: coalescing and request splitting boundaries | NBD `max_block_size` split sizes |
| Other VMs' load and I/O rate (noisy neighbors) | |
| Guest idle/halt exits (HLT exits), interrupt injection times | idle vs. busy guest |

## 7. Derived / learned features (build from the above)

- Embeddings: page ID, file ID, PC, PID embeddings (as in the brute model).
- Markov/n-gram state keyed by (PC, delta), (file, delta), (PID, delta), (file, offset bucket).
- Association-mining outputs (cminer / quickmine / mithril): is the page in an active association rule, rule confidence/support.
- Stream-tracker state: number of active streams, stream stride and confidence, assigned stream ID.
- Frequent-sequence match depth (how many of the last requests match the beginning of a known sequence).
- Counts of unique pages in the recent window, entropy of the delta distribution, fraction sequential vs. random over the last N requests (workload mode detectors).
- Time-series features (periodicity, autocorrelation of delta, FFT peaks).

## 8. Suggested analysis order

1. Compute mutual information / conditional entropy `H(delta_next | feature)` for each category-1 signal vs. baseline `H(delta_next | last delta)`. Keep the bin counts equal so features are comparable.
2. Add one non-host signal at a time (file identity, PID, PC, syscall args) and measure the marginal information gain over the host-only baseline. That gap is the upper bound on what guest cooperation could buy.
3. Measure the **censoring effect**: compare the stream with and without guest page cache hits. If the hidden stream is far more predictable, the real problem is guest cache filtering, not model quality.
4. Rank by gain, then try the best feature set in the existing models (brute, voyager, lstm, markov).
