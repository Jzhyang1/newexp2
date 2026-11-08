#!/bin/bash
# Capture a suite of real VM block streams (policy none, no tracer) into logs/pf_streams/<name>/features.csv.
# Three lanes in parallel (distinct NBD ports). Needs /dev/kvm; no root.
cd "$(dirname "$0")/../.."
RC="./bin/python tools/capture/run_capture.py --out-root logs/pf_streams --no-trace --prefetch none"
lane() { port=$1; shift; for spec in "$@"; do name=${spec%%:*}; args=${spec#*:}; $RC --name $name --root-port $port --data-port $((port+1)) $args >/dev/null 2>&1 || echo "FAILED $name"; echo "done $name"; done; }
lane 11000 "f_m1g:--workload faiss --n-queries 20000 --mem 1G" "f_m512:--workload faiss --n-queries 20000 --mem 512M" "f_ra32:--workload faiss --n-queries 20000 --mem 1G --guest-ra-kb 32" "f_np16:--workload faiss --n-queries 12000 --mem 1G --nprobe 16" &
lane 11100 "f_m2g:--workload faiss --n-queries 20000 --mem 2G" "f_ra512:--workload faiss --n-queries 20000 --mem 1G --guest-ra-kb 512" "f_ra0:--workload faiss --n-queries 20000 --mem 1G --guest-ra-kb 0" "y_nt:--workload ycsb --ycsb-ops 100000 --mem 1G" &
lane 11200 "th_m1g:--workload thesios --thesios-requests 150000 --mem 1G" "th_m4g:--workload thesios --thesios-requests 150000 --mem 4G" "th_ra32:--workload thesios --thesios-requests 150000 --mem 2G --guest-ra-kb 32" "th_ra512:--workload thesios --thesios-requests 150000 --mem 2G --guest-ra-kb 512" &
wait
