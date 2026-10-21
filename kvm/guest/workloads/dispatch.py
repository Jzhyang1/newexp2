"""Select a guest workload using workload=... from the kernel command line."""
import os
import subprocess


def kernel_args():
    with open("/proc/cmdline") as cmdline:
        return {
            item.split("=", 1)[0]: item.split("=", 1)[1]
            for item in cmdline.read().split()
            if "=" in item
        }


args = kernel_args()
workload = args.get("workload", "large_scan")
scripts = {
    "faiss": "faiss_bench.py",
    "gups": "gups_bench.py",
    "large_scan": "large_scan.py",
    "thesios": "thesios_bench.py",
    "ycsb": "ycsb_bench.py",
}
try:
    script = scripts[workload]
except KeyError as error:
    raise RuntimeError(
        f"unknown workload={workload!r}; choose from {', '.join(sorted(scripts))}"
    ) from error

script_path = os.path.join("/opt/workload/benchmarks", script)
subprocess.run(["python3", script_path], check=True)