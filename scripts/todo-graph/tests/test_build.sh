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
# Every node MUST carry these keys; schema_version is included so a
# future regression that drops the cache versioning field surfaces
# here instead of slipping through unnoticed (Codex quality M1).
required = {"id", "schema_version", "domain", "status", "title",
            "file_path", "created_at", "last_active_at", "sections",
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
# Test 6a: TRULY malformed YAML (unclosed fence) reports `malformed-yaml`.
# Test 2 above only exercises semantic validation (invalid-id /
# unknown-status); the YAML itself parses. This fixture has an unclosed
# fence so the YAML parser path is the one that fires. Codex quality M2
# caught the gap.
# ----------------------------------------------------------------------
MAL_ROOT="$TMP_DIR/mal-tree"
mkdir -p "$MAL_ROOT/01-test"
cat > "$MAL_ROOT/01-test/TODO-01-unclosed.md" <<'EOF'
---
id: unclosed-fence
schema_version: 1
domain: 01-test
status: draft
title: Missing closing fence

# Body content but no closing --- separator above
EOF
RC=0
python3 "$BUILD_PY" --quiet --root "$MAL_ROOT" --output "$TMP_DIR/cache-mal.json" --repo-root "$REPO_ROOT" \
    >"$TMP_DIR/mal.log" 2>&1 || RC=$?
if [ "$RC" = "1" ] && grep -q "malformed-yaml\|opening --- has no closing" "$TMP_DIR/mal.log"; then
    t_pass "unclosed frontmatter fence reports malformed-yaml category"
else
    t_fail "unclosed fence: expected malformed-yaml category, got rc=$RC log='$(cat $TMP_DIR/mal.log)'"
fi

# ----------------------------------------------------------------------
# Test 6b: BOM-prefixed frontmatter is parsed as valid (not silently
# dropped to no-frontmatter). Codex quality M2 + the BOM fix that
# landed during adversarial review.
# ----------------------------------------------------------------------
BOM_ROOT="$TMP_DIR/bom-tree"
mkdir -p "$BOM_ROOT/01-test"
# Write with explicit UTF-8 BOM prefix.
printf '\xef\xbb\xbf---\nschema_version: 1\nid: bom-test\ndomain: 01-test\nstatus: draft\ntitle: BOM fixture\n---\n\n# BOM fixture body\n' \
    > "$BOM_ROOT/01-test/TODO-01-bom.md"
python3 "$BUILD_PY" --quiet --root "$BOM_ROOT" --output "$TMP_DIR/cache-bom.json" --repo-root "$REPO_ROOT" \
    >"$TMP_DIR/bom.log" 2>&1
if [ -f "$TMP_DIR/cache-bom.json" ]; then
    BOM_STATUS=$(python3 -c "import json; print(json.load(open('$TMP_DIR/cache-bom.json'))[0]['status'])")
    BOM_ID=$(python3 -c "import json; print(json.load(open('$TMP_DIR/cache-bom.json'))[0]['id'])")
    if [ "$BOM_STATUS" = "draft" ] && [ "$BOM_ID" = "bom-test" ]; then
        t_pass "BOM-prefixed frontmatter parsed (id=bom-test, status=draft)"
    else
        t_fail "BOM frontmatter mis-parsed: id=$BOM_ID status=$BOM_STATUS (expected bom-test/draft)"
    fi
else
    t_fail "BOM frontmatter: cache not written (BOM strip path likely broken)"
fi

# ----------------------------------------------------------------------
# Test 6c: CRLF-line-ended frontmatter is parsed as valid. Same risk
# class as BOM (Windows editors).
# ----------------------------------------------------------------------
CRLF_ROOT="$TMP_DIR/crlf-tree"
mkdir -p "$CRLF_ROOT/01-test"
printf -- '---\r\nschema_version: 1\r\nid: crlf-test\r\ndomain: 01-test\r\nstatus: draft\r\ntitle: CRLF fixture\r\n---\r\n\r\n# CRLF body\r\n' \
    > "$CRLF_ROOT/01-test/TODO-01-crlf.md"
python3 "$BUILD_PY" --quiet --root "$CRLF_ROOT" --output "$TMP_DIR/cache-crlf.json" --repo-root "$REPO_ROOT" \
    >"$TMP_DIR/crlf.log" 2>&1
if [ -f "$TMP_DIR/cache-crlf.json" ]; then
    CRLF_STATUS=$(python3 -c "import json; print(json.load(open('$TMP_DIR/cache-crlf.json'))[0]['status'])")
    CRLF_ID=$(python3 -c "import json; print(json.load(open('$TMP_DIR/cache-crlf.json'))[0]['id'])")
    if [ "$CRLF_STATUS" = "draft" ] && [ "$CRLF_ID" = "crlf-test" ]; then
        t_pass "CRLF-line-ended frontmatter parsed (id=crlf-test, status=draft)"
    else
        t_fail "CRLF frontmatter mis-parsed: id=$CRLF_ID status=$CRLF_STATUS (expected crlf-test/draft)"
    fi
else
    t_fail "CRLF frontmatter: cache not written (CRLF normalize path likely broken)"
fi

# ----------------------------------------------------------------------
# Test 6d: JSON Schema sidecar at docs/infrastructure/todo-metadata.schema.json
# is itself a valid Draft 2020-12 schema AND the authoritative example
# in the spec doc validates against it. Mirrors the §1 acceptance gate
# from todo/00-infrastructure/TODO-06-todo-metadata-layer.md.
# ----------------------------------------------------------------------
SCHEMA_SIDECAR="$REPO_ROOT/docs/infrastructure/todo-metadata.schema.json"
python3 - "$SCHEMA_SIDECAR" <<'PY' 2>"$TMP_DIR/schema-sidecar.log"
import json, sys
try:
    import jsonschema
except ImportError:
    print("FAIL: jsonschema module not installed -- run scripts/setup-deps.sh", file=sys.stderr)
    sys.exit(2)
import yaml
schema = json.load(open(sys.argv[1]))
jsonschema.Draft202012Validator.check_schema(schema)
sample = yaml.safe_load("""
$schema: ../../docs/infrastructure/todo-metadata.schema.json
schema_version: 1
id: todo-metadata-layer
domain: 00-infrastructure
status: active
title: TODO Metadata Layer and Derived Graph
depends_on: []
""")
jsonschema.validate(sample, schema)
# Negative cases: each must raise ValidationError.
import copy
def must_reject(mutator, label):
    bad = copy.deepcopy(sample)
    mutator(bad)
    try:
        jsonschema.validate(bad, schema)
    except jsonschema.ValidationError:
        return
    print(f"FAIL: schema accepted invalid sample ({label})", file=sys.stderr)
    sys.exit(1)
must_reject(lambda d: d.update(id="Bad_ID"), "uppercase id")
must_reject(lambda d: d.update(id="todo-"), "trailing dash id")
must_reject(lambda d: d.update(id="1-foo"), "leading digit id")
must_reject(lambda d: d.update(status="in-progress"), "unknown status")
must_reject(lambda d: d.pop("title"), "missing required title")
must_reject(lambda d: d.update(schema_version="1"), "string schema_version")
must_reject(lambda d: d.update(schema_version=2), "future schema_version (Codex H2)")
must_reject(lambda d: d.update(depends_on=["Bad_ID"]), "uppercase depends_on element")
must_reject(lambda d: d.update(created_at="2026-04-23T00:00:00Z"), "hand-authored created_at (Codex M1)")
must_reject(lambda d: d.update(last_active_at="2026-04-23T00:00:00Z"), "hand-authored last_active_at (Codex M1)")
sys.exit(0)
PY
RC=$?
if [ "$RC" = "0" ]; then
    t_pass "schema sidecar valid + authoritative example validates + 10 negatives reject"
else
    t_fail "schema sidecar regression (rc=$RC, log=$TMP_DIR/schema-sidecar.log)"
    cat "$TMP_DIR/schema-sidecar.log" >&2 2>/dev/null || true
fi

# ----------------------------------------------------------------------
# Test 6e: generator parser ALSO rejects hand-authored cache-only fields
# with the forbidden-field FATAL category (mirrors the schema sidecar's
# not/anyOf/required clause). Codex pass 3 H1: schema and generator
# must enforce the same contract or the system is split-brained.
# ----------------------------------------------------------------------
FORBID_ROOT="$TMP_DIR/forbid-tree"
mkdir -p "$FORBID_ROOT/01-test"
cat > "$FORBID_ROOT/01-test/TODO-01-forbid-created.md" <<'EOF'
---
schema_version: 1
id: forbid-created-fixture
domain: 01-test
status: draft
title: Hand-authored created_at must be FATAL
created_at: "2026-04-23T00:00:00Z"
---

# Body
EOF
RC=0
python3 "$BUILD_PY" --quiet --root "$FORBID_ROOT" --output "$TMP_DIR/cache-forbid.json" --repo-root "$REPO_ROOT" \
    >"$TMP_DIR/forbid.log" 2>&1 || RC=$?
if [ "$RC" = "1" ] && grep -q "forbidden-field.*created_at" "$TMP_DIR/forbid.log" && [ ! -e "$TMP_DIR/cache-forbid.json" ]; then
    t_pass "generator rejects hand-authored created_at as forbidden-field FATAL"
else
    t_fail "generator forbidden-field check broken (rc=$RC, log=$(cat $TMP_DIR/forbid.log))"
fi

# ----------------------------------------------------------------------
# Test 6f: recursive YAML alias (`owners: &a [*a]`) MUST be rejected as
# malformed-yaml without a Python traceback. PyYAML safe_load will happily
# build the self-referential list; the json.dumps in main() then crashes
# with an uncaught circular-reference. The generator now walks the parsed
# tree and emits a categorized fatal error instead. Codex pass 4 H2.
# ----------------------------------------------------------------------
CYCLE_ROOT="$TMP_DIR/cycle-tree"
mkdir -p "$CYCLE_ROOT/01-test"
printf -- '---\nschema_version: 1\nid: cycle-fixture\ndomain: 01-test\nstatus: draft\ntitle: Recursive alias\nowners: &a [*a]\n---\n\n# Body\n' \
    > "$CYCLE_ROOT/01-test/TODO-01-cycle.md"
RC=0
python3 "$BUILD_PY" --quiet --root "$CYCLE_ROOT" --output "$TMP_DIR/cache-cycle.json" --repo-root "$REPO_ROOT" \
    >"$TMP_DIR/cycle.log" 2>&1 || RC=$?
if [ "$RC" = "1" ] && grep -q "malformed-yaml.*circular reference" "$TMP_DIR/cycle.log" && [ ! -e "$TMP_DIR/cache-cycle.json" ]; then
    t_pass "generator rejects recursive YAML alias as malformed-yaml (no traceback)"
else
    t_fail "generator alias-cycle check broken (rc=$RC, log=$(cat $TMP_DIR/cycle.log))"
fi

# ----------------------------------------------------------------------
# Test 6g: optional fields with WRONG types (owners: scalar, depends_on:
# scalar, file_patterns: list with empty string) MUST be rejected as
# invalid-field FATAL so the generator and the schema sidecar enforce
# identical contracts. Codex pass 4 H1.
# ----------------------------------------------------------------------
OPT_ROOT="$TMP_DIR/opt-tree"
mkdir -p "$OPT_ROOT/01-test"
printf -- '---\nschema_version: 1\nid: bad-optional-fixture\ndomain: 01-test\nstatus: draft\ntitle: Bad optional types\nowners: 7\ndepends_on: not-a-list\nfile_patterns: [""]\n---\n\n# Body\n' \
    > "$OPT_ROOT/01-test/TODO-01-opt.md"
RC=0
python3 "$BUILD_PY" --quiet --root "$OPT_ROOT" --output "$TMP_DIR/cache-opt.json" --repo-root "$REPO_ROOT" \
    >"$TMP_DIR/opt.log" 2>&1 || RC=$?
if [ "$RC" = "1" ] \
    && grep -q "invalid-field.*owners" "$TMP_DIR/opt.log" \
    && grep -q "invalid-field.*depends_on" "$TMP_DIR/opt.log" \
    && grep -q "invalid-field.*file_patterns" "$TMP_DIR/opt.log" \
    && [ ! -e "$TMP_DIR/cache-opt.json" ]; then
    t_pass "generator rejects bad optional types (owners scalar, depends_on scalar, empty glob)"
else
    t_fail "generator invalid-field check broken (rc=$RC, log=$(cat $TMP_DIR/opt.log))"
fi

# ----------------------------------------------------------------------
# Test 6h: clean optional fields (proper list-of-strings) MUST be accepted
# so the new invalid-field path doesn't over-reject valid frontmatter.
# ----------------------------------------------------------------------
CLEAN_ROOT="$TMP_DIR/clean-tree"
mkdir -p "$CLEAN_ROOT/01-test"
printf -- '---\nschema_version: 1\nid: good-optional\ndomain: 01-test\nstatus: draft\ntitle: Good optional types\nowners: ["alice", "bob"]\ndepends_on: ["foo-bar", "baz-qux"]\nsuperseded_by: replacement-id\nfile_patterns: ["src/foo/**", "include/foo.h"]\n---\n\n# Body\n' \
    > "$CLEAN_ROOT/01-test/TODO-01-clean.md"
python3 "$BUILD_PY" --quiet --root "$CLEAN_ROOT" --output "$TMP_DIR/cache-clean.json" --repo-root "$REPO_ROOT" \
    >"$TMP_DIR/clean.log" 2>&1
if [ -f "$TMP_DIR/cache-clean.json" ]; then
    t_pass "generator accepts well-formed optional fields (no invalid-field over-reject)"
else
    t_fail "generator over-rejected clean optional fields (log=$(cat $TMP_DIR/clean.log))"
fi

# ----------------------------------------------------------------------
# Test 6i: duplicate YAML mapping keys MUST fail closed as malformed-yaml.
# PyYAML's default safe_load uses last-key-wins; we install a custom loader
# that raises on the first duplicate so author typos / merge artifacts that
# would otherwise silently change metadata fail closed instead. Codex pass 5 H2.
# ----------------------------------------------------------------------
DUP_ROOT="$TMP_DIR/dup-tree"
mkdir -p "$DUP_ROOT/01-test"
printf -- '---\nschema_version: 1\nid: dup-key-fixture\nid: silently-overrides\ndomain: 01-test\nstatus: draft\ntitle: Duplicate id key\n---\n\n# Body\n' \
    > "$DUP_ROOT/01-test/TODO-01-dup.md"
RC=0
python3 "$BUILD_PY" --quiet --root "$DUP_ROOT" --output "$TMP_DIR/cache-dup.json" --repo-root "$REPO_ROOT" \
    >"$TMP_DIR/dup.log" 2>&1 || RC=$?
if [ "$RC" = "1" ] && grep -q "malformed-yaml.*duplicate key 'id'" "$TMP_DIR/dup.log" && [ ! -e "$TMP_DIR/cache-dup.json" ]; then
    t_pass "generator rejects duplicate YAML mapping keys as malformed-yaml"
else
    t_fail "duplicate-key check broken (rc=$RC, log=$(cat $TMP_DIR/dup.log))"
fi

# ----------------------------------------------------------------------
# Test 6j: schema/generator parity for the constraints that previously
# slipped past validate_frontmatter (Codex pass 5 H1): schema_version < 1,
# empty title, $schema non-string, duplicate depends_on entries. All four
# must fire as categorized FATAL errors so the generator and the schema
# sidecar agree on what is valid.
# ----------------------------------------------------------------------
PARITY_ROOT="$TMP_DIR/parity-tree"
mkdir -p "$PARITY_ROOT/01-test"

write_fixture() {
    local name="$1" body="$2"
    printf -- '%s' "$body" > "$PARITY_ROOT/01-test/TODO-01-${name}.md"
}
parity_check() {
    local name="$1" expect_pattern="$2"
    local out="$TMP_DIR/parity-${name}.log"
    local cache="$TMP_DIR/cache-parity-${name}.json"
    local rc=0
    python3 "$BUILD_PY" --quiet --root "$PARITY_ROOT" --output "$cache" --repo-root "$REPO_ROOT" \
        >"$out" 2>&1 || rc=$?
    if [ "$rc" = "1" ] && grep -q "$expect_pattern" "$out" && [ ! -e "$cache" ]; then
        t_pass "generator parity: $name fires '$expect_pattern'"
    else
        t_fail "generator parity broken for $name (rc=$rc, log=$(cat $out))"
    fi
    rm -f "$PARITY_ROOT/01-test/TODO-01-${name}.md"
}

write_fixture "svzero" '---
schema_version: 0
id: sv-zero-fixture
domain: 01-test
status: draft
title: SV zero
---

# Body
'
parity_check "svzero" "schema-version.*must be >= 1"

write_fixture "emptytitle" '---
schema_version: 1
id: empty-title-fixture
domain: 01-test
status: draft
title: ""
---

# Body
'
parity_check "emptytitle" "invalid-field.*title must be a non-empty string"

write_fixture "schemaint" '---
$schema: 7
schema_version: 1
id: bad-schemaref-fixture
domain: 01-test
status: draft
title: Bad schema ref
---

# Body
'
parity_check "schemaint" 'invalid-field.*\$schema must be a string'

write_fixture "dupdeps" '---
schema_version: 1
id: dup-deps-fixture
domain: 01-test
status: draft
title: Dup deps
depends_on: ["foo-bar", "foo-bar"]
---

# Body
'
parity_check "dupdeps" "invalid-field.*depends_on must be uniqueItems"

# ----------------------------------------------------------------------
# Test 7: performance budget (under 2s wall-clock per the generator spec).
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
