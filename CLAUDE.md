# CLAUDE.md

## What this is

A cache-replacement/prefetch-policy simulator for VM disk I/O. `nbd_server`
serves a backing file to real QEMU VMs over the NBD wire protocol; every
guest read is routed through a software cache (`policy::Cache`) and a
configurable evict/prefetch policy before touching the real file, so cache
policies get measured against genuine VM disk traffic rather than a
synthetic workload generator. Single-developer research/experiment repo
(`jzhyang1`), hosted at `github.com/Jzhyang1/newexp2`.

## Stack

- **C++20** (`g++ -std=c++20 -pthread`), no external C++ dependencies — just
  libc/pthread. Linux-only (`disk/nbd.cpp` needs `<endian.h>`).
- **Python 3 + PyYAML** for `kvm/launch.py` (`pip install pyyaml`).
- **Bash** for `kvm/guest/build_image.sh` (needs `debootstrap`, root, and a
  Debian/Ubuntu host — only required for the optional real-guest path).
- **GNU Make** is the actual entrypoint for everything; there is no other
  build system.

## Running it

Everything is Linux-only and driven by the `Makefile`. Full walkthrough is
in `README.md`; short version:

```bash
make disk                # stage 1: provision a backing file
make nbd-run              # stage 2: build + serve it over NBD (foreground)
make vms-up                # stage 3: attach QEMU VMs to it
# stage 4: test on VMs (interactive — ssh, or the guest's serial log)
make vms-down               # stage 5: tear down; nbd-run's Ctrl-C logs stats
```

`make experiment` / `make experiment-down` run stages 1-3 and 5
automatically (server backgrounded via `nbd-start`/`nbd-stop`).
`make install-qemu` installs `qemu-system-x86_64` if it isn't on `PATH`.
`make guest-image` (optional, needs root) bakes a real bootable Debian guest
with a workload script instead of `make disk`'s blank file — see
`kvm/guest/build_image.sh`.

No env vars beyond what's implicit in `PATH` (`qemu-system-x86_64`,
`python3`, `debootstrap` for `guest-image`). All tunables are Makefile
variables with `?=` defaults (`NBD_*`, `DISK_*`, `VMS_CONFIG`, `GUEST_*`) —
see the Makefile itself for the full list.

## Tests, linting, type-checking

No test framework, CI config or linter config. The only checks are
behavioural scripts: `python3 tools/pf/test_policies.py` (synthetic streams
through the learned prefetch policies, needs `tools/pf/build.sh` first) and
`python3 disk/test_fwd.py` (see the directory table). Don't invent other test
commands; if asked to "run the tests," say which of these exist.

## Directory structure

| Path | Purpose |
|---|---|
| `Makefile` | Real entrypoint: build + orchestrate the whole disk→server→VM workflow |
| `README.md` | User-facing walkthrough and config schema reference |
| `disk/nbd.cpp`, `disk/nbd.hpp` | NBD protocol server (`nbd_server`) — reads/writes the wire protocol |
| `disk/sim.hpp` | `SimReadClass` — drives `policy::Cache` from real NBD reads via `pread` |
| `disk/nbd.example.yaml`, `disk/nbd.yaml` | `nbd_server` config schema + committed default |
| `disk/fwd.hpp`, `disk/fwd.cpp` | Remote-disk forwarding: `RemoteDiskServer` (emulated link), `RemoteClient`, `DirectReadClass`, `FwdReadClass` (local cache + readahead); `nbd_server` uses them for `source: remote` |
| `disk/fwd_sim.cpp`, `disk/fwd_experiment.py` | `bin/fwd_sim serve/mkdisk/bench` and the direct-vs-cache experiment matrix (`make fwd-experiment`) |
| `disk/test_fwd.py` | End-to-end test of `source: remote` incl. pipelined reads (`make nbd fwd-sim` first) |
| `tools/capture/vm_remote_study.py` | VM-in-the-loop study: QEMU guest + FAISS over the emulated remote disk (needs `/dev/kvm`, no root); `--ablation` varies guest readahead, `--depth-sweep` the host's readahead depth, `--diag` why deep readahead loses |
| `disk/fwd_calibrate.py`, `disk/fwd_report.py` | Emulated-link accuracy check; builds the write-up page from `logs/fwd_dc2`, `logs/fwd_vm` |
| `policies/policy_api.h`, `policies/cache.cpp` | `Cache` + `CachePolicy` base API |
| `policies/policy_*.cpp` | Concrete evict/prefetch policies (see glossary for names) |
| `policies/policy_factory.h` | Spec parser + factory (`name[:k=v,..]`, `a+b` = union) shared by `nbd_server` and `psim`; defines the `learned` preset |
| `policies/policy_nextreq.cpp`, `policies/policy_neighborhood.cpp` | The learned VM prefetchers (`nextreq`, `neighborhood`; `learned` runs both); `policy_lru_seg.cpp` = `lru_seg` evict |
| `tools/pf/` | Policy research harness: `psim` (replay a captured VM stream through the real policies in seconds), stream-suite capture, live-VM eval, tests. See `tools/pf/README.md` |
| `tools/capture/build_thesios_disk.py` | Sparse 8 TiB data disk + condensed trace so `run_capture.py --workload thesios` runs Thesios in a VM without root |
| `policies/assoc_miner.h` | Shared association-mining engine for cminer/quickmine/mithril |
| `policies/stream_tracker.h` | Shared stream-detection for the `*_cxt_aware` policy pair |
| `kvm/launch.py` | Starts/stops/waits-on QEMU VMs from a YAML config |
| `kvm/vms.yaml`, `kvm/vms.example.yaml` | VM configs — `vms.yaml` is the real default, `.example` is a schema reference |
| `kvm/guest/build_image.sh` | Bakes a real bootable Debian guest with a workload (`make guest-image`) |
| `kvm/guest/workloads/faiss_bench.py` | Default baked-in workload (CPU-heavy FAISS benchmark) |
| `kvm/guest/prepare_thesios.py` | Downloads Thesios shards and condenses them to read ranges |
| `kvm/guest/workloads/thesios_bench.py` | Replays a baked condensed Thesios offset/length trace |

