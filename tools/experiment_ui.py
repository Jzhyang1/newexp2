#!/usr/bin/env python3
"""Local control panel for the NBD/cache experiment."""
from __future__ import annotations

import json
import csv
import math
import os
import signal
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "tools" / "experiment_ui.html"
LOG_DIR = ROOT / "logs" / "ui"
PID_FILE = LOG_DIR / "action.pid"
DEFAULT_WORKLOAD = "kvm/guest/workloads/ycsb_bench.py"
WORKLOADS = {
    "ycsb": "kvm/guest/workloads/ycsb_bench.py",
    "gups": "kvm/guest/workloads/gups_bench.py",
    "scan": "kvm/guest/workloads/large_scan.py",
}

CONFIGS = [
    {"id": "default", "label": "Default / oracle", "nbd": "disk/nbd.yaml", "vms": "kvm/vms.yaml"},
    {"id": "2vm", "label": "Two VMs", "nbd": "disk/nbd_2vm.yaml", "vms": "kvm/vms_2vm.yaml"},
    {"id": "4vm", "label": "Four VMs", "nbd": "disk/nbd_4vm.yaml", "vms": "kvm/vms_4vm.yaml"},
    {"id": "moremem", "label": "More memory", "nbd": "disk/nbd_moremem.yaml", "vms": "kvm/vms_moremem.yaml"},
    {"id": "mix4vm", "label": "Mixed four VM", "nbd": "disk/nbd_mix4vm.yaml", "vms": "kvm/vms_mix4vm.yaml"},
    {"id": "lru-readahead", "label": "LRU + readahead", "nbd": "disk/nbd_lru_readahead.yaml", "vms": "kvm/vms_lru_vs_cxtaware.yaml"},
    {"id": "cxt-aware", "label": "Context-aware", "nbd": "disk/nbd_cxt_aware.yaml", "vms": "kvm/vms_lru_vs_cxtaware.yaml"},
]


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def load_csv_tail(path: Path, limit: int = 25):
    if not path.exists():
        return []
    lines = path.read_text(errors="replace").splitlines()
    if not lines:
        return []
    header = lines[0].split(",")
    return [dict(zip(header, row.split(","))) for row in lines[-limit:] if row.strip()]


def pid_alive(path: Path) -> bool:
    try:
        pid = int(path.read_text().strip())
        os.kill(pid, 0)
        return True
    except (FileNotFoundError, ValueError, ProcessLookupError, PermissionError):
        return False


def process_snapshot():
    checks = {
        "nbd_server": ROOT / "disk/nbd_server.pid",
        "ui_action": PID_FILE,
    }
    result = {key: {"running": pid_alive(path), "pid_file": rel(path) if path.is_relative_to(ROOT) else str(path)}
              for key, path in checks.items()}
    run_dir = Path("/tmp/kvm-launch")
    vm_pids = []
    if run_dir.exists():
        for path in sorted(run_dir.glob("*.pid")):
            if pid_alive(path):
                vm_pids.append({"name": path.stem, "pid": path.read_text().strip()})
    result["vms"] = vm_pids
    return result


def state():
    disk = ROOT / "disk/data/nbd_disk.img"
    stats = load_csv_tail(ROOT / "logs/nbd_results.csv", 8)
    access = ROOT / "logs/nbd_access.csv"
    vm_logs = []
    log_dir = Path("/tmp/kvm-launch")
    if log_dir.exists():
        for serial_path in sorted(log_dir.glob("*.serial.log")):
            vm_logs.append({
                "name": serial_path.name.removesuffix(".serial.log"),
                "running": pid_alive(log_dir / f"{serial_path.name.removesuffix('.serial.log')}.pid"),
                "serial": tail_text(serial_path, 160),
                "qemu": tail_text(log_dir / f"{serial_path.name.removesuffix('.serial.log')}.qemu.log", 80),
            })
    return {
        "processes": process_snapshot(),
        "disk": {"path": rel(disk), "exists": disk.exists(), "bytes": disk.stat().st_size if disk.exists() else 0},
        "results": stats,
        "access": {"path": rel(access), "rows": max(0, sum(1 for _ in access.open(errors="ignore")) - 1) if access.exists() else 0},
        "server_log": tail_text(ROOT / "disk/nbd_server.log", 80),
        "action_log": tail_text(LOG_DIR / "latest.log", 120),
        "vm_logs": vm_logs,
    }


