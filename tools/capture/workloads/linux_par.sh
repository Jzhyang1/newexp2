#!/bin/sh
# Parallel checksumming of every file (8 concurrent md5sum processes, interleaved I/O inside one guest), @PASSES@ times.
i=0
while [ $i -lt @PASSES@ ]; do find linux-6.6 -type f -print0 | xargs -0 -P 8 -n 64 md5sum > /dev/null; i=$((i+1)); done
