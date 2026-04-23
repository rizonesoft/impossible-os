#!/usr/bin/env bash
# ============================================================================
# scripts/todo-graph/build-and-validate.sh -- rebuild cache + run validator.
#
# Owner: TODO-06 §6 (CI Gate and Make Target) in todo/00-infrastructure/.
#
# One-liner wrapper for the two-step todo-graph flow:
#   1. python3 scripts/todo-graph/build.py --quiet --output build/todo-cache.json
#   2. python3 scripts/todo-graph/validate.py --cache build/todo-cache.json
#
# Exit code: validator's exit code (0 if all 7 checks pass, 1 otherwise).
# If build.py fails (malformed frontmatter, missing-frontmatter FATAL, etc)
# the script aborts with build.py's exit code and never runs the validator.
#
# Flags:
#   --keep-cache     : leave build/todo-cache.json on disk after run.
#                      Default: delete on exit (CI-friendly; no dirty tree).
#                      Queries that consume the cache between runs should
#                      use this to skip the rebuild roundtrip.
#   --warnings-only  : pass through to validate.py; downgrades the
#                      pre-§5 migration-noise stale-xref failures to
#                      warnings (useful during the transition window,
#                      but the post-§5 tree should pass without it).
#   --diff BASELINE  : pass-through to validate.py in --diff mode;
#                      compare the freshly-rebuilt cache against BASELINE
#                      (typically main's last-green cache fetched as a
#                      CI artifact).
#   --quiet          : suppress per-check PASS lines.
#
# CI wiring: .github/workflows/build.yml calls this on paths filter
# `todo/**/*.md`. Make target: `make todo-graph`.
# ============================================================================

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_PY="$SCRIPT_DIR/build.py"
VALIDATE_PY="$SCRIPT_DIR/validate.py"
CACHE_PATH="$REPO_ROOT/build/todo-cache.json"

KEEP_CACHE=0
VALIDATE_FLAGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --keep-cache)
            KEEP_CACHE=1
            shift
            ;;
        --warnings-only)
            VALIDATE_FLAGS+=("--warnings-only")
            shift
            ;;
        --quiet)
            VALIDATE_FLAGS+=("--quiet")
            shift
            ;;
        --diff)
            if [ $# -lt 2 ]; then
                echo "[build-and-validate] FATAL: --diff requires a BASELINE path argument" >&2
                exit 2
            fi
            VALIDATE_FLAGS+=("--diff" "$2")
            shift 2
            ;;
        -h|--help)
            sed -n '/^# scripts/,/^# =/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "[build-and-validate] FATAL: unknown flag '$1'" >&2
            echo "Usage: $0 [--keep-cache] [--warnings-only] [--quiet] [--diff BASELINE]" >&2
            exit 2
            ;;
    esac
done

cleanup() {
    if [ "$KEEP_CACHE" = "0" ]; then
        rm -f "$CACHE_PATH" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Ensure build/ exists for the output.
mkdir -p "$REPO_ROOT/build"

# Step 1: rebuild cache. Exit on failure (malformed / missing frontmatter).
if ! python3 "$BUILD_PY" --quiet --output "$CACHE_PATH"; then
    echo "[build-and-validate] FATAL: build.py failed; not running validator" >&2
    exit 1
fi

# Step 2: validate. Exit code propagates -- must NOT use `exec` here
# because exec replaces the shell process and skips the cleanup trap,
# which would leak build/todo-cache.json on every invocation unless
# --keep-cache was passed.
python3 "$VALIDATE_PY" --cache "$CACHE_PATH" "${VALIDATE_FLAGS[@]}"
exit $?
