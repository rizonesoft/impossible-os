#!/usr/bin/env bash
# Build and run the host-side boot-trend scan-policy regression.
#
# Testing this here rather than from src/kernel/test/ costs the kernel image
# nothing, which is the whole reason it lives here: every kernel-side test
# function lands in kernel.exe, whose end must stay below the user base at
# 0x800000, and that budget is exhausted. Measured 2026-09-03 -- the change
# under test left 15 bytes of .text headroom. Same rationale as
# tools/boot-entries-parser-tests/run.sh and tools/boot-header-tests/run.sh.
#
#   bash tools/boot-trend-scan-tests/run.sh
#
# Exit: 0 pass, 1 a check failed, 2 setup failure.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$REPO_ROOT/tools/boot-trend-scan-tests"
OUT="${TMPDIR:-/tmp}/impossible-os-boot-trend-scan-tests"

CC_BIN="${CC:-cc}"
command -v "$CC_BIN" >/dev/null 2>&1 || {
    echo "boot-trend-scan-tests: no host compiler ($CC_BIN)" >&2; exit 2; }

mkdir -p "$OUT"
"$CC_BIN" -std=c11 -O1 -Wall -Wextra -Werror \
    -I"$REPO_ROOT/include" \
    -o "$OUT/test_scan" "$HERE/test_scan.c"

"$OUT/test_scan"

# The fixtures above drive a SIMULATOR of the publisher's loop, so they stay
# green if the shipped loop drifts away from the shared decision helper. This
# binds them to it from the source side, and carries its own control.
python3 "$HERE/check_production_loop.py"
