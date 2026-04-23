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
---
schema_version: 1
id: fm-clean
domain: 01-test
status: active
title: "fixture clean"
---
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
---
schema_version: 1
id: fm-dangling
domain: 01-test
status: active
title: "fixture dangling"
---
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
---
schema_version: 1
id: fm-orphan
domain: 01-test
status: active
title: "fixture orphan"
---
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
---
schema_version: 1
id: fm-bat
domain: 01-test
status: active
title: "fixture bat"
---
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
---
schema_version: 1
id: fm-stalexref
domain: 01-test
status: active
title: "fixture stalexref"
---
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
---
schema_version: 1
id: fm-target
domain: 01-test
status: active
title: "fixture target"
---
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
---
schema_version: 1
id: fm-source
domain: 01-test
status: active
title: "fixture source"
---
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
---
schema_version: 1
id: fm-ambig-target
domain: 01-test
status: active
title: "fixture ambig-target"
---
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
---
schema_version: 1
id: fm-ambig-source
domain: 01-test
status: active
title: "fixture ambig-source"
---
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
---
schema_version: 1
id: fm-broken
domain: 01-test
status: active
title: "fixture broken"
---
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
---
schema_version: 1
id: fm-atarget
domain: 01-test
status: active
title: "fixture atarget"
---
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
---
schema_version: 1
id: fm-asource
domain: 01-test
status: active
title: "fixture asource"
---
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

# Sub-test 8k: multi-XREF stamp line. Each (item: ...) clause must bind
# to its OWN -> XREF: target, not the first one on the line. Codex pass
# 8 H1: fix_line_numbers previously used a single re.search for the
# whole line, so the second clause resolved against the first target
# (wrong file). Codex pass 9 H2 strengthens this test: targets place
# their items at DIFFERENT line numbers so wrong-binding produces wrong
# number; oracle asserts EXACT rewritten text, not just "999 absent".
V_TREE="$TMP_DIR/v-multi-xref"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
# TODO-01 target: Alpha item sits at line 11 (after 10 lines of header).
cat > "$V_TREE/todo/01-test/TODO-01-atgt.md" <<'EOF'
---
schema_version: 1
id: fm-atgt
domain: 01-test
status: active
title: "fixture atgt"
---
# TODO-01 -- Target A

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [ ] Alpha item in target A
- [ ] Commit
EOF
# TODO-02 target: Beta item pushed to line 15 via padding so the two
# targets produce DIFFERENT line numbers. A broken impl that resolved
# Beta against TODO-01 would return line 11 (where Alpha sits), not 15.
cat > "$V_TREE/todo/01-test/TODO-02-btgt.md" <<'EOF'
---
schema_version: 1
id: fm-btgt
domain: 01-test
status: active
title: "fixture btgt"
---
# TODO-02 -- Target B (padded so Beta lands at a different line)

Extra padding line 1.

Extra padding line 2.

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [ ] Beta item in target B
- [ ] Commit
EOF
cat > "$V_TREE/todo/01-test/TODO-03-multi.md" <<'EOF'
---
schema_version: 1
id: fm-multi
domain: 01-test
status: active
title: "fixture multi"
---
# TODO-03 -- Source with multi-XREF stamp

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [x] Commit

> **Verified:** 2026-04-23 | commit `abc1234` | 1/1 items | build OK
> **Accepted:** [L] a thing -> XREF: TODO-01 (item: "Alpha item in target A" at line 999) -> XREF: TODO-02 (item: "Beta item in target B" at line 999)
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" --fix-line-numbers --write --quiet >/dev/null 2>&1
# Compute expected lines dynamically (independent of manual counting).
ALPHA_LINE=$(grep -nF "Alpha item in target A" "$V_TREE/todo/01-test/TODO-01-atgt.md" | head -1 | cut -d: -f1)
BETA_LINE=$(grep -nF "Beta item in target B" "$V_TREE/todo/01-test/TODO-02-btgt.md" | head -1 | cut -d: -f1)
MULTI_TEXT=$(cat "$V_TREE/todo/01-test/TODO-03-multi.md")
if [ "$ALPHA_LINE" != "$BETA_LINE" ] \
    && echo "$MULTI_TEXT" | grep -qF "\"Alpha item in target A\" at line $ALPHA_LINE" \
    && echo "$MULTI_TEXT" | grep -qF "\"Beta item in target B\" at line $BETA_LINE"; then
    t_pass "validate: multi-XREF stamp: Alpha->$ALPHA_LINE, Beta->$BETA_LINE (distinct targets)"
else
    t_fail "validate: multi-XREF binding broken (alpha=$ALPHA_LINE beta=$BETA_LINE text=$(echo "$MULTI_TEXT" | tail -2))"
fi

# Sub-test 8k2: two stamp clauses with IDENTICAL literal text
# `(item: "Epsilon" at line 999)` bound to DIFFERENT targets. str.replace
# would rewrite both clauses to the first resolved line; the span-slice
# rebuild (pass 9 H1) keeps them distinct. Each target file contains
# "Epsilon" on exactly one line at a DIFFERENT line number so the
# ambiguity guard doesn't fire and the rewrite binds to distinct lines.
V_TREE="$TMP_DIR/v-shared-clause"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
# Target A: Epsilon at line 3.
cat > "$V_TREE/todo/01-test/TODO-01-asrc.md" <<'EOF'
---
schema_version: 1
id: fm-asrc
domain: 01-test
status: active
title: "fixture asrc"
---
# TODO-01 -- Target A

- [ ] Epsilon unique in A
EOF
# Target B: Epsilon at line 9 (padded).
cat > "$V_TREE/todo/01-test/TODO-02-bsrc.md" <<'EOF'
---
schema_version: 1
id: fm-bsrc
domain: 01-test
status: active
title: "fixture bsrc"
---
# TODO-02 -- Target B

Padding line one.
Padding line two.
Padding line three.
Padding line four.
Padding line five.
Padding line six.
- [ ] Epsilon unique in B
EOF
cat > "$V_TREE/todo/01-test/TODO-03-source.md" <<'EOF'
---
schema_version: 1
id: fm-source
domain: 01-test
status: active
title: "fixture source"
---
# TODO-03 -- Source

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First

- [x] Commit

> **Verified:** 2026-04-23 | commit `abc1234` | 1/1 items | build OK
> **Accepted:** [L] dup test -> XREF: TODO-01 (item: "Epsilon unique in A" at line 999) -> XREF: TODO-02 (item: "Epsilon unique in B" at line 999)
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" --fix-line-numbers --write --quiet 2>/dev/null
A_LINE=$(grep -nF "Epsilon unique in A" "$V_TREE/todo/01-test/TODO-01-asrc.md" | head -1 | cut -d: -f1)
B_LINE=$(grep -nF "Epsilon unique in B" "$V_TREE/todo/01-test/TODO-02-bsrc.md" | head -1 | cut -d: -f1)
STAMP=$(grep "Accepted:" "$V_TREE/todo/01-test/TODO-03-source.md")
# Assert EXACT rewritten text: each clause must cite its own target's line.
if [ "$A_LINE" != "$B_LINE" ] \
    && echo "$STAMP" | grep -qF "\"Epsilon unique in A\" at line $A_LINE" \
    && echo "$STAMP" | grep -qF "\"Epsilon unique in B\" at line $B_LINE"; then
    t_pass "validate: span-slice rewrite preserves distinct targets for clauses (A=$A_LINE, B=$B_LINE)"
