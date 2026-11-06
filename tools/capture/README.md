# Signal capture harness

Extracts the signals listed in [offline/SIGNALS.md](../../offline/SIGNALS.md) from a running VM,
joins them per NBD request, and draws the analysis figures. Nothing here needs root or a rebuilt guest
image (the guest is booted with `init=/bin/sh`; the tracer reaches it over a read-only 9p share).

```
build_faiss_disk.py   host: build the FAISS IVF index + ext4 data image (disk/data/faiss_data.img, vdb in the guest)
run_capture.py        host: 2x nbd_server (root disk, data disk) + QEMU + guest tracer  -> logs/capture/<name>/
replay.py             host: replay a captured request stream into nbd_server (no VM), for policy/geometry sweeps
sweep.py              host: grid of replays (policy x page size x cache size)
localdisk_study.py    host: local-disk-as-page-cache vs prefetch study (live RAM arms + replay cache arms); localdisk_analyze.py / localdisk_late.py summarise -> capture_results/localdisk.md
analyze.py            host: join + figures + numbers.json  -> capture_results/
capture_lib.py        parsers, clock sync, extent-map layout, the per-request join
../../kvm/guest/trace/  guest side: bootstrap.sh (PID 1), guest_main.py (tracefs tracer + workload driver), faiss_ondisk.py
```

## Run it

```bash
make nbd                                                    # bin/nbd_server
pip install faiss-cpu                                       # host only; needed once for the data image
python3 tools/capture/build_faiss_disk.py                   # ~80 s, 1.6 GiB image + query set
python3 tools/capture/run_capture.py --name base --n-queries 20000   # ~40 s
./bin/python tools/capture/sweep.py --source logs/capture/base       # 18 replays, ~1 min
./bin/python tools/capture/analyze.py                                # figures -> capture_results/
```
(`analyze.py`/`replay.py`/`sweep.py` need pandas + matplotlib: the repo venv `./bin/python` has them.)

## What a capture directory holds

| file | content |
|---|---|
| `features.csv` | nbd_server, one row per NBD command: offset, length, cmd type/flags, handle, arrive/start/end ns (CLOCK_MONOTONIC), pread + policy time, think time, queued bytes, page hits/total, prefetch hits/issued |
| `cache.csv` | nbd_server, every admit/evict with prefetch provenance, residency time, hits while resident |
| `out/trace_blk.txt` | guest, all tasks: block rq issue/complete/merge, page-cache add/delete, readahead-window kprobes, fault-around |
| `out/trace_app.txt` | guest, workload pid only: syscalls, user page faults (VA + PC), sched switch/wakeup, app markers |
| `out/trace_stk.txt` | guest, workload pid: file-I/O syscalls with the user call stack |
| `out/samples.csv`, `out/proc_snapshot_*.json`, `out/sync.json` | memory/PSI at 10 Hz, VMA + fd tables, clock-sync probes |

## Caveats worth knowing

* **Tracing perturbs the guest.** The ftrace ring buffers and 9p cache are guest RAM. With 64 MB buffers
  the guest saw 3x the NBD requests of an untraced run; at the default 2 MB/cpu/instance it is +16% (and no
  events are dropped: check `out/sync.json`'s `cpu0_stats`). Use `replay.py` for policy comparisons.
* **Clock alignment** is estimated from O_DIRECT probes of reserved raw sectors (matched by offset on both sides);
  the host/guest CLOCK_MONOTONIC share a TSC, so the offset is constant to <1 µs, but it includes the probe's own
  guest-to-host latency (~tens of µs).
* The join matches ~96% of NBD reads 1:1 to a guest block request; the rest are QEMU-merged requests.
* `page_size` (nbd.yaml) changes the cache/policy unit. The `model-prefetcher` path (`offline/serve.py`) still
  assumes 512 B blocks, so keep `page_size: 512` when using it.
