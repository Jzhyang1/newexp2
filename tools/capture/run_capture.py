#!/usr/bin/env python3
"""One end-to-end capture run: NBD servers + QEMU guest + guest tracer -> logs/capture/<name>/.

  root disk (vda, the existing Debian guest image)   -> nbd_server on port --root-port (no policy; boot/lib reads only)
  data disk (vdb, ext4 with the FAISS IVF index)     -> nbd_server on port --data-port (the policy under study,
                                                         feature_log + cache_log on)
  tools share (9p, read-only)  = kvm/guest/trace + run_config.json + queries.npy
  out share   (9p, writable)   = the run directory (guest traces land here)

The guest is booted with init=/bin/sh (no systemd) so the tracer is the first
thing that runs and nothing else touches the disks; see kvm/guest/trace/.
Needs no root: the guest image is not modified, and /dev/kvm access is enough.

Example:
  python3 tools/capture/run_capture.py --name base --prefetch quickmine --page-size 65536 --capacity 4096
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def write_cfg(path, **kv):
    with open(path, "w") as f:
        for k, v in kv.items():
            f.write(f"{k}: {v}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="run")
    ap.add_argument("--out-root", default=os.path.join(ROOT, "logs/capture"))
    ap.add_argument("--root-img", default=os.path.join(ROOT, "disk/data/nbd_disk.img"))
    ap.add_argument("--data-img", default="", help="default: disk/data/<workload>_data.img")
    ap.add_argument("--queries", default=os.path.join(ROOT, "disk/data/faiss_queries.npy"))
    ap.add_argument("--kernel", default=os.path.join(ROOT, "disk/data/vmlinuz"))
    ap.add_argument("--initrd", default=os.path.join(ROOT, "disk/data/initrd.img"))
    ap.add_argument("--root-port", type=int, default=10809)
    ap.add_argument("--data-port", type=int, default=10810)
    ap.add_argument("--mem", default="1G")
    ap.add_argument("--vcpus", type=int, default=2)
    ap.add_argument("--workload", default="faiss", choices=["faiss", "ycsb"])
    ap.add_argument("--ycsb-ops", type=int, default=200000, help="YCSB: operations of the transaction phase (a prefix of the full run)")
    ap.add_argument("--ycsb-workload", default="workloadc")
    ap.add_argument("--ycsb-distribution", default="zipfian")
    ap.add_argument("--n-queries", type=int, default=3000)
    ap.add_argument("--nprobe", type=int, default=8)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--think-us", type=int, default=0)
    ap.add_argument("--page-size", type=int, default=65536)
    ap.add_argument("--capacity", type=int, default=4096, help="cache capacity in pages")
    ap.add_argument("--prefetch", default="none")
    ap.add_argument("--evict", default="lru")
    ap.add_argument("--oracle-file", default="", help="access.csv of an earlier run, for prefetch=oracle")
    ap.add_argument("--readahead-pages", type=int, default=512, help="window of prefetch=readahead, in cache pages")
    ap.add_argument("--prefetch-budget-pages", type=int, default=0, help="cap on new pages prefetched per request, any policy (0 = unlimited)")
    ap.add_argument("--latency-mode", default="simulated", choices=["simulated", "physical"],
                    help="physical = nbd_server sleeps --miss-ns per request with a miss, so guest timings feel it")
    ap.add_argument("--hit-ns", type=int, default=30000)
    ap.add_argument("--miss-ns", type=int, default=300000)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--buffer-kb", type=int, default=2048, help="ftrace ring buffer per cpu per instance (guest RAM it steals!)")
    ap.add_argument("--probe-debug", action="store_true")
    ap.add_argument("--no-trace", action="store_true", help="skip the guest tracer (host-only features)")
    # Remote data disk: the FAISS data image lives behind an emulated link (bin/fwd_sim serve) and nbd_server
    # reaches it with `source: remote` -- either directly (--fwd-mode direct) or through a local cache that
    # reads ahead (--fwd-mode cache, policies as --evict/--prefetch). See disk/fwd.hpp.
    ap.add_argument("--remote-rtt-us", type=int, default=None, help="enable the remote data disk with this link RTT")
    ap.add_argument("--remote-bw-mbps", type=float, default=10000)
    ap.add_argument("--remote-disk-us", type=int, default=100)
    ap.add_argument("--fwd-mode", default="cache", choices=["direct", "cache"])
    ap.add_argument("--read-concurrency", type=int, default=8, help="remote mode: reads served in parallel per NBD connection")
    ap.add_argument("--guest-ra-kb", type=int, default=None, help="set the guest's /sys/block/vdb/queue/read_ahead_kb (default: leave the kernel's 128)")
    ap.add_argument("--lean-local", action="store_true", help="local (non-remote) data disk: write no per-request logs, so the run is a clean local-disk reference")
    ap.add_argument("--local-read-us", type=int, default=0, help="remote cache mode: modeled local cache device latency")
    args = ap.parse_args()

    if not args.data_img:
        args.data_img = os.path.join(ROOT, "disk/data/%s_data.img" % args.workload)
    run = os.path.join(args.out_root, args.name)
    shutil.rmtree(run, ignore_errors=True)
    tools = os.path.join(run, "tools")
    out = os.path.join(run, "out")
    os.makedirs(tools)
    os.makedirs(out)
    for fn in os.listdir(os.path.join(ROOT, "kvm/guest/trace")):
        if fn.endswith((".py", ".sh")):
            shutil.copy(os.path.join(ROOT, "kvm/guest/trace", fn), tools)
    if args.workload == "faiss":
        shutil.copy(args.queries, os.path.join(tools, "queries.npy"))
    meta = json.load(open(args.data_img + ".meta.json"))
    run_cfg = dict(n_queries=args.n_queries, nprobe=args.nprobe, k=args.k, think_us=args.think_us, probe_debug=args.probe_debug, trace=not args.no_trace,
                   sync_tail_offset=meta["tail_offset"], sync_base=0,
                   marker_instances=["", "instances/blk/", "instances/app/", "instances/stk/"],
                   buffer_kb=args.buffer_kb, workload=args.workload, guest_ra_kb=args.guest_ra_kb)
    if args.workload == "ycsb":
        run_cfg["ycsb"] = dict(workload=args.ycsb_workload, ops=args.ycsb_ops, records=meta["records"], fields=meta["fields"],
                               field_length=meta["field_length"], distribution=args.ycsb_distribution)
    json.dump(run_cfg, open(os.path.join(tools, "run_config.json"), "w"))
    json.dump(dict(vars(args), meta_file=args.data_img + ".meta.json", mono_start_ns=time.clock_gettime_ns(time.CLOCK_MONOTONIC)),
              open(os.path.join(run, "args.json"), "w"), indent=1)

    nbd = os.path.join(ROOT, "bin/nbd_server")
    cfg_root = os.path.join(run, "nbd_root.yaml")
    cfg_data = os.path.join(run, "nbd_data.yaml")
    remote_srv = None
    if args.remote_rtt_us is not None:
        fwd_bin = os.path.join(ROOT, "bin/fwd_sim")
        remote_srv = subprocess.Popen([fwd_bin, "serve", "--file", args.data_img, "--port", "0", "--rtt-us", str(args.remote_rtt_us),
                                       "--bw-mbps", str(args.remote_bw_mbps), "--disk-us", str(args.remote_disk_us)],
                                      stdout=subprocess.PIPE, stderr=open(os.path.join(run, "remote_disk.log"), "w"), text=True)
        remote_port = int(remote_srv.stdout.readline().split()[1])
    write_cfg(cfg_root, source="local", file=args.root_img, ports=f"[{args.root_port}]", capacity=4096, page_size=65536,
              evict_policy="lru", prefetch_policy="none", log=os.path.join(run, "root_results.csv"),
              access_log=os.path.join(run, "root_access.csv"), prefetch_log=os.path.join(run, "root_prefetch.csv"),
              vm_log_dir=run)
    if remote_srv is None and args.lean_local:
        # SimReadClass logs every request and cache event under one lock; for a reference run that logging is the dominant cost
        write_cfg(cfg_data, source="local", file=args.data_img, ports=f"[{args.data_port}]", capacity=args.capacity, page_size=args.page_size,
                  evict_policy="lru", prefetch_policy="none", log=os.path.join(run, "results.csv"),
                  access_log="/dev/null", prefetch_log="/dev/null", vm_log_dir=run)
    elif remote_srv is None:
        write_cfg(cfg_data, source="local", file=args.data_img, ports=f"[{args.data_port}]", capacity=args.capacity,
                  page_size=args.page_size, hit_latency_ns=args.hit_ns, miss_latency_ns=args.miss_ns, latency_mode=args.latency_mode,
                  readahead_pages=args.readahead_pages, prefetch_budget_pages=args.prefetch_budget_pages,
                  evict_policy=args.evict, prefetch_policy=args.prefetch, oracle_file=(args.oracle_file or os.path.join(run, "access.csv")), log=os.path.join(run, "results.csv"),
                  access_log=os.path.join(run, "access.csv"), prefetch_log=os.path.join(run, "prefetch.csv"),
                  feature_log=os.path.join(run, "features.csv"), cache_log=os.path.join(run, "cache.csv"),
                  vm_log_dir=run)
    else:  # access/prefetch logs are SimReadClass-only; set explicit paths so the defaults under ./logs are never truncated
        write_cfg(cfg_data, source="remote", remote_host="127.0.0.1", remote_port=remote_port, fwd_mode=args.fwd_mode,
                  read_concurrency=args.read_concurrency, fwd_local_read_ns=args.local_read_us * 1000, ports=f"[{args.data_port}]",
                  capacity=args.capacity, page_size=args.page_size, readahead_pages=args.readahead_pages,
                  prefetch_budget_pages=args.prefetch_budget_pages, evict_policy=args.evict, prefetch_policy=args.prefetch,
                  overlay_dir=run, log=os.path.join(run, "results.csv"), access_log=os.path.join(run, "access.csv"),
                  prefetch_log=os.path.join(run, "prefetch.csv"), vm_log_dir=run)

    servers = []
    for name, cfg in (("root", cfg_root), ("data", cfg_data)):
        errf = open(os.path.join(run, f"nbd_{name}.log"), "w")
        servers.append(subprocess.Popen([nbd, cfg], stderr=errf, stdout=errf, cwd=ROOT))
    time.sleep(1.0)
    for s in servers:
        if s.poll() is not None:
            sys.exit("nbd_server died at startup, see " + run)

    boot = ("mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t tmpfs tmpfs /tmp; mount -t tmpfs tmpfs /run; "
            "modprobe 9pnet_virtio; modprobe 9p; mkdir -p /tmp/tools /tmp/out /tmp/data; "
            "mount -t 9p -o trans=virtio,version=9p2000.L,ro tools /tmp/tools; "
            "mount -t 9p -o trans=virtio,version=9p2000.L,msize=1048576 out /tmp/out; exec /bin/sh /tmp/tools/bootstrap.sh")
    append = f'console=ttyS0 root=/dev/vda ro panic=1 loglevel=4 init=/bin/sh -- -c "{boot}"'
    qemu = ["qemu-system-x86_64", "-name", f"{args.name},process=cap-{args.name}", "-enable-kvm", "-cpu", "host",
            "-m", args.mem, "-smp", str(args.vcpus),
            "-drive", f"driver=nbd,host=127.0.0.1,port={args.root_port},if=none,id=d0,cache=none",
            "-device", "virtio-blk-pci,drive=d0,bootindex=1",
            "-drive", f"driver=nbd,host=127.0.0.1,port={args.data_port},if=none,id=d1,cache=none",
            "-device", "virtio-blk-pci,drive=d1",
            "-nic", "none", "-display", "none", "-no-reboot",
            "-virtfs", f"local,id=tools,path={tools},mount_tag=tools,security_model=none,readonly=on",
            "-virtfs", f"local,id=out,path={out},mount_tag=out,security_model=none",
            "-kernel", args.kernel, "-initrd", args.initrd, "-append", append,
            "-serial", f"file:{os.path.join(run, 'serial.log')}", "-monitor", "none"]
    t0 = time.time()
    print("launching:", args.name, flush=True)
    q = subprocess.Popen(qemu)
    status = "ok"
    try:
        q.wait(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        status = "timeout"
        q.kill()
        q.wait()
    wall = time.time() - t0
    for s in servers:
        s.send_signal(signal.SIGTERM)
    if remote_srv:
        remote_srv.send_signal(signal.SIGTERM)
        try:
            remote_srv.wait(timeout=10)
        except subprocess.TimeoutExpired:
            remote_srv.kill()
    for s in servers:
        try:
            s.wait(timeout=20)
        except subprocess.TimeoutExpired:
            s.kill()
    json.dump(dict(status=status, wall_s=wall, qemu_rc=q.returncode), open(os.path.join(run, "status.json"), "w"))
    print(f"{args.name}: {status} in {wall:.0f}s -> {run}")
    return 0 if status == "ok" else 1


if __name__ == "__main__":
    sys.exit(main())
