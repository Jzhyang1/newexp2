#!/bin/sh
# app_launch.sh with ground-truth markers on the host clock: before and after every launch, one O_DIRECT 4 KiB read of
# a reserved sector past the data disk's file system (tail_offset, see build_dir_disk.py).  nbd_server logs it with
# arrive_ns like any request, so the data disk's features.csv carries (round, app, begin/end) at host time.
#   sector index k = 1 + 2 * (round * 8 + app) + (0 begin | 1 end)          (tail area = 4096 sectors)
# @ROUNDS@ rounds, @ORDER@ = fixed | shuf, @TAIL@ = tail_offset in 4 KiB units.
YCP=/opt/ycsb/conf:/opt/ycsb/lib/*
mark() { dd if=/dev/vdb of=/dev/null bs=4096 count=1 skip=$((@TAIL@ + $1)) iflag=direct 2>/dev/null; }
launch() {
  sync; echo 3 > /proc/sys/vm/drop_caches
  case $1 in
    py_faiss) python3 -c "import numpy, faiss" ;;
    py_std)   python3 -c "import sqlite3, json, email.mime.multipart, http.client, xml.dom.minidom, unittest, asyncio, decimal" ;;
    java)     java -Xmx64m -cp "$YCP" site.ycsb.Client -db site.ycsb.BasicDB -p recordcount=10 -p operationcount=10 -P /opt/ycsb/workloads/workloada > /dev/null 2>&1 ;;
    perl)     perl -MPOSIX -e 1 ;;
    sqlite)   sqlite3 :memory: "create table t(a); insert into t values(1); select * from t;" > /dev/null ;;
    apt)      apt-cache stats > /dev/null 2>&1 ;;
  esac
}
APPS="py_faiss py_std java perl sqlite apt"
idx() { case $1 in py_faiss) echo 0;; py_std) echo 1;; java) echo 2;; perl) echo 3;; sqlite) echo 4;; apt) echo 5;; esac; }
i=0
while [ $i -lt @ROUNDS@ ]; do
  if [ "@ORDER@" = shuf ]; then L=$(echo $APPS | tr ' ' '\n' | shuf | tr '\n' ' '); else L=$APPS; fi
  for a in $L; do
    k=$((1 + 2 * (i * 8 + $(idx $a))))
    sync; echo 3 > /proc/sys/vm/drop_caches
    mark $k
    launch $a
    mark $((k + 1))
  done
  echo "round $i: $L"
  i=$((i+1))
done
