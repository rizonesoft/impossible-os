#!/bin/bash
# =============================================================================
# check_make_duplicates.sh -- lint Check 38: no target has two recipes
#
# GNU make keeps the LAST recipe for a target and drops the earlier one with a
# warning, so a documented target silently does something else. Until
# 2026-11-13 run-test and run-debug were each defined twice, and the four
# warnings on every build trained readers to ignore make's warnings.
#
# make itself decides what a duplicate is: the Makefile is parsed (make -n on a
# target that does not exist, about 0.15 s) and any "overriding recipe" or
# "ignoring old recipe" warning fails. Pattern rules and double-colon rules do
# not warn, so they are not refused.
#
# Usage: check_make_duplicates.sh [MAKEFILE]   (default: the root Makefile)
# Exit:  0 clean, 1 duplicates (printed), 2 make could not be run
# =============================================================================

set -uo pipefail

MK="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/Makefile}"
if ! command -v make >/dev/null 2>&1; then
    echo "check_make_duplicates: make is not installed" >&2
    exit 2
fi
if [ ! -f "$MK" ]; then
    echo "check_make_duplicates: no Makefile at $MK" >&2
    exit 2
fi

# deadline: a parse-only make is bounded by timeout; 60 s is far above its
# measured 0.15 s.
OUT="$(cd "$(dirname "$MK")" && timeout 60 make -n -q --no-print-directory \
        -f "$MK" __check_make_duplicates_parse_only__ 2>&1)"
DUP="$(printf '%s\n' "$OUT" | grep -E 'warning: (overriding|ignoring old) recipe for target')"
if [ -n "$DUP" ]; then
    printf '%s\n' "$DUP"
    exit 1
fi
exit 0
