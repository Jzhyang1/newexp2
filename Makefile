# Toolchain
CXX := g++

# Build flags (override from CLI if needed, e.g., make CXX=clang++)
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?=

# Output directories
BUILD_DIR := build
BIN_DIR := bin

# Source layout
POLICY_SRCS := policies/cache.cpp policies/policy_lru.cpp policies/policy_lru_cxt_aware.cpp policies/policy_readahead.cpp
APP_SRC := app.cpp
DAT_SRC := dat.cpp
RUNNER_SRC := runner.cpp

APP_BIN := $(BIN_DIR)/app
DAT_BIN := $(BIN_DIR)/dat
RUNNER_BIN := $(BIN_DIR)/runner

# Default experiment parameters for `make run`
N ?= 1
SEED ?= 1
CAPACITY ?= 8192
CACHE_POLICY ?= lru
PREFETCH_POLICY ?= none
MISS_DELAY_NS ?= 300000
HIT_DELAY_NS ?= 1
FILE_DATA ?= ./data.bin
FRAC_SCAN ?= 0.5
WARMUP ?= 0
LOG ?= ./logs/results.csv

# Ensure these targets always run when requested.
.PHONY: all dirs app dat runner run clean print-config

all: dirs app dat runner

dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR) ./logs

# Build the app workload executable.
app: $(APP_SRC)
	$(CXX) $(CXXFLAGS) $< -o $(APP_BIN) $(LDFLAGS)

# Build the cache daemon with policy implementations linked in.
dat: $(DAT_SRC) $(POLICY_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $(DAT_BIN) $(LDFLAGS)

# Build the experiment orchestrator.
runner: $(RUNNER_SRC)
	$(CXX) $(CXXFLAGS) $< -o $(RUNNER_BIN) $(LDFLAGS)

print-config:
	@echo "N=$(N)"
	@echo "SEED=$(SEED)"
	@echo "CACHE_POLICY=$(CACHE_POLICY)"
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
		-n $(N) \
		--seed $(SEED) \
		--cache-policy $(CACHE_POLICY) \
		--prefetch $(PREFETCH_POLICY) \
		--capacity $(CAPACITY) \
		--miss-delay $(MISS_DELAY_NS) \
		--hit-delay $(HIT_DELAY_NS) \
		--file-data $(FILE_DATA) \
		--frac-scan $(FRAC_SCAN) \
		--warmup-period $(WARMUP) \
		--log $(LOG)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)