#!/usr/bin/env python3
"""How accurate is the emulated link?  Reads through bin/fwd_sim at each scenario's link settings with
no cache and no think time, and compares the measured mean read latency with the arithmetic model
(RTT + disk service + size/bandwidth).  The gap is software cost (loopback TCP, thread handoffs, copies).
Also measures plain time.sleep() overshoot, which is why the emulator spins the last ~100 us (SleepPrecise).
Writes logs/fwd_dc2/calibration.json.
"""
import json, os, re, subprocess, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "bin/fwd_sim")
DISK = os.path.join(ROOT, "disk/data/fwd_remote.img")
LINKS = {"zero": (0, 0, 0), "zone": (400, 10000, 100), "az": (1000, 10000, 100), "region": (60000, 1000, 100)}
SIZES = {"4K": ("random", 4096), "128K": ("seq", 131072)}
out = {"links": {}, "sleep_overshoot_us": {}}
for name, (rtt, bw, disk_us) in LINKS.items():
    srv = subprocess.Popen([BIN, "serve", "--file", DISK, "--port", "0", "--rtt-us", str(rtt), "--bw-mbps", str(bw), "--disk-us", str(disk_us)],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    port = int(srv.stdout.readline().split()[1])
    out["links"][name] = {"rtt_us": rtt, "bw_mbps": bw, "disk_us": disk_us}
    for sz, (wl, nbytes) in SIZES.items():
        n = 300 if name == "region" else 2000
        means = []
        for _ in range(3):
            p = subprocess.run([BIN, "bench", "--port", str(port), "--mode", "direct", "--workload", wl, "--req-bytes", str(nbytes),
                                "--requests", str(n), "--think-us", "0"], capture_output=True, text=True)
            means.append(float(re.search(r"mean_us=([\d.]+)", p.stdout).group(1)))
        model = rtt + disk_us + (nbytes * 8 / bw if bw else 0)
        out["links"][name][sz] = {"model_us": round(model, 1), "measured_us": round(sorted(means)[1], 1), "runs_us": means}
    srv.terminate(); srv.wait()
for us in (10, 50, 100, 200, 500, 1000):
    xs = []
    for _ in range(300):
        t = time.perf_counter_ns(); time.sleep(us / 1e6); xs.append((time.perf_counter_ns() - t) / 1e3 - us)
    xs.sort(); out["sleep_overshoot_us"][str(us)] = {"median": round(xs[150], 1), "p95": round(xs[285], 1)}
json.dump(out, open(os.path.join(ROOT, "logs/fwd_dc2/calibration.json"), "w"), indent=1)
print(json.dumps(out, indent=1))
