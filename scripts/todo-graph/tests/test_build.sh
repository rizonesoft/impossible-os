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
# Test 6k: structured depends_on shape (Codex pass 6 H1 fix). Each entry
# must be {target, sections[]} dict; bare cells default to target=self;
# space-separated D<dom> T<num> joins to D<dom>T<num>; sections after a
# cross-file target ride on it (the bare §9 in `TODO-08 §8, §9` belongs
# to TODO-08, not the source file).
# ----------------------------------------------------------------------
python3 - "$CACHE_RUN_A" <<'PY' 2>"$TMP_DIR/dep-shape.log"
import json, sys
nodes = json.load(open(sys.argv[1]))
shapes_seen = 0
self_shapes = 0
target_shapes = 0
for n in nodes:
    for s in n.get("sections", []):
        for d in s.get("depends_on", []):
            shapes_seen += 1
            if not isinstance(d, dict):
                print(f"FAIL: dep is not a dict in {n['file_path']}: {d!r}", file=sys.stderr)
                sys.exit(1)
            if "target" not in d or "sections" not in d:
                print(f"FAIL: dep missing target/sections in {n['file_path']}: {d!r}", file=sys.stderr)
                sys.exit(1)
            if not isinstance(d["sections"], list) or not all(isinstance(x, int) for x in d["sections"]):
                print(f"FAIL: sections not int list in {n['file_path']}: {d!r}", file=sys.stderr)
                sys.exit(1)
            if d["target"] == "self":
                self_shapes += 1
            else:
                target_shapes += 1
print(f"OK: {shapes_seen} dep groups parsed (self={self_shapes}, cross={target_shapes})")
sys.exit(0)
PY
if [ $? -eq 0 ]; then
    t_pass "structured depends_on shape: every entry is {target, sections[int]}"
else
    t_fail "structured depends_on shape broken (see $TMP_DIR/dep-shape.log)"
    cat "$TMP_DIR/dep-shape.log" >&2 2>/dev/null || true
fi

# ----------------------------------------------------------------------
# Test 8: validate.py (§3) -- 7 checks against synthetic fixtures.
# Each sub-test seeds a mini repo with a deliberate violation, runs
# build.py + validate.py, and asserts the expected check fires.
# ----------------------------------------------------------------------
VALIDATE_PY="$REPO_ROOT/scripts/todo-graph/validate.py"

# Helper: prepare a fresh fixture tree (wipes + recreates).
v_run() {
    local tree="$1"
    rm -rf "$tree"
    mkdir -p "$tree"
}

# Sub-test 8a: clean fixture passes all 7 checks.
V_TREE="$TMP_DIR/v-clean"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-clean.md" <<'EOF'
# TODO-01 -- Clean fixture

## Inputs

| Path | Purpose |
| ---- | ------- |
| `src/foo` | example |

## Outcome

Clean fixture for the validator regression suite.

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |
| 💎 | 2 | §2 | Second | §1 | [ ] |

## 1. First Section

Body for §1.

- [x] Item one
- [x] Commit

## 2. Second Section

Body for §2.

- [ ] Item two
- [ ] Commit
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" --quiet 2>&1)
V_RC=$?
if [ "$V_RC" = "0" ]; then
    t_pass "validate: clean fixture passes all 7 checks (exit 0)"
else
    t_fail "validate: clean fixture should pass; got rc=$V_RC out=$V_OUT"
fi

# Sub-test 8b: dangling-section fires when an IO row Depends-On §N
# does not match any heading in the target file.
V_TREE="$TMP_DIR/v-dangling"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-dangling.md" <<'EOF'
# TODO-01 -- Dangling section

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |
| 💎 | 2 | §2 | Second | §99 | [ ] |

## 1. First

- [x] Commit

## 2. Second

- [ ] Commit
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_RC=$?
if [ "$V_RC" = "1" ] && echo "$V_OUT" | grep -q "dangling-section.*§99"; then
    t_pass "validate: dangling-section fires on §99 reference to nonexistent heading"
else
    t_fail "validate: dangling-section check broken (rc=$V_RC, out=$V_OUT)"
