#!/bin/sh
# Dead-code report: functions the linker proves unreachable from the
# production binary. Rebuilds build/cclaw with per-function sections and
# --gc-sections, then lists every src/ function the linker discarded.
#
# Test-only code shows up here by construction: test binaries link
# libcclaw.a separately, so a function kept alive only by its tests is
# still dead in build/cclaw (how db_request.c survived 5 weeks unnoticed).
#
# Triage notes:
# - Function-pointer / table-referenced code is NOT reported (the reference
#   keeps its section) — no false positives from tool tables or JS bridge.
# - Semantically dead branches (flag never set, column never read) are
#   invisible to the linker; those still need grep.
# - The flag change makes .buildtag wipe build/ here and again on the next
#   normal make — a full rebuild each way is the price of the report.
set -e
cd "$(dirname "$0")/.."
rm -f build/cclaw   # force the link step — the report is its stderr
make EXTRA_CFLAGS="-ffunction-sections -fdata-sections" \
     LDFLAGS="-lcurl -lm -lpthread -ldl -Wl,--gc-sections -Wl,--print-gc-sections" \
     build/cclaw 2>&1 >/dev/null |
  sed -n "s|.*removing unused section '\.text\.\([^']*\)' in file 'build/\([^.]*\)\.o'.*|\2: \1|p" |
  grep -vE '^(sqlite3|civetweb|quickjs|qjs_cutils|qjs_dtoa|qjs_libunicode|qjs_libregexp|monocypher|templates):' |
  grep -v ': _curl_' |
  sort
