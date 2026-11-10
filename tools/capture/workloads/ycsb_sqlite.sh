#!/bin/sh
# YCSB (JDBC binding) against the SQLite database on the data disk (tools/capture/build_ycsb_disk.py).
# Placeholders filled by tools/capture/new_streams.sh: @WL@ workload file, @OPS@ operation count, @THREADS@.
cd /opt/ycsb
CP=/opt/ycsb/jdbc-binding/conf:/opt/ycsb/conf:/opt/ycsb/lib/*:/opt/ycsb/jdbc-binding/lib/*
exec java -Xmx256m -Djava.io.tmpdir=/tmp -cp "$CP" site.ycsb.Client -t -db site.ycsb.db.JdbcDBClient \
  -P workloads/@WL@ -p db.driver=org.sqlite.JDBC -p db.url=jdbc:sqlite:/tmp/data/ycsb.db \
  -p recordcount=10000000 -p operationcount=@OPS@ -p fieldcount=10 -p fieldlength=100 -threads @THREADS@ -s