fi

# Sub-test 8c: orphan-io-row fires when an IO row references a heading
# that doesn't exist in the body.
V_TREE="$TMP_DIR/v-orphan"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-orphan.md" <<'EOF'
# TODO-01 -- Orphan row

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |
| 💎 | 2 | §5 | Fifth (no body) | -- | [ ] |

## 1. First

- [x] Commit
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_RC=$?
if [ "$V_RC" = "1" ] && echo "$V_OUT" | grep -q "orphan-io-row.*§5"; then
    t_pass "validate: orphan-io-row fires when §5 row has no matching ## 5 heading"
else
    t_fail "validate: orphan-io-row check broken (rc=$V_RC, out=$V_OUT)"
fi

# Sub-test 8d: bat-alignment fires when a Test runner names a bat that
# does not exist on disk.
V_TREE="$TMP_DIR/v-bat"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-bat.md" <<'EOF'
# TODO-01 -- Bad bat

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [x] Commit

> **Test runner:** `scripts\debug\kernel\run-nonexistent-tests.bat` | 0/0 PASS
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_RC=$?
if [ "$V_RC" = "1" ] && echo "$V_OUT" | grep -q "bat-alignment.*run-nonexistent-tests.bat"; then
    t_pass "validate: bat-alignment fires on nonexistent run-*-tests.bat"
else
    t_fail "validate: bat-alignment check broken (rc=$V_RC, out=$V_OUT)"
fi

# Sub-test 8e: --warnings-only downgrades stale-xref FAILs to WARNs so a
# pre-§5 migration tree exits 0 instead of 1.
V_TREE="$TMP_DIR/v-warn"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-stalexref.md" <<'EOF'
# TODO-01 -- Stale XREF

## Inputs

- -> XREF: T999 §1 -- nonexistent target

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [x] Commit
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT_FAIL=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_RC_FAIL=$?
V_OUT_WARN=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" --warnings-only --quiet 2>&1)
V_RC_WARN=$?
if [ "$V_RC_FAIL" = "1" ] && [ "$V_RC_WARN" = "0" ] \
    && echo "$V_OUT_FAIL" | grep -q "stale-xref.*T999" \
    && echo "$V_OUT_WARN" | grep -q "warning"; then
    t_pass "validate: --warnings-only downgrades stale-xref to WARN (exit 0 vs 1)"
else
    t_fail "validate: --warnings-only behaviour broken (rc_fail=$V_RC_FAIL rc_warn=$V_RC_WARN)"
fi

# Sub-test 8f: --fix-line-numbers (dry-run by default) detects drifted
# (item: "NAME" at line N) parentheticals AND refuses to write without
# --write. With --write, the file is rewritten in place.
V_TREE="$TMP_DIR/v-fix"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-target.md" <<'EOF'
# TODO-01 -- Target

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [ ] Add unique fix-target item
- [ ] Commit
EOF
cat > "$V_TREE/todo/01-test/TODO-02-source.md" <<'EOF'
# TODO-02 -- Source carrying drifted stamp

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [x] Commit

> **Verified:** 2026-04-23 | commit `abc1234` | 1/1 items | build OK
> **Accepted:** [L] some finding -> XREF: TODO-01 (item: "Add unique fix-target item" at line 999)
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT_DRY=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" --fix-line-numbers --quiet 2>&1)
DRY_DIFF=$(grep -c "at line 999" "$V_TREE/todo/01-test/TODO-02-source.md" || true)
python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" --fix-line-numbers --write --quiet >/dev/null 2>&1
WRITE_LINE=$(grep -oP '"Add unique fix-target item" at line \K\d+' "$V_TREE/todo/01-test/TODO-02-source.md")
if [ "$DRY_DIFF" = "1" ] && [ -n "$WRITE_LINE" ] && [ "$WRITE_LINE" != "999" ]; then
    t_pass "validate: --fix-line-numbers dry-run does not mutate; --write rewrites to line=$WRITE_LINE"
else
    t_fail "validate: --fix-line-numbers broken (dry_kept=$DRY_DIFF write_line=$WRITE_LINE)"