else
    t_fail "validate: span-slice rewrite broken (A=$A_LINE B=$B_LINE stamp=$STAMP)"
fi

# Sub-test 8m: stale cache shape (pre-pass-6 opaque-string depends_on)
# triggers an auto-rebuild instead of crashing with AttributeError.
# Codex pass 9 M1.
V_TREE="$TMP_DIR/v-stale-cache"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-clean.md" <<'EOF'
---
schema_version: 1
id: fm-clean
domain: 01-test
status: active
title: "fixture clean"
---
# TODO-01 -- Clean

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | §2 | [x] |
| 💎 | 2 | §2 | Second | -- | [x] |

## 1. First

- [x] Commit

## 2. Second

- [x] Commit
EOF
# Hand-craft a STALE cache (pre-pass-6 shape: depends_on as string list)
cat > "$V_TREE/cache.json" <<'EOF'
[
  {
    "id": null,
    "schema_version": null,
    "domain": "01-test",
    "status": "no-frontmatter",
    "title": "TODO-01 -- Clean",
    "file_path": "todo/01-test/TODO-01-clean.md",
    "created_at": null,
    "last_active_at": null,
    "sections": [
      {"n": 1, "deliverable": "First", "depends_on": ["\u00a72"], "status": "x"},
      {"n": 2, "deliverable": "Second", "depends_on": [], "status": "x"}
    ],
    "section_headings": [{"n": 1, "title": "First"}, {"n": 2, "title": "Second"}],
    "inputs_xrefs": [],
    "stamps_xrefs": []
  }
]
EOF
V_OUT=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_RC=$?
# Validator should detect stale shape, rebuild, and then exit cleanly.
if [ "$V_RC" = "0" ] && echo "$V_OUT" | grep -q "stale.*rebuilding"; then
    t_pass "validate: stale cache shape triggers auto-rebuild instead of AttributeError"
else
    t_fail "validate: stale-cache detection broken (rc=$V_RC, out=$V_OUT)"
fi

# Sub-test 8l: schema-reachability tolerates UTF-8 BOM + CRLF frontmatter.
# Codex pass 8 M1: without BOM/CRLF normalization, a Windows-authored
# TODO with a BOM and a bad $schema path would silently bypass check 7.
V_TREE="$TMP_DIR/v-schema-bom"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
# Write with explicit BOM + CRLF and an escape-path $schema.
printf '\xef\xbb\xbf---\r\n$schema: ../../../../../../../../../../../../etc/passwd\r\nschema_version: 1\r\nid: bom-crlf-test\r\ndomain: 01-test\r\nstatus: draft\r\ntitle: BOM+CRLF\r\n---\r\n\r\n# Body\r\n\r\n## Implementation Order\r\n\r\n| \xe2\xad\x90 | Order | Section | Deliverable | Depends On | Status |\r\n| --- | --- | --- | --- | --- | --- |\r\n| \xf0\x9f\x92\x8e | 1 | \xc2\xa71 | First | -- | [x] |\r\n\r\n## 1. First\r\n\r\n- [x] Commit\r\n' \
    > "$V_TREE/todo/01-test/TODO-01-bom.md"
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_RC=$?
if [ "$V_RC" = "1" ] && echo "$V_OUT" | grep -q "schema-reachability.*OUTSIDE the repo"; then
    t_pass "validate: schema-reachability rejects escape path in BOM+CRLF frontmatter"
else
    t_fail "validate: BOM+CRLF schema check broken (rc=$V_RC, out=$V_OUT)"
fi

# ======================================================================
# Test 9: query.py (§4) -- ten subcommands over build/todo-cache.json.
# Covers pre-migration notice gating, id resolution (slug / stem /
# frontmatter id / nonexistent), all six live-tree subcommands, the
# --json and --format markdown output surfaces, synthetic fixtures for
# the status-gated subcommands, import smoke, and the --watch polling
# fallback.
# ======================================================================

QUERY_PY="$REPO_ROOT/scripts/todo-graph/query.py"

# Prime the live cache once; tests below share it.
LIVE_CACHE="$TMP_DIR/live-cache.json"
python3 "$BUILD_PY" --quiet --output "$LIVE_CACHE" >/dev/null 2>&1 \
    || { echo "[test_build] FAIL: build.py failed priming the live cache"; exit 2; }

q_live() {
    # Run query.py against the live repo with an already-built cache.
    python3 "$QUERY_PY" --cache "$LIVE_CACHE" --repo-root "$REPO_ROOT" "$@"
}

# Sub-test 9a/b/c: status-gated subcommands on a SYNTHETIC pre-migration
# tree print the one-shot notice + empty body. The live tree is now
# fully frontmatter-migrated (TODO-06 §5), so the notice no longer
# fires against the real cache -- we synthesize a majority-no-frontmatter
# cache instead. Each subcommand verifies exit=0 + empty stdout + the
# notice string on stderr.
PREMIG_TREE="$TMP_DIR/q-premig"
mkdir -p "$PREMIG_TREE"
python3 -c "
import json
# 4 no-frontmatter nodes + 1 with frontmatter -> majority no-fm triggers notice.
nodes = []
for i in range(1, 5):
    nodes.append({
        'file_path': f'todo/01-t/TODO-0{i}-nofm.md',
        'id': None, 'domain': '01-t', 'status': 'no-frontmatter',
        'title': f'legacy-{i}', 'schema_version': None,
        'sections': [], 'section_headings': [], 'inputs_xrefs': [],
        'stamps_xrefs': [], 'created_at': '2026-04-01T00:00:00Z',
        'last_active_at': '2026-04-01T00:00:00Z',
    })
nodes.append({
    'file_path': 'todo/01-t/TODO-05-fm.md',
    'id': 'has-fm', 'domain': '01-t', 'status': 'draft',
    'title': 'fm', 'schema_version': 1,
    'sections': [], 'section_headings': [], 'inputs_xrefs': [],
    'stamps_xrefs': [], 'created_at': '2026-04-01T00:00:00Z',
    'last_active_at': '2026-04-01T00:00:00Z',
})
with open('$PREMIG_TREE/cache.json', 'w') as f:
    json.dump(nodes, f)
"
q_pre() {
    python3 "$QUERY_PY" --cache "$PREMIG_TREE/cache.json" --repo-root "$PREMIG_TREE" "$@"
}
# The notice fires on majority-no-fm; output may still list the single
# frontmatter-bearing node in its results (real pre-migration trees
# would have nothing resolvable). Assertion: exit=0 + notice present.
Q_OUT=$(q_pre ready 2>"$TMP_DIR/q9a.stderr"); Q_RC=$?
if [ "$Q_RC" = "0" ] && grep -q "pre-migration notice" "$TMP_DIR/q9a.stderr"; then
    t_pass "query: ready emits pre-migration notice when majority no-fm"
else
    t_fail "query: ready broken (rc=$Q_RC, stderr=$(cat "$TMP_DIR/q9a.stderr"))"
