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

**None exist yet.** No test framework, no CI config, no linter config
anywhere in the repo. Don't invent a test command that isn't there — if
asked to "run the tests," say there aren't any.

## Directory structure

| Path | Purpose |
|---|---|
| `Makefile` | Real entrypoint: build + orchestrate the whole disk→server→VM workflow |
| `README.md` | User-facing walkthrough and config schema reference |
| `disk/nbd.cpp`, `disk/nbd.hpp` | NBD protocol server (`nbd_server`) — reads/writes the wire protocol |
| `disk/sim.hpp` | `SimReadClass` — drives `policy::Cache` from real NBD reads via `pread` |
| `disk/nbd.example.yaml`, `disk/nbd.yaml` | `nbd_server` config schema + committed default |
| `disk/fwd.cpp` | Empty stub — deliberate placeholder, not implemented |
| `policies/policy_api.h`, `policies/cache.cpp` | `Cache` + `CachePolicy` base API |
| `policies/policy_*.cpp` | Concrete evict/prefetch policies (see glossary for names) |
| `policies/assoc_miner.h` | Shared association-mining engine for cminer/quickmine/mithril |
| `policies/stream_tracker.h` | Shared stream-detection for the `*_cxt_aware` policy pair |
| `kvm/launch.py` | Starts/stops/waits-on QEMU VMs from a YAML config |
| `kvm/vms.yaml`, `kvm/vms.example.yaml` | VM configs — `vms.yaml` is the real default, `.example` is a schema reference |
| `kvm/guest/build_image.sh` | Bakes a real bootable Debian guest with a workload (`make guest-image`) |
| `kvm/guest/workloads/faiss_bench.py` | Default baked-in workload (CPU-heavy FAISS benchmark) |

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
- **Writes from guests are silently discarded.** `nbd_server` opens the
  backing file read-only and never persists guest writes — this is by
  design (it's a read-cache simulator), but it means a `make guest-image`
  guest boots with `systemd.volatile=yes` (tmpfs overlay) so it never
  depends on a write landing on disk.
- **`kvm/launch.py`'s serial console goes to a file, not stdio.**
  `-serial mon:stdio` is incompatible with `-daemonize` (QEMU detaches from
  the terminal); serial output goes to `<log_dir>/<name>.serial.log`
  instead, in both daemonize modes.
- **`sigwait`-based shutdown in `disk/nbd.cpp` doesn't behave reliably in
  every environment** — verified correct on Linux (the actual target), but
  a bare extra `pthread` breaks `sigwait` delivery on at least one macOS/
  Darwin libc build (unrelated to this code — a host libc quirk). Don't
  "fix" this pattern based on macOS test behavior.
- **`disk/fwd.cpp` is an intentional empty stub**, not dead code by
  accident — don't delete it without confirming with the author first.
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
