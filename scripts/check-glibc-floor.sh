#!/bin/bash
# Fail if a binary needs a newer glibc than the given floor.
# Usage: scripts/check-glibc-floor.sh <binary> <max-glibc-version>
# readelf (not objdump) so it works on binaries for any architecture.
set -euo pipefail

bin=$1 floor=$2
need=$(readelf -W --dyn-syms "$bin" | grep -o 'GLIBC_[0-9][0-9.]*' \
  | sed 's/GLIBC_//' | sort -uV | tail -1)
echo "$bin needs glibc ${need:-none} (floor $floor)"
if [ -n "$need" ] && [ "$(printf '%s\n' "$need" "$floor" | sort -V | tail -1)" != "$floor" ]; then
  echo "::error::$bin requires glibc $need, newer than the $floor floor"
  readelf -W --dyn-syms "$bin" | grep -E "GLIBC_($(echo "$need" | sed 's/\./\\./g'))" || true
  exit 1
fi
