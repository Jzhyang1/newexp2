#!/bin/bash
# Live VM runs with the in-band hint channel (guest shim -> /dev/vdc -> nbd_server -> policy on_hint). logs/pf_live/h_*
cd "$(dirname "$0")/../.."
RC="./bin/python tools/capture/run_capture.py --out-root logs/pf_live --no-trace --evict lru_seg"
FA="--workload faiss --n-queries 20000 --mem 1G"
TH="--workload thesios --thesios-requests 150000 --mem 2G"
run() { $RC --name "$1" $2 --prefetch "$3" --root-port $4 --data-port $(($4+1)) >/dev/null 2>&1 || echo "FAILED $1"; echo "done $1"; }
( run h_faiss_on_all   "$FA --hints on"  "learned+hint:filter=0" 13000; run h_faiss_aux_f30 "$FA --hints aux" "learned+hint:theta=30" 13100 ) &
( run h_faiss_on_f30   "$FA --hints on"  "learned+hint:theta=30"  13200; run h_faiss_aux_all "$FA --hints aux" "learned+hint:filter=0" 13300 ) &
( run h_th_k4_all "$TH --hints on --hint-k 4" "learned+hint:filter=0" 13400; run h_th_k16_all "$TH --hints on --hint-k 16" "learned+hint:filter=0" 13500 ) &
( run h_th_k1_all "$TH --hints on --hint-k 1" "learned+hint:filter=0" 13600; run h_th_k4_f30 "$TH --hints on --hint-k 4" "learned+hint:theta=30" 13700 ) &
wait
