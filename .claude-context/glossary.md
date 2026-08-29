# Glossary

Domain terms and internal names used in this codebase that aren't obvious
from context alone.

## Workflow stages (used throughout Makefile/README)

- **disk** — the backing file `nbd_server` serves (`disk/data/nbd_disk.img`
  by default). Either a blank zero-filled file (`make disk`) or a real
  bootable guest image (`make guest-image`).
- **worker_id / client identity** — which TCP port a VM's disk connected
  on; the unit the simulator uses to keep per-VM cache-warmup/virtual-time
  state separate. Not related to OS thread IDs despite the name.
- **export** — an NBD protocol concept (a named block device a server
  offers). This codebase's server ignores it — see glossary entry "port ≠
  export" below.

## NBD protocol terms

- **NBD** — Network Block Device: a Linux protocol for exposing a remote
  file/device as a local block device over TCP.
- **`IHAVEOPT`** — the magic value a *client* sends when starting an option
  negotiation request during the handshake.
- **`NBD_REP_MAGIC`** (`kRepMagic` in code) — the magic value a *server*
  must use in its option-reply header. Distinct from `IHAVEOPT` — conflating
  the two was a real bug here (see `DECISIONS.md`).
- **`NBD_OPT_GO` / `NBD_OPT_EXPORT_NAME`** — two ways a client can request
  an export. This server always replies "unsupported" to `NBD_OPT_GO` so
  the client falls back to the older `NBD_OPT_EXPORT_NAME`, which it does
  support.
- **Fixed newstyle handshake** — the modern NBD handshake variant this
  server implements (as opposed to the deprecated "oldstyle" handshake).
- **Simple-reply** — the (non-structured) NBD transmission-phase reply
  format used for read/write command responses.
- **port ≠ export** (project-specific usage): normally in NBD, one server
  can offer multiple named exports over one port. Here, one *port* = one
  client identity, and every port on a process serves the *same* file —
  the export name field in requests is accepted but ignored.

## Cache/policy terms

- **evict policy** — decides which cached page(s) to remove when the cache
  is full (`none`, `fifo`, `lifo`, `lru`, `lru_cxt_aware`).
- **prefetch policy** — decides which page(s) to speculatively load ahead
  of a request (`none`, `readahead`, `readahead_cxt_aware`, `cminer`,
  `quickmine`, `mithril`).
- **context** — an opaque client identifier (in practice, `worker_id`)
  passed to every policy hook, letting a policy keep per-client state
  instead of one global view. "Context-aware" policies (`*_cxt_aware`) use
  this; the plain ones ignore it.
- **C-Miner / QuickMine / Mithril** — three published prefetch-policy
  algorithms, all sharing one "windowed association mining" engine
  (`policies/assoc_miner.h`): remember which pages tend to follow which
  within a trailing window, then prefetch the strongest followers of the
  page just seen. They differ only in what the mining window is scoped to
  — see `ARCHITECTURE.md` and the header comment in `assoc_miner.h`.
- **stream** (in `stream_tracker.h`/`*_cxt_aware` policies) — a run of
  sequentially-adjacent page accesses under one context, detected the way a
  hardware stride/stream prefetcher would (a small fixed number of tracked
  "heads" per context, each just the most recently seen address). Not
  related to C++ `iostream` or network "streams."
- **`attach_window`** — how close a new access must be to a tracked
  stream's head to be considered part of that stream (`StreamTracker`).
- **hit / miss latency** — simulated, configured latency (`hit_latency_ns`,
  `miss_latency_ns`) charged per request depending on cache presence — not
  a measurement of real disk speed.
- **warmup** — a count of initial requests excluded from stats, so cold-
  cache startup doesn't skew reported hit ratio.

## VM/guest terms

- **`vms.yaml` vs `vms.example.yaml`** — `vms.yaml` is the real, committed
  default the Makefile actually uses (`VMS_CONFIG`); `.example.yaml` is a
  documentation-only multi-VM/multi-disk schema reference, never read by
  any target directly.
- **`workload.service`** — the systemd unit `make guest-image` bakes into
  the guest, which runs the configured `WORKLOAD_SCRIPT` once on every boot
  then powers the guest off (so `make vms-wait` can detect completion by
  watching for the QEMU process to exit).
- **`systemd.volatile=yes`** — kernel cmdline option that mounts guest root
  read-only with a tmpfs overlay for `/etc`/`/var`. Used because
  `nbd_server` discards every write a *running* guest makes anyway — the
  guest OS must not depend on writes persisting.
- **serial log** (`<log_dir>/<name>.serial.log`) — where a guest's console
  output goes (see `DECISIONS.md`'s `mon:stdio` entry) — the primary way to
  observe a headless (`network.type: none`) guest's output without ssh.
- **monitor socket** (`<run_dir>/<name>.monitor`) — a QEMU control socket
  (unix domain, `nowait`) for issuing QEMU monitor commands to a running VM
  interactively (e.g. via `socat`/`nc`).
