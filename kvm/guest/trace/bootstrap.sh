#!/bin/sh
# Runs as PID 1 (kernel cmdline: init=/bin/sh -- -c "<mount 9p; exec this>").
# The guest image is untouched: this directory is shared read-only over 9p
# as tag `tools`, and results go back through the writable `out` share, so
# nothing here needs root on the host or a rebuilt image.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null
mount -t debugfs nodev /sys/kernel/debug 2>/dev/null
mkdir -p /tmp/data
# The FAISS data disk is /dev/vdb (ext4, read-only: guest writes would be discarded by nbd_server anyway)
mount -t ext4 -o ro,noatime /dev/vdb /tmp/data > /tmp/out/mount.log 2>&1
exec python3 /tmp/tools/guest_main.py
