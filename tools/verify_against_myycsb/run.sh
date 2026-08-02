#!/usr/bin/env bash
# Verifies workloads/workload.cpp's key-generation against the exact upstream source
# it was ported from: My-YCSB at the pinned commit below. For each of our workloads'
# YCSB-relevant configurations, this builds a small harness against BOTH our code and
# the untouched upstream code, runs both with identical seeds/parameters, and diffs
# the resulting key (and scan-length) sequences. A match is a strong, concrete
# correctness signal: same seed + same rand_r()/zipfian/latest formulas + same op
# selection arithmetic + same output should only produce identical sequences if the
# port is faithful.
#
# Usage: tools/verify_against_myycsb/run.sh
set -euo pipefail

MYYCSB_COMMIT="df9f1a666c5c5ec97d62c6f7773876426c1d88c9"
MYYCSB_URL="https://github.com/xrp-project/My-YCSB.git"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOL_DIR="$REPO_ROOT/tools/verify_against_myycsb"
VENDOR_DIR="$REPO_ROOT/build/vendor/my-ycsb"
OUT_DIR="$REPO_ROOT/build/verify_myycsb"
CXX="${CXX:-g++}"
CXXFLAGS="-std=c++20 -O2"

mkdir -p "$OUT_DIR"

echo "== fetching My-YCSB@${MYYCSB_COMMIT:0:12} (reference implementation) =="
if [ ! -d "$VENDOR_DIR/.git" ]; then
    rm -rf "$VENDOR_DIR"
    git clone --quiet "$MYYCSB_URL" "$VENDOR_DIR"
fi
git -C "$VENDOR_DIR" checkout --quiet "$MYYCSB_COMMIT"

REF_WORKLOAD_CPP="$VENDOR_DIR/core/workload.cpp"
if [ ! -f "$REF_WORKLOAD_CPP" ]; then
    echo "error: expected $REF_WORKLOAD_CPP to exist at pinned commit" >&2
    exit 1
fi

# The upstream file unconditionally #includes <bpf/bpf.h> and <bpf/libbpf.h> for a
# Linux-only, already-commented-out BPF hook (fill_bpf_map_with_scan_pid and its
# call site are dead code in the pinned commit -- see the surrounding comments in
# core/workload.cpp). Strip only those two headers so this compiles without libbpf;
# no logic in the file is touched.
sed -i.bak -e '/#include <bpf\/bpf\.h>/d' -e '/#include <bpf\/libbpf\.h>/d' "$REF_WORKLOAD_CPP"
rm -f "$REF_WORKLOAD_CPP.bak"

echo "== building reference harness (upstream My-YCSB code) =="
"$CXX" $CXXFLAGS -I "$VENDOR_DIR/core/include" \
    "$TOOL_DIR/reference_dump.cpp" "$REF_WORKLOAD_CPP" \
    -o "$OUT_DIR/reference_dump"

echo "== building our harness (workloads/workload.cpp) =="
"$CXX" $CXXFLAGS \
    "$TOOL_DIR/our_dump.cpp" "$REPO_ROOT/workloads/workload.cpp" \
    -o "$OUT_DIR/our_dump"

# name | shared args (workload, nr-entry, nr-op, seed, plus workload-specific knobs)
declare -a CASES=(
    "ycsb_c_read_only|--workload zipfian --nr-entry 200000 --nr-op 20000 --seed 42 --alpha 0.99"
    "ycsb_e_scan|--workload zipfian --nr-entry 200000 --nr-op 5000 --seed 42 --alpha 0.99 --scan-ratio 0.95 --scan-length 100"
    "ycsb_d_latest|--workload latest --nr-entry 200000 --nr-op 20000 --seed 42 --alpha 0.99 --read-ratio 0.95"
    "uniform_read|--workload uniform --nr-entry 200000 --nr-op 20000 --seed 42"
    "uniform_scan|--workload uniform --nr-entry 200000 --nr-op 5000 --seed 42 --scan-ratio 0.3 --scan-length 20"
)

fail=0
for case_spec in "${CASES[@]}"; do
    name="${case_spec%%|*}"
    args="${case_spec#*|}"
    ref_out="$OUT_DIR/${name}.reference.txt"
    our_out="$OUT_DIR/${name}.ours.txt"
    # shellcheck disable=SC2086
    "$OUT_DIR/reference_dump" $args > "$ref_out"
    # shellcheck disable=SC2086
    "$OUT_DIR/our_dump" $args > "$our_out"
    if diff -q "$ref_out" "$our_out" > /dev/null; then
        n=$(wc -l < "$ref_out" | tr -d ' ')
        echo "PASS  $name  ($n ops, exact match)"
    else
        echo "FAIL  $name  (sequences diverge)"
        diff "$ref_out" "$our_out" | head -10
        fail=1
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "== all workload generators match My-YCSB@${MYYCSB_COMMIT:0:12} exactly =="
else
    echo "== mismatch(es) found against My-YCSB@${MYYCSB_COMMIT:0:12} =="
fi
exit "$fail"
