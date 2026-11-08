#!/bin/bash
# Live VM evaluation: the same workload in a real QEMU guest under each prefetch policy (nbd_server data disk, simulated
# latency, stats from nbd_server).  Results: logs/pf_live/<workload>_<tag>/ ; summarize with tools/pf/live_report.py.
# usage: tools/pf/live_eval.sh [faiss|thesios|both]
cd "$(dirname "$0")/../.."
W=${1:-both}
RC="./bin/python tools/capture/run_capture.py --out-root logs/pf_live --no-trace"
run() { # tag workload-args prefetch evict port
  $RC --name "$1" $2 --prefetch "$3" --evict "$4" --root-port $5 --data-port $(($5+1)) >/dev/null 2>&1 || echo "FAILED $1"; echo "done $1"; }
FA="--workload faiss --n-queries 20000 --mem 1G"
TH="--workload thesios --thesios-requests 150000 --mem 2G"
lane() { port=$1; shift; while [ $# -gt 0 ]; do run "$1" "$2" "$3" "$4" $port; shift 4; done; }
if [ "$W" != thesios ]; then
lane 12000 faiss_none "$FA" none lru   faiss_readahead "$FA" readahead lru   faiss_learned "$FA" learned lru &
lane 12100 faiss_ra16 "$FA" readahead:pages=16 lru   faiss_quickmine "$FA" quickmine lru   faiss_learned_seg "$FA" learned lru_seg &
lane 12200 faiss_learned30 "$FA" learned:theta=30 lru_seg   faiss_nextreq "$FA" nextreq lru_seg   faiss_neighborhood "$FA" neighborhood lru_seg &
wait
fi
if [ "$W" != faiss ]; then
lane 12300 thesios_none "$TH" none lru   thesios_readahead "$TH" readahead lru   thesios_learned "$TH" learned lru &
lane 12400 thesios_ra16 "$TH" readahead:pages=16 lru   thesios_quickmine "$TH" quickmine lru   thesios_learned_seg "$TH" learned lru_seg &
lane 12500 thesios_learned30 "$TH" learned:theta=30 lru_seg   thesios_nextreq "$TH" nextreq lru_seg   thesios_neighborhood "$TH" neighborhood lru_seg &
wait
fi
