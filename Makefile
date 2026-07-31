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