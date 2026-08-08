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
# Exit code: the validator's exit code, propagated verbatim. It has THREE
# values, not two, and the third is the one a caller must not misread:
#   0  clean -- every check passed.
#   1  GRAPH FINDINGS -- the corpus has problems a human should fix.
#   2  usage error or INFRASTRUCTURE REFUSAL -- the cache or the --diff
#      baseline could not be trusted, so NO verdict was reached and nothing is
#      asserted about the corpus. Re-run after rebuilding rather than treating
#      it as a graph failure.
# This header said "0 if all 7 checks pass, 1 otherwise" while rc 2 was already
# reachable, which is how an infrastructure failure came to read as a graph
# verdict; scripts/todo-graph/validate.py carries the same contract.
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
        # The corpus binding goes with the cache it describes (section 21). A
        # binding left behind is inert -- readers locate one by the digest of
        # the cache they read, so an orphan matches nothing -- but leaving it
        # would litter build/ once per discarded build.
        rm -f "$CACHE_PATH" "$CACHE_PATH".corpus-*.json 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Ensure build/ exists for the output.
mkdir -p "$REPO_ROOT/build"

# Step 1: rebuild cache. Exit on failure (malformed / missing frontmatter).
# BUILD.PY'S STATUS PROPAGATES UNCHANGED. This flattened every nonzero result to
# 1 while the header above promised the opposite, which mattered the moment
# build.py grew a second failure code: rc 3 (the corpus moved underneath the
# build) is RETRYABLE and rc 1 (malformed frontmatter) is not, and a caller that
# sees 1 for both cannot tell them apart (Codex adversarial, section 21).
# Captured from the BARE invocation: inside `if ! cmd; then`, `$?` is the status
# of the negation (always 0), not of the command -- which would have propagated
# a successful exit for every failure.
python3 "$BUILD_PY" --quiet --output "$CACHE_PATH"
BUILD_RC=$?
if [ "$BUILD_RC" -ne 0 ]; then
    echo "[build-and-validate] FATAL: build.py failed (rc=$BUILD_RC); not running validator" >&2
    exit "$BUILD_RC"
fi

# Step 2: validate. Exit code propagates -- must NOT use `exec` here
# because exec replaces the shell process and skips the cleanup trap,
# which would leak build/todo-cache.json on every invocation unless
# --keep-cache was passed.
python3 "$VALIDATE_PY" --cache "$CACHE_PATH" "${VALIDATE_FLAGS[@]}"
exit $?
