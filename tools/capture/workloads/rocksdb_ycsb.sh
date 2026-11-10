#!/bin/sh
# YCSB on RocksDB (rocksdbjni 5.11 from the guest image's YCSB rocksdb-binding), all phases in one boot:
#   load @RECORDS@ records (1 KiB each, 4 threads) -> marker 1 -> workload C (zipfian reads) -> marker 2 -> workload E (scans)
# Markers are O_DIRECT reads of the raw tail beyond the file system (tail + 8 MiB + k pages), so the host-side stream can be
# split per phase.  Page cache is dropped between phases.
cd /opt/ycsb
CP=/opt/ycsb/rocksdb-binding/conf:/opt/ycsb/conf:/opt/ycsb/lib/*:/opt/ycsb/rocksdb-binding/lib/*
Y="java -Xmx512m -Djava.io.tmpdir=/tmp -cp $CP site.ycsb.Client -db site.ycsb.db.rocksdb.RocksDBClient -p rocksdb.dir=/tmp/data/db -p recordcount=@RECORDS@ -threads 4 -s"
mark() { sync; echo 3 > /proc/sys/vm/drop_caches; dd if=/dev/vdb of=/dev/null bs=4096 count=1 iflag=direct skip=$(( @TAIL@ / 4096 + 2048 + $1 )) 2>/dev/null; echo "MARK $1"; }
$Y -load -P workloads/workloadc
mark 1
$Y -t -P workloads/workloadc -p operationcount=@OPS_C@
mark 2
$Y -t -P workloads/workloade -p operationcount=@OPS_E@
mark 3
