#!/usr/bin/env bash
# Build and run the host-side boot-entries parser regression.
#
# Testing this here rather than from src/kernel/test/ costs the kernel image
# nothing, which is the whole reason it lives here: every kernel-side test
# function lands in kernel.exe, whose end must stay below the user base at
# 0x800000, and that budget is exhausted. Measured 2026-09-03 -- the equivalent
# TEST_CAT_BOOT case was written and its build FAILED on the BSS-collision
# guard with 47 bytes of .text headroom. Same rationale as
# tools/boot-header-tests/run.sh.
#
#   bash tools/boot-entries-parser-tests/run.sh
#
# Exit: 0 pass, 1 a check failed, 2 setup failure.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$REPO_ROOT/tools/boot-entries-parser-tests"
OUT="${TMPDIR:-/tmp}/impossible-os-boot-entries-parser-tests"

CC_BIN="${CC:-cc}"
command -v "$CC_BIN" >/dev/null 2>&1 || {
    echo "boot-entries-parser-tests: no host compiler ($CC_BIN)" >&2; exit 2; }

mkdir -p "$OUT"
# -Wno-unused-function: the parser TU is written for the loader, so a host test
# that exercises the CRC surface legitimately leaves other statics unreferenced.
"$CC_BIN" -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-function \
    -o "$OUT/test_crc_poison" "$HERE/test_crc_poison.c"

"$OUT/test_crc_poison"
