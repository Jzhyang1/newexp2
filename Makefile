# Toolchain
CXX := g++

# Build flags (override from CLI if needed, e.g., make CXX=clang++)
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?=

# Output directories
BUILD_DIR := build
BIN_DIR := bin

# Source layout
POLICY_SRCS := $(wildcard policies/*.cpp)
WORKLOAD_SRCS := $(wildcard workloads/*.cpp)
APP_SRC := app.cpp
DAT_SRC := dat.cpp
RUNNER_SRC := runner.cpp

APP_BIN := $(BIN_DIR)/app
DAT_BIN := $(BIN_DIR)/dat
RUNNER_BIN := $(BIN_DIR)/runner

# Default experiment parameters for `make run`
SEED ?= 1
CAPACITY ?= 4096
EVICT_POLICY ?= lru
PREFETCH_POLICY ?= none
MISS_DELAY_NS ?= 300000
HIT_DELAY_NS ?=  100000
FILE_DATA ?= ./data.bin
CONFIG ?= configs/zipfian.txt
WARMUP ?= 0
LOG ?= ./logs/results.csv
PAGE_SPAN ?= 10485760
REQUESTS ?= 200000

# Ensure these targets always run when requested.
.PHONY: all dirs app dat runner run clean print-config

all: dirs app dat runner

dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR) ./logs

# Build the app workload executable.
app: $(APP_BIN)

$(APP_BIN): $(APP_SRC) $(WORKLOAD_SRCS) | dirs
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Build the cache daemon with policy implementations linked in.
dat: $(DAT_BIN)

$(DAT_BIN): $(DAT_SRC) $(POLICY_SRCS) | dirs
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Build the experiment orchestrator.
runner: $(RUNNER_BIN)

$(RUNNER_BIN): $(RUNNER_SRC) | dirs
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

print-config:
	@echo "N=$(N)"
	@echo "SEED=$(SEED)"
	@echo "EVICT_POLICY=$(EVICT_POLICY)"
	@echo "CAPACITY=$(CAPACITY)"
	@echo "MISS_DELAY_NS=$(MISS_DELAY_NS)"
	@echo "HIT_DELAY_NS=$(HIT_DELAY_NS)"
	@echo "FILE_DATA=$(FILE_DATA)"
	@echo "FRAC_SCAN=$(FRAC_SCAN)"
	@echo "WARMUP=$(WARMUP)"
	@echo "LOG=$(LOG)"

# Run one full experiment via runner. Override vars at invocation time.
run: all print-config
	$(RUNNER_BIN) \
		--seed $(SEED) \
		--evict-policy $(EVICT_POLICY) \
		--prefetch-policy $(PREFETCH_POLICY) \
		--capacity $(CAPACITY) \
		--miss-delay $(MISS_DELAY_NS) \
		--hit-delay $(HIT_DELAY_NS) \
		--file-data $(FILE_DATA) \
		--warmup-period $(WARMUP) \
		--log $(LOG) \
		--requests $(REQUESTS) \
		--config $(CONFIG)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# ---------------------------------------------------------------------------
# userfaultfd variant: lhook (LD_PRELOAD mmap interceptor), ldat (uffd-driven
# cache/policy daemon), lrunner (orchestrator). Linux-only (userfaultfd,
# process_madvise, /proc) -- not part of `all`/`run` since they cannot build
# on macOS. Build these inside the Linux VM with `make luffd`.
LIB_DIR := lib
LHOOK_SRC := lhook.cpp
LDAT_SRC := ldat.cpp
LRUNNER_SRC := lrunner.cpp
UFFD_PROTOCOL_HDR := uffd_protocol.h

LHOOK_LIB := $(LIB_DIR)/liblhook.so
LDAT_BIN := $(BIN_DIR)/ldat
LRUNNER_BIN := $(BIN_DIR)/lrunner

# lapp: a twin of app.cpp that actually performs its accesses (mmap + real
# reads) instead of describing them over a pipe -- see lapp.cpp. It has no
# userfaultfd dependency itself, so it builds on macOS too, but it's only
# useful for its intended purpose (verifying the uffd pipeline) run under
# lrunner on Linux, so it's grouped with the rest of luffd below.
LAPP_SRC := lapp.cpp
LAPP_BIN := $(BIN_DIR)/lapp

# Watch prefix + hook lib path for `make lrun`; APP_CMD is the unmodified
# real application to launch, e.g. APP_CMD="redis-server --daemonize no".
WATCH_PREFIX ?= /mnt/remote
SOCKET_PATH ?= /tmp/ldat_uffd.sock
APP_CMD ?=

# `make lverify` defaults: runs lapp itself (not a real app) through lrunner,
# as a self-contained sanity check of the whole uffd pipeline.
LAPP_FILE ?= $(WATCH_PREFIX)/lapp_test.dat
LAPP_BEHAVIOR ?= zipfian
LAPP_REQUESTS ?= 20000

.PHONY: luffd lhook ldat lrunner lapp lrun lverify ldirs

ldirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR) $(LIB_DIR) ./logs

luffd: lhook ldat lrunner lapp

lhook: $(LHOOK_LIB)

$(LHOOK_LIB): $(LHOOK_SRC) $(UFFD_PROTOCOL_HDR) | ldirs
	$(CXX) $(CXXFLAGS) -shared -fPIC $(LHOOK_SRC) -o $@ -ldl

ldat: $(LDAT_BIN)

$(LDAT_BIN): $(LDAT_SRC) $(POLICY_SRCS) $(UFFD_PROTOCOL_HDR) | ldirs
	$(CXX) $(CXXFLAGS) $(LDAT_SRC) $(POLICY_SRCS) -o $@ $(LDFLAGS)

lrunner: $(LRUNNER_BIN)

$(LRUNNER_BIN): $(LRUNNER_SRC) $(UFFD_PROTOCOL_HDR) | ldirs
	$(CXX) $(CXXFLAGS) $(LRUNNER_SRC) -o $@ $(LDFLAGS)

lapp: $(LAPP_BIN)

$(LAPP_BIN): $(LAPP_SRC) $(WORKLOAD_SRCS) | ldirs
	$(CXX) $(CXXFLAGS) $(LAPP_SRC) $(WORKLOAD_SRCS) -o $@ $(LDFLAGS)

# Run one real-app experiment via lrunner. Override vars at invocation time,
# e.g.: make lrun WATCH_PREFIX=/mnt/remote APP_CMD="redis-server /etc/redis.conf"
lrun: luffd
	$(LRUNNER_BIN) \
		--evict-policy $(EVICT_POLICY) \
		--prefetch-policy $(PREFETCH_POLICY) \
		--capacity $(CAPACITY) \
		--miss-delay $(MISS_DELAY_NS) \
		--hit-delay $(HIT_DELAY_NS) \
		--watch-prefix $(WATCH_PREFIX) \
		--socket $(SOCKET_PATH) \
		--hook-lib $(LHOOK_LIB) \
		--warmup-period $(WARMUP) \
		--log ./logs/lresults.csv \
		-- $(APP_CMD)

# Self-contained sanity check: runs lapp (not a real app) through lrunner, so
# there's nothing external to install first. A working run should show
# ldat's STATS line with a plausible hit_ratio, and lapp's own
# pages_touched count matching --requests (scan/zipfian's default
# scan-ratio=0 makes that an exact match; override LAPP_BEHAVIOR=scan or
# pass --scan-ratio to exercise multi-page ops too).
lverify: luffd
	$(LRUNNER_BIN) \
		--evict-policy $(EVICT_POLICY) \
		--prefetch-policy $(PREFETCH_POLICY) \
		--capacity $(CAPACITY) \
		--miss-delay $(MISS_DELAY_NS) \
		--hit-delay $(HIT_DELAY_NS) \
		--watch-prefix $(WATCH_PREFIX) \
		--socket $(SOCKET_PATH) \
		--hook-lib $(LHOOK_LIB) \
		--warmup-period $(WARMUP) \
		--log ./logs/lverify.csv \
		-- $(LAPP_BIN) --behavior $(LAPP_BEHAVIOR) --file $(LAPP_FILE) --page-span $(PAGE_SPAN) --requests $(LAPP_REQUESTS)