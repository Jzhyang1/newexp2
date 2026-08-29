# Builds and runs the disk-cache-policy simulation pipeline:
#
#   1. create disk   -> make disk
#   2. disk server   -> make nbd-run      (build + serve the disk over NBD)
#   3. spin up VMs   -> make vms-up       (attach VMs to the running server)
#   4. test on VMs   -> (interactive; ssh/console into a VM and drive it)
#   5. tear down     -> make vms-down
#
# `make experiment` runs stages 1-3 in one shot (disk server backgrounded
# instead of foregrounded, so it can move on to starting VMs) and leaves
# everything up for stage 4; `make experiment-down` is stage 5.
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
# why the default append line boots with systemd.volatile=yes. Needs root
# and debootstrap, and is Debian/Ubuntu-only -- skip it and use `make disk`
# instead if you just want the plain simulator with no guest workload.
GUEST_DISTRO ?= bookworm
GUEST_ARCH ?= amd64
GUEST_MIRROR ?= http://deb.debian.org/debian
GUEST_ROOTFS_SIZE ?= 3G
WORKLOAD_SCRIPT ?= kvm/guest/workloads/faiss_bench.py
GUEST_KERNEL ?= disk/data/vmlinuz
GUEST_INITRD ?= disk/data/initrd.img

.PHONY: guest-image
guest-image:
	$(SUDO) env ROOTFS_IMG=$(DISK_IMG) ROOTFS_SIZE=$(GUEST_ROOTFS_SIZE) DISTRO=$(GUEST_DISTRO) \
	  ARCH=$(GUEST_ARCH) MIRROR=$(GUEST_MIRROR) WORKLOAD_SCRIPT=$(WORKLOAD_SCRIPT) \
	  KERNEL_OUT=$(GUEST_KERNEL) INITRD_OUT=$(GUEST_INITRD) \
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
NBD_EVICT_POLICY ?= lru
NBD_PREFETCH_POLICY ?= none
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
#   make nbd-run NBD_PORTS="10809 10810" DISK_SIZE=4G NBD_EVICT_POLICY=fifo
nbd-run: nbd nbd-config disk
	$(NBD_BIN) $(NBD_CONFIG)

# Background counterpart to nbd-run, for driver targets (below) that need to
# start the server and then move on to starting VMs rather than blocking in
# the foreground. `make nbd-stop` sends SIGTERM, which triggers the same
# clean-shutdown stats logging as Ctrl-C'ing nbd-run.
NBD_PIDFILE ?= disk/nbd_server.pid
NBD_SERVER_LOG ?= disk/nbd_server.log

.PHONY: nbd-start nbd-stop
nbd-start: nbd nbd-config disk
	@if [ -f $(NBD_PIDFILE) ] && kill -0 "$$(cat $(NBD_PIDFILE))" 2>/dev/null; then \
		echo "nbd_server already running (pid $$(cat $(NBD_PIDFILE)))"; \
	else \
		nohup $(NBD_BIN) $(NBD_CONFIG) >$(NBD_SERVER_LOG) 2>&1 </dev/null & \
		echo $$! > $(NBD_PIDFILE); \
		sleep 1; \
		echo "nbd_server started (pid $$(cat $(NBD_PIDFILE))), log: $(NBD_SERVER_LOG)"; \
	fi

nbd-stop:
	@if [ -f $(NBD_PIDFILE) ]; then \
		kill -TERM "$$(cat $(NBD_PIDFILE))" 2>/dev/null || true; \
		rm -f $(NBD_PIDFILE); \
		echo "nbd_server stopped (stats appended to $(NBD_LOG))"; \
	else \
		echo "nbd_server not running (no $(NBD_PIDFILE))"; \
	fi

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

.PHONY: vms-up vms-down
vms-up:
	python3 kvm/launch.py $(VMS_CONFIG)

vms-down:
	python3 kvm/launch.py $(VMS_CONFIG) --stop

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
# Stages 1-3 end-to-end: create the disk, start the disk server in the
# background, and bring up the VMs -- then leaves everything running for
# stage 4 (test on VMs) interactively. Finish with `make experiment-down`,
# which is stage 5 (VMs stopped, then the server SIGTERM'd so it logs stats).
.PHONY: experiment experiment-down
experiment: nbd-start vms-up
	@echo ""
	@echo "Disk server and VMs are up -- test against the VMs now, then run:"
	@echo "  make experiment-down"

experiment-down: vms-down nbd-stop