fi

# Sub-test 8g: --fix-line-numbers refuses on ambiguous (non-unique) item
# name; Codex pass 6 M1 contract.
V_TREE="$TMP_DIR/v-fix-ambig"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-ambig-target.md" <<'EOF'
# TODO-01 -- Ambig target (item appears twice)

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [ ] Ambiguous item
- [ ] Ambiguous item
- [ ] Commit
EOF
cat > "$V_TREE/todo/01-test/TODO-02-ambig-source.md" <<'EOF'
# TODO-02 -- Source

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [x] Commit

> **Verified:** 2026-04-23 | commit `abc1234` | 1/1 items | build OK
> **Accepted:** [L] thing -> XREF: TODO-01 (item: "Ambiguous item" at line 999)
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" --fix-line-numbers --write 2>&1)
STILL_999=$(grep -c "at line 999" "$V_TREE/todo/01-test/TODO-02-ambig-source.md" || true)
if echo "$V_OUT" | grep -q "ambiguous item_name" && [ "$STILL_999" = "1" ]; then
    t_pass "validate: --fix-line-numbers refuses ambiguous match; original line preserved"
else
    t_fail "validate: ambiguity guard broken (out=$V_OUT still=$STILL_999)"
fi

# Sub-test 8h: broken cross-file IO Depends-On target fires stale-xref.
# Codex pass 7 H1: check_dangling_section_ref SKIPS unresolved targets, so
# `Depends On: TODO-99 §1` (TODO-99 does not exist) would slip through
# without check_stale_xref walking sections[].depends_on groups too.
V_TREE="$TMP_DIR/v-broken-target"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-broken.md" <<'EOF'
# TODO-01 -- Broken cross-file IO dep

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | TODO-99 §1 | [x] |

## 1. First

- [x] Commit
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_RC=$?
if [ "$V_RC" = "1" ] && echo "$V_OUT" | grep -q "stale-xref.*TODO-99"; then
    t_pass "validate: broken cross-file IO Depends-On target fires stale-xref"
else
    t_fail "validate: broken-target stale-xref check broken (rc=$V_RC, out=$V_OUT)"
fi

# Sub-test 8i: --fix-line-numbers --write exits non-zero when ambiguity
# forced a refused rewrite (Codex pass 7 H2).
V_TREE="$TMP_DIR/v-ambig-exit"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-atarget.md" <<'EOF'
# TODO-01 -- Target (ambig)

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [ ] Duplicate item
- [ ] Duplicate item
- [ ] Commit
EOF
cat > "$V_TREE/todo/01-test/TODO-02-asource.md" <<'EOF'
# TODO-02 -- Source

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [x] Commit

> **Verified:** 2026-04-23 | commit `abc1234` | 1/1 items | build OK
> **Accepted:** [L] thing -> XREF: TODO-01 (item: "Duplicate item" at line 999)
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" --fix-line-numbers --write --quiet >/dev/null 2>&1
V_RC=$?
if [ "$V_RC" = "1" ]; then
    t_pass "validate: --fix-line-numbers --write exits 1 on ambiguity (refused rewrite)"
else
    t_fail "validate: ambiguity exit code broken (expected rc=1 got rc=$V_RC)"
fi

# Sub-test 8j: $schema path that escapes the repo root (e.g.
# `../../../etc/passwd`) is rejected even when the host file exists.
# Codex pass 7 M1: validation must not be host-dependent.
V_TREE="$TMP_DIR/v-schema-escape"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-schemaescape.md" <<'EOF'
---
$schema: ../../../../../../../../../../etc/passwd
schema_version: 1
id: schema-escape
domain: 01-test
status: draft
title: Schema escape
---

# Body

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [x] Commit
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_RC=$?
if [ "$V_RC" = "1" ] && echo "$V_OUT" | grep -q "schema-reachability.*OUTSIDE the repo"; then
    t_pass "validate: \$schema path escaping repo root is rejected"
else
    t_fail "validate: \$schema escape-path check broken (rc=$V_RC, out=$V_OUT)"
fi

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
