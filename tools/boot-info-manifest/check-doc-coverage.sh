#!/usr/bin/env bash
# ============================================================================
# check-doc-coverage.sh -- fail the build if `include/kernel/boot_info.h`
# gains a field that is NOT documented in `docs/boot/boot-info-fields.md`.
#
# Thin shell wrapper around the Python checker (checker needs structured
# matrix parsing: walk section headings + extract only the first backticked
# identifier per markdown table row, then map manifest field paths to the
# expected section + field). Python3 is mandatory per CLAUDE.md's host
# bootstrap contract.
#
# Owner: boot-protocol section 10 (Cross-Domain Owner Audit for Every
# boot_info Field). Closes the silent-drift failure mode found by Codex
# re-review 2026-04-23 where 14 `boot_config` fields were absent from the
# matrix because nothing enforced documentation coverage.
#
# Usage:
#   bash tools/boot-info-manifest/check-doc-coverage.sh
#   bash tools/boot-info-manifest/check-doc-coverage.sh --help
#
# Exit codes:
#   0 = every non-sentinel field is documented in the matrix at row level
#   1 = one or more fields missing from the matrix (first offender named)
#   2 = prerequisite missing (dump-fields.inc, matrix, or python3 absent)
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

case "${1:-}" in
    -h|--help)
        sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    "")
        ;;
    *)
        echo "error: unknown argument '$1'; use --help" >&2
        exit 2
        ;;
esac

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 required but not installed" >&2
    exit 2
fi

exec python3 "$SCRIPT_DIR/check-doc-coverage.py"