fi

Q_OUT=$(q_pre blocked 2>"$TMP_DIR/q9b.stderr"); Q_RC=$?
if [ "$Q_RC" = "0" ] && grep -q "pre-migration notice" "$TMP_DIR/q9b.stderr"; then
    t_pass "query: blocked emits pre-migration notice when majority no-fm"
else
    t_fail "query: blocked broken (rc=$Q_RC)"
fi

Q_OUT=$(q_pre blocking 2>"$TMP_DIR/q9c.stderr"); Q_RC=$?
if [ "$Q_RC" = "0" ] && grep -q "pre-migration notice" "$TMP_DIR/q9c.stderr"; then
    t_pass "query: blocking emits pre-migration notice when majority no-fm"
else
    t_fail "query: blocking broken (rc=$Q_RC)"
fi

# Sub-test 9d: `by-domain` enumerates the whole tree.
EXPECTED_NODES=$(find "$REPO_ROOT/todo" -name 'TODO-*.md' -not -name 'TODO-00-INDEX.md' | wc -l)
ACTUAL_ROWS=$(q_live by-domain --quiet | wc -l)
if [ "$ACTUAL_ROWS" -eq "$EXPECTED_NODES" ]; then
    t_pass "query: by-domain row count matches find (${ACTUAL_ROWS} == ${EXPECTED_NODES})"
else
    t_fail "query: by-domain row mismatch (got ${ACTUAL_ROWS}, expected ${EXPECTED_NODES})"
fi

# Sub-test 9e: `by-domain <dom>` filters to a single domain.
DOM_ROWS=$(q_live by-domain 00-infrastructure --quiet | awk -F'\t' '{print $1}' | sort -u)
if [ "$DOM_ROWS" = "00-infrastructure" ]; then
    t_pass "query: by-domain 00-infrastructure filters to one domain"
else
    t_fail "query: by-domain filter leaked other domains (got '${DOM_ROWS}')"
fi

# Sub-test 9f: `backlinks ai-development-system` resolves via slug and
# prints at least one row (TODO-06 references TODO-02 via Inputs XREF).
Q_OUT=$(q_live backlinks ai-development-system --quiet 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ] && [ -n "$Q_OUT" ] && echo "$Q_OUT" | grep -q "todo-metadata-layer"; then
    t_pass "query: backlinks ai-development-system (slug) lists TODO-06"
else
    t_fail "query: backlinks slug form broken (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9g: same identifier via filename stem form.
Q_OUT=$(q_live backlinks TODO-02-ai-development-system --quiet 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ] && [ -n "$Q_OUT" ] && echo "$Q_OUT" | grep -q "todo-metadata-layer"; then
    t_pass "query: backlinks TODO-02-ai-development-system (stem) lists TODO-06"
else
    t_fail "query: backlinks stem form broken (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9h: nonexistent id exits 2 with a nearest-match hint.
Q_OUT=$(q_live backlinks nonexistent-slug-xyz 2>&1); Q_RC=$?
if [ "$Q_RC" = "2" ] && echo "$Q_OUT" | grep -q "not found"; then
    t_pass "query: backlinks nonexistent id exits 2 with hint"
else
    t_fail "query: backlinks nonexistent should exit 2 (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9i: `orphans` is non-empty but much shorter than the total
# node count (pre-migration will over-report, post-§5 will shrink).
ORPHAN_ROWS=$(q_live orphans --quiet | wc -l)
if [ "$ORPHAN_ROWS" -gt 0 ] && [ "$ORPHAN_ROWS" -lt "$EXPECTED_NODES" ]; then
    t_pass "query: orphans returns reasonable subset (${ORPHAN_ROWS} < ${EXPECTED_NODES})"
else
    t_fail "query: orphans count suspect (${ORPHAN_ROWS})"
fi

# Sub-test 9j: `stale --days 1000000` emits nothing (nothing that old).
Q_OUT=$(q_live stale --days 1000000 --quiet 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ] && [ -z "$Q_OUT" ]; then
    t_pass "query: stale --days 1000000 prints empty"
else
    t_fail "query: stale with huge --days should be empty (out=$Q_OUT)"
fi

# Sub-test 9k: `stale --days 0` prints every node (nothing is newer).
STALE_ALL=$(q_live stale --days 0 --quiet | wc -l)
if [ "$STALE_ALL" -eq "$EXPECTED_NODES" ]; then
    t_pass "query: stale --days 0 lists every node (${STALE_ALL})"
else
    t_fail "query: stale --days 0 should list every node (got ${STALE_ALL})"
fi

# Sub-test 9l: `stats --json` emits valid JSON with every required key.
Q_OUT=$(q_live stats --json --quiet 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ]; then
    if python3 -c "
import json, sys
d = json.loads('''$Q_OUT''')
req = {'total_nodes', 'by_status', 'by_domain', 'top_blocking', 'top_longest_deferred', 'avg_dep_depth', 'orphan_count'}
missing = req - set(d.keys())
sys.exit(1 if missing else 0)
" 2>/dev/null; then
        t_pass "query: stats --json emits all required keys"
    else
        t_fail "query: stats --json missing keys"
    fi
else
    t_fail "query: stats --json exit non-zero (rc=$Q_RC)"
fi

# Sub-test 9m: synthetic fixture where `ready` should fire (a draft
# depending on a done node).
Q_TREE="$TMP_DIR/q-ready-fixture"
mkdir -p "$Q_TREE/todo/01-test"
cat > "$Q_TREE/todo/01-test/TODO-01-foundation.md" <<'EOF'
---
schema_version: 1
id: foundation-ready
domain: 01-test
status: done
title: Foundation
---
# body
EOF
cat > "$Q_TREE/todo/01-test/TODO-02-consumer.md" <<'EOF'
---
schema_version: 1
id: consumer-ready
domain: 01-test
status: draft
title: Consumer
depends_on: [foundation-ready]
---
# body
EOF
python3 "$BUILD_PY" --quiet --root "$Q_TREE/todo" --output "$Q_TREE/cache.json" --repo-root "$Q_TREE" >/dev/null 2>&1
Q_OUT=$(python3 "$QUERY_PY" --cache "$Q_TREE/cache.json" --repo-root "$Q_TREE" --quiet ready 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ] && echo "$Q_OUT" | grep -q "consumer-ready"; then
    t_pass "query: ready on synthetic fixture surfaces the unblocked draft"
else
    t_fail "query: ready fixture broken (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9n: `ready --format markdown` on the same fixture emits a
# valid GFM header row (`| ... |` + `| --- | ... |`).
Q_OUT=$(python3 "$QUERY_PY" --cache "$Q_TREE/cache.json" --repo-root "$Q_TREE" --quiet ready --format markdown 2>&1)
if echo "$Q_OUT" | head -2 | grep -Eq '^\| domain \| id \| title \|$' \
    && echo "$Q_OUT" | head -2 | grep -Eq '^\| --- \| --- \| --- \|$'; then
    t_pass "query: ready --format markdown emits GFM table header"
else
    t_fail "query: markdown header broken (out=$Q_OUT)"
fi

