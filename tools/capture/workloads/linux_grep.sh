#!/bin/sh
# Repeated code search over the Linux 6.6 source tree (build_dir_disk.py --src-tar linux-6.6.tar.xz).
# @PASSES@ passes of `grep -r`; the tree (1.26 GiB) is larger than the guest's page cache, so every pass reads from disk.
i=0
while [ $i -lt @PASSES@ ]; do grep -r -c -e "@PAT@" linux-6.6 > /dev/null; i=$((i+1)); done
