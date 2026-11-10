#!/bin/sh
# Repeated cold application launches from the guest's root disk (the debootstrapped Debian image, /dev/vda):
# before every launch the guest page cache is dropped, as in a fresh container / function cold start.
# @ROUNDS@ rounds over 6 real programs; @ORDER@ = fixed (same order every round) or shuf (random order per round).
# The stream of interest is the ROOT disk's (root_access.csv), not the data disk's.
YCP=/opt/ycsb/conf:/opt/ycsb/lib/*
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
i=0
while [ $i -lt @ROUNDS@ ]; do
  if [ "@ORDER@" = shuf ]; then L=$(echo $APPS | tr ' ' '\n' | shuf | tr '\n' ' '); else L=$APPS; fi
  for a in $L; do launch $a; done
  echo "round $i: $L"
  i=$((i+1))
done
