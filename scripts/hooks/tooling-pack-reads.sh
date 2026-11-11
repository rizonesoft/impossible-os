#!/usr/bin/env bash
# The kernel, loader and user sources the tooling pack reads, one repo-relative
# path a line.
#
#   bash scripts/hooks/tooling-pack-reads.sh [path/to/test-tooling.sh]
#
# scripts/test-tooling.sh holds checks that read source TEXT (a grep for one
# expression in src/kernel/test/test_usermode.c, for one). The pre-push hook ran
# the pack only for a push that touched the tooling itself, so a source change
# that moved such a line passed every local gate and failed CI's tooling job:
# main went red on 0fd8bf1c0 (2026-11-11). The hook now also runs the pack when
# a push touches a file listed here. The list is read from the pack on every
# run, so a new source-text check is covered the moment it is written.
set -uo pipefail
PACK="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/test-tooling.sh}"
[ -r "$PACK" ] || { echo "tooling-pack-reads: cannot read $PACK" >&2; exit 2; }
grep -oE '\$REPO_ROOT/(src|include|user)/[A-Za-z0-9_./+-]+\.(c|h|asm|inc|ld|S)' "$PACK" \
    | sed 's|^\$REPO_ROOT/||' | sort -u
exit 0