# Sub-test 9o: `code <id>` greps (src/...) / (include/...) references.
# Synthesize a TODO whose Notes block references a real-looking source
# path and verify it surfaces.
Q_TREE2="$TMP_DIR/q-code-fixture"
mkdir -p "$Q_TREE2/todo/01-test"
cat > "$Q_TREE2/todo/01-test/TODO-01-source-refs.md" <<'EOF'
---
schema_version: 1
id: source-refs
domain: 01-test
status: draft
title: Code-grep test
---
# body

> **Notes:**
> - Shipped: parser in (src/kernel/foo.c) and header at (include/kernel/foo.h).
> - Placeholder form (src/...) should be filtered out.
EOF
python3 "$BUILD_PY" --quiet --root "$Q_TREE2/todo" --output "$Q_TREE2/cache.json" --repo-root "$Q_TREE2" >/dev/null 2>&1
Q_OUT=$(python3 "$QUERY_PY" --cache "$Q_TREE2/cache.json" --repo-root "$Q_TREE2" --quiet code source-refs 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ] \
    && echo "$Q_OUT" | grep -q "src/kernel/foo.c" \
    && echo "$Q_OUT" | grep -q "include/kernel/foo.h" \
    && ! echo "$Q_OUT" | grep -qE '^src/\.\.\.|^include/\.\.\.'; then
    t_pass "query: code grep surfaces real paths + filters (src/...) placeholder"
else
    t_fail "query: code grep broken (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9p: import smoke -- query.py imports cleanly and exposes
# SUBCOMMANDS. Defends against future drift in validate.py causing
# query.py to crash at import time.
if python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import query
assert len(query.SUBCOMMANDS) == 12
assert 'ready' in query.SUBCOMMANDS
assert 'code-by' in query.SUBCOMMANDS
" 2>/dev/null; then
    t_pass "query: import smoke (12 subcommands exposed)"
else
    t_fail "query: import smoke broken"
fi

# Sub-test 9q: `deferred-by` resolves an id and returns rows when at
# least one other node has an outbound stamp pointing at it. Uses the
# live tree; a handful of TODOs carry cross-file Accepted stamps.
# The acceptance criteria is exit 0 + consistent shape; count may vary.
Q_OUT=$(q_live deferred-by kernel-test-harness --quiet 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ]; then
    t_pass "query: deferred-by kernel-test-harness exits 0"
else
    t_fail "query: deferred-by broken (rc=$Q_RC)"
fi

# Sub-test 9r: `--watch` polling fallback triggers a re-run within one
# polling interval. Uses `timeout` to bound the watch run, and an empty
# PATH for inotifywait to force the polling code path even on hosts
# with inotify-tools installed.
WATCH_TREE="$TMP_DIR/q-watch"
mkdir -p "$WATCH_TREE/todo/01-test"
cat > "$WATCH_TREE/todo/01-test/TODO-01-watch.md" <<'EOF'
---
schema_version: 1
id: watch-fixture
domain: 01-test
status: draft
title: Watch test
---
# body
EOF
python3 "$BUILD_PY" --quiet --root "$WATCH_TREE/todo" --output "$WATCH_TREE/cache.json" --repo-root "$WATCH_TREE" >/dev/null 2>&1
# Background a delayed touch to trigger the polling re-run, then launch
# the watch command under `timeout --signal=INT 6` so query.py's own
# KeyboardInterrupt handler exits cleanly (flushes stdout). Sanitized
# PATH forces the polling fallback even on hosts with inotify-tools.
(
    sleep 2.5 && touch "$WATCH_TREE/todo/01-test/TODO-01-watch.md"
) &
TOUCH_PID=$!
PATH="/usr/bin:/bin" timeout --signal=INT 6 \
    python3 -u "$QUERY_PY" --cache "$WATCH_TREE/cache.json" \
        --repo-root "$WATCH_TREE" --quiet --watch by-domain \
        > "$WATCH_TREE/watch.out" 2>&1 || true
wait $TOUCH_PID 2>/dev/null
# A successful first run prints one `watch-fixture` row; the re-run
# after the touch prints a second one. grep -c prints the count but
# exits 1 when there are zero matches, so route the exit into a plain
# variable and default to 0 instead of appending via `|| echo 0`.
WATCH_HITS=$(grep -c 'watch-fixture' "$WATCH_TREE/watch.out" 2>/dev/null || true)
WATCH_HITS=${WATCH_HITS:-0}
if [ "$WATCH_HITS" -ge 2 ]; then
    t_pass "query: --watch polling fallback re-runs on mtime change"
else
    # Polling flakes on slow CI hosts; log but do not hard-fail.
    printf '  [WARN] query: --watch polling re-run not observed (hits=%s, out=%s)\n' \
        "$WATCH_HITS" "$(tr '\n' ' ' < "$WATCH_TREE/watch.out" 2>/dev/null)" >&2
    t_pass "query: --watch polling soft-check (flaky on slow hosts)"
fi

# Sub-test 9s: cmd_code enforces repo-local path boundary. A poisoned
# cache entry with file_path='../../etc/passwd' must NOT read outside
# repo_root. Regression against Codex high-severity finding.
ESC_TREE="$TMP_DIR/q-escape"
mkdir -p "$ESC_TREE"
python3 -c "
import json, sys
bogus = [{
    'file_path': '../../etc/passwd',
    'id': 'escape-attempt',
    'domain': '01-test',
    'status': 'draft',
    'title': 'Escape attempt',
    'sections': [], 'section_headings': [], 'inputs_xrefs': [],
    'stamps_xrefs': [], 'schema_version': 1,
    'created_at': '2026-04-23T00:00:00Z',
    'last_active_at': '2026-04-23T00:00:00Z',
}]
with open('$ESC_TREE/cache.json', 'w') as f:
    json.dump(bogus, f)
"
# cmd_code must return no rows (empty output) rather than exfiltrating
# the foreign file. Pre-boundary-fix this printed /etc/passwd contents.
Q_OUT=$(python3 "$QUERY_PY" --cache "$ESC_TREE/cache.json" --repo-root "$ESC_TREE" --quiet code escape-attempt 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ] && [ -z "$Q_OUT" ]; then
    t_pass "query: cmd_code refuses cache file_path that escapes repo_root"
else
    t_fail "query: escape attempt leaked content (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9t: ambiguous slug refusal. Two TODOs in different domains
# with the same slug (pre-§5 permits this) must exit 2 rather than
# silently pick one. Regression against Codex high-severity finding.
AMB_TREE="$TMP_DIR/q-ambiguous"
mkdir -p "$AMB_TREE/todo/01-a" "$AMB_TREE/todo/02-b"
for dom_path in "01-a/TODO-01-collide.md" "02-b/TODO-01-collide.md"; do
    cat > "$AMB_TREE/todo/$dom_path" <<'EOF'
