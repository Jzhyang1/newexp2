# Builds and runs the disk-cache-policy simulation pipeline:
#
#   1. create disk   -> make disk
#   2. disk server   -> make nbd-run      (build + serve the disk over NBD)
#   3. spin up VMs   -> make vms-up       (attach VMs to the running server)
#   4. test on VMs   -> (interactive; ssh/console into a VM and drive it)
#   5. tear down     -> make vms-down
#
# `make experiment` runs all 5 stages end-to-end: disk server backgrounded
# (instead of foregrounded, so it can move on to starting VMs), VMs brought
# up, then it blocks until every VM's QEMU process exits on its own (e.g. a
# guest-image VM powering itself off once its baked-in workload finishes)
# before tearing everything down. `make experiment-down` remains available
# to tear down by hand (e.g. after interactive testing against a blank-disk
# VM, which never exits on its own).
#
# All of it is Linux-only (disk/nbd.cpp needs <endian.h>; kvm/launch.py needs
# qemu-system and /dev/kvm) -- run this on the Linux host/VM that will
# actually host the disk server and the guests.

# Toolchain
CXX := g++

# Build flags (override from CLI if needed, e.g., make CXX=clang++)
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?=

# Output directories
BUILD_DIR := build
BIN_DIR := bin

POLICY_SRCS := $(wildcard policies/*.cpp)

.PHONY: dirs clean
dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR) ./logs

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# ---------------------------------------------------------------------------
# Stage 1: the disk. A backing file that disk/nbd.cpp will serve over the
# NBD wire protocol -- created once as a sparse, zero-filled file of
# DISK_SIZE and left alone after that (delete it yourself to resize/reset).
# `dd` real content into it, or write files to it from inside a guest, once
# VMs can attach.
DISK_IMG ?= disk/data/nbd_disk.img
DISK_SIZE ?= 1G

.PHONY: disk disk-clean
disk: $(DISK_IMG)

$(DISK_IMG):
	@mkdir -p $(dir $(DISK_IMG))
	truncate -s $(DISK_SIZE) $(DISK_IMG)

disk-clean:
	rm -rf $(dir $(DISK_IMG))

# ---------------------------------------------------------------------------
# Stage 1b (optional): a real bootable guest image instead of the plain
# zero-filled file from `make disk` above -- a Debian rootfs with Python +
# numpy + faiss baked in, plus a systemd service (workload.service) that
# runs WORKLOAD_SCRIPT once on every boot. Also extracts the kernel/initrd
# QEMU needs to boot it (kvm/vms.yaml's kernel/initrd/append point at the
# GUEST_KERNEL/GUEST_INITRD defaults below).
#
# Everything this writes goes straight to $(DISK_IMG) via debootstrap/chroot
# on the host -- a different path from nbd_server, which discards every
# write a *running* guest makes to this same file (disk/sim.hpp), which is
# why the default append line boots with systemd.volatile=state (root +
# /opt/etc read-only from the real disk, only /var as a tmpfs overlay --
# NOT systemd.volatile=yes, which tmpfs's everything outside /usr and would
# silently wipe anything baked in under /opt, e.g. the YCSB dataset below).
# Needs root and debootstrap, and is Debian/Ubuntu-only -- skip it and use
# `make disk` instead if you just want the plain simulator with no guest
# workload.
GUEST_DISTRO ?= bookworm
GUEST_ARCH ?= amd64
GUEST_MIRROR ?= http://deb.debian.org/debian
# Sized for the default GUEST_YCSB=1 dataset (~11.5G) plus base OS/JRE
# (~1G) and ext4 overhead -- drop back to 3G if you set GUEST_YCSB=0 and
# use the plain faiss_bench.py workload instead.
GUEST_ROOTFS_SIZE ?= 16G
WORKLOAD_SCRIPT ?= kvm/guest/workloads/ycsb_bench.py
GUEST_KERNEL ?= disk/data/vmlinuz
GUEST_INITRD ?= disk/data/initrd.img

# Bakes a real YCSB (Java) benchmark + a pre-loaded SQLite dataset into the
# image at build time -- see kvm/guest/build_image.sh's YCSB_* block and
# kvm/guest/workloads/ycsb_bench.py. Only the read-only transaction phase
# runs at boot; the load phase runs here, at build time, so the dataset is
# genuinely written to $(DISK_IMG) instead of the guest's volatile tmpfs.
# Only read-only workloads are viable at boot (default workloadc, 100%
# read) since the guest's root is mounted `ro`. The record count is chosen
# to land well above kvm/vms.yaml's guest `memory: 4G` -- otherwise the
# guest's own page cache would absorb repeats after the first pass and
# nbd_server's cache/prefetch policies would rarely see a real miss.
GUEST_YCSB ?= 1
GUEST_YCSB_VERSION ?= 0.17.0
SQLITE_JDBC_VERSION ?= 3.46.1.3
GUEST_YCSB_RECORDS ?= 10000000
GUEST_YCSB_FIELD_COUNT ?= 10
GUEST_YCSB_FIELD_LENGTH ?= 100
GUEST_YCSB_WORKLOAD ?= workloadc
GUEST_YCSB_OPERATIONS ?= 200000
GUEST_YCSB_DISTRIBUTION ?= zipfian
GUEST_YCSB_THREADS ?= 4

.PHONY: guest-image
guest-image:
	$(SUDO) apt-get install debootstrap
	$(SUDO) env ROOTFS_IMG=$(DISK_IMG) ROOTFS_SIZE=$(GUEST_ROOTFS_SIZE) DISTRO=$(GUEST_DISTRO) \
	  ARCH=$(GUEST_ARCH) MIRROR=$(GUEST_MIRROR) WORKLOAD_SCRIPT=$(WORKLOAD_SCRIPT) \
	  KERNEL_OUT=$(GUEST_KERNEL) INITRD_OUT=$(GUEST_INITRD) \
	  YCSB_ENABLE=$(GUEST_YCSB) YCSB_VERSION=$(GUEST_YCSB_VERSION) \
	  SQLITE_JDBC_VERSION=$(SQLITE_JDBC_VERSION) YCSB_RECORDS=$(GUEST_YCSB_RECORDS) \
	  YCSB_FIELD_COUNT=$(GUEST_YCSB_FIELD_COUNT) YCSB_FIELD_LENGTH=$(GUEST_YCSB_FIELD_LENGTH) \
	  YCSB_WORKLOAD=$(GUEST_YCSB_WORKLOAD) YCSB_OPERATIONS=$(GUEST_YCSB_OPERATIONS) \
	  YCSB_DISTRIBUTION=$(GUEST_YCSB_DISTRIBUTION) YCSB_THREADS=$(GUEST_YCSB_THREADS) \
	  bash kvm/guest/build_image.sh

# ---------------------------------------------------------------------------
# Stage 2: the disk server. nbd_server (disk/nbd.cpp) serves $(DISK_IMG)
# over the NBD wire protocol so that QEMU VMs can attach it as a plain
# virtio-blk disk -- see disk/nbd.example.yaml for the config schema.
NBD_SRC := disk/nbd.cpp
NBD_BIN := $(BIN_DIR)/nbd_server
NBD_CONFIG ?= disk/nbd.yaml

# One TCP port per client identity the simulator should track separately
# (see disk/nbd.example.yaml) -- space-separated, e.g. NBD_PORTS = 10809 10810.
NBD_PORTS ?= 10809
NBD_CAPACITY ?= 4096
NBD_HIT_LATENCY_NS ?= 30000
NBD_MISS_LATENCY_NS ?= 300000
NBD_WARMUP ?= 0
NBD_LOG ?= ./logs/nbd_results.csv

comma := ,
empty :=
space := $(empty) $(empty)
NBD_PORTS_YAML := $(subst $(space),$(comma)$(space),$(strip $(NBD_PORTS)))

.PHONY: nbd nbd-config nbd-run
nbd: $(NBD_BIN)

$(NBD_BIN): $(NBD_SRC) disk/nbd.hpp disk/sim.hpp $(POLICY_SRCS) | dirs
	$(CXX) $(CXXFLAGS) $(NBD_SRC) $(POLICY_SRCS) -o $@ $(LDFLAGS)


# Build, provision the backing disk, regenerate the config, and run in the
# foreground. Ctrl-C (or SIGTERM) stops it cleanly and appends one row of
# stats to NBD_LOG. Override any NBD_*/DISK_* variable at invocation, e.g.:
#   make nbd-run NBD_PORTS="10809 10810" DISK_SIZE=4G
nbd-run: nbd nbd-config disk
	$(NBD_BIN) $(NBD_CONFIG)

# Background counterpart to nbd-run, for driver targets (below) that need to
# start the server and then move on to starting VMs rather than blocking in
# the foreground. `make nbd-stop` sends SIGTERM, which triggers the same
# clean-shutdown stats logging as Ctrl-C'ing nbd-run.
NBD_PIDFILE ?= disk/nbd_server.pid
NBD_SERVER_LOG ?= disk/nbd_server.log

.PHONY: nbd-start nbd-stop nbd-stats
nbd-start: nbd nbd-config disk
	@if [ -f $(NBD_PIDFILE) ] && kill -0 "$$(cat $(NBD_PIDFILE))" 2>/dev/null; then \
		echo "nbd_server already running (pid $$(cat $(NBD_PIDFILE)))"; \
	else \
		nohup $(NBD_BIN) $(NBD_CONFIG) >$(NBD_SERVER_LOG) 2>&1 </dev/null & \
		echo $$! > $(NBD_PIDFILE); \
		sleep 1; \
		if kill -0 "$$(cat $(NBD_PIDFILE))" 2>/dev/null; then \
			echo "nbd_server started (pid $$(cat $(NBD_PIDFILE))), log: $(NBD_SERVER_LOG)"; \
		else \
			echo "nbd_server FAILED to start -- it exited immediately; last lines of $(NBD_SERVER_LOG):" >&2; \
			tail -n 20 $(NBD_SERVER_LOG) >&2 || true; \
			rm -f $(NBD_PIDFILE); \
			exit 1; \
		fi; \
	fi

nbd-stop:
	@if [ -f $(NBD_PIDFILE) ]; then \
		kill -TERM "$$(cat $(NBD_PIDFILE))" 2>/dev/null || true; \
		rm -f $(NBD_PIDFILE); \
		echo "nbd_server stopped (stats appended to $(NBD_LOG))"; \
	else \
		echo "nbd_server not running (no $(NBD_PIDFILE))"; \
	fi

# Requests a one-shot "STATS ..." snapshot (stderr/$(NBD_SERVER_LOG) only --
# not appended to $(NBD_LOG), to avoid duplicate CSV rows) from an
# already-running nbd_server, without stopping it. Useful for telling
# "still working" from "stuck" on a server left running unattended.
nbd-stats:
	@if [ -f $(NBD_PIDFILE) ] && kill -0 "$$(cat $(NBD_PIDFILE))" 2>/dev/null; then \
		kill -USR1 "$$(cat $(NBD_PIDFILE))"; \
		echo "requested stats snapshot from nbd_server (pid $$(cat $(NBD_PIDFILE))); see $(NBD_SERVER_LOG)"; \
	else \
		echo "nbd_server not running (no $(NBD_PIDFILE))"; \
	fi

# Fast, deterministic capture-correctness check: speaks the NBD wire
# protocol directly (no QEMU/VM needed) against a fresh nbd_server and
# verifies every read it sends is captured with the right worker_id/offset/
# length and correct data. See disk/test_protocol.py's docstring for the
# real-VM end-to-end complement (kvm/verify_capture.py).
.PHONY: test-protocol
test-protocol: nbd
	python3 disk/test_protocol.py --binary $(NBD_BIN)

# ---------------------------------------------------------------------------
# Stage 3: the VMs. Wraps kvm/launch.py, which reads a YAML config (see
# kvm/vms.yaml, a single VM matching the default disk/nbd.yaml; or
# kvm/vms.example.yaml for a multi-VM/multi-disk schema reference) describing
# one or more QEMU guests and wires each guest's disks to nbd_server over the
# network via `driver=nbd`. Point VMS_CONFIG at your own file once its
# `nbd.host`/`nbd.port` entries need to differ from the defaults.
#
# Needs qemu-system-x86_64 on PATH -- `make install-qemu` if it isn't there
# yet.
VMS_CONFIG ?= kvm/vms.yaml

# vms-wait progress/timeout: heartbeat prints "[Ns] still waiting for: ..."
# every VMS_WAIT_HEARTBEAT seconds so a long wait isn't indistinguishable
# from a hung one. VMS_WAIT_TIMEOUT is empty (disabled) by default so a
# legitimately long benchmark run is never killed by a default -- set it
# (seconds) when you specifically want vms-wait to give up and exit nonzero
# instead of blocking forever.
VMS_WAIT_TIMEOUT ?=
VMS_WAIT_HEARTBEAT ?= 60

.PHONY: vms-up vms-down vms-wait
vms-up:
	python3 kvm/launch.py $(VMS_CONFIG)

vms-down:
	python3 kvm/launch.py $(VMS_CONFIG) --stop

# Blocks until every VM's QEMU process exits -- e.g. a guest-image VM
# powering itself off once its baked-in workload.service finishes (see
# kvm/guest/build_image.sh) -- so a driver script knows when it's safe to
# collect the serial log and run `make vms-down`/`experiment-down`.
vms-wait:
	python3 kvm/launch.py $(VMS_CONFIG) --wait --wait-heartbeat $(VMS_WAIT_HEARTBEAT) $(if $(VMS_WAIT_TIMEOUT),--wait-timeout $(VMS_WAIT_TIMEOUT))

# Installs qemu-system-x86_64 via whatever package manager this host has, a
# no-op if it's already on PATH. Uses sudo unless already running as root.
# Only x86_64 is covered since that's what VMS_CONFIG/vms.example.yaml's
# qemu_binary defaults to -- override the package name below if you need a
# different arch's qemu-system-* package.
SUDO := $(shell [ "$$(id -u)" = "0" ] && echo || echo sudo)

.PHONY: install-qemu
install-qemu:
	@if command -v qemu-system-x86_64 >/dev/null 2>&1; then \
		echo "qemu-system-x86_64 already installed: $$(command -v qemu-system-x86_64)"; \
	elif command -v apt-get >/dev/null 2>&1; then \
		$(SUDO) apt-get update && $(SUDO) apt-get install -y qemu-system-x86; \
	elif command -v dnf >/dev/null 2>&1; then \
		$(SUDO) dnf install -y qemu-kvm; \
	elif command -v yum >/dev/null 2>&1; then \
		$(SUDO) yum install -y qemu-kvm; \
	elif command -v pacman >/dev/null 2>&1; then \
		$(SUDO) pacman -Sy --noconfirm qemu-system-x86; \
	elif command -v zypper >/dev/null 2>&1; then \
		$(SUDO) zypper install -y qemu-kvm; \
	elif command -v apk >/dev/null 2>&1; then \
		$(SUDO) apk add qemu-system-x86_64; \
	elif command -v brew >/dev/null 2>&1; then \
		brew install qemu; \
	else \
		echo "no supported package manager found (looked for apt-get/dnf/yum/pacman/zypper/apk/brew);" >&2; \
		echo "install qemu-system-x86_64 manually" >&2; \
		exit 1; \
	fi

# ---------------------------------------------------------------------------
# Stages 1-5 end-to-end: create the disk, start the disk server in the
# background, bring up the VMs, block until every VM's QEMU process exits on
# its own (e.g. a guest-image VM powering itself off once its baked-in
# workload.service finishes -- see kvm/guest/build_image.sh), then tear
# everything down (VMs stopped, server SIGTERM'd so it logs stats).
# `make experiment-down` remains available to tear down by hand instead
# (e.g. after interactive testing against a blank-disk VM, which never
# exits on its own).
.PHONY: experiment experiment-down
experiment: nbd-start vms-up
	$(MAKE) vms-wait
	$(MAKE) experiment-down

experiment-down: vms-down nbd-stop

# ---------------------------------------------------------------------------
# lru+readahead vs. lru_cxt_aware+readahead_cxt_aware comparison: the same
# 2-VM group (kvm/vms_lru_vs_cxtaware.yaml) run once against each policy
# pair's nbd_server config (disk/nbd_lru_readahead.yaml /
# disk/nbd_cxt_aware.yaml). Run sequentially, not concurrently -- one
# nbd_server process's Cache/evict/prefetch policy is shared across every VM
# attached to it (disk/nbd.cpp), so comparing two policy pairs needs two
# separate runs, and running them at the same time would have the two
# processes competing for host CPU during the VMs' workload, which would
# skew the comparison. Each writes its own stats row to a separate CSV (see
# each yaml's `log:`) for the two runs to be compared afterward. Needs
# `make guest-image` to have been run first, same as `make experiment`.
.PHONY: experiment-lru-readahead experiment-cxt-aware
experiment-lru-readahead:
	$(MAKE) experiment NBD_CONFIG=disk/nbd_lru_readahead.yaml VMS_CONFIG=kvm/vms_lru_vs_cxtaware.yaml

experiment-cxt-aware:
	$(MAKE) experiment NBD_CONFIG=disk/nbd_cxt_aware.yaml VMS_CONFIG=kvm/vms_lru_vs_cxtaware.yaml
