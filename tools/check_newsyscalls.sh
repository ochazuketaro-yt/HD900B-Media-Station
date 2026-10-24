#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Static check (nothing is executed): does an ELF still contain *inline* syscalls that Linux 2.6.34 does not have?
# Looks for "li v0,N" with N = 4366 (statx), 4338 (prlimit64), 4353 (getrandom), 4351 (renameat2) inside functions.
# Note: a few hits can be plain constants (e.g. OpenSSL's FFC parameter code contains 4353) -- inspect the function name.
# Usage: tools/check_newsyscalls.sh ELF...     (objdump is taken from $OBJDUMP, or mipsel-linux-objdump on PATH)
OD=${OBJDUMP:-mipsel-linux-objdump}
for b in "$@"; do
  n=$("$OD" -d --no-show-raw-insn "$b" | awk '/^[0-9a-f]+ <.*>:$/{f=$2} /li\t+v0,(4366|4338|4353|4351)$/{print "    " f " " $NF}' | sort -u)
  if [ -z "$n" ]; then echo "CLEAN  $b"; else echo "HAS    $b  (inspect: constants vs real syscalls)"; echo "$n"; fi
done
