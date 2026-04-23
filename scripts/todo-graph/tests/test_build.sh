#!/usr/bin/env bash
# ============================================================================
# test_build.sh -- regression tests for scripts/todo-graph/build.py
#
# Wired into scripts/test-tooling.sh via the [todo-graph] block. Runs
# under `make test-tooling` and on every CI push.
#
# Spec: the Unit Tests block of the TODO-metadata-layer plan in
# todo/00-infrastructure/.
#
# Coverage (mirrors the spec's Unit Tests checklist):
#   1. Generator exits 0 on a repo with valid frontmatter (uses the live
#      todo/ tree which today has zero frontmatter; "no-frontmatter" mode
#      is the back-compat path validated here).
#   2. Generator exits 1 with a clear file:line message on a single
#      deliberately-malformed frontmatter (synthesizes a fixture under
#      $tmp; deletes after).
#   3. Running twice produces byte-identical cache (determinism).
#   4. Cache node count matches `find todo -name 'TODO-*.md' -not -name
#      'TODO-00-INDEX.md' | wc -l`.
#
# Output style: t_pass / t_fail (matches scripts/test-tooling.sh).
# ============================================================================

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_PY="$REPO_ROOT/scripts/todo-graph/build.py"

PASS=0
FAIL=0

t_pass() {
    PASS=$((PASS + 1))
    printf '  [PASS] %s\n' "$1"
}

t_fail() {
    FAIL=$((FAIL + 1))
    printf '  [FAIL] %s\n' "$1" >&2
}

cleanup() {
    rm -rf "${TMP_DIR:-}" 2>/dev/null || true
}
trap cleanup EXIT

TMP_DIR="$(mktemp -d -t todograph-build-test.XXXXXX)" \
    || { echo "[test_build] FAIL: mktemp failed"; exit 2; }

# ----------------------------------------------------------------------
# Test 1: clean run on the live repo exits 0 + writes a parseable cache.
# ----------------------------------------------------------------------
CACHE_OUT="$TMP_DIR/cache-1.json"
if python3 "$BUILD_PY" --quiet --output "$CACHE_OUT" >"$TMP_DIR/run1.log" 2>&1; then
    if [ -s "$CACHE_OUT" ]; then
        if python3 -c "import json; json.load(open('$CACHE_OUT'))" 2>/dev/null; then
            t_pass "clean run on todo/ exits 0 and writes parseable JSON"
        else
            t_fail "cache JSON not parseable"
        fi
    else
        t_fail "cache file empty after clean run"
    fi
else
    t_fail "build.py exited non-zero on clean repo (see $TMP_DIR/run1.log)"
fi

# ----------------------------------------------------------------------
# Test 2: malformed frontmatter exits 1 + names the file/category +
# does NOT write a partial cache.
# ----------------------------------------------------------------------
BAD_ROOT="$TMP_DIR/bad-tree"
mkdir -p "$BAD_ROOT/01-test"
cat > "$BAD_ROOT/01-test/TODO-01-malformed.md" <<'EOF'
---
schema_version: 1
id: bad id with spaces
domain: 01-test
status: not-a-real-status
title: Bad fixture
---

# Bad fixture
EOF
BAD_CACHE="$TMP_DIR/cache-bad.json"
RC=0
python3 "$BUILD_PY" --quiet --root "$BAD_ROOT" --output "$BAD_CACHE" --repo-root "$REPO_ROOT" \
    >"$TMP_DIR/bad.log" 2>&1 || RC=$?
if [ "$RC" = "1" ]; then
    t_pass "malformed frontmatter exits 1"
    if grep -q "TODO-01-malformed.md" "$TMP_DIR/bad.log"; then
        t_pass "error message names the offending file"
    else
        t_fail "error message lacks file path (expected TODO-01-malformed.md)"
    fi
    if grep -q "invalid-id\|unknown-status" "$TMP_DIR/bad.log"; then
        t_pass "error message names a category from the generator-spec catalog"
    else
        t_fail "error message lacks category from generator-spec catalog"
    fi
    if [ ! -e "$BAD_CACHE" ]; then
        t_pass "cache NOT written on malformed input (fail-closed)"
    else
        t_fail "cache was written on malformed input (should be fail-closed)"
    fi