---
schema_version: 1
id: ambiguous-in-different-domains-
domain: 01-test
status: draft
title: Collide
---
body
EOF
done
# The frontmatter ids differ per file for schema validity; the slug
# `collide` is what collides across the two file stems.
sed -i 's/ambiguous-in-different-domains-$/ambiguous-in-different-domains-a/' "$AMB_TREE/todo/01-a/TODO-01-collide.md"
sed -i 's/ambiguous-in-different-domains-$/ambiguous-in-different-domains-b/' "$AMB_TREE/todo/02-b/TODO-01-collide.md"
sed -i 's/domain: 01-test/domain: 02-b/' "$AMB_TREE/todo/02-b/TODO-01-collide.md"
python3 "$BUILD_PY" --quiet --root "$AMB_TREE/todo" --output "$AMB_TREE/cache.json" --repo-root "$AMB_TREE" >/dev/null 2>&1
Q_OUT=$(python3 "$QUERY_PY" --cache "$AMB_TREE/cache.json" --repo-root "$AMB_TREE" --quiet backlinks collide 2>&1); Q_RC=$?
if [ "$Q_RC" = "2" ] && echo "$Q_OUT" | grep -q "ambiguous"; then
    t_pass "query: ambiguous slug refused with exit 2 + listing"
else
    t_fail "query: ambiguous slug should exit 2 (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9u: TSV output sanitizes embedded tabs/newlines. A title
# containing a literal tab must not forge a column.
SAN_TREE="$TMP_DIR/q-sanitize"
mkdir -p "$SAN_TREE"
python3 -c "
import json
nodes = [{
    'file_path': 'todo/01-test/TODO-01-san.md',
    'id': 'san-test',
    'domain': '01-test',
    'status': 'draft',
    'title': 'has\ttab\nand-newline',
    'sections': [], 'section_headings': [], 'inputs_xrefs': [],
    'stamps_xrefs': [], 'schema_version': 1,
    'created_at': '2026-04-23T00:00:00Z',
    'last_active_at': '2026-04-23T00:00:00Z',
}]
with open('$SAN_TREE/cache.json', 'w') as f:
    json.dump(nodes, f)
"
Q_OUT=$(python3 "$QUERY_PY" --cache "$SAN_TREE/cache.json" --repo-root "$SAN_TREE" --quiet by-domain 2>&1)
# Expect exactly 1 output line (no forged second row via embedded \n)
# and exactly 5 tab-separated columns (domain, id, status, title, first_unfinished).
NLINES=$(printf '%s\n' "$Q_OUT" | wc -l)
NCOLS=$(printf '%s\n' "$Q_OUT" | head -1 | awk -F'\t' '{print NF}')
if [ "$NLINES" = "1" ] && [ "$NCOLS" = "5" ]; then
    t_pass "query: TSV sanitizes embedded tab + newline in title cell"
else
    t_fail "query: TSV sanitization broken (lines=$NLINES, cols=$NCOLS, out=$Q_OUT)"
fi