## Naming conventions actually observed

- **C++**: `PascalCase` for free functions in `disk/nbd.cpp` (`ReadFull`,
  `LoadConfig`, `AppendLog`), `PascalCase` for classes everywhere
  (`SimReadClass`, `LRUPolicy`), `snake_case` for member functions/variables
  inside classes (`on_access`, `hit_latency_ns`), `kPascalCase` for
  `constexpr` constants (`kNbdMagic`, `kRepMagic`). Mixed convention is
  consistent within each file — match whichever file you're editing.
- **Python**: standard `snake_case` (`kvm/launch.py`).
- **YAML config keys**: `snake_case` (`hit_latency_ns`, `evict_policy`).
- Policy/source names are consistently the paper/algorithm name
  lowercased (`cminer`, `quickmine`, `mithril`, `lru_cxt_aware`).

## Known gotchas / footguns

- **`disk/nbd.cpp` is a hand-rolled NBD server, not a library.** It got a
  real wire-protocol bug (server option replies reusing the client's
  `IHAVEOPT` magic instead of `NBD_REP_MAGIC`) that `nbd-client` tolerated
  silently but QEMU's stricter client rejected outright. If VMs fail to
  attach with a wire-protocol-sounding error, suspect the handshake code
  (`DoHandshake`) before anything else.
- **All ports on one `nbd_server` process share the same backing file.**
  `ports:` only gives each VM a distinct *identity* for cache/timing
  purposes (`worker_id`) — it does not give them distinct disk contents. Run
  separate `nbd_server` processes/configs if VMs need different data.
- **Guest writes persist only for the life of the server.** `nbd_server`
  opens the backing file read-only; writes and write-zeroes go to a
  copy-on-write overlay (`disk/overlay.hpp`): an unlinked temp file in
  `overlay_dir` holding the modified `overlay_block_size` blocks, plus a
  block->slot map that reads are patched from. It vanishes on exit and the
  backing file is never changed. Trim is a no-op and writes don't affect
  cache residency. All ports share one overlay, so VMs on different ports
  see each other's writes.
- **`kvm/launch.py`'s serial console goes to a file, not stdio.**
  `-serial mon:stdio` is incompatible with `-daemonize` (QEMU detaches from
  the terminal); serial output goes to `<log_dir>/<name>.serial.log`
  instead, in both daemonize modes.
- **`sigwait`-based shutdown in `disk/nbd.cpp` doesn't behave reliably in
  every environment** — verified correct on Linux (the actual target), but
  a bare extra `pthread` breaks `sigwait` delivery on at least one macOS/
  Darwin libc build (unrelated to this code — a host libc quirk). Don't
  "fix" this pattern based on macOS test behavior.
- **`disk/fwd.cpp` is no longer a stub** — it holds the remote-disk forwarding
  layer (see the directory table). Its latency numbers are wall-clock, so run
  `make fwd-experiment` on a quiet machine; the link's RTT/bandwidth are real
  `sleep`s (~60 µs of timer slop each), and `fwd_sim` is a separate binary from
  `nbd_server` (both compile `fwd.cpp`).
- **Policies see byte-exact requests through `CachePolicy::on_request`**
  (offset/length in bytes + arrival ns), called just before
  `on_prefetch_request`, whose arguments are page-granular. `nextreq` needs it
  (a 120 KiB and a 128 KiB window are the same page count). Callers: `SimReadClass`,
  `FwdReadClass`, `psim`. New callers of `on_prefetch_request` must call it too.
- **`bin/psim` and `bin/nbd_server` are built separately** (`tools/pf/build.sh`
  vs `make nbd`); rebuild both after touching `policies/`. `psim` matched
  `nbd_server`'s replay of the same stream to 0.01% when last checked.
- **`prefetch_policy:`/`evict_policy:` accept specs** (`learned:theta=30`,
  `readahead:pages=16`, `lru_seg:unused=75`); the old `readahead_pages:` key only
  applies to a bare `readahead`. `,` in a spec is replaced by `;` in the results CSV.
- The Makefile's `NBD_CONFIG` default (`disk/nbd.yaml`) and `VMS_CONFIG`
  default (`kvm/vms.yaml`) are **regenerated/expected to match each other**
  (same host:port) — if you change one's ports, update the other.

## Dangerous / never-automate commands

- `make guest-image` runs `debootstrap`/`mount`/`chroot` **as root** and
  writes directly to `DISK_IMG` — don't run this without the user's
  explicit go-ahead; it modifies system state (loop devices, mounts) beyond
  the repo.
- `make install-qemu` invokes the host's package manager with `sudo` — same
  caution as any other system package install.
- `make disk-clean` deletes the backing disk image directory
  (`disk/data/`) — real (if regenerable) data loss, including a baked
  `guest-image` build.
