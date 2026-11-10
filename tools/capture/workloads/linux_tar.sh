#!/bin/sh
# Backup-style full read of the Linux source tree: tar to a checksum, @PASSES@ times.
i=0
while [ $i -lt @PASSES@ ]; do tar cf - linux-6.6 | cksum; i=$((i+1)); done
