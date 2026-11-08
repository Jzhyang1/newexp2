# Prefetch-policy research harness (`tools/pf`)

Fast, VM-faithful evaluation of evict/prefetch policies. The idea: a real QEMU guest (with its own page cache and
readahead) is only needed once, to *capture* the block stream it sends over NBD; every policy experiment after that is a
replay of that stream through the real policy code, in seconds, with no VM.

```
tools/pf/build.sh            -> bin/psim   (links policies/*.cpp; mirrors SimReadClass::read's order of operations)
tools/pf/sweep.py            policies x streams table: hit % / traffic / precision / futile %
tools/pf/frontier.py         sweep a threshold, report hit % interpolated at 1.25x / 1.5x / 2x traffic
tools/pf/gen_streams.sh      capture the stream suite below from real VMs (no root, needs /dev/kvm)  -> logs/pf_streams/
tools/pf/live_eval.sh        the same workload live in a VM under each policy                        -> logs/pf_live/
tools/pf/live_report.py      table of the live runs (nbd_server's own hit_ratio)
tools/pf/test_policies.py    behavioural checks on synthetic streams (sequential, random, out-of-order, 2 workers)
tools/pf/ml_probe*.py        offline feature-headroom probes (gradient boosting vs the online tables) used to design the policies
```

## `psim`

```
bin/psim --trace T.csv --policy learned [--evict lru_seg] [--page 65536] [--cap 4096] [--delay K] [--warm N] [--csv]
```

* Trace: CSV with `offset,length` (bytes); `worker`/`worker_id`, `type` (non-read rows skipped) and `arrive_ns` are used if
  present, so `logs/capture/*/features.csv`, `logs/pf_streams/*/features.csv` and the old `oracle_*.csv` all work
  (`--max-offset` drops the sync-probe tail of a data disk).
* It is **faithful**: on the traced FAISS capture its hit ratio equals `nbd_server`'s replay of the same stream to 0.01%
  (73.00% both), and a live VM with the same policy landed within ~0.5%.
* `--delay K`: a prefetched page only counts once K more requests have been served (nbd_server's simulation admits
  prefetches instantly; K=2..4 is closer to a real link, see `README.md` "With a VM in the loop").
* Reported: `full_hit` (reads entirely from cache), `page_hit` (nbd_server's `hit_ratio`), prefetched pages, how many were
  used (`used`), how many were never requested again at all (`futile`), `traffic` = pages admitted (demand misses +
  prefetches); `tools/pf/sweep.py` divides it by the no-prefetch run so "1.7x" means 70% more backend pages than demand
  fetching alone. Compare policies on hit **and** traffic: plain readahead buys hits with 2-30x traffic.
* Analysis-only policies built in: `localoracle:w=64,d=256` (best any neighborhood prefetcher could do: knows the next W
  requests), `oracle`, `file:path=...` (prefetch from an external model's predictions), and `--evict belady`.
* `--dump F` / `--dump-req F` write per-prefetch / per-request outcomes for analysis.

## The stream suite (`logs/pf_streams/`, gitignored)

Real VM streams with policy `none`, captured by `gen_streams.sh`:

| name | workload | guest |
|---|---|---|
| `f_m1g` `f_m512` `f_m2g` | FAISS on-disk IVF, 20k queries | 1 GiB / 512 MiB / 2 GiB RAM |
| `f_ra0` `f_ra32` `f_ra512` | same | guest `read_ahead_kb` 0 / 32 / 512 (default 128) |
| `f_np16` | nprobe 16 | |
| `th_m1g` `th_m4g` `th_ra32` `th_ra512` | Thesios, 150k reads | 1 GiB / 4 GiB RAM, readahead 32 / 512 KiB |
| `y_nt` | YCSB (SQLite point reads) | a random-access control |

Thesios in a VM without root: `tools/capture/build_thesios_disk.py` builds a sparse 8 TiB data disk plus the condensed
trace from `disk/data/thesios_cache`; `tools/capture/run_capture.py --workload thesios` replays it as buffered reads
(same hashed file placement as `kvm/guest/workloads/thesios_bench.py`).

## Things that are easy to get wrong

* Run `nbd_server` configs with explicit `access_log`/`prefetch_log` (see `CLAUDE.md`): `run_capture.py` already does.
* `bin/psim` and `bin/nbd_server` are built separately (`tools/pf/build.sh` vs `make nbd`); rebuild both after touching
  `policies/`.
* Spec syntax for `--policy` / `prefetch_policy:`: `name[:k=v,k=v]`, `a+b` for the union (`policies/policy_factory.h`).