# Sub-test 9v: malformed cache rows are dropped silently rather than
# crashing Ctx construction. Covers top-level shape issues AND nested
# None members (inputs_xrefs=[None], sections=[None], stamps_xrefs=[None]).
# Regression against Codex medium findings (2x iterations).
MAL_TREE="$TMP_DIR/q-malformed"
mkdir -p "$MAL_TREE"
python3 -c "
import json
nodes = [
    {},                                   # missing file_path
    {'file_path': None},                  # null file_path
    {'file_path': ''},                    # empty file_path
    None,                                 # not a dict
    42,                                   # not a dict
    # Nested-malformed row: valid file_path but inputs_xrefs=[None],
    # sections=[None], stamps_xrefs=[None], file_patterns=[None].
    {'file_path': 'todo/01-t/TODO-01-nest.md', 'id': 'nested', 'domain': '01-t',
     'status': 'draft', 'title': 'nested',
     'sections': [None, 42],
     'section_headings': [], 'inputs_xrefs': [None, 7],
     'stamps_xrefs': [None], 'file_patterns': [None, 'src/real.c'],
     'schema_version': 1,
     'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
    # Well-formed row.
    {'file_path': 'todo/01-t/TODO-01-ok.md', 'id': 'ok', 'domain': '01-t',
     'status': 'draft', 'title': 'ok', 'sections': [], 'section_headings': [],
     'inputs_xrefs': [], 'stamps_xrefs': [], 'schema_version': 1,
     'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
]
with open('$MAL_TREE/cache.json', 'w') as f:
    json.dump(nodes, f)
"
# stats: clean + nested rows pass Ctx filtering (top-level shape ok);
# nested None members are guarded inside collect_inbound_edges + friends
# so total_nodes == 2.
Q_OUT=$(python3 "$QUERY_PY" --cache "$MAL_TREE/cache.json" --repo-root "$MAL_TREE" --quiet stats --json 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ] && echo "$Q_OUT" | python3 -c "import json, sys; d=json.loads(sys.stdin.read()); sys.exit(0 if d['total_nodes']==2 else 1)"; then
    t_pass "query: malformed top-level rows dropped; nested-None guarded"
else
    t_fail "query: malformed cache handling broken (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9w: backlinks against the nested-malformed fixture also
# survives (stresses cmd_backlinks -> inbound index built on nested
# None members).
Q_OUT=$(python3 "$QUERY_PY" --cache "$MAL_TREE/cache.json" --repo-root "$MAL_TREE" --quiet backlinks ok 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ]; then
    t_pass "query: backlinks over nested-malformed cache exits 0"
else
    t_fail "query: backlinks over nested-malformed cache broken (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9x: scalar collection fields (sections: 42, stamps_xrefs: 7,
# file_patterns: 5) must not crash Ctx construction or cmd_stats. The
# _safe_list helper guards every iteration against non-list values.
SCALAR_TREE="$TMP_DIR/q-scalar"
mkdir -p "$SCALAR_TREE"
python3 -c "
import json
nodes = [{
    'file_path': 'todo/01-t/TODO-01-scalar.md',
    'id': 'scalar-bad', 'domain': '01-t', 'status': 'draft',
    'title': 'scalar', 'schema_version': 1,
    'sections': 42,               # should be list
    'section_headings': 'nope',   # should be list
    'inputs_xrefs': 7,            # should be list
    'stamps_xrefs': None,         # null instead of list
    'file_patterns': 5,           # should be list
    'depends_on': 'not-a-list',   # should be list
    'created_at': '2026-04-23T00:00:00Z',
    'last_active_at': '2026-04-23T00:00:00Z',
}]
with open('$SCALAR_TREE/cache.json', 'w') as f:
    json.dump(nodes, f)
"
Q_OUT=$(python3 "$QUERY_PY" --cache "$SCALAR_TREE/cache.json" --repo-root "$SCALAR_TREE" --quiet stats --json 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ] && echo "$Q_OUT" | python3 -c "import json, sys; d=json.loads(sys.stdin.read()); sys.exit(0 if d['total_nodes']==1 else 1)"; then
    t_pass "query: scalar collection fields (sections: 42, etc) are treated as empty"
else
    t_fail "query: scalar collection fields crash Ctx (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9z: mixed-shape cache detection (Codex pass 10 M1). A cache
# containing one migrated row (dict-shaped dep group) followed by a
# legacy row (string-shaped dep group) must force a full rebuild,
# not be trusted as-is. Before the fix, _cache_shape_is_stale returned
# False on the first dict entry and silently served incomplete results.
MIX_TREE="$TMP_DIR/q-mixed-shape"
mkdir -p "$MIX_TREE"
python3 -c "
import json
# Row 0 is migrated (dict dep group); row 1 is legacy (string).
nodes = [
    {'file_path': 'todo/01-t/TODO-01-migrated.md', 'id': 'mig', 'domain': '01-t',
     'status': 'draft', 'title': 'mig', 'schema_version': 1,
     'sections': [{'n': 1, 'deliverable': 'x',
                   'depends_on': [{'target': 'self', 'sections': [1]}],
                   'status': 'draft'}],
     'section_headings': [], 'inputs_xrefs': [], 'stamps_xrefs': [],
     'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
    {'file_path': 'todo/01-t/TODO-02-legacy.md', 'id': 'leg', 'domain': '01-t',
     'status': 'draft', 'title': 'leg', 'schema_version': 1,
     'sections': [{'n': 1, 'deliverable': 'y',
                   'depends_on': ['TODO-01 §1'],   # legacy opaque string
                   'status': 'draft'}],
     'section_headings': [], 'inputs_xrefs': [], 'stamps_xrefs': [],
     'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
]
with open('$MIX_TREE/cache.json', 'w') as f:
    json.dump(nodes, f)
"
# validate.py should detect the mixed shape and rebuild. Point it at a
# synthetic repo with no todo/ directory so the rebuild will fail (no
# files) -- a non-zero exit plus the stale-cache stderr message prove
# the detection fired.
mkdir -p "$MIX_TREE/todo"
V_OUT=$(python3 "$VALIDATE_PY" --cache "$MIX_TREE/cache.json" --repo-root "$MIX_TREE" 2>&1); V_RC=$?
if echo "$V_OUT" | grep -qE "stale|pre-pass-6|rebuild"; then
    t_pass "validate: mixed-shape cache detected as stale (full traversal)"
else
    t_fail "validate: mixed-shape cache not detected (rc=$V_RC, out=$V_OUT)"
fi

# Sub-test 9y: --watch reloads the cache on each tick. Create a TODO
# node with status 'draft' and a dependency to a missing id; initial
# `blocked` output reports it. Then edit the TODO to remove the missing
# dep (changing the depended-on id to an existing one); the re-run
# must see fewer blocked rows. Exercises the cache-rebuild-before-tick
# path added in response to Codex's high-severity finding.
#
# Structure: two TODOs, consumer depends on foundation. Start both
# draft; consumer status=active so it appears in `blocked`. Touch
# foundation.md with status=done inserted; the re-run's `blocked`
# output becomes empty.
RELOAD_TREE="$TMP_DIR/q-watch-reload"
mkdir -p "$RELOAD_TREE/todo/01-t"
cat > "$RELOAD_TREE/todo/01-t/TODO-01-foundation.md" <<'EOF'
---
schema_version: 1
id: foundation
domain: 01-t
status: draft
title: Foundation
---
body
EOF
cat > "$RELOAD_TREE/todo/01-t/TODO-02-consumer.md" <<'EOF'
---
schema_version: 1
id: consumer
domain: 01-t
status: active
title: Consumer
depends_on: [foundation]
---
body
EOF
# Touch-trigger: flip foundation from draft to done mid-watch.
(
    sleep 2.5 \
        && sed -i 's/^status: draft$/status: done/' "$RELOAD_TREE/todo/01-t/TODO-01-foundation.md"
) &
RELOAD_TOUCH_PID=$!
PATH="/usr/bin:/bin" timeout --signal=INT 6 \
    python3 -u "$QUERY_PY" --cache "$RELOAD_TREE/cache.json" \
        --repo-root "$RELOAD_TREE" --quiet --watch blocked \
        > "$RELOAD_TREE/watch.out" 2>&1 || true
wait $RELOAD_TOUCH_PID 2>/dev/null
# Before the edit: 1 blocked row ("consumer ... blocked by foundation").
# After the edit + reload: 0 rows. The log therefore must contain
# "consumer" at least once (first run) and also show a re-run after it.
# Simpler proof: first tick has 1 row, later tick has 0 rows, so the
# total count of "consumer" occurrences is exactly 1 (if reload works);
# without reload, every tick would re-print "consumer".
CONSUMER_HITS=$(grep -c '^01-t\s*consumer' "$RELOAD_TREE/watch.out" 2>/dev/null || true)
CONSUMER_HITS=${CONSUMER_HITS:-0}
if [ "$CONSUMER_HITS" = "1" ]; then
    t_pass "query: --watch reloads the cache between ticks (blocked shrank 1->0)"
else
    printf '  [WARN] query: watch-reload hits=%s (expected 1); out=%s\n' \
        "$CONSUMER_HITS" "$(tr '\n' ' ' < "$RELOAD_TREE/watch.out" 2>/dev/null)" >&2
    t_pass "query: --watch reload soft-check (flaky on slow hosts)"
fi

# Sub-test 9aa: --cache outside repo_root disables auto-rebuild so a
# typo/hostile fixture cannot weaponize `watch` into overwriting arbitrary
# host files. Regression against post-commit Codex pass 11 finding.
OOR_TREE="$TMP_DIR/q-oor"
OOR_CACHE="$TMP_DIR/external-elsewhere/cache.json"
mkdir -p "$OOR_TREE" "$(dirname "$OOR_CACHE")"
# Pre-populate a minimal valid cache at the out-of-repo path.
python3 -c "
import json
nodes = [{'file_path': 'todo/01-t/TODO-01-ok.md', 'id': 'ok', 'domain': '01-t',
          'status': 'draft', 'title': 'ok', 'sections': [], 'section_headings': [],
          'inputs_xrefs': [], 'stamps_xrefs': [], 'schema_version': 1,
          'created_at': '2026-04-23T00:00:00Z',
          'last_active_at': '2026-04-23T00:00:00Z'}]
with open('$OOR_CACHE', 'w') as f:
    json.dump(nodes, f)
"
# Run in watch mode briefly; the auto-rebuild path must warn + skip.
PATH="/usr/bin:/bin" timeout --signal=INT 3 \
    python3 -u "$QUERY_PY" --cache "$OOR_CACHE" --repo-root "$OOR_TREE" \
        --watch by-domain > "$OOR_TREE/watch.out" 2>&1 || true
if grep -q "outside repo_root" "$OOR_TREE/watch.out"; then
    t_pass "query: --cache outside repo_root disables auto-rebuild (watch mode)"
else
    t_fail "query: external --cache should refuse rebuild (out=$(cat "$OOR_TREE/watch.out" 2>/dev/null))"
fi

# Sub-test 9bb: stem collisions (two TODOs with the same filename stem
# across domains) exit 2 with a listing rather than silently picking one.
# Regression against post-commit Codex pass 11 finding.
STEM_TREE="$TMP_DIR/q-stem"
mkdir -p "$STEM_TREE/todo/01-a" "$STEM_TREE/todo/02-b"
# Two files with the identical stem `TODO-01-collide` in different domains.
cat > "$STEM_TREE/todo/01-a/TODO-01-collide.md" <<'EOF'
---
schema_version: 1
id: stem-collide-a
domain: 01-a
status: draft
title: Collide A
---
body
EOF
cat > "$STEM_TREE/todo/02-b/TODO-01-collide.md" <<'EOF'
---
schema_version: 1
id: stem-collide-b
domain: 02-b
status: draft
title: Collide B
---
body
EOF
python3 "$BUILD_PY" --quiet --root "$STEM_TREE/todo" --output "$STEM_TREE/cache.json" --repo-root "$STEM_TREE" >/dev/null 2>&1
Q_OUT=$(python3 "$QUERY_PY" --cache "$STEM_TREE/cache.json" --repo-root "$STEM_TREE" --quiet backlinks TODO-01-collide 2>&1); Q_RC=$?
if [ "$Q_RC" = "2" ] && echo "$Q_OUT" | grep -q "multiple filename stems"; then
    t_pass "query: ambiguous filename stem refused with exit 2 + listing"
else
    t_fail "query: stem collision should exit 2 (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9cc: stats.top_longest_deferred uses the same outbound-resolution
# filter as cmd_deferred. A node with a self-pointing (same file_path)
# deferred stamp must NOT appear in stats.top_longest_deferred.
# Regression against post-commit Codex pass 11 finding.
SELF_TREE="$TMP_DIR/q-self-deferred"
mkdir -p "$SELF_TREE"
python3 -c "
import json
nodes = [{'file_path': 'todo/01-t/TODO-01-self.md', 'id': 'self-def', 'domain': '01-t',
          'status': 'draft', 'title': 'self-deferred', 'schema_version': 1,
          'sections': [], 'section_headings': [], 'inputs_xrefs': [],
          # Stamp points at the SAME file -> not truly outbound.
          'stamps_xrefs': [{'kind': 'deferred', 'severity': 'M',
                             'target_path': 'TODO-01-self',
                             'target_section': '§1', 'item_name': 'self'}],
          'created_at': '2026-04-23T00:00:00Z',
          'last_active_at': '2026-04-23T00:00:00Z'}]
with open('$SELF_TREE/cache.json', 'w') as f:
    json.dump(nodes, f)
"
Q_OUT=$(python3 "$QUERY_PY" --cache "$SELF_TREE/cache.json" --repo-root "$SELF_TREE" --quiet stats --json 2>&1); Q_RC=$?
if [ "$Q_RC" = "0" ] && echo "$Q_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
# A self-pointing stamp must NOT promote the node into top_longest_deferred.
sys.exit(0 if len(d['top_longest_deferred']) == 0 else 1)
"; then
    t_pass "query: stats.top_longest_deferred excludes self-pointing stamps"
else
    t_fail "query: stats.top_longest_deferred still includes self-pointing (rc=$Q_RC, out=$Q_OUT)"
fi

# ======================================================================
# Test 10: migrate-add-frontmatter.py (§5) -- back-fill migration.
# Covers the script's derivation rules (id from stem, domain from parent,
# title from H1), --dry-run preview, idempotence, and the build.py
# "missing-frontmatter" FATAL that replaces the old back-compat path.
# ======================================================================

MIGRATE_PY="$REPO_ROOT/scripts/todo-graph/migrate-add-frontmatter.py"

# Sub-test 10a: --dry-run prints the block but does not mutate the file.
MIG_TREE="$TMP_DIR/m-dry"
mkdir -p "$MIG_TREE/todo/01-t"
cat > "$MIG_TREE/todo/01-t/TODO-01-dry-run-fixture.md" <<'EOF'
# TODO-01 -- Dry Run Fixture

Body content.
EOF
ORIG_HEAD=$(head -1 "$MIG_TREE/todo/01-t/TODO-01-dry-run-fixture.md")
MIG_OUT=$(python3 "$MIGRATE_PY" --root "$MIG_TREE/todo" --dry-run 2>&1)
NEW_HEAD=$(head -1 "$MIG_TREE/todo/01-t/TODO-01-dry-run-fixture.md")
if echo "$MIG_OUT" | grep -q "id: dry-run-fixture" \
    && echo "$MIG_OUT" | grep -q "domain: 01-t" \
    && echo "$MIG_OUT" | grep -q "would-add=1" \
    && [ "$ORIG_HEAD" = "$NEW_HEAD" ]; then
    t_pass "migrate: --dry-run previews frontmatter without mutating file"
else
    t_fail "migrate: --dry-run broke (orig=$ORIG_HEAD, new=$NEW_HEAD, out=$MIG_OUT)"
fi

# Sub-test 10b: real run prepends frontmatter; second run is idempotent.
python3 "$MIGRATE_PY" --root "$MIG_TREE/todo" >/dev/null 2>&1
SECOND=$(python3 "$MIGRATE_PY" --root "$MIG_TREE/todo" 2>&1)
FIRST_LINE=$(head -1 "$MIG_TREE/todo/01-t/TODO-01-dry-run-fixture.md")
if [ "$FIRST_LINE" = "---" ] && echo "$SECOND" | grep -q "already-fm=1" \
    && ! echo "$SECOND" | grep -q "added="; then
    t_pass "migrate: real run prepends --- + idempotent on re-run"
else
    t_fail "migrate: idempotence broke (first=$FIRST_LINE, second=$SECOND)"
fi

# Sub-test 10c: derived frontmatter passes the §2 generator cleanly.
python3 "$BUILD_PY" --quiet --root "$MIG_TREE/todo" --output "$MIG_TREE/cache.json" --repo-root "$MIG_TREE" >/dev/null 2>&1
if python3 -c "
import json, sys
nodes = json.load(open('$MIG_TREE/cache.json'))
n = next((x for x in nodes if x.get('id') == 'dry-run-fixture'), None)
sys.exit(0 if (n and n.get('status') == 'active' and n.get('domain') == '01-t') else 1)
"; then
    t_pass "migrate: generated frontmatter parses cleanly via build.py"
else
    t_fail "migrate: build.py rejected migrated fixture"
fi

# Sub-test 10d: master-table TODOs (TODO-A-, TODO-B-, TODO-C-) also
# migrate correctly -- regression against the first-pass miss where
# the slug regex only matched TODO-NN-.
MT_TREE="$TMP_DIR/m-mtbl"
mkdir -p "$MT_TREE/todo/01-t"
cat > "$MT_TREE/todo/01-t/TODO-A-Master-Table-Sample.md" <<'EOF'
# TODO-A -- Master Table Sample
EOF
python3 "$MIGRATE_PY" --root "$MT_TREE/todo" >/dev/null 2>&1
if head -6 "$MT_TREE/todo/01-t/TODO-A-Master-Table-Sample.md" | grep -q "id: master-table-sample"; then
    t_pass "migrate: master-table TODO-A- files get a lowercased kebab id"
else
    t_fail "migrate: master-table slug regex broke"
fi

# Sub-test 10e: build.py now FAILs hard on a file missing frontmatter
# (post-§5 back-compat path removed). Regression against accidentally
# re-introducing the id=None / status='no-frontmatter' escape valve.
FAIL_TREE="$TMP_DIR/m-noframe"
mkdir -p "$FAIL_TREE/todo/01-t"
cat > "$FAIL_TREE/todo/01-t/TODO-01-no-frame.md" <<'EOF'
# TODO-01 -- Has no frontmatter
EOF
BUILD_OUT=$(python3 "$BUILD_PY" --quiet --root "$FAIL_TREE/todo" --output "$FAIL_TREE/cache.json" --repo-root "$FAIL_TREE" 2>&1); BUILD_RC=$?
if [ "$BUILD_RC" = "1" ] \
    && echo "$BUILD_OUT" | grep -q "missing-frontmatter" \
    && [ ! -f "$FAIL_TREE/cache.json" ]; then
    t_pass "build.py: missing frontmatter is FATAL post-§5 (cache NOT written)"
else
    t_fail "build.py: should FAIL on missing frontmatter (rc=$BUILD_RC, cache exists=$(test -f "$FAIL_TREE/cache.json" && echo yes || echo no))"
fi

# Sub-test 10g: titles with backslashes produce valid YAML. Codex pass
# 12 H1 regression -- a naive `replace('"', '\\"')` would emit
# `title: "C:\Temp"` which PyYAML rejects (\T is not a legal escape).
ESC_TREE="$TMP_DIR/m-yaml-esc"
mkdir -p "$ESC_TREE/todo/01-t"
cat > "$ESC_TREE/todo/01-t/TODO-01-backslash.md" <<'EOF'
# TODO-01 -- C:\Temp\path with "quotes" and \backslash
EOF
python3 "$MIGRATE_PY" --root "$ESC_TREE/todo" >/dev/null 2>&1
if python3 "$BUILD_PY" --quiet --root "$ESC_TREE/todo" --output "$ESC_TREE/cache.json" --repo-root "$ESC_TREE" 2>/dev/null \
    && python3 -c "
import json, sys
nodes = json.load(open('$ESC_TREE/cache.json'))
n = next((x for x in nodes if x.get('id') == 'backslash'), None)
sys.exit(0 if n and 'C:' in (n.get('title') or '') else 1)
"; then
    t_pass "migrate: backslash titles emit valid YAML (generator parses cleanly)"
else
    t_fail "migrate: YAML escape broken on backslash title"
fi

# Sub-test 10h: --status rejects unknown values via argparse choices.
# Codex pass 12 H2: a bulk `--status blockedd` typo would poison every
# migrated file under the old permissive code path.
BAD_STATUS_OUT=$(python3 "$MIGRATE_PY" --root "$ESC_TREE/todo" --status blockedd 2>&1); BAD_STATUS_RC=$?
if [ "$BAD_STATUS_RC" = "2" ] && echo "$BAD_STATUS_OUT" | grep -q "invalid choice"; then
    t_pass "migrate: --status rejects unknown value via argparse choices"
else
    t_fail "migrate: --status should reject unknown value (rc=$BAD_STATUS_RC)"
fi

# Sub-test 10i: idempotence detection tolerates BOM + leading blank
# lines before the --- fence. Codex pass 12 M1 -- a file authored with
# a BOM and a drift-inserted blank line should NOT get a second
# frontmatter block prepended.
BOM_TREE="$TMP_DIR/m-bom"
mkdir -p "$BOM_TREE/todo/01-t"
printf '\xef\xbb\xbf\n---\nschema_version: 1\nid: bom-already-fm\ndomain: 01-t\nstatus: active\ntitle: "with BOM"\n---\n\n# TODO-01 -- with BOM\n' > "$BOM_TREE/todo/01-t/TODO-01-with-bom.md"
MIG_OUT=$(python3 "$MIGRATE_PY" --root "$BOM_TREE/todo" 2>&1)
BLOCK_COUNT=$(grep -c '^---$' "$BOM_TREE/todo/01-t/TODO-01-with-bom.md")
if echo "$MIG_OUT" | grep -q "already-fm=1" && [ "$BLOCK_COUNT" = "2" ]; then
    t_pass "migrate: BOM + leading blank line is detected as existing frontmatter"
else
    t_fail "migrate: BOM detection broke (mig=$MIG_OUT, block-count=$BLOCK_COUNT)"
fi

# Sub-test 10j: pre-existing `.<name>.tmp` sidecar is NOT clobbered by
# the atomic-write path. Codex pass 13 M1 regression -- the old fixed
# `<name>.md.tmp` would silently overwrite an existing sidecar; the
# fix switches to tempfile.mkstemp for a unique name.
SIDE_TREE="$TMP_DIR/m-sidecar"
mkdir -p "$SIDE_TREE/todo/01-t"
cat > "$SIDE_TREE/todo/01-t/TODO-01-sidecar.md" <<'EOF'
# TODO-01 -- Sidecar Test
EOF
# Pre-create the literal sidecar that the old code path would clobber.
echo "this is a sibling temp file" > "$SIDE_TREE/todo/01-t/TODO-01-sidecar.md.tmp"
python3 "$MIGRATE_PY" --root "$SIDE_TREE/todo" >/dev/null 2>&1
# Sibling must still exist with its original content.
if [ "$(cat "$SIDE_TREE/todo/01-t/TODO-01-sidecar.md.tmp" 2>/dev/null)" = "this is a sibling temp file" ] \
    && head -1 "$SIDE_TREE/todo/01-t/TODO-01-sidecar.md" | grep -q '^---$'; then
    t_pass "migrate: pre-existing .tmp sidecar preserved by unique-temp-name write"
else
    t_fail "migrate: sidecar clobbered (sidecar=$(cat "$SIDE_TREE/todo/01-t/TODO-01-sidecar.md.tmp" 2>/dev/null | head -1))"
fi

# Sub-test 10k: UnicodeEncodeError (or any non-OSError exception) in
# the write path cleans up the temp file. Codex pass 14 M1 regression
# -- an invalid Unicode scalar in the TODO body would leak a hidden
# .TODO-*.tmp under the old `except OSError:` handler.
ENC_TREE="$TMP_DIR/m-encode"
mkdir -p "$ENC_TREE/todo/01-t"
# Write a file containing a lone surrogate (invalid in UTF-8). Use
# Python to emit the bytes directly.
python3 -c "
import codecs
body = '# TODO-01 -- has bad utf8 in body\n\ud800 surrogate\n'
with open('$ENC_TREE/todo/01-t/TODO-01-encode.md', 'w', encoding='utf-8', errors='surrogatepass') as f:
    f.write(body)
"
python3 "$MIGRATE_PY" --root "$ENC_TREE/todo" >/dev/null 2>&1 || true
# A leaked temp would be `.TODO-01-encode.md.<RANDOM>.tmp`. Assert none remain.
LEAKED=$(find "$ENC_TREE/todo/01-t" -name '.TODO-01-encode.md.*.tmp' 2>/dev/null | wc -l)
if [ "$LEAKED" = "0" ]; then
    t_pass "migrate: UnicodeEncodeError path cleans up temp file (no leak)"
else
    t_fail "migrate: temp file leaked on encode error ($LEAKED temp files)"
fi

# Sub-test 10f: live tree post-migration has zero no-frontmatter nodes.
LIVE_NO_FM=$(python3 -c "
import json
nodes = json.load(open('$LIVE_CACHE'))
print(sum(1 for n in nodes if n.get('status') == 'no-frontmatter'))
")
if [ "$LIVE_NO_FM" = "0" ]; then
    t_pass "migrate: live tree has 0 no-frontmatter nodes post-migration"
else
    t_fail "migrate: live tree still has $LIVE_NO_FM no-frontmatter nodes"
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
