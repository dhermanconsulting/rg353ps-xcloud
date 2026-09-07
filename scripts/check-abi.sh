#!/bin/sh
# Fail the build if a binary would not start on the RG353 (glibc 2.32).
# Run this on every artefact before deploying.
set -e
BIN="$1"
[ -n "$BIN" ] || { echo "usage: check-abi.sh <binary>" >&2; exit 2; }

OBJDUMP=${OBJDUMP:-objdump}
READELF=${READELF:-readelf}

echo "== $BIN =="
file "$BIN" 2>/dev/null || true
$READELF -d "$BIN" | grep -E 'NEEDED|RPATH|RUNPATH' || true
echo "-- highest glibc symbol versions --"
$OBJDUMP -T "$BIN" | grep -o 'GLIBC_[0-9]\+\.[0-9]\+' | sort -uV | tail -5
echo "-- highest GLIBCXX --"
$OBJDUMP -T "$BIN" | grep -o 'GLIBCXX_[0-9.]\+' | sort -uV | tail -5 || true
$READELF -l "$BIN" | grep -i interpreter || true

if $OBJDUMP -T "$BIN" | grep -qE 'GLIBC_2\.(3[3-9]|[4-9][0-9])'; then
  echo "ABI ERROR: references glibc newer than 2.32 - will not run on the RG353" >&2
  exit 1
fi
echo "ABI OK for glibc 2.32"
