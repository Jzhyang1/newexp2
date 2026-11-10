#!/bin/sh
# Two applications at once in one guest: a code search and a backup of the same tree, @PASSES@ rounds.
i=0
while [ $i -lt @PASSES@ ]; do
  grep -r -c -e "@PAT@" linux-6.6 > /dev/null &
  tar cf - linux-6.6/drivers linux-6.6/arch | cksum
  wait; i=$((i+1))
done
