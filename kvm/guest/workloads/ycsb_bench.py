"""Runs YCSB's transaction phase against the SQLite dataset that
kvm/guest/build_image.sh pre-loaded at image-build time (see its YCSB_*
block). Only the read/update mix happens here, at boot -- the load phase
(the actual dataset writes) already happened at build time, directly
against the disk image, since anything written by a *running* guest lands
on its volatile tmpfs overlay for /var (disk/sim.hpp) and would never
reach nbd_server at all.

Reads /opt/workload/ycsb_config.json (written by build_image.sh) so the
record count/field shape/workload used here always matches what was
actually loaded, instead of duplicating those numbers in two places.
"""
import json
import subprocess

with open("/opt/workload/ycsb_config.json") as f:
    cfg = json.load(f)

ycsb_home = cfg["ycsb_home"]
# Invokes java directly instead of the release tarball's bin/ycsb launcher --
# that launcher is a thin Python 2 script (Debian bookworm ships no python2)
# that just resolves "jdbc" to this same Java class and builds this same
# classpath before exec'ing java itself; see build_image.sh's matching
# `ycsb load` invocation for why it wasn't worth porting.
classpath = ":".join([
    f"{ycsb_home}/jdbc-binding/conf",
    f"{ycsb_home}/conf",
    f"{ycsb_home}/lib/*",
    f"{ycsb_home}/jdbc-binding/lib/*",
])
cmd = [
    "java", "-cp", classpath, "site.ycsb.Client", "-t",
    "-db", "site.ycsb.db.JdbcDBClient",
    "-P", f"{ycsb_home}/workloads/{cfg['workload']}",
    "-p", "db.driver=org.sqlite.JDBC",
    "-p", f"db.url=jdbc:sqlite:{cfg['db_path']}",
    "-p", f"recordcount={cfg['recordcount']}",
    "-p", f"operationcount={cfg['operationcount']}",
    "-p", f"fieldcount={cfg['fieldcount']}",
    "-p", f"fieldlength={cfg['fieldlength']}",
    "-p", f"requestdistribution={cfg['requestdistribution']}",
    "-threads", str(cfg["threads"]),
]
print("YCSB_BENCH starting:", " ".join(cmd), flush=True)
subprocess.run(cmd, check=True)
print("YCSB_BENCH complete", flush=True)
