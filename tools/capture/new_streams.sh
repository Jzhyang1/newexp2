#!/bin/bash
# Capture more real VM block streams (policy none) into logs/pf_streams/<name>/: Linux source tree tools, YCSB on
# RocksDB, more YCSB-on-SQLite mixes.  All run inside the guest (guest page cache + readahead in the loop); see
# tools/capture/workloads/*.sh.  Needs /dev/kvm, no root.  Disks: tools/capture/build_dir_disk.py,
# tools/capture/build_ycsb_disk.py.  RocksDB runs are split per phase afterwards by tools/capture/split_phases.py.
cd "$(dirname "$0")/../.."
W=tools/capture/workloads; GEN=logs/pf_streams/_scripts; mkdir -p $GEN
RT=$(python3 -c "import json;print(json.load(open('disk/data/rocks_data.img.meta.json'))['tail_offset'])")
mk() { out=$GEN/$1.sh; shift; src=$1; shift; sed "$@" $W/$src > $out; echo $out; }
LX="--data-img disk/data/linux_data.img"; RK="--data-img disk/data/rocks_data.img --rw"; YS="--data-img disk/data/ycsb_data.img --rw"
RC="./bin/python tools/capture/run_capture.py --out-root logs/pf_streams --no-trace --prefetch none --workload cmd --timeout 3000"
cap() { port=$1; name=$2; shift 2; $RC --name $name --root-port $port --data-port $((port+1)) "$@" >/dev/null 2>&1 || echo "FAILED $name"; echo "done $name"; }
lane1() {
  cap 13000 lx_grep   --mem 512M $LX --cmd-file $(mk lx_grep linux_grep.sh -e s/@PASSES@/3/ -e s/@PAT@/spin_lock/)
  cap 13000 lx_tar    --mem 512M $LX --cmd-file $(mk lx_tar linux_tar.sh -e s/@PASSES@/2/)
  cap 13000 lx_grep_ra512 --mem 512M --guest-ra-kb 512 $LX --cmd-file $GEN/lx_grep.sh
  cap 13000 y_a       --mem 1G $YS --cmd-file $(mk y_a ycsb_sqlite.sh -e s/@WL@/workloada/ -e s/@OPS@/100000/ -e s/@THREADS@/1/)
}
lane2() {
  cap 13100 lx_par    --mem 512M $LX --cmd-file $(mk lx_par linux_par.sh -e s/@PASSES@/2/)
  cap 13100 lx_mix    --mem 512M $LX --cmd-file $(mk lx_mix linux_mixed.sh -e s/@PASSES@/2/ -e s/@PAT@/spin_lock/)
  cap 13100 y_b       --mem 1G $YS --cmd-file $(mk y_b ycsb_sqlite.sh -e s/@WL@/workloadb/ -e s/@OPS@/100000/ -e s/@THREADS@/1/)
}
lane3() {
  cap 13200 rk_m1g    --mem 1G $RK --cmd-file $(mk rk rocksdb_ycsb.sh -e s/@RECORDS@/6000000/ -e s/@OPS_C@/200000/ -e s/@OPS_E@/20000/ -e s/@TAIL@/$RT/)
  cap 13200 rk_m2g    --mem 2G $RK --cmd-file $GEN/rk.sh
}
lane1 & lane2 & lane3 & wait
