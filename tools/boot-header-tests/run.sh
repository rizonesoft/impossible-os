#!/usr/bin/env bash
# Build and run the host-side tests for the pure headers under include/boot/.
#
# These headers are plain C over plain types precisely so that the loader and a
# test can compile them unchanged. Testing them here rather than from
# src/kernel/test/ costs the kernel image nothing: every kernel-side test
# function lands in kernel.exe, whose BSS end must stay below the user base at
# 0x800000, and that budget is exhausted (measured 2026-08-19: 8192 bytes free
# against a 8664-byte suite).
#
# The Authenticode expectation is cross-checked against an INDEPENDENT
# transcription of the spec's numbered steps in authenticode_oracle.py, so a
# drift in either one fails the run rather than both agreeing with themselves.
#
#   bash tools/boot-header-tests/run.sh
#
# Exit: 0 pass, 1 a check failed or the oracle disagrees, 2 setup failure.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$REPO_ROOT/tools/boot-header-tests"
OUT="${TMPDIR:-/tmp}/impossible-os-boot-header-tests"

CC_BIN="${CC:-cc}"
command -v "$CC_BIN" >/dev/null 2>&1 || { echo "boot-header-tests: no host compiler ($CC_BIN)" >&2; exit 2; }

mkdir -p "$OUT"
"$CC_BIN" -std=c11 -O1 -Wall -Wextra -Werror \
    -o "$OUT/test_boot_headers" "$HERE/test_boot_headers.c"

# The oracle is the reason the digest assertion means anything: it recomputes
# the expectation from the spec steps and must still agree with the constant
# compiled into the test.
oracle_digest="$(python3 "$HERE/authenticode_oracle.py" | awk '/^authenticode/ {print $3}')"
compiled_digest="$(grep -A2 'expect_authenticode\[SHA256B_DIGEST_LEN\] = {' "$HERE/test_boot_headers.c" \
    | tr -d ' \n' | grep -oE '0x[0-9a-f]{2}' | sed 's/0x//' | tr -d '\n')"
if [ "$oracle_digest" != "$compiled_digest" ]; then
    echo "boot-header-tests: ORACLE DRIFT" >&2
    echo "  oracle:   $oracle_digest" >&2
    echo "  compiled: $compiled_digest" >&2
    exit 1
fi

exec "$OUT/test_boot_headers"
