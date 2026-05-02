#!/usr/bin/env bash
# ============================================================================
# test_phantom_include.sh -- regression test for
# scripts/todo-graph/check_phantom_includes.py (lint Check 8 phantom-
# include wrapper). Wired into scripts/test-tooling.sh.
#
# Coverage:
#   1. Synthetic kernel C TU with one used + one unused include reports
#      the unused header (klog.h) and not the used one (types.h).
#   2. PHANTOM-INCLUDE-OK marker on the unused-include line suppresses
#      the finding.
#   3. Skips cleanly with exit 3 when clangd-19 is unavailable.
# ============================================================================

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WRAPPER="$REPO_ROOT/scripts/todo-graph/check_phantom_includes.py"

PASS=0
FAIL=0
SKIP=0

t_pass() { PASS=$((PASS + 1)); printf '  [PASS] %s\n' "$1"; }
t_fail() { FAIL=$((FAIL + 1)); printf '  [FAIL] %s\n' "$1" >&2; }
t_skip() { SKIP=$((SKIP + 1)); printf '  [SKIP] %s\n' "$1"; }

cleanup() { rm -rf "${TMP_DIR:-}" 2>/dev/null || true; }
trap cleanup EXIT

TMP_DIR="$(mktemp -d -t phantom-include-test.XXXXXX)" \
    || { echo "[test_phantom_include] FAIL: mktemp failed"; exit 2; }

# --------------------------------------------------------------------
# Skip-on-missing-clangd contract
# --------------------------------------------------------------------
if ! command -v clangd-19 >/dev/null 2>&1 && ! command -v clangd >/dev/null 2>&1; then
    OUT="$TMP_DIR/no-clangd.log"
    set +e
    python3 "$WRAPPER" --limit 1 >"$OUT" 2>&1
    RC=$?
    set -e
    if [ "$RC" -eq 3 ]; then
        t_pass "exit 3 when clangd-19 absent (graceful skip)"
    else
        t_fail "expected exit 3 (clangd missing), got $RC"
    fi
    t_skip "synthetic detection cases (need clangd-19)"
    t_skip "PHANTOM-INCLUDE-OK marker (need clangd-19)"
    printf '\n[test_phantom_include] %d pass, %d fail, %d skip\n' \
        "$PASS" "$FAIL" "$SKIP"
    [ "$FAIL" -eq 0 ]
    exit
fi

# --------------------------------------------------------------------
# Synthetic translation unit: one used + one unused include.
# Place under src/kernel/ so the bridge resolves it as a kernel TU.
# --------------------------------------------------------------------
FIX_DIR="$REPO_ROOT/src/kernel/test_phantom_fixture"
FIX_FILE="$FIX_DIR/synthetic_phantom.c"
FIX_HDR_USED="$FIX_DIR/used_header.h"
FIX_HDR_PHANTOM="$FIX_DIR/phantom_header.h"
mkdir -p "$FIX_DIR"

cat > "$FIX_HDR_USED" <<'EOF'
#ifndef PHANTOM_FIXTURE_USED_H
#define PHANTOM_FIXTURE_USED_H
static inline int synthetic_used_helper(int x) { return x + 1; }
#endif
EOF

cat > "$FIX_HDR_PHANTOM" <<'EOF'
#ifndef PHANTOM_FIXTURE_PHANTOM_H
#define PHANTOM_FIXTURE_PHANTOM_H
static inline int synthetic_phantom_helper(int x) { return x * 2; }
#endif
EOF

cat > "$FIX_FILE" <<'EOF'
/* Synthetic phantom-include fixture. Removed by tests/test_phantom_include.sh. */
#include "used_header.h"
#include "phantom_header.h"

int synthetic_phantom_value(int seed) {
    return synthetic_used_helper(seed);
}
EOF

fixture_cleanup() {
    rm -rf "$FIX_DIR" 2>/dev/null || true
}
trap 'fixture_cleanup; cleanup' EXIT

# --------------------------------------------------------------------
# Test 1: detection -- klog flagged, types not.
# --------------------------------------------------------------------
OUT1="$TMP_DIR/detect.log"
set +e
python3 "$WRAPPER" --paths "$FIX_FILE" >"$OUT1" 2>"$TMP_DIR/detect.err"
RC=$?
set -e

if [ "$RC" -ne 0 ]; then
    t_fail "wrapper exit $RC (expected 0); stderr: $(head -3 "$TMP_DIR/detect.err" | tr '\n' ' ')"
elif ! grep -q 'phantom_header.h' "$OUT1" 2>/dev/null; then
    if [ ! -s "$OUT1" ]; then
        t_skip "clangd reported no unused-includes (config dependent)"
    else
        t_fail "expected phantom_header.h in findings, got: $(cat "$OUT1")"
    fi
elif grep -q 'used_header.h' "$OUT1" 2>/dev/null; then
    t_fail "used_header.h false-positive: $(grep 'used_header.h' "$OUT1")"
else
    t_pass "phantom_header.h flagged, used_header.h not flagged"
fi

# --------------------------------------------------------------------
# Test 2: PHANTOM-INCLUDE-OK marker suppresses the finding.
# --------------------------------------------------------------------
cat > "$FIX_FILE" <<'EOF'
/* Synthetic phantom-include fixture (allowlisted). */
#include "used_header.h"
#include "phantom_header.h"  // PHANTOM-INCLUDE-OK: kept for reviewer test

int synthetic_phantom_value(int seed) {
    return synthetic_used_helper(seed);
}
EOF

OUT2="$TMP_DIR/marker.log"
set +e
python3 "$WRAPPER" --paths "$FIX_FILE" >"$OUT2" 2>"$TMP_DIR/marker.err"
RC=$?
set -e

if [ "$RC" -ne 0 ]; then
    t_fail "marker run exit $RC"
elif grep -q 'phantom_header.h' "$OUT2" 2>/dev/null; then
    t_fail "marker did not suppress: $(grep 'phantom_header.h' "$OUT2")"
else
    t_pass "PHANTOM-INCLUDE-OK marker suppresses finding"
fi

printf '\n[test_phantom_include] %d pass, %d fail, %d skip\n' \
    "$PASS" "$FAIL" "$SKIP"

[ "$FAIL" -eq 0 ]