def heatmap_data():
    path = ROOT / "logs/nbd_access.csv"
    time_bins, page_bins = 96, 42
    if not path.exists():
        return {"rows": 0, "time_bins": time_bins, "page_bins": page_bins, "cells": []}
    rows = []
    with path.open(newline="", errors="replace") as source:
        for row in csv.DictReader(source):
            try:
                rows.append((int(row["offset"]) // 512, max(1, int(row["length"]) // 512)))
            except (KeyError, ValueError):
                continue
    if not rows:
        return {"rows": 0, "time_bins": time_bins, "page_bins": page_bins, "cells": []}
    cells = [[0 for _ in range(time_bins)] for _ in range(page_bins)]
    max_log = max(1.0, math.log2(max(page for page, _ in rows) + 2))
    for index, (page, length) in enumerate(rows):
        time_bin = min(time_bins - 1, index * time_bins // len(rows))
        low = int(math.log2(page + 1) / max_log * page_bins)
        high_page = page + length - 1
        high = int(math.log2(high_page + 2) / max_log * page_bins)
        for page_bin in range(max(0, low), min(page_bins, max(low + 1, high + 1))):
            cells[page_bin][time_bin] += 1
    peak = max(max(row) for row in cells) or 1
    return {"rows": len(rows), "time_bins": time_bins, "page_bins": page_bins,
            "peak": peak, "cells": cells}


def tail_text(path: Path, limit: int):
    if not path.exists():
        return ""
    return "\n".join(path.read_text(errors="replace").splitlines()[-limit:])


def run_action(name: str, args: list[str]):
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log = LOG_DIR / "latest.log"
    with log.open("w") as out:
        out.write(f"$ {' '.join(args)}\n\n")
        out.flush()
        process = subprocess.Popen(args, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
        PID_FILE.write_text(str(process.pid))
        code = process.wait()
        out.write(f"\n[exit {code}]\n")
    PID_FILE.unlink(missing_ok=True)
    return code


def selected_config(config_id):
    return next((item for item in CONFIGS if item["id"] == config_id), CONFIGS[0])


def action_worker(action, payload):
    config = selected_config(payload.get("config", "default"))
    if action == "build":
        workload = payload.get("workload", "ycsb")
        script = WORKLOADS.get(workload, DEFAULT_WORKLOAD)
        args = ["make", "guest-image", f"WORKLOAD_SCRIPT={script}"]
        if workload == "ycsb":
            args += ["GUEST_YCSB=1", "GUEST_YCSB_THREADS=1"]
        elif workload == "gups":
            args += ["GUEST_GUPS=1", "GUEST_YCSB=0"]
        else:
            args += ["GUEST_YCSB=0", "GUEST_GUPS=0"]
        if payload.get("force"):
            args.append("FORCE_REBUILD=1")
        return run_action(action, args)
    if action == "compile":
        return run_action(action, ["make", "nbd"])
    if action == "launch":
        return run_action(action, ["make", "experiment", f"NBD_CONFIG={config['nbd']}", f"VMS_CONFIG={config['vms']}", "RANDOM=0"])
    if action == "teardown":
        return run_action(action, ["make", "experiment-down"])
    if action == "force-teardown":
        run_action(action, ["python3", "kvm/launch.py", config["vms"], "--stop", "--force"])
        return run_action(action, ["make", "nbd-stop"])
    raise ValueError(f"unknown action: {action}")


def start_action(action, payload):
    if pid_alive(PID_FILE):
        return {"ok": False, "error": "Another action is already running."}
    thread = threading.Thread(target=action_worker, args=(action, payload), daemon=True)
    thread.start()
    return {"ok": True, "started": action}


def safe_image_path(raw):
    value = raw or "/"
    if not value.startswith("/"):
        value = "/" + value
    normalized = os.path.normpath(value)
    if ".." in Path(normalized).parts:
        raise ValueError("invalid image path")
    return normalized


def image_ls(raw):
    image = ROOT / "disk/data/nbd_disk.img"
    if not image.exists():
        return {"ok": False, "error": "Backing image does not exist."}
    path = safe_image_path(raw)
    command = ["debugfs", "-R", f"ls -l {path}", str(image)]
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=8)
    except FileNotFoundError:
        return {"ok": False, "error": "debugfs is not installed; install e2fsprogs to inspect ext4 images."}
    output = result.stdout + result.stderr
    if result.returncode == 0 and Path(path).suffix in {".json", ".txt", ".csv", ".py", ".yaml", ".log", ".sh"}:
        preview = subprocess.run(["debugfs", "-R", f"cat {path}", str(image)],
                                 cwd=ROOT, capture_output=True, text=True, timeout=8)
        output += "\n\n--- file preview ---\n" + preview.stdout + preview.stderr
    return {"ok": result.returncode == 0, "path": path, "output": output}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send_json(self, payload, code=200):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/":
            body = WEB.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif parsed.path == "/api/state":
            self.send_json(state())
        elif parsed.path == "/api/configs":
            self.send_json({"configs": CONFIGS})
        elif parsed.path == "/api/heatmap":
            self.send_json(heatmap_data())
        elif parsed.path == "/api/image":
            try:
                self.send_json(image_ls(parse_qs(parsed.query).get("path", ["/"])[0]))
            except ValueError as error:
                self.send_json({"ok": False, "error": str(error)}, 400)
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != "/api/action":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length) or b"{}")
            action = payload.get("action")
            if action not in {"build", "compile", "launch", "teardown", "force-teardown"}:
                raise ValueError("unsupported action")
            self.send_json(start_action(action, payload))
        except (ValueError, json.JSONDecodeError) as error:
            self.send_json({"ok": False, "error": str(error)}, 400)


if __name__ == "__main__":
    port = int(os.environ.get("EXPERIMENT_UI_PORT", "8765"))
    print(f"Experiment UI: http://127.0.0.1:{port}")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