else
    t_fail "malformed frontmatter expected exit 1, got $RC"
fi

# ----------------------------------------------------------------------
# Test 3: byte-identical re-run (idempotency / determinism).
# ----------------------------------------------------------------------
CACHE_RUN_A="$TMP_DIR/cache-a.json"
CACHE_RUN_B="$TMP_DIR/cache-b.json"
python3 "$BUILD_PY" --quiet --output "$CACHE_RUN_A" >/dev/null 2>&1
python3 "$BUILD_PY" --quiet --output "$CACHE_RUN_B" >/dev/null 2>&1
if cmp -s "$CACHE_RUN_A" "$CACHE_RUN_B"; then
    t_pass "two consecutive runs produce byte-identical output"
else
    t_fail "consecutive runs differ (expected byte-identical for determinism)"
fi

# ----------------------------------------------------------------------
# Test 4: cache node count matches `find` count of TODO-*.md files
# (excluding TODO-00-INDEX.md per the generator spec).
# ----------------------------------------------------------------------
EXPECTED=$(find "$REPO_ROOT/todo" -name 'TODO-*.md' -not -name 'TODO-00-INDEX.md' | wc -l)
ACTUAL=$(python3 -c "import json; print(len(json.load(open('$CACHE_RUN_A'))))")
if [ "$EXPECTED" = "$ACTUAL" ]; then
    t_pass "cache node count ($ACTUAL) matches find-count ($EXPECTED)"
else
    t_fail "node count mismatch: cache=$ACTUAL find=$EXPECTED"
fi

# ----------------------------------------------------------------------
# Test 5: cache schema sanity (every node has required keys).
# ----------------------------------------------------------------------
python3 - "$CACHE_RUN_A" <<'PY' 2>"$TMP_DIR/schema.log"
import json, sys
nodes = json.load(open(sys.argv[1]))
required = {"id", "domain", "status", "title", "file_path",
            "created_at", "last_active_at", "sections",
            "section_headings", "inputs_xrefs", "stamps_xrefs"}
for n in nodes:
    missing = required - set(n.keys())
    if missing:
        print(f"NODE MISSING KEYS {sorted(missing)}: {n.get('file_path')}", file=sys.stderr)
        sys.exit(1)
    # created_at <= last_active_at when both present
    c, l = n.get("created_at"), n.get("last_active_at")
    if c and l and c > l:
        print(f"TS INVARIANT BROKEN: {n['file_path']}: {c} > {l}", file=sys.stderr)
        sys.exit(2)
sys.exit(0)
PY
if [ $? -eq 0 ]; then
    t_pass "every node has required keys + created_at <= last_active_at invariant"
else
    t_fail "schema/invariant check failed (see $TMP_DIR/schema.log)"
    cat "$TMP_DIR/schema.log" >&2 2>/dev/null || true
fi

# ----------------------------------------------------------------------
# Test 6: performance budget (under 2s wall-clock per the generator spec).
# ----------------------------------------------------------------------
START_NS=$(date +%s%N)
python3 "$BUILD_PY" --quiet --output "$TMP_DIR/cache-perf.json" >/dev/null 2>&1
END_NS=$(date +%s%N)
ELAPSED_MS=$(( (END_NS - START_NS) / 1000000 ))
if [ "$ELAPSED_MS" -lt 2000 ]; then
    t_pass "build under 2s wall-clock (${ELAPSED_MS}ms)"
else
    t_fail "build exceeded 2s budget: ${ELAPSED_MS}ms"
fi

# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
TOTAL=$((PASS + FAIL))
echo
echo "[test_build] ${PASS}/${TOTAL} passed, ${FAIL} failed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
