#!/usr/bin/env python3
"""Spin up several KVM guests, each with one or more disks backed by remote
NBD exports.

Every disk listed for a VM becomes a virtio-blk device wired to its own
`driver=nbd` QEMU block backend (see nbd.cpp for a compatible server). QEMU
speaks the NBD wire protocol to the remote host itself -- no kernel nbd
module or nbd-client is involved -- so inside the guest each disk enumerates
as a completely ordinary local block device: the first disk listed is
/dev/vda, the second /dev/vdb, and so on (Linux's virtio-blk driver assigns
letters in PCI probe order, which follows the order devices are added on the
command line).

All VM/host/network parameters come from a YAML config file. See
vms.example.yaml for the schema.

Usage:
    launch.py vms.yaml                 # start every VM in the config
    launch.py vms.yaml --only vm0      # start just one
    launch.py vms.yaml --dry-run       # print qemu-system commands, don't run
    launch.py vms.yaml --stop          # stop every VM (SIGTERM via pidfile)
    launch.py vms.yaml --stop --force  # SIGKILL if SIGTERM doesn't land

Requires PyYAML (`pip install pyyaml`) and, on the host that actually runs
this, qemu-system-{arch} with KVM available.
"""

import argparse
import copy
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time

try:
    import yaml
except ImportError:
    sys.exit("launch.py requires PyYAML: pip install pyyaml")


def deep_merge(base, override):
    """Recursively merge `override` onto a copy of `base`; dicts merge, everything else is replaced."""
    result = copy.deepcopy(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = deep_merge(result[key], value)
        else:
            result[key] = copy.deepcopy(value)
    return result


DEFAULTS = {
    "qemu_binary": "qemu-system-x86_64",
    "kvm": True,
    "vcpus": 2,
    "memory": "2G",
    "run_dir": "/tmp/kvm-launch",
    "log_dir": "/tmp/kvm-launch",
    "daemonize": True,
    "display": "none",
    "disks": [],
    "network": {
        "type": "user",
        "hostfwd": [],
        "mac": None,
    },
    "kernel": None,
    "initrd": None,
    "append": None,
    "extra_drives": [],
    "extra_args": [],
}


def load_config(path):
    with open(path) as f:
        raw = yaml.safe_load(f) or {}

    top_defaults = deep_merge(DEFAULTS, raw.get("defaults", {}))
    vms_raw = raw.get("vms", [])
    if not vms_raw:
        sys.exit(f"{path}: no VMs listed under 'vms:'")

    vms = []
    seen_names = set()
    for entry in vms_raw:
        if "name" not in entry:
            sys.exit(f"{path}: every entry under 'vms:' needs a 'name'")
        name = entry["name"]
        if name in seen_names:
            sys.exit(f"{path}: duplicate VM name {name!r}")
        seen_names.add(name)
        vm = deep_merge(top_defaults, entry)
        if not vm.get("disks"):
            sys.exit(f"{path}: VM {name!r} has no 'disks' entries")
        for i, disk in enumerate(vm["disks"]):
            nbd = disk.get("nbd", {})
            if not nbd.get("host"):
                sys.exit(f"{path}: VM {name!r} disk {i} ({disk.get('device', '?')}) is missing nbd.host")
        vms.append(vm)

    return vms


def pidfile_path(vm):
    return os.path.join(vm["run_dir"], f"{vm['name']}.pid")


def monitor_path(vm):
    return os.path.join(vm["run_dir"], f"{vm['name']}.monitor")


def log_path(vm):
    return os.path.join(vm["log_dir"], f"{vm['name']}.log")


def serial_log_path(vm):
    return os.path.join(vm["log_dir"], f"{vm['name']}.serial.log")


def qemu_log_path(vm):
    # QEMU's own -D logfile -- separate from log_path(vm), which only ever
    # captures the pre-daemonize invocation's stdout/stderr. -D persists
    # across -daemonize forking off and detaching from the terminal, so it's
    # the only place QEMU's own runtime-internal messages can land once
    # daemonized (best-effort: QEMU still redirects its own stdio internally
    # on daemonize, so this isn't a guarantee for every runtime message).
    return os.path.join(vm["log_dir"], f"{vm['name']}.qemu.log")


def disk_device_name(index):
    """0 -> vda, 1 -> vdb, ... 25 -> vdz, 26 -> vdaa, matching Linux's virtio-blk naming."""
    letters = ""
    n = index
    while True:
        n, rem = divmod(n, 26)
        letters = chr(ord("a") + rem) + letters
        if n == 0:
            break
        n -= 1
    return f"vd{letters}"


def build_disk_args(vm):
    """Build one -drive/-device pair per configured disk, in order (disk 0 -> /dev/vda, disk 1 -> /dev/vdb, ...)."""
    args = []
    for i, disk in enumerate(vm["disks"]):
        nbd = disk["nbd"]
        drive_id = f"drive{i}"
        parts = [
            "driver=nbd",
            f"host={nbd['host']}",
            f"port={nbd.get('port', 10809)}",
            "if=none",
            f"id={drive_id}",
            f"cache={disk.get('cache', 'none')}",
        ]
        if nbd.get("export"):
            parts.append(f"export={nbd['export']}")
        args += ["-drive", ",".join(parts)]

        device = f"virtio-blk-pci,drive={drive_id}"
        if i == 0:
            device += ",bootindex=1"
        args += ["-device", device]
    return args


def build_network_args(vm):
    net = vm["network"]
    net_type = net.get("type", "user")
    args = []
    if net_type == "none":
        args += ["-nic", "none"]
        return args

    if net_type == "user":
        netdev = "user,id=net0"
        for fwd in net.get("hostfwd", []):
            netdev += f",hostfwd={fwd}"
    elif net_type == "tap":
        ifname = net.get("ifname", f"tap-{vm['name']}")
        netdev = f"tap,id=net0,ifname={ifname},script=no,downscript=no"
    else:
        sys.exit(f"{vm['name']}: unknown network.type {net_type!r} (use user, tap, or none)")

    args += ["-netdev", netdev]
    device = "virtio-net-pci,netdev=net0"
    if net.get("mac"):
        device += f",mac={net['mac']}"
    args += ["-device", device]
    return args


def build_command(vm):
    cmd = [vm["qemu_binary"], "-name", f"{vm['name']},process={vm['name']}"]
    cmd += ["-m", str(vm["memory"])]
    cmd += ["-smp", str(vm["vcpus"])]

    if vm["kvm"]:
        cmd += ["-enable-kvm", "-cpu", "host"]
    else:
        cmd += ["-cpu", "qemu64"]

    # Remote-backed disks, presented to the guest as plain virtio-blk
    # devices (/dev/vda, /dev/vdb, ...) -- the NBD transport is invisible to it.
    cmd += build_disk_args(vm)

    for extra in vm.get("extra_drives", []):
        cmd += ["-drive", extra]

    cmd += build_network_args(vm)

    if vm.get("kernel"):
        cmd += ["-kernel", vm["kernel"]]
        if vm.get("initrd"):
            cmd += ["-initrd", vm["initrd"]]
        if vm.get("append"):
            cmd += ["-append", vm["append"]]

    display = vm.get("display", "none")
    if display == "none":
        # mon:stdio doesn't work with -daemonize (QEMU detaches from the
        # controlling terminal before the chardev would attach), and isn't
        # useful for daemonize:false's own already-redirected stdout either
        # -- route serial to its own log file instead, which works the same
        # way in both modes and persists the console for later inspection.
        cmd += ["-display", "none", "-serial", f"file:{serial_log_path(vm)}"]
    else:
        cmd += ["-display", display]

    cmd += ["-monitor", f"unix:{monitor_path(vm)},server,nowait"]
    cmd += ["-pidfile", pidfile_path(vm)]
    cmd += ["-D", qemu_log_path(vm)]

    if vm.get("daemonize", True):
        cmd += ["-daemonize"]

    for extra in vm.get("extra_args", []):
        cmd += shlex.split(str(extra))

    return cmd


def ensure_dirs(vm):
    os.makedirs(vm["run_dir"], exist_ok=True)
    os.makedirs(vm["log_dir"], exist_ok=True)


def start_vm(vm, dry_run):
    ensure_dirs(vm)
    cmd = build_command(vm)

    if dry_run:
        print(f"# {vm['name']}")
        for i, disk in enumerate(vm["disks"]):
            nbd = disk["nbd"]
            target = f"{nbd['host']}:{nbd.get('port', 10809)}"
            if nbd.get("export"):
                target += f"/{nbd['export']}"
            print(f"#   /dev/{disk_device_name(i)} <- nbd://{target}")
        print(" ".join(shlex.quote(c) for c in cmd))
        return

    if not shutil.which(cmd[0]):
        sys.exit(f"{vm['name']}: qemu binary {cmd[0]!r} not found on PATH")
    if vm["kvm"] and not os.path.exists("/dev/kvm"):
        sys.exit(f"{vm['name']}: kvm enabled but /dev/kvm is not present on this host")

    pidfile = pidfile_path(vm)
    if os.path.exists(pidfile):
        try:
            pid = int(open(pidfile).read().strip())
            os.kill(pid, 0)
            sys.exit(f"{vm['name']}: already running (pid {pid}, {pidfile})")
        except (ValueError, ProcessLookupError, PermissionError):
            os.remove(pidfile)

    if vm.get("daemonize", True):
        # -daemonize backgrounds and detaches qemu itself; wait for it to
        # write the pidfile so callers know the launch actually succeeded.
        # The pre-fork invocation's stdout/stderr are captured to log_path(vm)
        # (same file the non-daemonize branch below uses) instead of being
        # inherited, so an early failure (bad drive, bad extra_args, ...)
        # leaves a record instead of vanishing once qemu daemonizes.
        log = open(log_path(vm), "ab")
        try:
            subprocess.run(cmd, check=True, stdout=log, stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as e:
            sys.exit(f"{vm['name']}: qemu failed to start (exit {e.returncode}) -- see {log_path(vm)}")
        finally:
            log.close()
        for _ in range(50):
            if os.path.exists(pidfile):
                break
            time.sleep(0.1)
        else:
            sys.exit(f"{vm['name']}: qemu did not write a pidfile within 5s -- "
                     f"check {log_path(vm)} and {qemu_log_path(vm)}")
        pid = open(pidfile).read().strip()
        print(f"{vm['name']}: started (pid {pid})")
    else:
        log = open(log_path(vm), "ab")
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
        with open(pidfile, "w") as f:
            f.write(str(proc.pid))
        print(f"{vm['name']}: started (pid {proc.pid}, log {log_path(vm)})")


WORKLOAD_RESULT_RE = re.compile(r"WORKLOAD_RESULT:\s*(PASS|FAIL(?:\s+exit=\d+)?)")


def check_workload_result(vm):
    """Best-effort: scan the serial log for build_image.sh's WORKLOAD_RESULT
    marker (see kvm/guest/build_image.sh's run.sh). Returns None if there's
    no serial log to check (e.g. display != none); otherwise the marker
    found, or a note that none was found (the guest may not have reached/
    finished workload.service at all -- e.g. an early-boot stall)."""
    path = serial_log_path(vm)
    if not os.path.exists(path):
        return None
    result = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = WORKLOAD_RESULT_RE.search(line)
            if m:
                result = m.group(0)
    return result or "no WORKLOAD_RESULT marker found in serial log -- workload may not have run to completion"


def wait_vms(vms, interval, timeout=None, heartbeat=30.0):
    """Block until every VM's pidfile is gone or its process has exited --
    e.g. after a guest-initiated poweroff once a baked-in workload finishes
    (see kvm/guest/build_image.sh's workload.service). Prints a heartbeat
    every `heartbeat` seconds so a long-but-legitimate wait isn't
    indistinguishable from a hang, and exits nonzero after `timeout` seconds
    if given (default: wait forever)."""
    pending = {vm["name"]: vm for vm in vms}
    if not pending:
        return
    print("waiting for: " + ", ".join(pending))
    start = time.monotonic()
    last_heartbeat = start
    while pending:
        for name, vm in list(pending.items()):
            pidfile = pidfile_path(vm)
            if not os.path.exists(pidfile):
                print(f"{name}: exited (pidfile gone)")
                del pending[name]
            else:
                try:
                    pid = int(open(pidfile).read().strip())
                    os.kill(pid, 0)
                except (ValueError, ProcessLookupError, PermissionError):
                    print(f"{name}: exited")
                    del pending[name]
                else:
                    continue
            note = check_workload_result(vm)
            if note:
                tag = "warn" if "FAIL" in note or "no WORKLOAD_RESULT" in note else "ok"
                print(f"  [{tag}] {name}: {note}")
        if not pending:
            break
        elapsed = time.monotonic() - start
        if timeout is not None and elapsed >= timeout:
            sys.exit(
                f"timed out after {timeout:.0f}s waiting for: {', '.join(pending)} -- still running; "
                f"check their serial logs (e.g. {serial_log_path(next(iter(pending.values())))})"
            )
        if time.monotonic() - last_heartbeat >= heartbeat:
            print(f"[{elapsed:.0f}s] still waiting for: {', '.join(pending)}")
            last_heartbeat = time.monotonic()
        time.sleep(interval)


def stop_vm(vm, force):
    pidfile = pidfile_path(vm)
    if not os.path.exists(pidfile):
        print(f"{vm['name']}: not running (no pidfile)")
        return

    pid = int(open(pidfile).read().strip())
    sig = signal.SIGKILL if force else signal.SIGTERM
    try:
        os.kill(pid, sig)
        print(f"{vm['name']}: sent {sig.name} to pid {pid}")
    except ProcessLookupError:
        print(f"{vm['name']}: pid {pid} not running")
    finally:
        os.remove(pidfile)
    monitor = monitor_path(vm)
    if os.path.exists(monitor):
        os.remove(monitor)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("config", help="path to a YAML config file describing the VMs to launch")
    parser.add_argument("--only", metavar="NAME", help="only act on the VM with this name")
    parser.add_argument("--dry-run", action="store_true", help="print qemu-system commands instead of running them")
    parser.add_argument("--stop", action="store_true", help="stop VMs instead of starting them")
    parser.add_argument("--force", action="store_true", help="with --stop, SIGKILL instead of SIGTERM")
    parser.add_argument("--wait", action="store_true",
                         help="block until every targeted VM's QEMU process exits "
                              "(e.g. after a guest-initiated poweroff), then exit")
    parser.add_argument("--wait-interval", type=float, default=2.0,
                         help="seconds between pidfile checks with --wait (default: 2)")
    parser.add_argument("--wait-timeout", type=float, default=None,
                         help="give up and exit nonzero after this many seconds with --wait "
                              "(default: wait forever)")
    parser.add_argument("--wait-heartbeat", type=float, default=30.0,
                         help="seconds between progress messages while waiting (default: 30)")
    args = parser.parse_args()

    vms = load_config(args.config)
    if args.only:
        vms = [vm for vm in vms if vm["name"] == args.only]
        if not vms:
            sys.exit(f"no VM named {args.only!r} in {args.config}")

    if args.wait:
        wait_vms(vms, args.wait_interval, timeout=args.wait_timeout, heartbeat=args.wait_heartbeat)
        return

    for vm in vms:
        if args.stop:
            stop_vm(vm, args.force)
        else:
            start_vm(vm, args.dry_run)


if __name__ == "__main__":
    main()
