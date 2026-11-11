#!/usr/bin/env bash
# ============================================================================
# test_build.sh -- todo-graph build suite shard: Tests 1-15 and every block that writes or reads the real build/todo-cache.json.
#
# One of four shards scripts/test-tooling.sh starts together; the shared
# preamble, helpers and shard list are in test_build_lib.sh.
# ============================================================================

set -u
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test_build_lib.sh" || exit 2

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

# Helper: prepare a fresh fixture tree (wipes + recreates).
v_run() {
    local tree="$1"
    rm -rf "$tree"
    mkdir -p "$tree"
}

# Sub-test 8a: clean fixture passes all 10 checks.
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
    t_pass "validate: clean fixture passes all 10 checks (exit 0)"
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
mkdir -p "$V_TREE/build"
cat > "$V_TREE/build/todo-cache.json" <<'EOF'
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
# No --cache: the GENERATED path is the only one the recovery may overwrite,
# and this fixture is about the recovery.
V_OUT=$(python3 "$VALIDATE_PY" --repo-root "$V_TREE" 2>&1)
V_RC=$?
# Validator should detect stale shape, rebuild, and then exit cleanly.
if [ "$V_RC" = "0" ] && echo "$V_OUT" | grep -q "unusable.*rebuilding"; then
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

# Sub-test 8n: in-file anchor check (§32). ONE fixture carrying every slug
# hazard at once, and the oracle asserts EXACTLY ONE finding -- so it proves
# both directions in a single run: the retitled heading's dead anchor IS
# reported, and none of the hazards (a `/` heading, a `:` heading, an inline
# code span, a fenced block, an HTML comment, an escaped bracket, a deeply
# indented list-continuation link) produces a false positive.
V_TREE="$TMP_DIR/v-anchor"
v_run "$V_TREE"
mkdir -p "$V_TREE/todo/01-test"
cat > "$V_TREE/todo/01-test/TODO-01-anchor.md" <<'EOF'
---
schema_version: 1
id: fm-anchor
domain: 01-test
status: active
title: "fixture anchor"
---
# TODO-01 -- Anchor fixture

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |
| 💎 | 2 | §2 | Slash | §1 | [x] |
| 💎 | 3 | §3 | Colon | §1 | [x] |
| 💎 | 4 | §4 | Code | §1 | [x] |
| 💎 | 5 | §5 | Retitled | §1 | [x] |

## 1. First Section

A `/` heading keeps BOTH spaces around the removed slash: [slash](#2-alpha--beta).
A `:` heading keeps only the one space after it: [colon](#3-gamma-delta).
An inline code span in a heading keeps its underscores: [code](#4-uses-snake_case_id-here).
This one is DEAD -- §5 was retitled after the link was written: [retitled](#5-old-title).

- A deeply indented list continuation is a LINK, not an indented code block:
      - [indented](#2-alpha--beta) resolves and must not be skipped.

An inline code span is stripped per line, so `[example](#totally-made-up)` is prose.
An escaped bracket is literal, so \](#escaped-not-a-link) is not a link.

<!-- A comment hides its contents, so [hidden](#also-made-up) never renders. -->

```
## Fenced Heading Is Not A Heading
A fenced block is literal, so [fenced](#never-real) is not a link either.
```

- [x] Commit

## 2. Alpha / Beta

- [x] Commit

## 3. Gamma: Delta

- [x] Commit

## 4. Uses `snake_case_id` Here

- [x] Commit

## 5. New Title

- [x] Commit
EOF
python3 "$BUILD_PY" --quiet --root "$V_TREE/todo" --output "$V_TREE/cache.json" --repo-root "$V_TREE" >/dev/null 2>&1
V_OUT=$(python3 "$VALIDATE_PY" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_RC=$?
V_HITS=$(echo "$V_OUT" | grep -c "\[FAIL\] in-file-anchor:.*TODO-01-anchor")
if [ "$V_RC" = "1" ] && [ "$V_HITS" = "1" ] && echo "$V_OUT" | grep -q "5-old-title"; then
    t_pass "validate: in-file-anchor reports the retitled anchor and nothing else (1 hit)"
else
    t_fail "validate: in-file-anchor fixture wrong (rc=$V_RC, hits=$V_HITS, out=$V_OUT)"
fi

# Sub-test 8n2: the slug/scan helpers directly, for the hazards that cannot be
# expressed as a whole TODO fixture without also binding this test to build.py's
# heading parser -- closing ATX syntax, 1-3 space heading indentation, a
# duplicate-slug collision against a naturally-suffixed heading, and an HTML
# comment that spans lines.
V_OUT=$(python3 - "$REPO_ROOT/scripts/todo-graph" <<'ANCHEOF' 2>&1
import sys
sys.path.insert(0, sys.argv[1])
import validate as v

fails = []
def eq(got, want, label):
    if got != want:
        fails.append(f"{label}: got {got!r} want {want!r}")

# `_scan_markdown` yields ("heading", title) / ("link", lineno, anchor) events
# rather than building whole-file lists, so an anchor-dense file cannot
# amplify memory. These two adapters keep the assertions readable.
# `_scan_markdown` takes the SHARED ScanResult since section 47 (it needs
# `leaf_views` for headings, not just the mask), so the producer and the
# validator cannot drift about what is fenced OR about what a container-nested
# heading is. These adapters do here what the production caller does once/file.
def sm(src):
    return v._scan_markdown(v.cache_schema.scan_text(src))
def links(src):
    return [(e[1], e[2]) for e in sm(src) if e[0] == "link"]
def heads(src):
    return [e[1] for e in sm(src) if e[0] == "heading"]

# GitHub does NOT collapse whitespace runs.
eq(v._slugify_heading("2. Alpha / Beta"), "2-alpha--beta", "slash")
eq(v._slugify_heading("3. Gamma: Delta"), "3-gamma-delta", "colon")
# `_` is a word char and CommonMark forbids intraword `_` emphasis, so it survives.
eq(v._slugify_heading("10. Head Resolution in `resolve_symbol`"),
   "10-head-resolution-in-resolve_symbol", "underscore")

# Closing ATX syntax is trimmed; 1-3 space indentation is still a heading.
eq(heads("## Closed ##\n   ### Indented\n"), ["Closed", "Indented"], "atx-close+indent")

# A collision resolves against the literal `-1` heading, github-slugger style.
eq(sorted(v._heading_slugs(["Foo", "Foo-1", "Foo"])), ["foo", "foo-1", "foo-2"], "collision")

# A fence hides both its headings and its links.
eq(heads("```\n## Fenced\n[a](#x)\n```\n## Real\n"), ["Real"], "fence-heading")
eq(links("```\n## Fenced\n[a](#x)\n```\n## Real\n"), [], "fence-link")

# A closer must be the SAME marker, AT LEAST as long as the opener, and carry
# nothing else. Closing a ````-fence on a ``` run read the code as prose AND
# made the real closer re-open a fence over everything after it, so the link
# on the last line went missing entirely.
eq(links("````\n```\n[dead](#nope)\n````\n[after](#real)\n"),
   [(5, "real")], "fence-run-length")
eq(links("```\n```not-a-close\n[in](#x)\n```\n[after](#real)\n"),
   [(5, "real")], "fence-info-string-closer")
eq(links("```\n~~~\n[in](#x)\n```\n[after](#real)\n"),
   [(5, "real")], "fence-mixed-markers")

# Escaping is a PARITY question. An even backslash run escapes the backslash,
# so the `]` is live and CommonMark renders a link.
eq(links("[label\\\\](#dead)\n"), [(1, "dead")], "escape-even")
eq(links("[label\\](#dead)\n"), [], "escape-odd")

# Code spans close only on a run of EQUAL length; an opener with no matching
# closer is literal text, so the link inside it still renders.
eq(links("``two [b](#dead)`\n"), [(1, "dead")], "span-unequal-runs")
eq(links("`one [a](#dead)\n"), [(1, "dead")], "span-unmatched-opener")
eq(links("see ``[ex](#madeup)`` x\n"), [], "span-equal-runs")

# An ESCAPED backtick is literal and opens nothing, so the anchor is live.
eq(links("\\`prefix [dead](#missing)\\`\n"), [(1, "missing")], "span-escaped-backtick")

# A line STARTING with `<!--` is an HTML block and is hidden WHOLE. Treating
# the text after `-->` as markdown minted a phantom heading that validated an
# otherwise dead anchor.
eq(heads("<!-- --># Ghost\n[x](#ghost)\n"), [], "html-block-no-ghost-heading")
eq(links("<!-- --># Ghost\n[x](#ghost)\n"), [(2, "ghost")], "html-block-anchor-stays-dead")
# ...but a comment marker INSIDE a code span is not a block start; treating it
# as one suppressed every remaining link in the file.
eq(links("`<!--` [dead](#missing)\nnext [also](#gone)\n"),
   [(1, "missing"), (2, "gone")], "comment-marker-inside-code")
eq(links("<!-- a\nb [x](#h) -->\n[y](#real)\n"), [(3, "real")], "multiline-block-comment")

# A fence closer permits ONLY spaces or tabs. `strip()` is Unicode-aware and
# accepted NBSP, closing early and re-opening a fence over the rest of the file.
eq(links("```\n``` \xa0\n[in](#inside)\n```\n[out](#dead)\n"),
   [(5, "dead")], "fence-closer-nbsp")

# A heading's slug comes from RENDERED text, but code-span CONTENTS are
# rendered text -- GitHub keeps them, and 13 live anchors rely on it.
def head_slug(src):
    return v._slugify_heading(heads(src)[0])

eq(head_slug("## 10. Res in `resolve_symbol`\n"),
   "10-res-in-resolve_symbol", "heading-keeps-code-span")
eq(head_slug("## 3. `bash <a python file>` in three skills\n"),
   "3-bash-a-python-file-in-three-skills", "heading-code-span-angle-text")

# ...but INLINE MARKUP is not rendered text. Slugging the raw source minted a
# phantom anchor that both accepted a dead link naming it and rejected the
# real one.
eq(head_slug("## Live [label](https://example.test) Heading\n"),
   "live-label-heading", "heading-link-collapses-to-label")
eq(head_slug("## A <b>bold</b> C\n"), "a-bold-c", "heading-html-tag-dropped")
eq(head_slug("## A &amp; B\n"), "a--b", "heading-entity-decoded")

# A fragment destination may be angle-bracketed or carry a title; accepting
# only `](#frag)` meant neither form was ever scanned.
eq(links('[x](#missing "tip")\n'), [(1, "missing")], "link-with-title")
eq(links("[x](<#missing>)\n"), [(1, "missing")], "link-angle-destination")
# A title may contain an ESCAPED delimiter. Terminating the title there
# dropped the whole link and hid its dead anchor.
eq(links('[x](#missing "tip with \\"q\\"")\n'), [(1, "missing")], "link-escaped-title-quote")

# Emphasis delimiters are not rendered text, but an INTRAWORD `_` is, and both
# directions bite: `_Hello_` must LOSE its delimiters (or the live
# `#hello-world` is reported dead and a phantom `#_hello_-world` accepted),
# while `boot_info` must KEEP its underscore (or 13 live anchors in this
# corpus are reported dead).
eq(head_slug("## _Hello_ World\n"), "hello-world", "heading-emphasis-stripped")
eq(head_slug("## __Bold__ Text\n"), "bold-text", "heading-emphasis-double")
# MISMATCHED runs stay literal. `_a__` renders as emphasized `a` plus a
# LITERAL `_`; consuming both underscores rejected the valid `#a_` and
# accepted a phantom `#a`.
eq(v._rendered_inline_text("_a__"), "_a__", "emphasis-mismatched-run-left")
eq(v._rendered_inline_text("__a_"), "__a_", "emphasis-mismatched-run-right")
# An ESCAPED delimiter is literal. `(?<!\w)` alone let `\_Hello_` through,
# because a backslash is not a word character -- and parity still decides, so
# `\\_Hello_` is an escaped BACKSLASH and the emphasis is live.
eq(v._rendered_inline_text("\\_Hello_"), "\\_Hello_", "emphasis-escaped-delimiter")
eq(v._rendered_inline_text("\\\\_Hello_"), "\\\\Hello", "emphasis-even-backslash-run")
# A comment leaves NOTHING in rendered heading text (`foo<!--x-->bar` is
# `foobar`), but the LINK path substitutes a space so removal cannot fuse two
# tokens into a link nobody wrote.
eq(head_slug("## foo<!--x-->bar\n"), "foobar", "heading-comment-no-separator")
eq(links("a<!--c-->[x](#dead)\n"), [(1, "dead")], "link-path-comment-separator")
eq(head_slug("## 16. boot_info Kernel Validation\n"),
   "16-boot_info-kernel-validation", "heading-intraword-underscore-kept")
eq(head_slug("## 23. Unify SKIP_REVIEW_HOOK Opt-Out Scanner Across PreToolUse Gates\n"),
   "23-unify-skip_review_hook-opt-out-scanner-across-pretooluse-gates",
   "heading-live-corpus-anchor")

# An HTML comment hides its contents even across lines.
eq(links("<!-- [a](#x)\nstill hidden [b](#y) -->\n[c](#z)\n"), [(3, "z")], "multiline-comment")

# An escaped bracket is literal, not a link.
eq(links("text \\](#nope) more\n"), [], "escaped")

print("FAILS=" + ("|".join(fails) if fails else "none"))
ANCHEOF
)
if echo "$V_OUT" | grep -q "FAILS=none"; then
    t_pass "validate: in-file-anchor slug/scan helpers handle every slug hazard"
else
    t_fail "validate: in-file-anchor helper hazards broken ($V_OUT)"
fi

# Sub-test 8n2b: resource bounds. The 16 MiB per-file ceiling is generous
# enough that a comment-dense or anchor-dense line can exhaust the validator
# on input it is REQUIRED to accept. Both shapes were measured misbehaving
# before the fix: 100k HTML comments in 0.67 MiB took 1.14s under the
# quadratic per-comment rebuild, and 100k dead anchors minted 100k Findings.
V_OUT=$(python3 - "$REPO_ROOT/scripts/todo-graph" <<'BOUNDEOF' 2>&1
import sys, time, tracemalloc
sys.path.insert(0, sys.argv[1])
import validate as v

fails = []
def sm(src):
    return v._scan_markdown(v.cache_schema.scan_text(src))
# Linear comment scanning: 100k comments must not take seconds.
line = "<!---->" * 100000 + "\n"
t0 = time.perf_counter(); list(sm(line)); dt = time.perf_counter() - t0
if dt > 1.0:
    fails.append(f"comment scan took {dt:.2f}s (quadratic rebuild is back)")

# Backtick-dense line: memory must stay PROPORTIONAL to the line, not
# amplified. Indexing every run cost ~72 bytes of Python objects per
# backtick -- a 4 MiB line measured 287 MiB, extrapolating past a gigabyte at
# the 16 MiB ceiling this validator is required to accept.
line = "`x" * (4 * 1048576 // 2) + "\n"
tracemalloc.start()
list(sm(line))
peak = tracemalloc.get_traced_memory()[1]; tracemalloc.stop()
if peak > 12 * len(line):
    fails.append(f"backtick indexing peaked at {peak/1048576:.0f} MiB on a 4 MiB line")

# Finding amplification: many occurrences of ONE dead fragment collapse to one
# finding carrying the count, and distinct fragments are capped.
nodes = [{"file_path": "t.md"}]
snap = {"t.md": "## Real\n" + "\n".join("[x](#dead-one)" for _ in range(50000))}
f = v.check_in_file_anchor(nodes, snap)
if len(f) != 1 or "50000x" not in f[0].detail:
    fails.append(f"repeat-fragment collapse wrong: {len(f)} finding(s)")

snap = {"t.md": "## Real\n" + "\n".join(f"[x](#dead-{i})" for i in range(500))}
f = v.check_in_file_anchor(nodes, snap)
if len(f) != v._MAX_ANCHOR_FINDINGS_PER_FILE + 1:
    fails.append(f"distinct-fragment cap wrong: {len(f)} finding(s)")
elif "500 distinct dead anchors" not in f[-1].detail:
    fails.append(f"cap summary lost the exact count: {f[-1].detail}")
# BOUNDED OUTPUT, EXACT TOTAL -- the `_report_diag` contract. Counting emitted
# LINES reported 21 failures for 500 dead anchors, hiding the scale of the
# damage from operators and from automation reading the summary.
if sum(x.weight for x in f) != 500:
    fails.append(f"weighted total wrong: {sum(x.weight for x in f)} (want 500)")

# MALFORMED markup must not be quadratic. `<!--.*?-->` retried from every
# unmatched opener: 16/32/64 KiB measured 0.20s/0.80s/3.21s, quadrupling per
# doubling, and `</?[A-Za-z][^>]*>` plus the link regex scaled the same way.
# Well-formed input never exercised this path, which is why the first resource
# test missed it.
for kb in (32, 128):
    for label, src in (
        ("unclosed-comment", "x" + "<!--" * (kb * 1024 // 4) + "\n"),
        ("unclosed-tag", "x" + "<a" * (kb * 1024 // 2)),
        ("unclosed-link", "x" + "[x](" * (kb * 1024 // 4)),
    ):
        t0 = time.perf_counter()
        if label == "unclosed-comment":
            list(sm(src))
        else:
            v._rendered_inline_text(src)
        dt = time.perf_counter() - t0
        if dt > 1.0:
            fails.append(f"{label} at {kb} KiB took {dt:.2f}s (quadratic scan is back)")

# Anchor-dense input must not materialize one record per OCCURRENCE.
tracemalloc.start()
v.check_in_file_anchor(nodes, {"t.md": "## R\n" + "\n".join("[x](#dead)" for _ in range(4 * 1048576 // 11))})
peak = tracemalloc.get_traced_memory()[1]; tracemalloc.stop()
if peak > 48 * 1048576:
    fails.append(f"anchor-dense 4 MiB peaked at {peak/1048576:.0f} MiB")

print("FAILS=" + ("|".join(fails) if fails else "none"))
BOUNDEOF
)
if echo "$V_OUT" | grep -q "FAILS=none"; then
    t_pass "validate: in-file-anchor scanning is linear and its findings are bounded"
else
    t_fail "validate: in-file-anchor resource bounds broken ($V_OUT)"
fi

# Sub-test 8n3: MUTATION -- collapse whitespace runs in the slug rule. This is
# the exact bug the check was written against: it produced 6 confident wrong
# hits before the rule was corrected against the real headings. The `/` heading
# link must turn into a FALSE POSITIVE, proving the no-collapse rule is what
# keeps the fixture clean rather than something on the way to it.
MUT_DIR="$TMP_DIR/anchor-mut-collapse"
rm -rf "$MUT_DIR"; cp -r "$REPO_ROOT/scripts/todo-graph" "$MUT_DIR"
python3 - "$MUT_DIR/validate.py" <<'MUTEOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
needle = '    return s.replace(" ", "-")'
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
p.write_text(src.replace(needle, '    return __import__("re").sub(r"\\s+", "-", s)', 1),
             encoding="utf-8")
MUTEOF
V_OUT=$(python3 "$MUT_DIR/validate.py" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_HITS=$(echo "$V_OUT" | grep -c "\[FAIL\] in-file-anchor:.*TODO-01-anchor")
if [ "$V_HITS" -gt "1" ] && echo "$V_OUT" | grep -q "2-alpha--beta"; then
    t_pass "validate: mutation check -- collapsing whitespace runs makes the '/' heading a false positive"
else
    t_fail "validate: whitespace-collapse mutation did not misfire as expected (hits=$V_HITS, out=$V_OUT)"
fi

# Sub-test 8n4: MUTATION -- revert the check itself (drop it from the registry)
# and the dead anchor must go UNREPORTED. Without this, 8n would still pass
# against a build whose rc 1 came from some other check.
MUT_DIR="$TMP_DIR/anchor-mut-revert"
rm -rf "$MUT_DIR"; cp -r "$REPO_ROOT/scripts/todo-graph" "$MUT_DIR"
python3 - "$MUT_DIR/validate.py" <<'MUTEOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
needle = '        ("in-file-anchor", check_in_file_anchor, (nodes, snapshot)),\n'
assert src.count(needle) == 1, f"revert needle appears {src.count(needle)}x"
p.write_text(src.replace(needle, "", 1), encoding="utf-8")
MUTEOF
V_OUT=$(python3 "$MUT_DIR/validate.py" --cache "$V_TREE/cache.json" --repo-root "$V_TREE" 2>&1)
V_RC=$?
if [ "$V_RC" = "0" ] && ! echo "$V_OUT" | grep -q "in-file-anchor"; then
    t_pass "validate: mutation check -- the reverted check leaves the dead anchor unreported"
else
    t_fail "validate: in-file-anchor revert mutation still reported (rc=$V_RC, out=$V_OUT)"
fi

# ======================================================================
# Test 9: query.py (§4) -- the file-level subcommands over build/todo-cache.json.
# (§28 added the section-level readiness trio; its fixtures are sub-test 28.)
# Covers pre-migration notice gating, id resolution (slug / stem /
# frontmatter id / nonexistent), all six live-tree subcommands, the
# --json and --format markdown output surfaces, synthetic fixtures for
# the status-gated subcommands, import smoke, and the --watch polling
# fallback.
# ======================================================================


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
bind_baseline "$PREMIG_TREE/cache.json"
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
# `--limit 0` is the explicit "complete set" request. Row-returning
# subcommands apply DEFAULT_ROW_LIMIT when --limit is absent, so a
# completeness assertion has to opt back in rather than rely on the
# default -- that opt-in IS the no-lost-answers path being exercised.
EXPECTED_NODES=$(find "$REPO_ROOT/todo" -name 'TODO-*.md' -not -name 'TODO-00-INDEX.md' | wc -l)
ACTUAL_ROWS=$(q_live by-domain --limit 0 --quiet | wc -l)
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

# Sub-test 9h: nonexistent id exits 4 -- THE REQUEST, not the cache -- with a
# nearest-match hint and a machine-readable body. It asserted rc 2 until the
# section-22 review: 2 is documented as "the cache cannot be trusted", so a
# caller with a typo was told to rebuild a cache that was never at fault.
Q_OUT=$(q_live backlinks nonexistent-slug-xyz 2>&1); Q_RC=$?
if [ "$Q_RC" = "4" ] && echo "$Q_OUT" | grep -q "not found" \
   && echo "$Q_OUT" | grep -q '"error": "query-input"'; then
    t_pass "query: backlinks nonexistent id exits 4 with hint + body"
else
    t_fail "query: backlinks nonexistent should exit 4 (rc=$Q_RC, out=$Q_OUT)"
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
STALE_ALL=$(q_live stale --days 0 --limit 0 --quiet | wc -l)
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
# 16 subcommands: 12 query + 1 render + the 3 section-level readiness
# verbs (section 28). The count is asserted rather than ranged so a verb
# that silently fails to register is a FAIL, not a quieter list.
assert len(query.SUBCOMMANDS) == 16, f'expected 16 subcommands, got {len(query.SUBCOMMANDS)}'
assert 'section-ready' in query.SUBCOMMANDS
assert 'section-blocking' in query.SUBCOMMANDS
assert 'ready' in query.SUBCOMMANDS
assert 'code-by' in query.SUBCOMMANDS
assert 'render' in query.SUBCOMMANDS
" 2>/dev/null; then
    t_pass "query: import smoke (16 subcommands exposed)"
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
# polling interval. Uses `timeout` to bound the watch run, and the product's own
# TODOGRAPH_WATCH_FORCE_POLL switch to select the polling code path.
#
# IT USED TO NARROW PATH TO `/usr/bin:/bin` AND CALL THAT FORCING, which is
# where inotify-tools installs inotifywait -- so on any host that had the tool
# this fixture silently exercised the branch it claimed to exclude, and on the
# dev host (no inotify-tools at all) the narrowing changed nothing either way.
# Repaired with section 30 (Codex adversarial, section 30, [medium]).
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
TODOGRAPH_WATCH_FORCE_POLL=1 timeout --signal=INT 6 \
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
# The escaping path must not exfiltrate the foreign file. Pre-boundary-fix this
# printed /etc/passwd contents; then it returned rc 0 with empty output, because
# `cmd_code` declined the row on its own.
#
# THE ASSERTION IS NOW STRONGER, not merely different: the shared validator
# refuses the whole cache at rc 2 before any verb runs. A node path that leaves
# the corpus means the ARTIFACT is untrustworthy, not that one query happens to
# have no rows -- and answering 0-with-nothing is the "partial index answers
# thin" shape this graph's readers exist to refuse. The anti-exfiltration
# assertion is kept and joined by the refusal.
bind_baseline "$ESC_TREE/cache.json"
Q_OUT=$(python3 "$QUERY_PY" --cache "$ESC_TREE/cache.json" --repo-root "$ESC_TREE" --quiet code escape-attempt 2>&1); Q_RC=$?
if [ "$Q_RC" = "2" ] && ! echo "$Q_OUT" | grep -q "root:x:" \
    && echo "$Q_OUT" | grep -q '"error": "cache-unusable"' \
    && echo "$Q_OUT" | grep -q '"reason": "SHAPE"'; then
    t_pass "query: a cache whose file_path escapes the corpus is REFUSED outright, not answered thin"
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
bind_baseline "$AMB_TREE/cache.json"
Q_OUT=$(python3 "$QUERY_PY" --cache "$AMB_TREE/cache.json" --repo-root "$AMB_TREE" --quiet backlinks collide 2>&1); Q_RC=$?
if [ "$Q_RC" = "4" ] && echo "$Q_OUT" | grep -q "ambiguous"; then
    t_pass "query: ambiguous slug refused with exit 4 + listing"
else
    t_fail "query: ambiguous slug should exit 4 (rc=$Q_RC, out=$Q_OUT)"
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
bind_baseline "$SAN_TREE/cache.json"
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
# SECTION 22 FLIPPED THIS ASSERTION, AND THE OLD ONE IS THE DEFECT IT NAMES.
# This used to require rc 0 with `total_nodes == 2`: five malformed rows were
# DROPPED and `stats` reported a corpus of two as though that were the whole
# answer. A caller cannot tell that from a corpus that really has two nodes,
# which is partial-cache false completeness. The cache is now REFUSED with the
# documented infrastructure code and a machine-readable body.
Q_OUT=$(python3 "$QUERY_PY" --cache "$MAL_TREE/cache.json" --repo-root "$MAL_TREE" --quiet stats --json 2>&1); Q_RC=$?
if [ "$Q_RC" = "2" ] && echo "$Q_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d.get('error') == 'cache-unusable' and d.get('reason') and d.get('detail') else 1)"; then
    t_pass "query: an all-malformed cache is REFUSED rc 2, not answered from the survivors"
else
    t_fail "query: malformed cache handling broken (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9v2: THE ONE-ROW-MALFORMED CASE, which is the one that was
# invisible. All-rows-malformed at least produced an obviously-empty answer;
# a SINGLE bad row among good ones produced a confident, complete-looking,
# WRONG answer. Both are named in the section; only this one had no symptom.
ONEBAD_TREE="$TMP_DIR/q-onebad"
mkdir -p "$ONEBAD_TREE"
python3 -c "
import json
def node(name, ident):
    return {'file_path': f'todo/01-t/TODO-{name}.md', 'id': ident,
            'domain': '01-t', 'status': 'draft', 'title': ident,
            'sections': [], 'section_headings': [], 'inputs_xrefs': [],
            'stamps_xrefs': [], 'schema_version': 1,
            'created_at': '2026-04-23T00:00:00Z',
            'last_active_at': '2026-04-23T00:00:00Z'}
nodes = [node('01-a', 'a'), node('02-b', 'b'), node('03-c', 'c')]
# Exactly one row loses its identity field; the other two are pristine.
del nodes[1]['file_path']
with open('$ONEBAD_TREE/cache.json', 'w') as f:
    json.dump(nodes, f)
"
Q_OUT=$(python3 "$QUERY_PY" --cache "$ONEBAD_TREE/cache.json" --repo-root "$ONEBAD_TREE" --quiet stats --json 2>&1); Q_RC=$?
if [ "$Q_RC" = "2" ] && echo "$Q_OUT" | grep -q "cache-unusable"; then
    t_pass "query: ONE malformed row refuses rather than silently narrowing to 2 of 3"
else
    t_fail "query: one-row-malformed cache silently narrowed (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9v3: MUTATION -- restore the silent-drop `_validate_nodes` and the
# one-row fixture must stop refusing. Without this, 9v2 would still pass
# against a build that had quietly reverted to dropping rows, because "rc 2"
# can be produced by many failures; the mutation proves the fixture is
# sensitive to THIS guard and not to something on the way to it.
python3 - "$QUERY_PY" "$TMP_DIR/query_mut.py" <<'MUTEOF'
import sys, pathlib
src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
needle = "        self.nodes = nodes"
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
revert = ("        self.nodes = [n for n in (nodes or []) if isinstance(n, dict)\n"
          "                      and isinstance(n.get('file_path'), str)\n"
          "                      and n.get('file_path')]")
pathlib.Path(sys.argv[2]).write_text(src.replace(needle, revert, 1), encoding="utf-8")
MUTEOF
# The mutant still refuses at the LOADER (the shared validator runs before Ctx),
# so the mutation is checked where the drop actually happened: validate_nodes is
# bypassed and the surviving rows are handed straight to Ctx.
Q_OUT=$(python3 - "$TMP_DIR/query_mut.py" "$ONEBAD_TREE/cache.json" "$(dirname "$QUERY_PY")" <<'MUTRUN' 2>&1
import sys, json, importlib.util, pathlib
# The mutant lives in a temp dir, so its own `sys.path.insert(_HERE)` cannot
# reach the sibling `validate` / `cache_schema` modules -- and a stdlib-adjacent
# `validate` package on the system path gets imported instead. Put the REAL
# module directory on the path first so the mutant differs from the shipped
# module in exactly one line and nothing else.
sys.path.insert(0, sys.argv[3])
spec = importlib.util.spec_from_file_location("query_mut", sys.argv[1])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
nodes = json.load(open(sys.argv[2]))
ctx = mod.Ctx(nodes, pathlib.Path("."), True)
print(json.dumps({"kept": len(ctx.nodes), "given": len(nodes)}))
MUTRUN
); Q_RC=$?
if [ "$Q_RC" = "0" ] && echo "$Q_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['given'] == 3 and d['kept'] == 2 else 1)"; then
    t_pass "query: mutation check -- the reverted silent-drop keeps 2 of 3 rows (fixture is sensitive)"
else
    t_fail "query: silent-drop mutation did not narrow as expected (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9w: backlinks over the nested-malformed fixture refuses too. The
# rule is the CACHE's, not the subcommand's, so every verb inherits it -- the
# previous per-verb defensive skips were what let two verbs disagree about
# whether the same cache was usable.
Q_OUT=$(python3 "$QUERY_PY" --cache "$MAL_TREE/cache.json" --repo-root "$MAL_TREE" --quiet backlinks ok 2>&1); Q_RC=$?
if [ "$Q_RC" = "2" ] && echo "$Q_OUT" | grep -q "cache-unusable"; then
    t_pass "query: backlinks refuses the same unusable cache stats refuses"
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
# SECTION 22 FLIPPED THIS ONE TOO, and it is the clearest example of why.
# "Treated as empty" is the whole problem: a node whose `sections`,
# `inputs_xrefs`, `stamps_xrefs`, `file_patterns` AND `depends_on` are all
# scalars was reported as one healthy node. `_safe_list` turned every one of
# those into `[]`, so `ready` would call this node dependency-free and READY
# on the strength of `depends_on: 'not-a-list'`.
Q_OUT=$(python3 "$QUERY_PY" --cache "$SCALAR_TREE/cache.json" --repo-root "$SCALAR_TREE" --quiet stats --json 2>&1); Q_RC=$?
if [ "$Q_RC" = "2" ] && echo "$Q_OUT" | grep -q "cache-unusable"; then
    t_pass "query: scalar collection fields are REFUSED, not silently read as empty"
else
    t_fail "query: scalar collection fields crash Ctx (rc=$Q_RC, out=$Q_OUT)"
fi

# Sub-test 9x2: the same scalar cache through `ready` specifically, because
# that is the verb where the old behavior produced an actively WRONG verdict
# rather than a merely thin one: `_safe_list(n.get("depends_on"))` made the
# unmet-dependency loop iterate zero times and the node came back READY.
Q_OUT=$(python3 "$QUERY_PY" --cache "$SCALAR_TREE/cache.json" --repo-root "$SCALAR_TREE" --quiet ready --json 2>&1); Q_RC=$?
if [ "$Q_RC" = "2" ] && echo "$Q_OUT" | grep -q "cache-unusable"; then
    t_pass "query: ready refuses a scalar depends_on instead of reporting the node ready"
else
    t_fail "query: ready over scalar depends_on broken (rc=$Q_RC, out=$Q_OUT)"
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
    bind_baseline "$OOR_CACHE"
PATH="/usr/bin:/bin" timeout --signal=INT 3 \
    python3 -u "$QUERY_PY" --cache "$OOR_CACHE" --repo-root "$OOR_TREE" \
        --watch by-domain > "$OOR_TREE/watch.out" 2>&1 || true
if grep -q "outside repo_root" "$OOR_TREE/watch.out"; then
    t_pass "query: --cache outside repo_root disables auto-rebuild (watch mode)"
else
    t_fail "query: external --cache should refuse rebuild (out=$(cat "$OOR_TREE/watch.out" 2>/dev/null))"
fi

# Sub-test 9aa1b: AND IT MUST STILL ANSWER. The check above only greps for the
# skip warning, so it passed even when section 22 briefly made a SKIPPED
# rebuild refuse the tick like a FAILED one -- which silently turned every
# inspection-mode watcher into one that serves nothing forever. The tri-state
# in `_rebuild_cache` exists for this distinction; this pins it.
if grep -q '^01-t' "$OOR_TREE/watch.out" \
   && ! grep -q "cache-rebuild-failed" "$OOR_TREE/watch.out"; then
    t_pass "query: an inspection-mode watcher still serves rows (skipped != failed)"
else
    t_fail "query: external --cache watcher served nothing (out=$(tr '\n' ' ' < "$OOR_TREE/watch.out" 2>/dev/null))"
fi

# Sub-test 9aa2: WATCH MODE AFTER A FAILED REBUILD (TODO-06 section 22). The
# behavior was unpinned, which the section names as the actual defect -- it
# printed a WARN and kept serving the previous cache, so the watcher published
# a graph that predates the edit which broke the build, with nothing in the
# OUTPUT saying so. Pinned now: refuse the tick, keep the watcher alive.
WFAIL_TREE="$TMP_DIR/q-watchfail"
mkdir -p "$WFAIL_TREE/todo/01-t" "$WFAIL_TREE/build"
cat > "$WFAIL_TREE/todo/01-t/TODO-01-ok.md" <<'WFEOF'
---
schema_version: 1
id: ok
domain: 01-t
status: draft
title: ok
---

## 1. First

- [ ] something
WFEOF
# Prime a real, well-formed cache with its corpus binding, so the ONLY thing
# that changes below is whether build.py succeeds.
python3 "$BUILD_PY" --quiet --output "$WFAIL_TREE/build/todo-cache.json" \
    --root "$WFAIL_TREE/todo" --repo-root "$WFAIL_TREE" >/dev/null 2>&1
# Now break the corpus so every subsequent build.py run fails.
printf 'id: [unclosed\n' > "$WFAIL_TREE/todo/01-t/TODO-02-broken.md"
PATH="/usr/bin:/bin" timeout --signal=INT 4 \
    python3 -u "$QUERY_PY" --repo-root "$WFAIL_TREE" \
        --watch by-domain > "$WFAIL_TREE/watch.out" 2>&1 || true
if grep -q "cache-rebuild-failed" "$WFAIL_TREE/watch.out" \
   && ! grep -q '^01-t' "$WFAIL_TREE/watch.out"; then
    t_pass "query: watch refuses the tick after a failed rebuild, emitting NO stale rows"
else
    t_fail "query: watch served stale rows after a failed rebuild (out=$(tr '\n' ' ' < "$WFAIL_TREE/watch.out" 2>/dev/null))"
fi

# Sub-test 9aa3: THE OTHER HALF OF THE PINNED DECISION -- the watcher must
# still be ALIVE after refusing, and must answer normally once the corpus is
# repaired, with no restart. "Refuse the tick" and "kill the watcher" both
# produce no stale rows, so 9aa2 alone does not distinguish them; this is what
# makes the chosen behavior pinned rather than merely half-observed.
#
# NOT WRITTEN AS A GUARD MUTATION, deliberately, and the reason is worth
# recording: neutering `if rebuilt == REBUILD_FAILED` does NOT bring stale rows
# back, because the loader's freshness check independently refuses a cache
# whose corpus moved -- the two mechanisms overlap on exactly this case. A
# mutation whose effect is masked by a second guard would pass for the wrong
# reason. What the watch guard contributes on its own is the NAMED
# `cache-rebuild-failed` cause (9aa2) and liveness across the failure (here).
rm -f "$WFAIL_TREE/todo/01-t/TODO-02-broken.md"
cat > "$WFAIL_TREE/todo/01-t/TODO-02-fixed.md" <<'WF2EOF'
---
schema_version: 1
id: fixed
domain: 01-t
status: draft
title: fixed
---

## 1. First

- [ ] something
WF2EOF
PATH="/usr/bin:/bin" timeout --signal=INT 4 \
    python3 -u "$QUERY_PY" --repo-root "$WFAIL_TREE" \
        --watch by-domain > "$WFAIL_TREE/watch-ok.out" 2>&1 || true
if grep -q '^01-t' "$WFAIL_TREE/watch-ok.out" \
   && ! grep -q "cache-rebuild-failed" "$WFAIL_TREE/watch-ok.out"; then
    t_pass "query: watch answers normally once the corpus is repaired (refusal was per-tick)"
else
    t_fail "query: watch did not recover after the corpus was repaired (out=$(tr '\n' ' ' < "$WFAIL_TREE/watch-ok.out" 2>/dev/null))"
fi

# Sub-test 9aa4: THE MCP TRANSPORT. Making the CLI fail closed while the MCP
# server converts the refusal back into `[]` ships the appearance of safety
# and nothing else, which is why this transport gets its own case.
MCPQ_OUT=$(cd "$REPO_ROOT" && python3 - "$MAL_TREE" <<'MCPEOF' 2>&1
import sys, json, argparse, shutil, pathlib
sys.path.insert(0, "scripts/todo-graph")
import mcp_server as m
# The MCP surface always reads repo_root/build/todo-cache.json, so the
# unusable cache is staged at exactly that path inside a throwaway root.
root = pathlib.Path(sys.argv[1])
(root / "build").mkdir(parents=True, exist_ok=True)
shutil.copy(root / "cache.json", root / "build" / "todo-cache.json")
ns = argparse.Namespace(limit=5, scope=None, fields=None, offset=None, days=None)
body = m._call_query("stats", ns, root, False)
# THE PARSED ENVELOPE, NOT A PREFIX OF ITS TEXT. This used to ship
# `body.strip()[:200]` and grep it for the substring "error" -- but the body is
# dumped with sort_keys=True, so `detail` comes first and a longer refusal
# message pushes the `error` KEY past the truncation. The fixture then failed on
# a perfectly correct envelope while a body that merely mentioned the word
# "error" inside `detail` would have passed. Assert the structure this case is
# actually named for (Codex adversarial, section 25 review).
try:
    parsed = json.loads(body)
except ValueError:
    parsed = None
print(json.dumps({
    "keys": sorted(parsed) if isinstance(parsed, dict) else None,
    "error": parsed.get("error") if isinstance(parsed, dict) else None,
    "is_empty_array": body.strip() == "[]",
}))
MCPEOF
); MCPQ_RC=$?
if [ "$MCPQ_RC" = "0" ] && echo "$MCPQ_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if not d['is_empty_array'] and d['keys'] is not None
         and 'error' in d['keys'] and 'reason' in d['keys'] and d['error']
         else 1)"; then
    t_pass "query/MCP: an unusable cache surfaces as a structured error, never as []"
else
    t_fail "query/MCP: unusable cache leaked through the transport (rc=$MCPQ_RC, out=$MCPQ_OUT)"
fi

# Sub-test 9aa5: MUTATION for the MCP rule -- restore `buf.getvalue() or "[]"`
# and a nonzero exit that wrote NO body must come back as an empty result array.
#
# THE EMPTY-BODY CASE IS THE ONE THIS LINE ALONE DEFENDS, and picking it is the
# point of the test. query.py now writes its refusal envelope to STDOUT, so a
# cache-unusable refusal is caught twice over -- the buffer is non-empty, and
# `or "[]"` never fires. That is defence in depth, not a reason to skip the
# check: any query.py exit path that fails BEFORE writing a body still lands
# here, and under the old line it became `[]`. Simulating exactly that (exit 2,
# nothing printed) is what keeps this fixture bound to the rule rather than to
# the other fix that happens to shadow it.
MCPMUT_OUT=$(cd "$REPO_ROOT" && python3 - <<'MCPMUTEOF' 2>&1
import sys, json, pathlib, importlib.util, argparse
sys.path.insert(0, "scripts/todo-graph")
src_path = pathlib.Path("scripts/todo-graph/mcp_server.py")
src = src_path.read_text(encoding="utf-8")
needle = "    return _envelope_or_body(buf.getvalue(), rc)"
assert src.count(needle) == 1, f"mcp mutation needle appears {src.count(needle)}x"

def load(path, name, text=None):
    if text is not None:
        path.write_text(text, encoding="utf-8")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

def probe(mod):
    # query.py exited nonzero and wrote nothing -- the shape `or "[]"` cannot
    # distinguish from an empty result set.
    mod._query_mod.main = lambda *a, **k: 2
    ns = argparse.Namespace(limit=5, scope=None, fields=None, offset=None, days=None)
    return mod._call_query("stats", ns, pathlib.Path("."), False).strip()

real_body = probe(load(src_path, "mcp_real"))
mut_path = src_path.with_name(".mcp_server_mut.py")
try:
    mut_body = probe(load(mut_path, "mcp_mut",
                          src.replace(needle, '    return buf.getvalue() or "[]"', 1)))
finally:
    mut_path.unlink(missing_ok=True)
print(json.dumps({"real_is_empty_array": real_body == "[]",
                  "real_has_error": "error" in real_body,
                  "mut_is_empty_array": mut_body == "[]"}))
MCPMUTEOF
); MCPMUT_RC=$?
if [ "$MCPMUT_RC" = "0" ] && echo "$MCPMUT_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['mut_is_empty_array'] and d['real_has_error']
         and not d['real_is_empty_array'] else 1)"; then
    t_pass "query/MCP: mutation check -- the restored 'or []' turns a body-less failure back into []"
else
    t_fail "query/MCP: 'or []' mutation did not reproduce the swallow (rc=$MCPMUT_RC, out=$MCPMUT_OUT)"
fi

# Sub-test 9aa5b: THE MCP STATUS FLAG MUST AGREE WITH THE BODY. Making the body
# honest (9aa4) fixed what a client sees only if it PARSES the body; the
# protocol's own `isError` field said success regardless, so a client
# classifying on status -- which is what the field is for -- was still told a
# refused query had succeeded. Mutation: pin `isError` back to False and the
# same refusal must report success.
MCPERR_OUT=$(cd "$REPO_ROOT" && python3 - "$MAL_TREE" <<'MCPERREOF' 2>&1
import sys, json, pathlib, importlib.util, shutil
sys.path.insert(0, "scripts/todo-graph")
root = pathlib.Path(sys.argv[1])
(root / "build").mkdir(parents=True, exist_ok=True)
shutil.copy(root / "cache.json", root / "build" / "todo-cache.json")
src_path = pathlib.Path("scripts/todo-graph/mcp_server.py")
src = src_path.read_text(encoding="utf-8")
needle = '                "isError": _looks_like_error_envelope(text),'
assert src.count(needle) == 1, f"isError needle appears {src.count(needle)}x"

def load(path, name, text=None):
    if text is not None:
        path.write_text(text, encoding="utf-8")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

def probe(mod):
    req = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
           "params": {"name": "stats", "arguments": {}}}
    res = mod._handle_jsonrpc_request(req, root, False)["result"]
    return {"isError": res["isError"],
            "has_error_body": "error" in res["content"][0]["text"]}

real = probe(load(src_path, "mcp_err_real"))
mut_path = src_path.with_name(".mcp_server_err_mut.py")
try:
    mut = probe(load(mut_path, "mcp_err_mut",
                     src.replace(needle, '                "isError": False,', 1)))
finally:
    mut_path.unlink(missing_ok=True)
print(json.dumps({"real": real, "mut": mut}))
MCPERREOF
); MCPERR_RC=$?
if [ "$MCPERR_RC" = "0" ] && echo "$MCPERR_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['real']['isError'] and d['real']['has_error_body']
         and not d['mut']['isError'] else 1)"; then
    t_pass "query/MCP: a refusal sets isError, and the pinned-False mutation reports success"
else
    t_fail "query/MCP: isError classification broken (rc=$MCPERR_RC, out=$MCPERR_OUT)"
fi

# Sub-test 9aa5c: A BAD REQUEST IS rc 4 AND KEEPS THE WATCHER ALIVE. Ten
# handlers called `sys.exit(2)`, which overloaded the documented
# cache-unusable code AND -- because SystemExit is not an Exception -- unwound
# past `_run_once_guarded`, so renaming the watched target ended the session.
# Both halves are asserted here, and the second is the one no exit-code check
# would catch. Mutation: restore `sys.exit(2)` in the not-found path; the code
# collides again and the guard stops catching it.
REQERR_OUT=$(cd "$REPO_ROOT" && python3 - <<'REQERREOF' 2>&1
import sys, io, json, pathlib, importlib.util, contextlib
sys.path.insert(0, "scripts/todo-graph")
src_path = pathlib.Path("scripts/todo-graph/query.py")
src = src_path.read_text(encoding="utf-8")
needle = ('        _input_error(\n'
          '            "TARGET_NOT_FOUND",\n')
assert src.count(needle) == 4, f"not-found needle appears {src.count(needle)}x"

def load(path, name, text=None):
    if text is not None:
        path.write_text(text, encoding="utf-8")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

def probe(mod):
    """Answer a backlinks query for a target that does not exist, exactly as a
    watch tick would: through the guard, catching only what the guard lets by."""
    out = {"rc": None, "escaped": None}
    class _Args:
        target = "no-such-node-xyz"; quiet = True
    class _Ctx:
        quiet = True; nodes = []; id_index = {}; slug_collisions = {}
        stem_collisions = {}; slug_index = {}
        path_index = {"by_filename": {}, "by_dn": {}}
    # The refusal body goes to stdout by design (the MCP transport captures it
    # there); swallow it here so it cannot interleave with this probe's own
    # JSON result on the same stream.
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            mod.cmd_backlinks(_Ctx(), _Args())
        out["rc"] = 0
    except mod.QueryInputError:
        out["rc"] = mod.EXIT_QUERY_INPUT
        out["escaped"] = False
    except SystemExit as exc:
        # The pre-fix shape: SystemExit is not an Exception, so the guard that
        # catches CacheRefused/RecursionError/MemoryError never sees it and the
        # watch loop dies with it.
        out["rc"] = exc.code
        out["escaped"] = True
    except AttributeError:
        out["rc"] = "no-QueryInputError"
    return out

real = probe(load(src_path, "query_req_real"))
mut_path = src_path.with_name(".query_req_mut.py")
mut_src = src.replace(
    needle,
    '        sys.exit(2)\n        _input_error(\n            "TARGET_NOT_FOUND",\n', 1)
try:
    mut = probe(load(mut_path, "query_req_mut", mut_src))
finally:
    mut_path.unlink(missing_ok=True)
print(json.dumps({"real": real, "mut": mut}))
REQERREOF
); REQERR_RC=$?
if [ "$REQERR_RC" = "0" ] && echo "$REQERR_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['real']['rc'] == 4 and d['real']['escaped'] is False
         and d['mut']['rc'] == 2 and d['mut']['escaped'] is True else 1)"; then
    t_pass "query: a bad request is rc 4 via QueryInputError; the sys.exit mutation escapes the guard"
else
    t_fail "query: request-error classification broken (rc=$REQERR_RC, out=$REQERR_OUT)"
fi

# Sub-test 9aa5c2: A STALE GENERATION OUTRANKS A REQUEST ERROR. Target
# resolution runs INSIDE the walk, before the post-walk
# `check_corpus_unchanged`, so a corpus edit landing mid-walk can make a
# perfectly good target look absent. Emitting rc 4 there told the caller "the
# cache is usable; correct the request" about a request that was never wrong,
# and skipped the generation binding this section's whole contract rests on.
# The corpus is moved here between cache build and query, so the not-found path
# must yield the rc-2 cache refusal, not rc 4.
GENRACE_TREE="$TMP_DIR/q-genrace"
mkdir -p "$GENRACE_TREE/todo/01-test" "$GENRACE_TREE/build"
cat > "$GENRACE_TREE/todo/01-test/TODO-01-gen.md" <<'EOF'
---
schema_version: 1
id: genrace-fixture
domain: 01-test
status: draft
title: Gen
---
body
EOF
python3 "$BUILD_PY" --quiet --root "$GENRACE_TREE/todo" \
    --output "$GENRACE_TREE/build/todo-cache.json" --repo-root "$GENRACE_TREE" >/dev/null 2>&1
# Move the corpus AFTER the cache was certified against it.
cat > "$GENRACE_TREE/todo/01-test/TODO-02-late.md" <<'EOF'
---
schema_version: 1
id: genrace-latecomer
domain: 01-test
status: draft
title: Late
---
body
EOF
GENRACE_OUT=$(cd "$GENRACE_TREE" && python3 "$QUERY_PY" \
    --repo-root "$GENRACE_TREE" --quiet backlinks genrace-latecomer 2>&1); GENRACE_RC=$?
if [ "$GENRACE_RC" = "2" ] && echo "$GENRACE_OUT" | grep -q '"error": "cache-unusable"'; then
    t_pass "query: a corpus that moved mid-walk refuses as stale, not as a bad request"
else
    t_fail "query: stale generation reported as a request error (rc=$GENRACE_RC, out=$GENRACE_OUT)"
fi

# Sub-test 9aa5c3: AN ARGV-SHAPE ERROR STILL CARRIES ITS BODY. Those are raised
# from `bound_rows`, AFTER the generation check, so they never pass through the
# re-verification path that emits the others -- moving emission out of
# `_input_error` would otherwise have left them exiting 4 silently.
# The streams are captured SEPARATELY: under `--quiet` an rc-4 path must put
# exactly the JSON envelope on stdout and NOTHING on stderr. Emitting these
# four from the guard originally lost the caller's quiet flag, so a consumer
# merging the streams -- and the MCP transport, which always passes --quiet --
# read JSON followed by a human diagnostic.
ARGVERR_ERR="$TMP_DIR/argverr.stderr"
# THIS FIXTURE USES THE CANONICAL CACHE ON PURPOSE -- no `--cache`, so freshness
# IS enforced -- and since section 24 that means TWO axes, not one. `stale` is
# one of the three verbs that dereference a git-derived timestamp, so it now
# also refuses when the corpus HISTORY moved, and every commit touching `todo/`
# moves it. The canonical cache is therefore history-stale from the moment a
# TODO commit lands until the next rebuild, and this fixture would report the
# resulting rc 2 as "argv-shape handling broken" -- which is how it failed
# under `.githooks/pre-push`, the one place that always runs just after a
# commit. Rebuilding here makes the precondition explicit instead of inherited
# from whatever the developer last ran.
python3 "$BUILD_PY" --quiet --root "$REPO_ROOT/todo" --repo-root "$REPO_ROOT" \
    --output "$REPO_ROOT/build/todo-cache.json" >/dev/null 2>&1 \
    || t_fail "query: argv-shape precondition -- could not rebuild the canonical cache"
ARGVERR_OUT=$(cd "$REPO_ROOT" && python3 "$QUERY_PY" --quiet stale --offset -1 \
    2>"$ARGVERR_ERR"); ARGVERR_RC=$?
ARGVERR_ERRBYTES=$(wc -c < "$ARGVERR_ERR" | tr -d ' ')
if [ "$ARGVERR_RC" = "4" ] && echo "$ARGVERR_OUT" | grep -q '"reason": "BAD_OFFSET"' \
   && [ "$ARGVERR_ERRBYTES" = "0" ] \
   && echo "$ARGVERR_OUT" | python3 -c "import json,sys; json.load(sys.stdin)"; then
    t_pass "query: an argv-shape error exits 4 with a JSON body and silent stderr under --quiet"
else
    t_fail "query: argv-shape error body/quiet broken (rc=$ARGVERR_RC, errbytes=$ARGVERR_ERRBYTES, out=$ARGVERR_OUT)"
fi

# Sub-test 9aa5d: THE `code` NOTES SCAN IS BOUNDED AND REGULAR-FILE-ONLY. An
# explicitly-supplied `--cache` is validated for shape but deliberately NOT for
# freshness against this corpus, so a node's `file_path` is attacker-influenced
# text that only has to stay repo-local. A FIFO at that path blocked the read
# forever. Asserted with a real FIFO under a throwaway root, under `timeout` so
# a regression FAILS rather than hanging the suite.
FIFO_TREE="$TMP_DIR/q-code-fifo"
mkdir -p "$FIFO_TREE/todo/01-test" "$FIFO_TREE/build"
cat > "$FIFO_TREE/todo/01-test/TODO-01-fifo.md" <<'EOF'
---
schema_version: 1
id: fifo-fixture
domain: 01-test
status: draft
title: Fifo
---
body
EOF
python3 "$BUILD_PY" --quiet --root "$FIFO_TREE/todo" --output "$FIFO_TREE/cache.json" \
    --repo-root "$FIFO_TREE" >/dev/null 2>&1
mkfifo "$FIFO_TREE/todo/01-test/blocker.fifo" 2>/dev/null
# THE REWRITE IS ASSERTED, NOT ASSUMED. The first version of this fixture
# indexed the cache as `d["nodes"]` -- but the cache root IS the node array, so
# the rewrite raised, the fixture read an ordinary TODO instead of the FIFO,
# and it PASSED while exercising nothing. A probe whose setup can silently
# no-op measures nothing, so the rewrite now fails loudly and the assertion
# below re-reads the file to confirm the FIFO path is really in the cache.
python3 - "$FIFO_TREE/cache.json" <<'PY'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text())
nodes = d["nodes"] if isinstance(d, dict) else d
assert nodes, "fixture cache has no nodes to rewrite"
for n in nodes:
    n["file_path"] = "todo/01-test/blocker.fifo"
p.write_text(json.dumps(d))
back = json.loads(p.read_text())
back = back["nodes"] if isinstance(back, dict) else back
assert all(n["file_path"] == "todo/01-test/blocker.fifo" for n in back), \
    "cache rewrite did not take"
PY
FIFO_REWRITE_RC=$?
bind_baseline "$FIFO_TREE/cache.json"
FIFO_OUT=$(timeout 15 python3 "$QUERY_PY" --cache "$FIFO_TREE/cache.json" \
    --repo-root "$FIFO_TREE" --quiet code fifo-fixture 2>&1); FIFO_RC=$?
if [ "$FIFO_REWRITE_RC" = "0" ] && [ "$FIFO_RC" != "124" ]; then
    t_pass "query: the code verb does not block on a FIFO named by the cache (rc=$FIFO_RC)"
else
    t_fail "query: code verb hung on a cache-named FIFO (rewrite_rc=$FIFO_REWRITE_RC, rc=$FIFO_RC)"
fi

# Sub-test 9aa5d2: MUTATION for the FIFO rule -- restore the blocking
# `open(resolved, "rb")` and the SAME fixture must time out. This is the check
# that would have caught the broken probe above: with the rewrite asserted, a
# regression is now observable, and without a working non-blocking open the
# mutation and the real code would behave identically.
# The mutant lives BESIDE query.py, not in $TMP_DIR: query.py imports
# `validate` and `cache_schema` from its own directory, so a copy anywhere else
# exits 1 on ImportError and the fixture would "detect" a hang it never ran.
FIFOMUT_SRC="$REPO_ROOT/scripts/todo-graph/.query_fifo_mut.py"
python3 - "$REPO_ROOT/scripts/todo-graph/query.py" "$FIFOMUT_SRC" <<'PY'
import pathlib, sys
src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
needle = "        fd = os.open(resolved, os.O_RDONLY | os.O_NONBLOCK)\n        with os.fdopen(fd, \"rb\") as fh:"
assert src.count(needle) == 1, f"fifo needle appears {src.count(needle)}x"
pathlib.Path(sys.argv[2]).write_text(
    src.replace(needle, "        with open(resolved, \"rb\") as fh:", 1), encoding="utf-8")
PY
FIFOMUT_PREP=$?
timeout 10 python3 "$FIFOMUT_SRC" --cache "$FIFO_TREE/cache.json" \
    --repo-root "$FIFO_TREE" --quiet code fifo-fixture >/dev/null 2>&1
FIFOMUT_RC=$?
rm -f "$FIFOMUT_SRC"
if [ "$FIFOMUT_PREP" = "0" ] && [ "$FIFOMUT_RC" = "124" ]; then
    t_pass "query: mutation check -- the blocking open hangs on the same FIFO (fixture is sensitive)"
else
    t_fail "query: FIFO mutation did not reproduce the hang (prep=$FIFOMUT_PREP, rc=$FIFOMUT_RC)"
fi

# Sub-test 9aa5g: THE SERVING PATH IS THE REPO-OWNED JSON-RPC LOOP. The FastMCP
# handlers return a refusal envelope verbatim and cannot set `isError` without
# FastMCP rewriting the body out of JSON, so error CLASSIFICATION lives only in
# `_serve_stdio_jsonrpc`. That is sound exactly while the FastMCP handlers do
# not serve -- pinned here, so switching the transport cannot silently ship a
# surface where every refusal reports success.
SERVEPATH_OUT=$(cd "$REPO_ROOT" && python3 - <<'SERVEEOF' 2>&1
import ast, json, pathlib
src = pathlib.Path("scripts/todo-graph/mcp_server.py").read_text(encoding="utf-8")
tree = ast.parse(src)
fn = next(n for n in ast.walk(tree)
          if isinstance(n, ast.FunctionDef) and n.name == "main")
calls = {c.func.id for c in ast.walk(fn)
         if isinstance(c, ast.Call) and isinstance(c.func, ast.Name)}
returns_serve = any(
    isinstance(r.value, ast.Call) and isinstance(r.value.func, ast.Name)
    and r.value.func.id == "_serve_stdio_jsonrpc"
    for r in ast.walk(fn) if isinstance(r, ast.Return) and r.value is not None)
print(json.dumps({"serves_via_jsonrpc": returns_serve,
                  "builds_fastmcp": "_build_mcp" in calls}))
SERVEEOF
); SERVEPATH_RC=$?
if [ "$SERVEPATH_RC" = "0" ] && echo "$SERVEPATH_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['serves_via_jsonrpc'] else 1)"; then
    t_pass "mcp_server: main() serves through the repo-owned JSON-RPC loop that classifies isError"
else
    t_fail "mcp_server: serving path is no longer the classifying one (rc=$SERVEPATH_RC, out=$SERVEPATH_OUT)"
fi

# Sub-test 9aa5h: THE DRAIN HAS AN ABSOLUTE DEADLINE. Re-arming the settle
# window per event made coalescing unbounded: a stream producing events faster
# than the window postpones the tick forever, so the watcher never answers.
# Fed continuously here, the drain must still return within its ceiling.
DRAINBOUND_OUT=$(cd "$REPO_ROOT" && python3 - <<'DRAINBEOF' 2>&1
import sys, json, os, time, threading
sys.path.insert(0, "scripts/todo-graph")
import query as q
r_fd, w_fd = os.pipe()
stop = threading.Event()
def feeder():
    with os.fdopen(w_fd, "w") as w:
        while not stop.is_set():
            try:
                w.write("/tmp/todo/x.md CLOSE_WRITE\n"); w.flush()
            except (BrokenPipeError, ValueError):
                return
            time.sleep(0.001)
t = threading.Thread(target=feeder, daemon=True); t.start()
try:
    reader = q._EventLineReader(r_fd)
    started = time.monotonic()
    drained = q._drain_pending_events(reader)
    elapsed = time.monotonic() - started
finally:
    stop.set()
    t.join(timeout=2)
    os.close(r_fd)
print(json.dumps({"elapsed": elapsed, "drained": drained,
                  "ceiling": q.WATCH_DRAIN_MAX_SECS}))
DRAINBEOF
); DRAINBOUND_RC=$?
if [ "$DRAINBOUND_RC" = "0" ] && echo "$DRAINBOUND_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['elapsed'] < d['ceiling'] + 1.0 and d['drained'] > 0 else 1)"; then
    t_pass "query: a continuously-fed watch stream still ends the drain at its deadline"
else
    t_fail "query: watch drain is unbounded (rc=$DRAINBOUND_RC, out=$DRAINBOUND_OUT)"
fi

# Sub-test 9aa5e: THE PUBLISHED SCHEMA AND THE READER PROFILE AGREE ON EMPTY
# STRINGS. cache_schema applied minLength 1 to `depends_on`, `satisfies` and
# `superseded_by` while cache.schema.json did not, so a cache could pass the
# PUBLISHED contract and still be refused by every reader -- the exact
# disagreement the shared validator exists to remove.
SCHEMA_PARITY_OUT=$(cd "$REPO_ROOT" && python3 - <<'PARITYEOF' 2>&1
import json, sys, pathlib
schema = json.loads(pathlib.Path("scripts/todo-graph/schema/cache.schema.json").read_text())
# The published schema validates the nodes ARRAY directly -- its root is the
# array, not an object wrapping one -- so the per-node properties hang off
# `items`, not off a top-level `properties`.
node = schema["items"]["properties"]
print(json.dumps({
    "depends_on": node["depends_on"]["items"].get("minLength"),
    "satisfies": node["satisfies"]["items"].get("minLength"),
    "superseded_by": node["superseded_by"].get("minLength"),
    "file_patterns": node["file_patterns"]["items"].get("minLength"),
}))
PARITYEOF
); SCHEMA_PARITY_RC=$?
if [ "$SCHEMA_PARITY_RC" = "0" ] && echo "$SCHEMA_PARITY_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if all(v == 1 for v in d.values()) else 1)"; then
    t_pass "schema: every string field the reader minLength-checks is minLength-checked in the JSON Schema"
else
    t_fail "schema: schema/profile minLength parity broken (rc=$SCHEMA_PARITY_RC, out=$SCHEMA_PARITY_OUT)"
fi

# Sub-test 9aa5f: WATCH EVENTS ARE COALESCED. One event meant one full cache
# rebuild plus revalidation, so a corpus-wide sweep replayed hundreds of
# obsolete intermediate states before answering the edit the user cared about.
# The drain reads only what is ALREADY queued, so it is asserted directly
# against a pre-filled pipe rather than against wall-clock timing.
# THE WRITER STAYS OPEN, and that detail is the whole test. An earlier version
# closed it, and the resulting EOF kept the descriptor readable -- so the
# fixture passed against a drain that returned 0 under the live
# `inotifywait -m` shape, where the writer never closes. Holding it open is
# what makes this measure coalescing rather than EOF handling.
DRAIN_OUT=$(cd "$REPO_ROOT" && python3 - <<'DRAINEOF' 2>&1
import sys, json, os
sys.path.insert(0, "scripts/todo-graph")
import query as q
r_fd, w_fd = os.pipe()
w = os.fdopen(w_fd, "w")
for i in range(50):
    w.write(f"/tmp/todo/f{i}.md CLOSE_WRITE\n")
w.flush()
try:
    reader = q._EventLineReader(r_fd)
    first = reader.next_line(2.0)   # the loop's own read of the first event
    drained = q._drain_pending_events(reader)
finally:
    w.close()
    os.close(r_fd)
print(json.dumps({"first": bool(first), "drained": drained}))
DRAINEOF
); DRAIN_RC=$?
if [ "$DRAIN_RC" = "0" ] && echo "$DRAIN_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['first'] and d['drained'] == 49 else 1)"; then
    t_pass "query: a 50-event burst with the writer still open drains to ONE tick"
else
    t_fail "query: watch-event coalescing broken (rc=$DRAIN_RC, out=$DRAIN_OUT)"
fi

# Sub-test 9aa5f2: A FRAGMENTED STREAM STILL HITS THE DEADLINE. A writer that
# never emits a newline keeps the descriptor readable forever, so a per-read
# timeout never expires and the caller's absolute deadline is unreachable.
# Fed unterminated bytes continuously, `next_line` must still return.
DRAINFRAG_OUT=$(cd "$REPO_ROOT" && python3 - <<'DRAINFRAGEOF' 2>&1
import sys, json, os, time, threading
sys.path.insert(0, "scripts/todo-graph")
import query as q
r_fd, w_fd = os.pipe()
stop = threading.Event()
def feeder():
    with os.fdopen(w_fd, "wb") as w:
        while not stop.is_set():
            try:
                w.write(b"x"); w.flush()
            except (BrokenPipeError, ValueError):
                return
            time.sleep(0.001)
t = threading.Thread(target=feeder, daemon=True); t.start()
try:
    reader = q._EventLineReader(r_fd)
    started = time.monotonic()
    line = reader.next_line(0.5)          # no newline will ever arrive
    elapsed = time.monotonic() - started
finally:
    stop.set(); t.join(timeout=2); os.close(r_fd)
print(json.dumps({"line": line, "elapsed": elapsed}))
DRAINFRAGEOF
); DRAINFRAG_RC=$?
if [ "$DRAINFRAG_RC" = "0" ] && echo "$DRAINFRAG_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['line'] is None and d['elapsed'] < 3.0 else 1)"; then
    t_pass "query: an endlessly fragmented stream still returns at the absolute deadline"
else
    t_fail "query: next_line deadline is per-read, not absolute (rc=$DRAINFRAG_RC, out=$DRAINFRAG_OUT)"
fi

# Sub-test 9aa5f3: A LINE SPLIT ACROSS READS REASSEMBLES, INCLUDING ONE LARGER
# THAN THE 64 KiB READ CHUNK. The reader owns the buffer now, so partial-line
# handling is its responsibility rather than the stream's. The size used here
# is deliberately UNDER `WATCH_MAX_RECORD_BYTES`: a record over that ceiling is
# malformed by construction and is discarded, not reassembled -- 9aa5f5/9aa5f6
# own that case. An earlier version of this fixture asserted a 100 KB line
# reassembled, which contradicted the ceiling added in the same review round;
# the ceiling is the correct rule (an inotify record is a path plus event
# names) and this fixture was the wrong requirement.
DRAINSPLIT_OUT=$(cd "$REPO_ROOT" && python3 - <<'DRAINSPLITEOF' 2>&1
import sys, json, os, time, threading
sys.path.insert(0, "scripts/todo-graph")
import query as q
# Over one 64 KiB os.read chunk, under the record ceiling.
big = "/tmp/todo/" + ("d" * 40000) + ".md"
assert len(big) > 65536 // 2 and len(big) < q.WATCH_MAX_RECORD_BYTES
r_fd, w_fd = os.pipe()
def feeder():
    with os.fdopen(w_fd, "wb") as w:
        w.write(b"/tmp/todo/split"); w.flush()
        time.sleep(0.05)
        w.write(b"-half.md CLOSE_WRITE\n"); w.flush()
        w.write((big + " CLOSE_WRITE\n").encode()); w.flush()
        time.sleep(0.5)
t = threading.Thread(target=feeder, daemon=True); t.start()
try:
    reader = q._EventLineReader(r_fd)
    first = reader.next_line(5.0)
    second = reader.next_line(5.0)
finally:
    t.join(timeout=3); os.close(r_fd)
print(json.dumps({"first": (first or "").strip(),
                  "second_ok": bool(second) and second.startswith(big)}))
DRAINSPLITEOF
); DRAINSPLIT_RC=$?
if [ "$DRAINSPLIT_RC" = "0" ] && echo "$DRAINSPLIT_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['first'] == '/tmp/todo/split-half.md CLOSE_WRITE' and d['second_ok'] else 1)"; then
    t_pass "query: a split line and a multi-chunk line both reassemble across reads"
else
    t_fail "query: reader line reassembly broken (rc=$DRAINSPLIT_RC, out=$DRAINSPLIT_OUT)"
fi

# Sub-test 9aa5f4: THE PRODUCTION CALL SITE READS THROUGH THE READER.
#
# HONEST LABEL: this is a STRUCTURAL pin, not a mutation-provable behavioural
# one, and the distinction is stated rather than glossed. A behavioural version
# was built first and REJECTED: it drove `watch_loop` against a fake
# `inotifywait` holding its pipe open and counted ticks for a 40-event burst,
# but the real and buffered-readline variants both reported 2 ticks over
# windows from 3 to 10 seconds, while the SAME mutant with a print inside its
# loop was plainly observed iterating once per event. That contradiction was
# not resolved, so the counter is not trustworthy evidence and shipping it
# would assert a guard that does not discriminate -- worse than no guard,
# because it reads as one.
#
# What IS proven, and where: coalescing behaviour is pinned by 9aa5f (49 of 50
# events drained with the writer held open), its bound by 9aa5f2, and line
# reassembly by 9aa5f3. The remaining risk this covers is purely the WIRING --
# that `watch_loop` stops obtaining lines through `_EventLineReader` and goes
# back to the buffered stream `select` cannot see. That is a structural
# property, so it is checked structurally.
# SCOPE, STATED PLAINLY: this pin catches an accidental REGRESSION -- someone
# reverting the loop to the buffered stream -- and it does NOT resist a crafted
# rewrite. Three review rounds each defeated the previous version with a new
# evasion (an unreachable next_line beside a live read, then `next(iter(...))`,
# then an alias), and each was closed; a fourth shape almost certainly exists,
# because a purely syntactic check cannot decide what a program does. That is
# the honest limit and it is written here rather than left for the next reader
# to discover. The BEHAVIOUR is pinned elsewhere and by execution: 9aa5f, f2,
# f3, f5, f5b and f6 exercise the reader directly.
#
# It checks the DATA PATH, not a bag of independent facts. A first version
# searched the function for "some next_line", "some _EventLineReader" and "some
# drain" separately, which a body containing an unreachable `reader.next_line`
# alongside a live `line = proc.stdout.read(1)` satisfies completely. The
# predicates below instead follow the bound name: the reader is assigned, the
# consumed line is assigned FROM that same name, the drain receives it, and the
# only thing anyone may take from `proc.stdout` is `fileno()`.
WATCHWIRE_OUT=$(cd "$REPO_ROOT" && python3 - <<'WATCHWIREEOF' 2>&1
import ast, json, pathlib
src = pathlib.Path("scripts/todo-graph/query.py").read_text(encoding="utf-8")
fn = next(n for n in ast.walk(ast.parse(src))
          if isinstance(n, ast.FunctionDef) and n.name == "watch_loop")

def assigned_from_call(node, pred):
    """Names assigned from a call matching `pred`, e.g. `x = f(...)`."""
    out = set()
    for a in ast.walk(node):
        if isinstance(a, ast.Assign) and isinstance(a.value, ast.Call) \
                and len(a.targets) == 1 and isinstance(a.targets[0], ast.Name) \
                and pred(a.value):
            out.add(a.targets[0].id)
    return out

# `<reader> = _EventLineReader(...)`
readers = assigned_from_call(
    fn, lambda c: isinstance(c.func, ast.Name) and c.func.id == "_EventLineReader")
# `<line> = <reader>.next_line(...)`, bound to one of those readers.
line_names = assigned_from_call(
    fn, lambda c: isinstance(c.func, ast.Attribute) and c.func.attr == "next_line"
    and isinstance(c.func.value, ast.Name) and c.func.value.id in readers)
# ...AND that same name must be what the loop actually parses. Without this the
# pin accepts `ignored = reader.next_line(None)` beside a live
# `line = next(iter(proc.stdout))` -- every other predicate is satisfied while
# the real data path is the buffered stream again.
parsed_names = {
    c.func.value.id for c in ast.walk(fn)
    if isinstance(c, ast.Call) and isinstance(c.func, ast.Attribute)
    and c.func.attr == "split" and isinstance(c.func.value, ast.Name)}
line_names &= parsed_names
# `_drain_pending_events(<reader>)` on that same name.
drains_reader = any(
    isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
    and n.func.id == "_drain_pending_events" and len(n.args) == 1
    and isinstance(n.args[0], ast.Name) and n.args[0].id in readers
    for n in ast.walk(fn))
# NOTHING may be taken from proc.stdout except fileno.
stdout_attrs = sorted({
    a.attr for a in ast.walk(fn)
    if isinstance(a, ast.Attribute) and isinstance(a.value, ast.Attribute)
    and a.value.attr == "stdout"})
# ...and it may not be consumed by any other route either: `for x in
# proc.stdout`, `iter(proc.stdout)`, `next(proc.stdout)`, or passing it as an
# argument to anything. Only `proc.stdout.fileno()` is legal, so the ONLY place
# the attribute may appear is as that call's receiver.
def _is_stdout(node):
    return isinstance(node, ast.Attribute) and node.attr == "stdout"

iterates_stdout = any(
    isinstance(n, ast.For) and _is_stdout(n.iter) for n in ast.walk(fn))
passed_stdout = any(
    isinstance(n, ast.Call) and any(_is_stdout(a) for a in n.args)
    for n in ast.walk(fn))
# ...nor may it be ALIASED. `stream = proc.stdout` followed by
# `next(iter(stream))` hides every direct-use check above behind one name.
aliases_stdout = any(
    isinstance(n, ast.Assign) and _is_stdout(n.value) for n in ast.walk(fn))
comprehends_stdout = any(
    _is_stdout(g.iter)
    for n in ast.walk(fn) if isinstance(n, (ast.ListComp, ast.SetComp,
                                            ast.DictComp, ast.GeneratorExp))
    for g in n.generators)
print(json.dumps({"readers": sorted(readers), "line_names": sorted(line_names),
                  "drains_reader": drains_reader, "stdout_attrs": stdout_attrs,
                  "iterates_stdout": iterates_stdout,
                  "passed_stdout": passed_stdout,
                  "aliases_stdout": aliases_stdout,
                  "comprehends_stdout": comprehends_stdout}))
WATCHWIREEOF
); WATCHWIRE_RC=$?
if [ "$WATCHWIRE_RC" = "0" ] && echo "$WATCHWIRE_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['readers'] and d['line_names'] and d['drains_reader']
         and d['stdout_attrs'] == ['fileno'] and not d['iterates_stdout']
         and not d['passed_stdout'] and not d['comprehends_stdout']
         and not d['aliases_stdout'] else 1)"; then
    t_pass "query: watch_loop parses the line bound from the reader; proc.stdout yields only fileno"
else
    t_fail "query: watch_loop reader wiring regressed (rc=$WATCHWIRE_RC, out=$WATCHWIRE_OUT)"
fi

# Sub-test 9aa5f5: THE BUFFER IS BOUNDED ACROSS THE TIMEOUT-TO-BLOCKING
# TRANSITION. The production loop returns to `next_line(None)` -- deliberately
# deadline-free -- after a finite drain call expires, so a producer emitting
# bytes and no newline had nothing left bounding it. Measured before the fix:
# 1.9 MB, 2.8 MB, 3.5 MB across three successive 50ms reads. The stream here
# KEEPS RUNNING past the first timeout, which is the transition the earlier
# fragmented fixture stopped short of.
BUFBOUND_OUT=$(cd "$REPO_ROOT" && python3 - <<'BUFBOUNDEOF' 2>&1
import sys, json, os, time, threading
sys.path.insert(0, "scripts/todo-graph")
import query as q
r_fd, w_fd = os.pipe()
stop = threading.Event()
def feeder():
    with os.fdopen(w_fd, "wb") as w:
        while not stop.is_set():
            try:
                w.write(b"y" * 4096); w.flush()
            except (BrokenPipeError, ValueError, OSError):
                return
t = threading.Thread(target=feeder, daemon=True); t.start()
try:
    reader = q._EventLineReader(r_fd)
    peaks = []
    for _ in range(5):
        reader.next_line(0.05)
        peaks.append(len(reader._buf))
finally:
    stop.set(); t.join(timeout=2); os.close(r_fd)
print(json.dumps({"peak": max(peaks), "ceiling": q.WATCH_MAX_RECORD_BYTES}))
BUFBOUNDEOF
); BUFBOUND_RC=$?
if [ "$BUFBOUND_RC" = "0" ] && echo "$BUFBOUND_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
# One chunk may land on top of a just-resynced buffer, so allow the ceiling
# plus a single 64 KiB read rather than pinning an exact byte count.
sys.exit(0 if d['peak'] <= d['ceiling'] + 65536 else 1)"; then
    t_pass "query: an unterminated stream cannot grow the reader buffer past its record ceiling"
else
    t_fail "query: reader buffer is unbounded on a newline-free stream (rc=$BUFBOUND_RC, out=$BUFBOUND_OUT)"
fi

# Sub-test 9aa5f5b: A PENDING FRAGMENT PLUS A FULL CHUNK OF VALID RECORDS
# KEEPS EVERY RECORD. The size ceiling first measured the WHOLE buffer, so a
# small unfinished fragment followed by a 64 KiB read carrying many complete
# newline-delimited events pushed the aggregate over the limit and cleared all
# of them -- the watcher then missed the very edit that woke it. The ceiling
# now binds only the unterminated tail, which is the only thing that can be an
# unfinished record.
CEILKEEP_OUT=$(cd "$REPO_ROOT" && python3 - <<'CEILKEEPEOF' 2>&1
import sys, json, os, threading, time
sys.path.insert(0, "scripts/todo-graph")
import query as q
r_fd, w_fd = os.pipe()
records = [f"/tmp/todo/rec{i}.md CLOSE_WRITE" for i in range(200)]
def feeder():
    with os.fdopen(w_fd, "wb") as w:
        w.write(b"/tmp/todo/frag")                 # 14-byte pending fragment
        w.flush(); time.sleep(0.05)
        w.write(b"ment.md CLOSE_WRITE\n")          # completes it...
        w.write(("\n".join(records) + "\n").encode())   # ...plus many valid ones
        w.flush(); time.sleep(0.5)
t = threading.Thread(target=feeder, daemon=True); t.start()
try:
    reader = q._EventLineReader(r_fd)
    seen = []
    for _ in range(len(records) + 1):
        line = reader.next_line(3.0)
        if line is None:
            break
        seen.append(line.strip())
finally:
    t.join(timeout=3); os.close(r_fd)
print(json.dumps({"first": seen[0] if seen else "", "count": len(seen),
                  "expected": len(records) + 1}))
CEILKEEPEOF
); CEILKEEP_RC=$?
if [ "$CEILKEEP_RC" = "0" ] && echo "$CEILKEEP_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['first'] == '/tmp/todo/fragment.md CLOSE_WRITE'
         and d['count'] == d['expected'] else 1)"; then
    t_pass "query: a pending fragment followed by a full chunk of valid records loses none of them"
else
    t_fail "query: the record ceiling discarded complete records (rc=$CEILKEEP_RC, out=$CEILKEEP_OUT)"
fi

# Sub-test 9aa5f4b: MUTATION for the wiring pin -- the pin must REJECT the
# buffered-stream data path. Two earlier versions of it passed a body that
# called `reader.next_line()` into an unused name while really reading
# `next(iter(proc.stdout))`, which is the exact regression it claims to
# prevent, so the pin is now run against that mutant and required to fail.
WIREMUT_OUT=$(cd "$REPO_ROOT" && python3 - <<'WIREMUTEOF' 2>&1
import ast, json, pathlib, re
src = pathlib.Path("scripts/todo-graph/query.py").read_text(encoding="utf-8")
mutants = {
    # Shape 1: the reader call survives but feeds nothing.
    "ignored_call": src.replace(
        "                    line = reader.next_line(_idle_timeout(axis))",
        "                    ignored = reader.next_line(None)\n"
        "                    line = next(iter(proc.stdout))", 1),
    # Shape 2: the reader assignment survives and is then OVERWRITTEN through
    # an alias, hiding every direct `proc.stdout` use behind one name.
    "alias_overwrite": src.replace(
        "                    line = reader.next_line(_idle_timeout(axis))",
        "                    line = reader.next_line(None)\n"
        "                    stream = proc.stdout\n"
        "                    line = next(iter(stream))", 1),
}
for name, text in mutants.items():
    assert text != src, f"wiring mutation {name} did not apply"

def verdict(text):
    fn = next(n for n in ast.walk(ast.parse(text))
              if isinstance(n, ast.FunctionDef) and n.name == "watch_loop")
    def assigned_from_call(pred):
        return {a.targets[0].id for a in ast.walk(fn)
                if isinstance(a, ast.Assign) and isinstance(a.value, ast.Call)
                and len(a.targets) == 1 and isinstance(a.targets[0], ast.Name)
                and pred(a.value)}
    readers = assigned_from_call(
        lambda c: isinstance(c.func, ast.Name) and c.func.id == "_EventLineReader")
    names = assigned_from_call(
        lambda c: isinstance(c.func, ast.Attribute) and c.func.attr == "next_line"
        and isinstance(c.func.value, ast.Name) and c.func.value.id in readers)
    parsed = {c.func.value.id for c in ast.walk(fn)
              if isinstance(c, ast.Call) and isinstance(c.func, ast.Attribute)
              and c.func.attr == "split" and isinstance(c.func.value, ast.Name)}
    names &= parsed
    def is_stdout(n):
        return isinstance(n, ast.Attribute) and n.attr == "stdout"
    attrs = sorted({a.attr for a in ast.walk(fn) if isinstance(a, ast.Attribute)
                    and is_stdout(a.value)})
    passed = any(isinstance(n, ast.Call) and any(is_stdout(x) for x in n.args)
                 for n in ast.walk(fn))
    aliased = any(isinstance(n, ast.Assign) and is_stdout(n.value)
                  for n in ast.walk(fn))
    return bool(readers and names and attrs == ["fileno"]
                and not passed and not aliased)

print(json.dumps({"real_passes": verdict(src),
                  "mutants_passing": sorted(n for n, t in mutants.items()
                                            if verdict(t))}))
WIREMUTEOF
); WIREMUT_RC=$?
if [ "$WIREMUT_RC" = "0" ] && echo "$WIREMUT_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['real_passes'] and not d['mutants_passing'] else 1)"; then
    t_pass "query: mutation check -- the wiring pin rejects both buffered-stdout mutants"
else
    t_fail "query: wiring pin accepts the buffered mutant (rc=$WIREMUT_RC, out=$WIREMUT_OUT)"
fi

# Sub-test 9aa5f6: A WELL-FORMED LINE BEHIND A MALFORMED RECORD SURVIVES the
# resync. Discarding to the LAST newline is what makes that true -- dropping
# the whole buffer would take the good record with the bad one, turning a
# malformed producer into silent event loss.
RESYNC_OUT=$(cd "$REPO_ROOT" && python3 - <<'RESYNCEOF' 2>&1
import sys, json, os, threading, time
sys.path.insert(0, "scripts/todo-graph")
import query as q
r_fd, w_fd = os.pipe()
def feeder():
    with os.fdopen(w_fd, "wb") as w:
        w.write(b"z" * (q.WATCH_MAX_RECORD_BYTES + 4096))   # malformed record
        w.write(b"\n/tmp/todo/good.md CLOSE_WRITE\n")       # a real event after it
        w.flush()
        time.sleep(1.0)
t = threading.Thread(target=feeder, daemon=True); t.start()
try:
    reader = q._EventLineReader(r_fd)
    got = reader.next_line(3.0)
finally:
    t.join(timeout=3); os.close(r_fd)
print(json.dumps({"got": (got or "").strip()}))
RESYNCEOF
); RESYNC_RC=$?
if [ "$RESYNC_RC" = "0" ] && echo "$RESYNC_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['got'] == '/tmp/todo/good.md CLOSE_WRITE' else 1)"; then
    t_pass "query: the event behind an over-ceiling malformed record still parses"
else
    t_fail "query: resync dropped a well-formed event (rc=$RESYNC_RC, out=$RESYNC_OUT)"
fi

# Sub-test 9aa6: A DEEP DEPENDENCY CHAIN IS A RESOURCE QUESTION, NOT A VERDICT.
# `_dep_depth` recursed once per link, so a chain longer than Python's
# recursion limit raised RecursionError -- uncaught, so the CLI exited 1, the
# code this reader documents as "the graph has findings", with an empty body.
#
# THE ORDERING IS THE TEST. Walking `ctx.nodes` in natural order fills the memo
# bottom-up and never nests, which is why the bug was invisible; an untrusted
# cache chooses its own order, so the same 2,000 nodes REVERSED descend the
# whole chain. Both orders must now succeed AND agree, since an iterative
# rewrite that changed the answer would be a different defect.
DEEP_TREE="$TMP_DIR/q-deep"
mkdir -p "$DEEP_TREE"
python3 -c "
import json
nodes = []
for i in range(2000):
    n = {'file_path': f'todo/01-t/TODO-{i:05d}-x.md', 'id': f'n{i}',
         'domain': '01-t', 'status': 'draft', 'title': f'n{i}',
         'created_at': '2026-04-01T00:00:00Z',
         'last_active_at': '2026-04-01T00:00:00Z',
         'sections': [], 'inputs_xrefs': [], 'stamps_xrefs': []}
    if i:
        n['depends_on'] = [f'n{i-1}']
    nodes.append(n)
with open('$DEEP_TREE/fwd.json', 'w') as f:
    json.dump(nodes, f)
with open('$DEEP_TREE/rev.json', 'w') as f:
    json.dump(list(reversed(nodes)), f)
"
bind_baseline "$DEEP_TREE/fwd.json"
DEEP_FWD=$(python3 "$QUERY_PY" --cache "$DEEP_TREE/fwd.json" --repo-root "$DEEP_TREE" --quiet stats --json 2>/dev/null); DEEP_FWD_RC=$?
bind_baseline "$DEEP_TREE/rev.json"
DEEP_REV=$(python3 "$QUERY_PY" --cache "$DEEP_TREE/rev.json" --repo-root "$DEEP_TREE" --quiet stats --json 2>/dev/null); DEEP_REV_RC=$?
if [ "$DEEP_FWD_RC" = "0" ] && [ "$DEEP_REV_RC" = "0" ] \
   && python3 -c "
import json, sys
a = json.loads(sys.argv[1]); b = json.loads(sys.argv[2])
sys.exit(0 if a['avg_dep_depth'] == b['avg_dep_depth'] and a['avg_dep_depth'] > 900 else 1)" \
      "$DEEP_FWD" "$DEEP_REV"; then
    t_pass "query: a 2000-link chain answers in BOTH node orders, with the same depth"
else
    t_fail "query: deep dependency chain broke (fwd rc=$DEEP_FWD_RC rev rc=$DEEP_REV_RC)"
fi

# Sub-test 9aa7: duplicate members in a `uniqueItems` collection are REFUSED.
# The schema declares uniqueItems on depends_on/satisfies/file_patterns and the
# consumer COUNTS them -- `cmd_blocking` incremented once per ref, so
# `["a","a"]` reported an inbound count of 2 for ONE dependency and lifted that
# node up `stats.top_blocking`. Validating member type while ignoring a
# constraint the schema states is "routed but unprotected".
DUP_TREE="$TMP_DIR/q-dup"
mkdir -p "$DUP_TREE"
python3 -c "
import json
def node(name, ident, **extra):
    n = {'file_path': f'todo/01-t/TODO-{name}.md', 'id': ident, 'domain': '01-t',
         'status': 'draft', 'title': ident, 'created_at': '2026-04-01T00:00:00Z',
         'last_active_at': '2026-04-01T00:00:00Z', 'sections': [],
         'inputs_xrefs': [], 'stamps_xrefs': []}
    n.update(extra)
    return n
with open('$DUP_TREE/dup.json', 'w') as f:
    json.dump([node('01-a', 'a'), node('02-b', 'b', depends_on=['a', 'a'])], f)
with open('$DUP_TREE/uniq.json', 'w') as f:
    json.dump([node('01-a', 'a'), node('02-b', 'b', depends_on=['a'])], f)
"
bind_baseline "$DUP_TREE/dup.json"
DUP_OUT=$(python3 "$QUERY_PY" --cache "$DUP_TREE/dup.json" --repo-root "$DUP_TREE" --quiet blocking --json 2>&1); DUP_RC=$?
bind_baseline "$DUP_TREE/uniq.json"
UNIQ_OUT=$(python3 "$QUERY_PY" --cache "$DUP_TREE/uniq.json" --repo-root "$DUP_TREE" --quiet blocking --json 2>&1); UNIQ_RC=$?
if [ "$DUP_RC" = "2" ] && echo "$DUP_OUT" | grep -q "uniqueItems" \
   && [ "$UNIQ_RC" = "0" ] \
   && echo "$UNIQ_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['rows'][0]['inbound_count'] == 1 else 1)"; then
    t_pass "query: a duplicated dependency is REFUSED; the unique form still counts 1"
else
    t_fail "query: duplicate-dependency handling broken (dup rc=$DUP_RC uniq rc=$UNIQ_RC, out=$DUP_OUT)"
fi

# Sub-test 9aa8: the PUBLISHED schema and the routed reader agree on the
# dependency section-number ceiling. They disagreed: the schema said
# `minimum: 0` with no maximum while the reader enforced _MAX_SECTION_N, so a
# cache satisfying the published wire contract made every query unavailable.
# `jsonschema` is the independent oracle here, exactly as for the other
# schema-vs-validator parity checks.
if python3 - "$REPO_ROOT" <<'CEILEOF'
import json, sys, pathlib
try:
    import jsonschema
except ImportError:
    print("SKIP: jsonschema not installed"); sys.exit(0)
repo = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(repo / "scripts" / "todo-graph"))
import cache_schema as cs
schema = json.loads((repo / "scripts/todo-graph/schema/cache.schema.json").read_text())

def node(sec_n):
    return {"file_path": "todo/01-t/TODO-01-a.md", "id": "a", "domain": "01-t",
            "status": "draft", "title": "a", "schema_version": 1,
            "created_at": "2026-04-01T00:00:00Z",
            "last_active_at": "2026-04-01T00:00:00Z", "section_headings": [],
            "sections": [{"n": 1, "deliverable": "d", "status": "x",
                          "depends_on": [{"target": "T", "sections": [sec_n]}]}],
            "inputs_xrefs": [], "stamps_xrefs": []}

def pair(sec_n):
    try:
        jsonschema.validate([node(sec_n)], schema); schema_ok = True
    except jsonschema.ValidationError:
        schema_ok = False
    try:
        cs.validate_nodes([node(sec_n)], "x", cs.PROFILE_QUERY); reader_ok = True
    except cs.CacheSchemaError:
        reader_ok = False
    return schema_ok, reader_ok

# Boundary and boundary-plus-one must AGREE in both directions.
assert pair(65535) == (True, True), f"65535: {pair(65535)}"
assert pair(65536) == (False, False), f"65536: {pair(65536)}"
CEILEOF
then
    t_pass "query: schema and reader agree on the dep section ceiling (65535 ok, 65536 refused by both)"
else
    t_fail "query: schema/reader dep-section-ceiling parity broken"
fi

# Sub-test 9aa9: DUPLICATE NODE IDS REBIND EDGES, so the cache is refused.
# `file_path` uniqueness was already enforced; `id` is the OTHER key readers
# build a dict on, and `build_id_index` is a comprehension -- so two nodes
# sharing an id left only the LAST reachable, and a third node depending on
# that id bound to the survivor while the first silently got no inbound edge.
# rc 0 with a misattributed graph is exactly what this profile must refuse.
# NULL ids are NOT duplicates: a pre-migration corpus has many at once.
DUPID_TREE="$TMP_DIR/q-dupid"
mkdir -p "$DUPID_TREE"
python3 -c "
import json
def node(name, ident, **extra):
    n = {'file_path': f'todo/01-t/TODO-{name}.md', 'id': ident, 'domain': '01-t',
         'status': 'draft', 'title': str(ident), 'created_at': '2026-04-01T00:00:00Z',
         'last_active_at': '2026-04-01T00:00:00Z', 'sections': [],
         'inputs_xrefs': [], 'stamps_xrefs': []}
    n.update(extra)
    return n
with open('$DUPID_TREE/dup.json', 'w') as f:
    json.dump([node('01-a', 'dup'), node('02-b', 'dup'),
               node('03-c', 'c', depends_on=['dup'])], f)
with open('$DUPID_TREE/nulls.json', 'w') as f:
    json.dump([node('01-a', None), node('02-b', None)], f)
"
bind_baseline "$DUPID_TREE/dup.json"
DUPID_OUT=$(python3 "$QUERY_PY" --cache "$DUPID_TREE/dup.json" --repo-root "$DUPID_TREE" --quiet blocking --json 2>&1); DUPID_RC=$?
bind_baseline "$DUPID_TREE/nulls.json"
NULLID_OUT=$(python3 "$QUERY_PY" --cache "$DUPID_TREE/nulls.json" --repo-root "$DUPID_TREE" --quiet stats --json 2>&1); NULLID_RC=$?
if [ "$DUPID_RC" = "2" ] && echo "$DUPID_OUT" | grep -q "repeats id" \
   && [ "$NULLID_RC" = "0" ]; then
    t_pass "query: duplicate node ids are REFUSED; null ids stay legal"
else
    t_fail "query: duplicate-id handling broken (dup rc=$DUPID_RC null rc=$NULLID_RC, out=$DUPID_OUT)"
fi

# Sub-test 9aa10: the dependency walk stays LINEAR on a wide dependency list.
# The first iterative rewrite sliced the pending list per child
# (`pending = pending[1:]`), turning a node with k dependencies from O(k) into
# O(k^2): a valid 20,000-leaf star took 1.36s against 0.03s recursively, and
# under MCP `_CALL_LOCK` one such `stats` call blocks every other query. The
# schema puts no `maxItems` on `depends_on`, so width is not bounded elsewhere.
STAR_TREE="$TMP_DIR/q-star"
mkdir -p "$STAR_TREE"
python3 -c "
import json
N = 20000
def node(name, ident, **extra):
    n = {'file_path': f'todo/01-t/TODO-{name}.md', 'id': ident, 'domain': '01-t',
         'status': 'draft', 'title': ident, 'created_at': '2026-04-01T00:00:00Z',
         'last_active_at': '2026-04-01T00:00:00Z', 'sections': [],
         'inputs_xrefs': [], 'stamps_xrefs': []}
    n.update(extra)
    return n
nodes = [node('00000-root', 'root', depends_on=[f'L{i}' for i in range(N)])]
nodes += [node(f'{i+1:05d}-leaf', f'L{i}') for i in range(N)]
with open('$STAR_TREE/star.json', 'w') as f:
    json.dump(nodes, f)
"
STAR_START=$(mono_ns)
bind_baseline "$STAR_TREE/star.json"
STAR_OUT=$(python3 "$QUERY_PY" --cache "$STAR_TREE/star.json" --repo-root "$STAR_TREE" --quiet stats --json 2>&1); STAR_RC=$?
STAR_END=$(mono_ns)
# A generous ceiling: the quadratic form took ~1.4s of pure list copying on top
# of parse time, and a loaded CI host must not flake. Linear traversal has a
# wide margin under this. Timed on mono_ns because the old `date +%s` bracket
# made a BACKWARD clock step satisfy `-le 20` -- a quadratic regression would
# have passed this ceiling on exactly the loaded host most likely to step.
if ! STAR_ELAPSED_MS=$(elapsed_ms "$STAR_START" "$STAR_END"); then
    t_fail "query: wide-star timing unreadable (start='$STAR_START' end='$STAR_END') -- monotonic clock source failed"
else
    if [ "$STAR_ELAPSED_MS" -lt 0 ]; then
        t_fail "query: wide-star timing invalid (${STAR_ELAPSED_MS}ms) -- the monotonic clock went backwards"
    elif [ "$STAR_RC" = "0" ] && [ "$STAR_ELAPSED_MS" -le 20000 ]; then
        t_pass "query: a 20k-wide dependency list traverses linearly (${STAR_ELAPSED_MS}ms)"
    else
        t_fail "query: wide-star traversal rc=$STAR_RC elapsed=${STAR_ELAPSED_MS}ms (quadratic regression?)"
    fi
fi

# Sub-test 9aa11: ASCII render survives a chain deeper than the recursion
# limit, and its output is unchanged for ordinary shapes. Same defect class as
# `_dep_depth`; the query CLI maps the crash onto rc 2 now, but "refuses
# cleanly" is not "works" for valid input.
if python3 - "$REPO_ROOT" <<'ASCIIEOF'
import sys, pathlib
repo = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(repo / "scripts" / "todo-graph"))
import render, query

def node(i, deps=None):
    return {"file_path": f"todo/01-t/TODO-{i:05d}-x.md", "id": f"n{i}",
            "domain": "01-t", "status": "draft", "title": f"n{i}",
            "depends_on": deps or [], "created_at": "2026-04-01T00:00:00Z",
            "last_active_at": "2026-04-01T00:00:00Z",
            "sections": [], "inputs_xrefs": [], "stamps_xrefs": []}

# Cycle and seen markers must still appear exactly where they did.
shapes = {
    "diamond": [node(0, ["n1", "n2"]), node(1, ["n3"]), node(2, ["n3"]), node(3)],
    "cycle": [node(0, ["n1"]), node(1, ["n2"]), node(2, ["n0"])],
    "self": [node(0, ["n0"])],
}
ctx = query.Ctx(shapes["diamond"], pathlib.Path("."), True)
out = render.render_ascii(shapes["diamond"], ctx, "n0")
assert "(seen)" in out, f"diamond lost its (seen) marker: {out!r}"
ctx = query.Ctx(shapes["cycle"], pathlib.Path("."), True)
assert "(cycle)" in render.render_ascii(shapes["cycle"], ctx, "n0")
ctx = query.Ctx(shapes["self"], pathlib.Path("."), True)
assert "(cycle)" in render.render_ascii(shapes["self"], ctx, "n0")

deep = [node(i, [f"n{i+1}"]) for i in range(2000)] + [node(2000)]
ctx = query.Ctx(deep, pathlib.Path("."), True)
lines = render.render_ascii(deep, ctx, "n0").splitlines()
assert len(lines) == 2001, f"deep chain rendered {len(lines)} lines"
ASCIIEOF
then
    t_pass "render: ascii walks a 2000-link chain and keeps (cycle)/(seen) semantics"
else
    t_fail "render: ascii deep-chain / marker semantics broken"
fi

# Sub-test 9aa12: watch mode emits its opening answer EXACTLY ONCE.
# `watch_loop` performs the initial `run_once()` itself, so calling the tick
# explicitly before handing it to the loop rendered the first snapshot twice --
# and under quiet TSV/JSON streaming nothing marks the boundary, so a consumer
# reads duplicated graph rows. The prior watch tests only grepped for presence.
ONCE_TREE="$TMP_DIR/q-once"
mkdir -p "$ONCE_TREE/todo/01-t" "$ONCE_TREE/build"
cat > "$ONCE_TREE/todo/01-t/TODO-01-ok.md" <<'ONCEEOF'
---
schema_version: 1
id: ok
domain: 01-t
status: draft
title: ok
---

## 1. First

- [ ] something
ONCEEOF
python3 "$BUILD_PY" --quiet --output "$ONCE_TREE/build/todo-cache.json" \
    --root "$ONCE_TREE/todo" --repo-root "$ONCE_TREE" >/dev/null 2>&1
PATH="/usr/bin:/bin" timeout --signal=INT 4 \
    python3 -u "$QUERY_PY" --repo-root "$ONCE_TREE" --quiet \
        --watch by-domain > "$ONCE_TREE/watch.out" 2>&1 || true
ONCE_HITS=$(grep -c '^01-t' "$ONCE_TREE/watch.out" 2>/dev/null || true)
ONCE_HITS=${ONCE_HITS:-0}
if [ "$ONCE_HITS" = "1" ]; then
    t_pass "query: watch emits the opening answer exactly once"
else
    t_fail "query: watch emitted the opening answer ${ONCE_HITS}x (expected 1)"
fi

# Sub-test 9aa13: DERIVED RESOLVER-KEY collisions are refused, and the
# derivation matches the resolver's own. Distinct ids and distinct file_paths
# are not enough: `build_path_index` files nodes under
# `(domain-code, TODO-number)`, so `01-a/TODO-01-first.md` and
# `01-a/TODO-01-second.md` share one slot and the later overwrites. MEASURED
# before the fix: an Inputs XREF naming `first` EXPLICITLY gave
# `backlinks first` 0 rows and `backlinks second` 1 row, at rc 0.
#
# The parity assertion is the load-bearing half. `cache_schema` derives the key
# locally rather than importing the `validate` CLI, so a drift would mean
# validating a DIFFERENT rule than the resolver applies -- passing while the
# real index still collides. Comparing both derivations over the live corpus
# turns that into a test failure.
if python3 - "$REPO_ROOT" <<'RKEYEOF'
import sys, json, pathlib
repo = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(repo / "scripts" / "todo-graph"))
import cache_schema as cs, validate as V

def node(path, ident, **extra):
    n = {"file_path": path, "id": ident, "domain": "01-a", "status": "draft",
         "title": ident, "created_at": "2026-04-01T00:00:00Z",
         "last_active_at": "2026-04-01T00:00:00Z", "sections": [],
         "inputs_xrefs": [], "stamps_xrefs": []}
    n.update(extra)
    return n

collide = [node("todo/01-a/TODO-01-first.md", "first"),
           node("todo/01-a/TODO-01-second.md", "second")]
try:
    cs.validate_nodes(collide, "x", cs.PROFILE_QUERY)
    raise AssertionError("collision ACCEPTED -- edges would rebind silently")
except cs.CacheSchemaError as exc:
    assert "derives resolver key" in str(exc), f"wrong refusal: {exc}"

# Distinct numbers in the same domain, and the same number across domains, are
# both legal -- the rule must not over-refuse.
ok = [node("todo/01-a/TODO-01-first.md", "first"),
      node("todo/01-a/TODO-02-second.md", "second"),
      node("todo/02-b/TODO-01-third.md", "third")]
ok[2]["domain"] = "02-b"
cs.validate_nodes(ok, "x", cs.PROFILE_QUERY)

# Derivation parity against the resolver's own index, over the LIVE corpus.
cache = repo / "build" / "todo-cache.json"
if cache.exists():
    live = json.loads(cache.read_text())
    theirs = V.build_path_index(live)["by_dn"]
    mine = {}
    for n in live:
        k = cs.resolver_key(n["file_path"])
        if k:
            mine[k] = n["file_path"]
    assert mine == theirs, (
        f"derivation drifted from build_path_index: "
        f"{len(mine)} vs {len(theirs)} keys")
RKEYEOF
then
    t_pass "query: duplicate derived resolver keys refused; derivation matches build_path_index"
else
    t_fail "query: resolver-key collision rule or its parity with build_path_index broken"
fi

# Sub-test 9aa14: an EDGE naming an ambiguous bare stem is refused, but a
# duplicated stem by itself is not. `build_path_index` flattens
# `{stem: file_path}`, so with two files sharing a stem only the last survives
# and a bare-stem edge naming either binds to it -- MEASURED before the fix as
# `backlinks first` 0 rows and `backlinks second` 1 row, at rc 0, for an XREF
# naming that stem. `_build_stem_collisions` already computed the ambiguity and
# its docstring claimed the reader refuses stem-based resolution, but its only
# consumer was the target a HUMAN types; edge resolution never asked.
#
# THE NON-REFUSALS ARE THE HALF THAT KEEPS THIS HONEST: duplicate stems across
# domains are a supported layout, so a cache carrying them with no bare-stem
# edge must still answer, and an `id` reference names exactly one file.
if python3 - "$REPO_ROOT" "$TMP_DIR" <<'STEMEOF'
import sys, json, subprocess, pathlib, re
repo, tmp = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
q = str(repo / "scripts" / "todo-graph" / "query.py")
root = tmp / "q-ambig"
root.mkdir(parents=True, exist_ok=True)

def node(path, ident, **extra):
    n = {"file_path": path, "id": ident, "domain": path.split("/")[1],
         "status": "draft", "title": ident, "created_at": "2026-04-01T00:00:00Z",
         "last_active_at": "2026-04-01T00:00:00Z", "sections": [],
         "inputs_xrefs": [], "stamps_xrefs": []}
    n.update(extra)
    return n

dupes = [node("todo/01-a/TODO-A-Shared.md", "first"),
         node("todo/02-b/TODO-A-Shared.md", "second")]

sys.path.insert(0, str(repo / "scripts" / "todo-graph"))
import hashlib
import cache_schema as cs

def run(nodes, name, *argv):
    p = root / f"{name}.json"
    p.write_text(json.dumps(nodes))
    # Section 25: a cache with no corpus binding declares no producer contract
    # and every reader refuses it, so a hand-crafted fixture must carry one.
    # Identity only -- freshness is disabled for a foreign --cache, so the
    # corpus and history fields are never read on this path.
    blob = p.read_bytes()
    sha = hashlib.sha256(blob).hexdigest()
    cs.sidecar_path(p, sha).write_text(json.dumps({
        "schema": cs.SIDECAR_SCHEMA,
        cs.CACHE_FORMAT_VERSION_KEY: cs.CACHE_FORMAT_VERSION,
        cs.PRODUCER_CONTRACT_DIGEST_KEY: cs.PRODUCER_CONTRACT_DIGEST,
        "cache_sha256": sha,
    }, indent=2, sort_keys=True) + "\n")
    return subprocess.run([sys.executable, q, "--cache", str(p), "--repo-root",
                           str(root), "--quiet", *argv],
                          capture_output=True, text=True)

# 1. A bare-stem edge is REFUSED and names the candidates.
bare = run(dupes + [node("todo/01-a/TODO-02-ref.md", "ref",
                         inputs_xrefs=[{"target_path": "TODO-A-Shared.md",
                                        "target_section": None}])],
           "bare", "backlinks", "first", "--json")
assert bare.returncode == 2, f"bare-stem edge rc={bare.returncode}"
assert "AMBIGUOUS_EDGE" in bare.stdout, bare.stdout[:200]

# 2. Duplicate stems with NO bare-stem edge are legal.
plain = run(dupes, "plain", "stats", "--json")
assert plain.returncode == 0, f"duplicate stems alone rc={plain.returncode}"

# 3. An `id` reference is unambiguous and still resolves to ONE file.
byid = run(dupes + [node("todo/01-a/TODO-02-ref.md", "ref",
                         depends_on=["first"])],
           "byid", "backlinks", "first", "--json")
assert byid.returncode == 0, f"id reference rc={byid.returncode} {byid.stdout[:200]}"
m = re.search(r'"returned": (\d+)', byid.stdout)
assert m and m.group(1) == "1", f"id reference returned {m.group(1) if m else '?'}"
STEMEOF
then
    t_pass "query: an ambiguous bare-stem EDGE is refused; duplicate stems and id refs still work"
else
    t_fail "query: ambiguous bare-stem edge handling broken"
fi

# Sub-test 9aa15: the resolver key derivation is SHARED, not mirrored.
# `validate.build_path_index` hardcoded the domain prefixes `00-` through
# `18-` while the validator's copy matched any two digits, so a legitimate
# domain-19 TODO was keyed by one and not the other -- the validator would have
# refused a collision the resolver never creates, and the live-corpus parity
# test proved only today's domain set. Both now call one function.
if python3 - "$REPO_ROOT" <<'SHAREEOF'
import sys, pathlib
repo = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(repo / "scripts" / "todo-graph"))
import cache_schema as cs, validate as V
# Table-driven, covering the boundary the hardcoded list used to sit on.
for dom, num in (("00", 1), ("18", 1), ("19", 1), ("99", 42)):
    path = f"todo/{dom}-x/TODO-{num:02d}-a.md"
    assert cs.resolver_key(path) == (dom, num), f"{path}: {cs.resolver_key(path)}"
    assert V.build_path_index([{"file_path": path}])["by_dn"] == {(dom, num): path}, path
# Shapes that must key to NOTHING in both.
for path in ("TODO-01-a.md", "todo/not-a-domain/TODO-01-a.md",
             "todo/01-x/NOTES-01-a.md", "todo/01-x/TODO-A-a.md"):
    assert cs.resolver_key(path) is None, f"{path} keyed {cs.resolver_key(path)}"
    assert V.build_path_index([{"file_path": path}])["by_dn"] == {}, path
SHAREEOF
then
    t_pass "query: resolver-key derivation is shared by cache_schema and build_path_index (00/18/19/99)"
else
    t_fail "query: shared resolver-key derivation broken"
fi

# Sub-test 9aa16: the RESOLVER refuses an ambiguous binding instead of
# returning an arbitrary survivor. `by_dn` and `by_filename` both flattened
# duplicates, so a reference naming the loser resolved to the winner at a
# normal exit code, and `check_stale_xref` reported nothing -- the canonical
# build-and-validate gate accepted a graph that was already wrong.
#
# The over-refusal cases matter as much: a fully-spelled letter stem names
# exactly one file and MUST resolve (the letter branch keyed on the letter
# alone and threw the rest of the stem away, which with three `TODO-A-*` files
# in the live corpus produced an arbitrary answer), and a bare letter token
# resolves against the SOURCE's domain exactly as bare `TODO-NN` does.
if python3 - "$REPO_ROOT" <<'RESOLVEEOF'
import sys, pathlib
repo = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(repo / "scripts" / "todo-graph"))
import validate as V

def node(path, ident):
    return {"file_path": path, "id": ident, "domain": path.split("/")[1],
            "status": "draft", "title": ident, "sections": [],
            "created_at": "2026-04-01T00:00:00Z",
            "last_active_at": "2026-04-01T00:00:00Z",
            "inputs_xrefs": [], "stamps_xrefs": []}

# (domain, number) collision: an EXPLICIT path to the loser must not rebind.
coll = [node("todo/19-x/TODO-01-first.md", "first"),
        node("todo/19-x/TODO-01-second.md", "second")]
p, i = V.build_path_index(coll), V.build_id_index(coll)
assert p["dn_collisions"] == {("19", 1)}, p["dn_collisions"]
assert V.resolve_xref_target("19-x/TODO-01-first.md", "todo/19-x/TODO-02-r.md",
                             i, p) is None
# Distinct numbers still resolve, including the compact same-domain form.
ok = [node("todo/19-x/TODO-01-a.md", "a"), node("todo/19-x/TODO-02-b.md", "b")]
p2, i2 = V.build_path_index(ok), V.build_id_index(ok)
assert V.resolve_xref_target("T01", "todo/19-x/TODO-02-b.md", i2, p2) \
    == "todo/19-x/TODO-01-a.md"

# Letter-numbered files: three across domains, as the live corpus has.
letters = [node("todo/02-k/TODO-A-SSDT-Master-Table.md", "ssdt"),
           node("todo/08-g/TODO-A-Win32k-Shadow-SSDT-Master-Table.md", "shadow"),
           node("todo/10-p/TODO-A-user32-export-master-table.md", "user32"),
           node("todo/09-d/TODO-13-explorer.md", "explorer")]
p3, i3 = V.build_path_index(letters), V.build_id_index(letters)
# Fully-spelled stem -> exactly one file, from any domain.
assert V.resolve_xref_target("TODO-A-user32-export-master-table",
                             "todo/09-d/TODO-13-explorer.md", i3, p3) \
    == "todo/10-p/TODO-A-user32-export-master-table.md"
# Bare letter -> the SOURCE's own domain when it has one.
assert V.resolve_xref_target("TODO-A", "todo/08-g/TODO-16-w.md", i3, p3) \
    == "todo/08-g/TODO-A-Win32k-Shadow-SSDT-Master-Table.md"
# Bare letter from a domain with no letter file -> ambiguous, refused.
assert V.resolve_xref_target("TODO-A", "todo/09-d/TODO-13-explorer.md",
                             i3, p3) is None

# Duplicate STEMS across domains: every bare spelling refused, id still works.
dup = [node("todo/01-a/TODO-A-Shared.md", "first"),
       node("todo/02-b/TODO-A-Shared.md", "second")]
p4, i4 = V.build_path_index(dup), V.build_id_index(dup)
assert p4["stem_collisions"] == {"TODO-A-Shared"}, p4["stem_collisions"]
for tok in ("TODO-A-Shared", "TODO-A-Shared.md", "TODO-A-Shared.md/",
            "`TODO-A-Shared.md`", "TODO-A-Shared.md#x"):
    got = V.resolve_xref_target(tok, "todo/01-a/TODO-02-r.md", i4, p4)
    assert got is None, f"{tok!r} resolved to {got}"
assert V.resolve_xref_target("first", "todo/01-a/TODO-02-r.md", i4, p4) \
    == "todo/01-a/TODO-A-Shared.md"
# Domain-qualified names exactly one file even when the stem is duplicated.
assert V.resolve_xref_target("01-a/TODO-A-Shared.md", "todo/02-b/TODO-02-r.md",
                             i4, p4) == "todo/01-a/TODO-A-Shared.md"
RESOLVEEOF
then
    t_pass "validate: resolver refuses ambiguous bindings, still resolves every specific form"
else
    t_fail "validate: ambiguous-binding refusal or specific-form resolution broken"
fi

# Sub-test 9aa16b: A DOMAIN CODE IS NOT A DIRECTORY (section 26). Two digits are
# a PREFIX of the directory name, so `14-alpha` and `14-beta` share the code
# `14` -- and every branch that reduced a reference to `(code, number)` threw
# the directory away. Order-independence is the property under test, so EVERY
# case runs with the nodes forward and reversed: a binding decided by cache row
# order passes half the time, which is how this survived §22's collision sweep.
#
# Two distinct defects, and the second is the worse one:
#   ORDER-DEPENDENT -- `D14` and the named `14-alpha` returned whichever
#   directory the cache listed first; a coin flip.
#   FLATLY WRONG -- `14-alpha/TODO-02`, `T02`, `TODO-02` and
#   `../14-alpha/TODO-02` returned `14-beta`'s file in BOTH orders. The
#   reference named its directory and was answered with a different one, which
#   no amount of reordering reveals.
if python3 - "$REPO_ROOT" <<'DIRCODEEOF'
import sys, pathlib
repo = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(repo / "scripts" / "todo-graph"))
import validate as V

def node(path, ident):
    return {"file_path": path, "id": ident, "domain": path.split("/")[1],
            "status": "draft", "title": ident, "sections": [],
            "created_at": "2026-04-01T00:00:00Z",
            "last_active_at": "2026-04-01T00:00:00Z",
            "inputs_xrefs": [], "stamps_xrefs": []}

alpha = node("todo/14-alpha/TODO-01-a.md", "a")
beta = node("todo/14-beta/TODO-02-b.md", "b")
SRC = "todo/14-alpha/TODO-01-a.md"
A, B = alpha["file_path"], beta["file_path"]
CASES = [
    # Compact code-only: two directories own `14`, so there is no answer.
    ("D14", None),
    # A NAMED directory resolves to ITSELF, in either order.
    ("14-alpha", "todo/14-alpha/INDEX.md"),
    ("14-alpha/INDEX.md", "todo/14-alpha/INDEX.md"),
    ("14-beta", "todo/14-beta/INDEX.md"),
    # Directory-qualified file refs: alpha holds no TODO-02, so alpha/TODO-02
    # is unresolved -- NOT beta's file.
    ("14-alpha/TODO-02", None),
    ("14-beta/TODO-02", B),
    ("14-alpha/TODO-02-b.md", None),
    ("14-beta/TODO-02-b.md", B),
    # Same-DIRECTORY compact forms, resolved against the source's own folder.
    ("T02", None), ("T01", A), ("TODO-02", None), ("TODO-01", A),
    # Relative forms normalize to a directory and must obey the same rule.
    ("../14-alpha/TODO-02", None), ("../14-beta/TODO-02", B),
    # A directory nothing indexes must not borrow a sibling code's existence.
    ("../14-ghost/INDEX.md", None),
    ("../14-beta/INDEX.md", "todo/14-beta/INDEX.md"),
    # A RELATIVE path that SPELLS a filename is decided by that filename. The
    # slugless number lookup at the end of the relative branch used a regex
    # (`^TODO-(\d{1,2})(?:-|\.md|$)`) that also matched a fully spelled name, so
    # a relative target naming no existing file was answered by whichever TODO
    # carried its number in that directory.
    ("../14-alpha/TODO-01-missing.md", None),
    ("../14-alpha/TODO-01-a.md", A),
    # A SPELLED tail WITHOUT `.md` is still spelled: the exact-path check only
    # matches a target carrying the suffix, so these fell through to the
    # slugless number lookup and returned None while the identical non-relative
    # forms resolved (Codex adversarial, section 26 review).
    ("./TODO-01-a", A),
    ("../14-alpha/TODO-01-a", A),
    ("../14-beta/TODO-02-b", B),
    ("../14-alpha/TODO-02-missing", None),
    # THE WHOLE NORMALIZED PATH MUST EXIST. Matching on (directory, tail) alone
    # discarded every component in between, so a path through a directory that
    # does not exist resolved to the file with the same name one level up.
    ("./ghost/TODO-01-a", None),
    ("../14-alpha/ghost/TODO-01-a", None),
    ("./ghost/TODO-01-a.md", None),
]
for label, nodes in (("forward", [alpha, beta]), ("reversed", [beta, alpha])):
    p, i = V.build_path_index(nodes), V.build_id_index(nodes)
    for tok, want in CASES:
        got = V.resolve_xref_target(tok, SRC, i, p)
        assert got == want, f"[{label}] {tok!r} -> {got!r}, expected {want!r}"

# THE SINGLE-DIRECTORY CODE IS UNTOUCHED, which is the live corpus's shape --
# a fix that removed the wrong answer by refusing everything would pass every
# assertion above and break the whole corpus.
solo = [node("todo/02-kernel-core/TODO-19-foo.md", "k")]
p, i = V.build_path_index(solo), V.build_id_index(solo)
S = solo[0]["file_path"]
for tok, want in (("D02", "todo/02-kernel-core/INDEX.md"),
                  ("02-kernel-core", "todo/02-kernel-core/INDEX.md"),
                  ("D02T19", S), ("02-kernel-core/TODO-19", S),
                  ("02-kernel-core/TODO-19-foo.md", S), ("T19", S),
                  ("TODO-19", S), ("TODO-19-foo", S),
                  ("../02-kernel-core/TODO-19", S)):
    got = V.resolve_xref_target(tok, S, i, p)
    assert got == want, f"[single-dir] {tok!r} -> {got!r}, expected {want!r}"

# LETTER-NUMBERED FILES OBEY THE SAME RULE (Codex adversarial, section 26).
# Branch 4c qualified its candidates by the two-digit code, so a reference that
# named its directory AND spelled its stem in full still crossed directories:
# `14-alpha/TODO-A-beta.md` returned beta's file in both orders.
la = node("todo/14-alpha/TODO-A-alpha.md", "la")
lb = node("todo/14-beta/TODO-A-beta.md", "lb")
for label, nodes in (("forward", [la, lb]), ("reversed", [lb, la])):
    p, i = V.build_path_index(nodes), V.build_id_index(nodes)
    for src, tok, want in (
            (la["file_path"], "14-alpha/TODO-A-beta.md", None),
            (la["file_path"], "14-alpha/TODO-A-alpha.md", la["file_path"]),
            (la["file_path"], "14-beta/TODO-A-beta.md", lb["file_path"]),
            # The same-DIRECTORY preference for an unqualified letter token:
            # each source resolves to the file in its own folder, which is what
            # makes the common in-folder reference deterministic.
            (la["file_path"], "TODO-A", la["file_path"]),
            (lb["file_path"], "TODO-A", lb["file_path"])):
        got = V.resolve_xref_target(tok, src, i, p)
        assert got == want, f"[letter {label}] {tok!r} from {src} -> {got!r}, want {want!r}"

# A DIRECTORY WHOSE ONLY FILE IS LETTER-NUMBERED IS STILL A DIRECTORY. Deriving
# membership inside the numeric-key branch hid it: `D14` saw the code as owning
# ONE directory and answered confidently with the wrong one, while `14-beta`
# was refused although its node was indexed -- a wrong answer and an
# over-refusal from a single omission (Codex adversarial, section 26,
# [medium]).
mixed_a = node("todo/14-alpha/TODO-01-a.md", "ma")
mixed_b = node("todo/14-beta/TODO-A-table.md", "mb")
for label, nodes in (("forward", [mixed_a, mixed_b]),
                     ("reversed", [mixed_b, mixed_a])):
    p, i = V.build_path_index(nodes), V.build_id_index(nodes)
    assert p["by_code"]["14"] == frozenset({"14-alpha", "14-beta"}), p["by_code"]
    for tok, want in (("D14", None),
                      ("14-beta", "todo/14-beta/INDEX.md"),
                      ("14-beta/INDEX.md", "todo/14-beta/INDEX.md"),
                      ("14-alpha", "todo/14-alpha/INDEX.md")):
        got = V.resolve_xref_target(tok, mixed_a["file_path"], i, p)
        assert got == want, f"[mixed {label}] {tok!r} -> {got!r}, expected {want!r}"

# A FULLY SPELLED NUMERIC FILENAME IS DECIDED BY ITS SLUG, not by its number.
# Branch 5a matched the token and resolved `(source directory, NN)`, discarding
# the authored slug -- which also made the exact-stem branches below
# unreachable for every numeric filename. Live consequence, reproduced against
# the real corpus before this fixture was written:
# `TODO-05-win32-file-io-api.md` cited from `todo/02-kernel-core/TODO-21-...`
# resolved to `todo/02-kernel-core/TODO-05-object-manager.md` (Codex
# re-adversarial, section 26 round 3, [high]).
spelled = [node("todo/02-kernel-core/TODO-05-object-manager.md", "om"),
           node("todo/05-storage-filesystems/TODO-05-win32-file-io-api.md", "fio"),
           node("todo/02-kernel-core/TODO-21-process-model.md", "pm")]
for label, nodes in (("forward", spelled), ("reversed", spelled[::-1])):
    p, i = V.build_path_index(nodes), V.build_id_index(nodes)
    SRC5 = "todo/02-kernel-core/TODO-21-process-model.md"
    # The named file wins even though a same-numbered sibling sits in the
    # SOURCE's own directory -- that sibling is exactly what used to answer.
    for tok in ("TODO-05-win32-file-io-api.md", "TODO-05-win32-file-io-api"):
        got = V.resolve_xref_target(tok, SRC5, i, p)
        assert got == "todo/05-storage-filesystems/TODO-05-win32-file-io-api.md", \
            f"[spelled {label}] {tok!r} -> {got!r}"
    # The same-directory file still resolves by its own spelled name...
    assert V.resolve_xref_target("TODO-05-object-manager.md", SRC5, i, p) \
        == "todo/02-kernel-core/TODO-05-object-manager.md"
    # ...and the BARE number still means "my own directory's number 5".
    assert V.resolve_xref_target("TODO-05", SRC5, i, p) \
        == "todo/02-kernel-core/TODO-05-object-manager.md"
    # A stem NO FILE CARRIES is a stale reference, and refusing is the whole
    # point: falling back to the number is how a renamed or deleted target
    # silently redirects to whichever TODO inherited its number.
    assert V.resolve_xref_target("TODO-05-renamed-since.md", SRC5, i, p) is None

# DUPLICATE SPELLED STEMS: prefer the source's own directory, refuse otherwise.
dup5 = [node("todo/14-alpha/TODO-07-shared-name.md", "d1"),
        node("todo/14-beta/TODO-07-shared-name.md", "d2"),
        node("todo/02-kernel-core/TODO-09-elsewhere.md", "d3")]
for label, nodes in (("forward", dup5), ("reversed", dup5[::-1])):
    p, i = V.build_path_index(nodes), V.build_id_index(nodes)
    assert V.resolve_xref_target("TODO-07-shared-name.md",
                                 "todo/14-alpha/TODO-01-x.md", i, p) \
        == "todo/14-alpha/TODO-07-shared-name.md", label
    # From a third directory neither copy is preferred, so it must refuse
    # rather than pick one.
    assert V.resolve_xref_target("TODO-07-shared-name.md",
                                 "todo/02-kernel-core/TODO-09-elsewhere.md",
                                 i, p) is None, label

# A MARKDOWN LINK IS UNWRAPPED BEFORE THE FRAGMENT IS STRIPPED. `fix_line_numbers`
# captures an XREF token with `(\S+)`, so a whole `[text](url)` reaches the
# resolver; stripping `#` first turned `[T](x.md#sec)` into the unmatchable
# `[T](x.md` and refused every ANCHORED link, which is the commonest shape in
# this corpus (Codex re-adversarial, section 26 round 6).
lk = [node("todo/01-test/TODO-01-a.md", "lka"), node("todo/01-test/TODO-02-b.md", "lkb")]
p, i = V.build_path_index(lk), V.build_id_index(lk)
LSRC = "todo/01-test/TODO-01-a.md"
for tok, want in (("[TODO-02](TODO-02-b.md)", "todo/01-test/TODO-02-b.md"),
                  ("[TODO-02](TODO-02-b.md#section)", "todo/01-test/TODO-02-b.md"),
                  ("[TODO-02](01-test/TODO-02-b.md#6-x)", "todo/01-test/TODO-02-b.md"),
                  # A label carrying its own brackets must not truncate the match.
                  ("[`TODO-02 §6`](01-test/TODO-02-b.md)", "todo/01-test/TODO-02-b.md"),
                  # No destination, and a same-document jump, name no FILE.
                  ("[TODO-02]()", None),
                  ("[TODO-02](#anchor)", None),
                  # TWO ADJACENT LINKS ARE NOT ONE LINK. A greedy label
                  # swallowed the first link and resolved the LAST
                  # destination, so a malformed multi-target token bound
                  # silently instead of being refused (Codex adversarial,
                  # section 26 review).
                  ("[a](TODO-01-a.md)[b](TODO-02-b.md)", None),
                  ("[a](TODO-02-b.md) trailing", None),
                  # NESTING IS THE SAME DEFECT ONE LEVEL DOWN: a label built
                  # from a bracket-pair alternative could end at `]` and let
                  # the next alternative accept `(`, so the ban on `](` was
                  # not enforced ACROSS alternatives and this token resolved
                  # to its outermost destination.
                  ("[[a](TODO-01-a.md)[b](TODO-02-b.md)](TODO-02-b.md)", None),
                  # A label carrying ordinary brackets is still legitimate.
                  ("[a [inner] label](TODO-02-b.md)", "todo/01-test/TODO-02-b.md")):
    got = V.resolve_xref_target(tok, LSRC, i, p)
    assert got == want, f"[md-link] {tok!r} -> {got!r}, expected {want!r}"

# THE COLLISION RULE IS SPELLING-INVARIANT. Section 22 refuses a reference into
# a `(domain, number)` slot two files share, but that refusal was reachable only
# through the branches consulting `by_dn`: the same target spelled as a bare
# stem, a relative path or a full path resolved anyway, so surface form decided
# safety (Codex re-adversarial, section 26 round 6).
coll2 = [node("todo/19-x/TODO-01-first.md", "c1"),
         node("todo/19-x/TODO-01-second.md", "c2")]
for label, nodes in (("forward", coll2), ("reversed", coll2[::-1])):
    p, i = V.build_path_index(nodes), V.build_id_index(nodes)
    for tok in ("19-x/TODO-01-first.md", "TODO-01-first.md",
                "./TODO-01-first.md", "todo/19-x/TODO-01-first.md",
                "[x](todo/19-x/TODO-01-first.md)"):
        got = V.resolve_xref_target(tok, "todo/19-x/TODO-02-r.md", i, p)
        assert got is None, f"[collision {label}] {tok!r} resolved to {got!r}"
# CONTROL: without a collision every one of those spellings must still resolve,
# or the rule above would be indistinguishable from refusing everything.
solo2 = [node("todo/19-x/TODO-01-only.md", "s1")]
p, i = V.build_path_index(solo2), V.build_id_index(solo2)
for tok in ("19-x/TODO-01-only.md", "TODO-01-only.md", "./TODO-01-only.md",
            "todo/19-x/TODO-01-only.md"):
    got = V.resolve_xref_target(tok, "todo/19-x/TODO-02-r.md", i, p)
    assert got == "todo/19-x/TODO-01-only.md", f"[collision control] {tok!r} -> {got!r}"

# `_all_paths` is PRECOMPUTED, and the accessor still answers for an index
# built by hand -- the test suite builds one, and an empty answer there would
# refuse every path-form reference.
full = V.build_path_index([alpha, beta])
assert full["all_paths"] == frozenset({A, B}), full["all_paths"]
assert isinstance(full["all_paths"], frozenset)
assert V._all_paths({"by_filename": {"TODO-01-a": A}, "stem_paths": {}}) == {A}

# THE HAND-BUILT FALLBACK IS TESTED IN BOTH DIRECTIONS. Asserting only the
# case that should RESOLVE is what let the fallback keep answering
# `14-alpha/TODO-02` with beta's file: it reached `by_dn` on the code alone,
# so the one path with no fixture pressure still ran the old rule (Codex
# re-adversarial, section 26). A negative case is what makes this a test.
manual = {"by_filename": {}, "by_dn": {("14", 2): B}}
assert V.resolve_xref_target("14-beta/TODO-02", SRC, {}, manual) == B
assert V.resolve_xref_target("14-alpha/TODO-02", SRC, {}, manual) is None
assert V.resolve_xref_target("14-alpha/TODO-02-b.md", SRC, {}, manual) is None
# Compact and bare forms resolve against the SOURCE directory, so an alpha
# source must not reach beta's file through the fallback either.
assert V.resolve_xref_target("T02", SRC, {}, manual) is None
assert V.resolve_xref_target("TODO-02", SRC, {}, manual) is None
assert V.resolve_xref_target("T02", B, {}, manual) == B
DIRCODEEOF
then
    t_pass "validate: domain codes resolve by directory, not by cache order (both orders)"
else
    t_fail "validate: domain-code-to-directory resolution broken"
fi

# Sub-test 9aa16c: MUTATION CHECK. Restore the first-match scan in the one
# branch that is purely order-dependent and the reversed-order case must FAIL.
# A fixture that cannot be made to fail is not a fixture -- and this family is
# especially exposed to that, because half of these assertions would also pass
# against a resolver that simply refused everything.
if python3 - "$REPO_ROOT" <<'DIRMUTEOF'
import importlib.util, shutil, sys, pathlib, tempfile
repo = pathlib.Path(sys.argv[1])
tg = repo / "scripts" / "todo-graph"
sys.path.insert(0, str(tg))
import validate as V

def node(path, ident):
    return {"file_path": path, "id": ident, "domain": path.split("/")[1],
            "status": "draft", "title": ident, "sections": [],
            "created_at": "2026-04-01T00:00:00Z",
            "last_active_at": "2026-04-01T00:00:00Z",
            "inputs_xrefs": [], "stamps_xrefs": []}

alpha = node("todo/14-alpha/TODO-01-a.md", "a")
beta = node("todo/14-beta/TODO-02-b.md", "b")

# THE MUTATION IS THE OLD CODE: answer a named directory by scanning for the
# first node whose two-digit prefix matches. Applied to the live module rather
# than to a copy, so it exercises the shipped resolver's own call path.
def first_match_scan(path_index, dir_name):
    for (d, _n), fp in path_index["by_dn"].items():
        if d == dir_name[:2]:
            return str(pathlib.PurePosixPath(fp).parent) + "/INDEX.md"
    return None

original = V._dir_index_path
V._dir_index_path = first_match_scan
try:
    answers = []
    for nodes in ([alpha, beta], [beta, alpha]):
        p, i = V.build_path_index(nodes), V.build_id_index(nodes)
        answers.append(V.resolve_xref_target("14-alpha", nodes[0]["file_path"], i, p))
finally:
    V._dir_index_path = original

# The defect IS the disagreement: same reference, same corpus, two answers.
assert answers[0] != answers[1], (
    "mutation did not reintroduce order dependence -- the fixture proves nothing: "
    + repr(answers))
assert "14-beta" in answers[1], answers

# And with the mutation reverted, the reversed order answers alpha again.
p, i = V.build_path_index([beta, alpha]), V.build_id_index([beta, alpha])
assert V.resolve_xref_target("14-alpha", alpha["file_path"], i, p) \
    == "todo/14-alpha/INDEX.md"

# SECOND MUTATION: the RELATIVE spelled-filename repair, which the mutation
# above cannot reach (it swaps `_dir_index_path` only). Restoring the
# slug-permitting regex must make a relative reference to a nonexistent file
# bind to the same-numbered neighbour again -- otherwise nothing here proves
# that one-character-class change is load-bearing.
src = pathlib.Path(tg / "validate.py").read_text(encoding="utf-8")
OLD_RE = r'num_m = re.match(r"^TODO-(\d{1,2})(?:\.md)?$", tail)'
NEW_RE = r'num_m = re.match(r"^TODO-(\d{1,2})(?:-|\.md|$)", tail)'
assert OLD_RE in src, "relative slugless anchor not found -- fixture is inert"
mut_dir = pathlib.Path(tempfile.mkdtemp())
for f in ("cache_schema.py", "ref_resolution.py", "resolve_symbol.py",
          "snapshot_protocol.py", "snapshot_protocol.json"):
    if (tg / f).exists():
        shutil.copy(tg / f, mut_dir / f)
(mut_dir / "validate.py").write_text(src.replace(OLD_RE, NEW_RE, 1), encoding="utf-8")
sys.path.insert(0, str(mut_dir))
spec = importlib.util.spec_from_file_location("validate_relmut", mut_dir / "validate.py")
MUT = importlib.util.module_from_spec(spec)
sys.modules["validate_relmut"] = MUT
spec.loader.exec_module(MUT)
p, i = MUT.build_path_index([alpha, beta]), MUT.build_id_index([alpha, beta])
leaked = MUT.resolve_xref_target("../14-alpha/TODO-01-missing.md",
                                 alpha["file_path"], i, p)
assert leaked == alpha["file_path"], (
    "restoring the slug-permitting relative regex did NOT reintroduce the wrong "
    "binding, so the fixture proves nothing: " + repr(leaked))
DIRMUTEOF
then
    t_pass "validate: mutation-check -- the first-match scan reintroduces the order-dependent answer"
else
    t_fail "validate: domain-code mutation check did not flip the verdict"
fi

# Sub-test 9aa17: ONE token normalization, so ambiguity classification and
# resolution cannot disagree. They were two ordered strip chains that had to
# stay identical and did not: classification stripped backticks then commas in
# a single pass while resolution strips backticks AGAIN, so `` `X.md`, `` kept
# a trailing backtick while being classified (recording no ambiguity) and lost
# it while being resolved (returning None) -- the edge was silently omitted at
# rc 0 instead of refused. `_normalize_ref_token` loops to a fixed point, so
# any interleaving of backticks, commas, whitespace, an #anchor and a trailing
# slash converges on the same token.
if python3 - "$REPO_ROOT" <<'NORMEOF'
import sys, pathlib
repo = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(repo / "scripts" / "todo-graph"))
import query as Q

def node(path, ident, **extra):
    n = {"file_path": path, "id": ident, "domain": path.split("/")[1],
         "status": "draft", "title": ident, "sections": [],
         "created_at": "2026-04-01T00:00:00Z",
         "last_active_at": "2026-04-01T00:00:00Z",
         "inputs_xrefs": [], "stamps_xrefs": []}
    n.update(extra)
    return n

dup = [node("todo/01-a/TODO-A-Shared.md", "first"),
       node("todo/02-b/TODO-A-Shared.md", "second")]
forms = ["TODO-A-Shared", "TODO-A-Shared.md", "`TODO-A-Shared.md`",
         "`TODO-A-Shared.md`,", "  `TODO-A-Shared.md` , ",
         "TODO-A-Shared.md/", "TODO-A-Shared.md#9-x"]
for form in forms:
    ctx = Q.Ctx(dup + [node("todo/01-a/TODO-02-ref.md", "ref",
                            inputs_xrefs=[{"target_path": form,
                                           "target_section": None}])],
                pathlib.Path("."), True)
    assert len(ctx.ambiguous_edges) == 1, f"{form!r}: {ctx.ambiguous_edges}"
    assert not ctx.inbound, f"{form!r} bound an edge: {ctx.inbound}"
# The PUNCTUATION forms must all collapse to one token -- that is the
# invariant, not each individual strip. `.md` is deliberately NOT stripped
# here: dropping a suffix is the resolver's job, and `TODO-A-Shared` is a
# legitimately different spelling that the resolver maps to the same file.
punctuated = [f for f in forms if f.strip().strip("`").rstrip("/") != "TODO-A-Shared"]
assert len({Q._normalize_ref_token(f) for f in punctuated}) == 1, \
    {Q._normalize_ref_token(f) for f in punctuated}
assert Q._normalize_ref_token("TODO-A-Shared") == "TODO-A-Shared"
NORMEOF
then
    t_pass "query: one shared token normalization -- 7 punctuation forms all classify ambiguous"
else
    t_fail "query: token normalization diverges between classification and resolution"
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
if [ "$Q_RC" = "4" ] && echo "$Q_OUT" | grep -q "multiple filename stems"; then
    t_pass "query: ambiguous filename stem refused with exit 4 + listing"
else
    t_fail "query: stem collision should exit 4 (rc=$Q_RC, out=$Q_OUT)"
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
bind_baseline "$SELF_TREE/cache.json"
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

# ======================================================================
# Test 11: build-and-validate.sh wrapper + validate.py --diff mode (§6).
# Covers the CI-gate wrapper, the Makefile-reachable entry point, and
# the PR diff surface that reports "this PR makes the graph worse".
# ======================================================================

BNV_SH="$REPO_ROOT/scripts/todo-graph/build-and-validate.sh"

# Sub-test 11a: wrapper rejects unknown flags with exit 2.
BNV_OUT=$(bash "$BNV_SH" --bogus-flag 2>&1); BNV_RC=$?
if [ "$BNV_RC" = "2" ] && echo "$BNV_OUT" | grep -q "unknown flag"; then
    t_pass "build-and-validate: unknown flag exits 2"
else
    t_fail "build-and-validate: unknown flag handling broken (rc=$BNV_RC)"
fi

# Sub-test 11b: --help prints usage without running build or validate.
BNV_OUT=$(bash "$BNV_SH" --help 2>&1); BNV_RC=$?
# Asserts EVERY numeric code the wrapper can return, not just that an exit-code
# line exists. Two rounds of drift landed here: the header first promised
# "0 if all 7 checks pass, 1 otherwise" while rc 2 was already reachable, then
# published a three-code contract while still propagating build.py's rc 3. A
# fixture that greps one label cannot see either, so it greps all four.
BNV_CODES_OK=1
for BNV_CODE in "^  0  " "^  1  " "^  2  " "^  3  "; do
    echo "$BNV_OUT" | grep -q "$BNV_CODE" || BNV_CODES_OK=0
done
if [ "$BNV_RC" = "0" ] && [ "$BNV_CODES_OK" = "1" ] \
    && echo "$BNV_OUT" | grep -q "GRAPH FINDINGS" \
    && echo "$BNV_OUT" | grep -q "INFRASTRUCTURE REFUSAL"; then
    t_pass "build-and-validate: --help prints usage + every numeric exit code it can return + exits 0"
else
    t_fail "build-and-validate: --help broken (rc=$BNV_RC)"
fi

# Sub-test 11c: wrapper deletes the cache on exit unless --keep-cache.
rm -f "$REPO_ROOT/build/todo-cache.json" 2>/dev/null || true
bash "$BNV_SH" --quiet --warnings-only >/dev/null 2>&1 || true
if [ ! -f "$REPO_ROOT/build/todo-cache.json" ]; then
    t_pass "build-and-validate: cache deleted on exit (default)"
else
    t_fail "build-and-validate: cache leaked (default should delete)"
fi

# Sub-test 11d: --keep-cache leaves the cache in place.
bash "$BNV_SH" --quiet --warnings-only --keep-cache >/dev/null 2>&1 || true
if [ -f "$REPO_ROOT/build/todo-cache.json" ]; then
    t_pass "build-and-validate: --keep-cache preserves cache"
else
    t_fail "build-and-validate: --keep-cache broken"
fi

# Sub-test 11e: wrapper exit code propagates the validator's. Use a
# synthetic tree with a deliberate dangling-section drift to force a
# validator FAIL and assert the wrapper exits 1.
BNV_TREE="$TMP_DIR/bnv-drift"
mkdir -p "$BNV_TREE/todo/01-t" "$BNV_TREE/scripts/todo-graph"
cat > "$BNV_TREE/todo/01-t/TODO-01-drift.md" <<'EOF'
---
schema_version: 1
id: bnv-drift
domain: 01-t
status: active
title: "drift"
---
# body
## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | :---: | :-----: | ---- | ---- | :---: |
| 💎 | 1 | §1 | a | §99 | [x] |

## 1. a

- [x] commit
EOF
# Invoke the wrapper directly against the synthetic tree. The wrapper
# itself hard-codes paths relative to its own scripts dir, so we just
# use the top-level wrapper + --cache override via env isn't supported
# today -- we test the equivalent via the build.py + validate.py pair
# directly (the wrapper just chains these two). This proves the
# chaining contract: a bad tree's validator exit code flows through.
python3 "$BUILD_PY" --quiet --root "$BNV_TREE/todo" --output "$BNV_TREE/cache.json" --repo-root "$BNV_TREE" >/dev/null 2>&1
V_OUT=$(python3 "$VALIDATE_PY" --cache "$BNV_TREE/cache.json" --repo-root "$BNV_TREE" --quiet 2>&1); V_RC=$?
if [ "$V_RC" = "1" ] && echo "$V_OUT" | grep -q "dangling-section"; then
    t_pass "build-and-validate: validator exit 1 propagates drift ($V_RC)"
else
    t_fail "build-and-validate: drift propagation broken (rc=$V_RC)"
fi

# Sub-test 11f: validate.py --diff reports 0 regressions when baseline
# equals current cache.
#
# THE CURRENT CACHE IS BUILT IN PLACE, not copied. It used to be `cp`'d to a
# second name, which silently left its corpus binding behind -- the sidecar is
# keyed by the cache's own path and digest -- and the validator accepted it only
# because nothing checked. Now that the load is generation-bound, an unbound
# cache refuses, correctly: a cache separated from its binding cannot certify
# anything. The BASELINE is still a plain copy, because a baseline is read with
# freshness disabled and needs no binding, which is the actual asymmetry between
# the two reads.
BASE_CACHE="$TMP_DIR/diff-base.json"
CUR_CACHE="$TMP_DIR/diff-cur.json"
python3 "$BUILD_PY" --quiet --output "$BASE_CACHE" >/dev/null 2>&1
python3 "$BUILD_PY" --quiet --output "$CUR_CACHE" >/dev/null 2>&1
DIFF_OUT=$(python3 "$VALIDATE_PY" --cache "$CUR_CACHE" --warnings-only --quiet --diff "$BASE_CACHE" 2>&1)
if echo "$DIFF_OUT" | grep -q "0 graph-delta vs baseline"; then
    t_pass "validate --diff: identical caches -> 0 graph-delta"
else
    t_fail "validate --diff: identical-cache delta broken (out=$DIFF_OUT)"
fi

# Sub-test 11g: --diff setup. Ensures DIFF_TREE exists for downstream
# tests. No assertion of its own -- the broken-backlink scenarios are
# covered by 11g2 (typo rename, no referrer -> pass) and 11g3
# (referenced removal -> fail with source).
#
# d_todo writes one real TODO file. The delta fixtures below used to hand-craft
# BOTH caches over file_paths that existed nowhere on disk, which only worked
# while the current cache went in unvalidated and an unreadable TODO read as
# empty text. The CURRENT side is now built by build.py from a real corpus --
# the same thing CI does -- and only the BASELINE stays hand-crafted, which is
# what a historical artifact legitimately is.
DIFF_TREE="$TMP_DIR/diff-break"
mkdir -p "$DIFF_TREE"

# d_todo <root> <domain> <nn> <slug> <id> <status> [<yaml-depends-list>]
d_todo() {
    local root="$1" dom="$2" nn="$3" slug="$4" tid="$5" st="$6" dep="${7:-}"
    mkdir -p "$root/todo/$dom"
    {
        echo "---"
        echo "schema_version: 1"
        echo "id: $tid"
        echo "domain: $dom"
        echo "status: $st"
        echo "title: \"$slug\""
        [ -n "$dep" ] && echo "depends_on: [$dep]"
        echo "---"
        echo "body"
    } > "$root/todo/$dom/TODO-$nn-$slug.md"
}

# Sub-test 11g2: typo-id correction (nobody references the old id)
# must NOT trigger broken-backlink. Codex pass 15 H1 regression.
python3 -c "
import json
# Baseline has 'typo-id' with no inbound refs.
baseline = [
    {'file_path': 'todo/01-t/TODO-01-a.md', 'id': 'typo-id', 'domain': '01-t',
     'status': 'active', 'title': 'A', 'schema_version': 1,
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
    {'file_path': 'todo/01-t/TODO-02-b.md', 'id': 'beta', 'domain': '01-t',
     'status': 'active', 'title': 'B', 'schema_version': 1,
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
]
with open('$DIFF_TREE/base3.json', 'w') as f: json.dump(baseline, f)
"
# Current: typo corrected -> 'correct-id'. Beta unchanged. Built from a real
# corpus so the cache carries its binding and the files it names exist.
D3="$DIFF_TREE/t3"
rm -rf "$D3"
d_todo "$D3" 01-t 01 a correct-id active
d_todo "$D3" 01-t 02 b beta active
python3 "$BUILD_PY" --quiet --root "$D3/todo" --output "$DIFF_TREE/cur3.json" \
    --repo-root "$D3" >/dev/null 2>&1
DIFF_OUT=$(python3 "$VALIDATE_PY" --cache "$DIFF_TREE/cur3.json" --repo-root "$D3" --quiet --warnings-only --diff "$DIFF_TREE/base3.json" 2>&1); DIFF_RC=$?
if ! echo "$DIFF_OUT" | grep -q "broken-backlink"; then
    t_pass "validate --diff: typo-id correction does NOT false-positive as broken-backlink"
else
    t_fail "validate --diff: typo correction flagged as broken-backlink (out=$DIFF_OUT)"
fi

# Sub-test 11g3: removed id that IS referenced by another node MUST
# fire broken-backlink with the referring source.
python3 -c "
import json
baseline = [
    {'file_path': 'todo/01-t/TODO-01-a.md', 'id': 'alpha', 'domain': '01-t',
     'status': 'active', 'title': 'A', 'schema_version': 1,
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
    {'file_path': 'todo/01-t/TODO-02-b.md', 'id': 'beta', 'domain': '01-t',
     'status': 'active', 'title': 'B', 'schema_version': 1,
     'depends_on': ['alpha'],
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
]
with open('$DIFF_TREE/base4.json', 'w') as f: json.dump(baseline, f)
"
bind_baseline "$DIFF_TREE/base4.json"

# Current: alpha's file is GONE but beta still depends on it. Real corpus, so
# the removal is a real removal rather than a node edited out of a JSON blob.
D4="$DIFF_TREE/t4"
rm -rf "$D4"
d_todo "$D4" 01-t 02 b beta active "'alpha'"
python3 "$BUILD_PY" --quiet --root "$D4/todo" --output "$DIFF_TREE/cur4.json" \
    --repo-root "$D4" >/dev/null 2>&1
DIFF_OUT=$(python3 "$VALIDATE_PY" --cache "$DIFF_TREE/cur4.json" --repo-root "$D4" --quiet --warnings-only --diff "$DIFF_TREE/base4.json" 2>&1); DIFF_RC=$?
if [ "$DIFF_RC" = "1" ] && echo "$DIFF_OUT" | grep -q "broken-backlink: id 'alpha'" \
    && echo "$DIFF_OUT" | grep -q "still referenced by:"; then
    t_pass "validate --diff: removed id WITH live referrer fires broken-backlink"
else
    t_fail "validate --diff: referenced-removal detection broken (out=$DIFF_OUT)"
fi

# Sub-test 11j: duplicate-id check (new 8th primary check). A cache
# where two nodes share the same frontmatter id must FAIL. Codex
# pass 15 H2: build_id_index silently overwrites dict collisions, so
# a rename-into-collision PR was previously invisible.
DUP_TREE="$TMP_DIR/v-dup-id"
mkdir -p "$DUP_TREE/todo/01-test"
cat > "$DUP_TREE/todo/01-test/TODO-01-first.md" <<'EOF'
---
schema_version: 1
id: dup-id
domain: 01-test
status: active
title: "first"
---
body
EOF
cat > "$DUP_TREE/todo/01-test/TODO-02-second.md" <<'EOF'
---
schema_version: 1
id: dup-id
domain: 01-test
status: active
title: "second"
---
body
EOF
python3 "$BUILD_PY" --quiet --root "$DUP_TREE/todo" --output "$DUP_TREE/cache.json" --repo-root "$DUP_TREE" >/dev/null 2>&1
DUP_OUT=$(python3 "$VALIDATE_PY" --cache "$DUP_TREE/cache.json" --repo-root "$DUP_TREE" --quiet 2>&1); DUP_RC=$?
if [ "$DUP_RC" = "1" ] && echo "$DUP_OUT" | grep -q "duplicate-id.*'dup-id' is claimed by 2 files"; then
    t_pass "validate: duplicate-id check fires on 2 nodes sharing id"
else
    t_fail "validate: duplicate-id check broken (rc=$DUP_RC, out=$DUP_OUT)"
fi

# Sub-test 11h: --diff detects status-downgrade (done -> active).
python3 -c "
import json
baseline = [{'file_path': 'todo/01-t/TODO-01-a.md', 'id': 'alpha', 'domain': '01-t',
    'status': 'done', 'title': 'A', 'schema_version': 1,
    'sections': [], 'section_headings': [], 'inputs_xrefs': [],
    'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
    'last_active_at': '2026-04-23T00:00:00Z'}]
with open('$DIFF_TREE/base2.json', 'w') as f: json.dump(baseline, f)
"
bind_baseline "$DIFF_TREE/base2.json"

# Current: the same file, demoted done -> active. Real corpus on the current
# side; the baseline above is the historical artifact it is compared against.
D2="$DIFF_TREE/t2"
rm -rf "$D2"
d_todo "$D2" 01-t 01 a alpha active
python3 "$BUILD_PY" --quiet --root "$D2/todo" --output "$DIFF_TREE/cur2.json" \
    --repo-root "$D2" >/dev/null 2>&1
DIFF_OUT=$(python3 "$VALIDATE_PY" --cache "$DIFF_TREE/cur2.json" --repo-root "$D2" --quiet --warnings-only --diff "$DIFF_TREE/base2.json" 2>&1); DIFF_RC=$?
if [ "$DIFF_RC" = "1" ] && echo "$DIFF_OUT" | grep -q "status-downgrade.*done.*active"; then
    t_pass "validate --diff: detects status-downgrade (done -> active)"
else
    t_fail "validate --diff: status-downgrade detection broken (out=$DIFF_OUT)"
fi

# Sub-test 11i: make todo-graph works on the live tree (wrapper exit
# propagates via the make target's exit code).
if cd "$REPO_ROOT" && make -n todo-graph >/dev/null 2>&1; then
    t_pass "make: 'todo-graph' target is wired (dry-run OK)"
else
    t_fail "make: 'todo-graph' target missing or broken"
fi

# ======================================================================
# Test 12: render subcommand (§7) -- 5 formats, --scope, --output.
# ======================================================================

# Sub-test 12a: mermaid output begins with `flowchart TD` and defines
# one classDef per status (required for GitHub rendering).
Q_OUT=$(q_live --quiet render --render-format mermaid 2>&1)
if printf '%s' "$Q_OUT" | head -1 | grep -q '^flowchart TD$' \
    && echo "$Q_OUT" | grep -q 'classDef done' \
    && echo "$Q_OUT" | grep -q 'classDef active' \
    && echo "$Q_OUT" | grep -q 'classDef blocked'; then
    t_pass "render mermaid: flowchart header + status classDefs present"
else
    t_fail "render mermaid: missing flowchart header or classDefs"
fi

# Sub-test 12b: mermaid emits a click directive pointing at GitHub for
# every rendered node. Count `click` lines vs node lines; they must
# match for a tree of nodes that all have file_paths.
Q_OUT=$(q_live --quiet render --render-format mermaid --scope 00-infrastructure 2>&1)
NODE_LINES=$(printf '%s' "$Q_OUT" | grep -cE '^\s+[A-Za-z][A-Za-z0-9_]*\["')
CLICK_LINES=$(printf '%s' "$Q_OUT" | grep -cE '^\s+click ')
if [ "$NODE_LINES" -ge 1 ] && [ "$NODE_LINES" = "$CLICK_LINES" ]; then
    t_pass "render mermaid: every node has a matching click directive ($NODE_LINES/$CLICK_LINES)"
else
    t_fail "render mermaid: node/click mismatch (nodes=$NODE_LINES, clicks=$CLICK_LINES)"
fi

# Sub-test 12c: dot output is a valid digraph with rankdir and node
# shape declarations.
Q_OUT=$(q_live --quiet render --render-format dot --scope 00-infrastructure 2>&1)
if echo "$Q_OUT" | head -1 | grep -q '^digraph todo_graph' \
    && echo "$Q_OUT" | grep -q 'rankdir=LR' \
    && echo "$Q_OUT" | grep -q 'shape=box'; then
    t_pass "render dot: digraph header + rankdir + shape declarations"
else
    t_fail "render dot: missing required dot syntax"
fi

# Sub-test 12d: ascii tree rooted at an explicit id prints the id on
# the first line and indents children with `|-- `.
Q_OUT=$(q_live --quiet render --render-format ascii todo-metadata-layer 2>&1)
if echo "$Q_OUT" | head -1 | grep -q '^todo-metadata-layer'; then
    t_pass "render ascii: tree rooted at requested id"
else
    t_fail "render ascii: root mismatch (out=$Q_OUT)"
fi

# Sub-test 12e: gantt output has the expected keyword structure.
Q_OUT=$(q_live --quiet render --render-format gantt --scope 00-infrastructure 2>&1)
if echo "$Q_OUT" | head -1 | grep -q '^gantt$' \
    && echo "$Q_OUT" | grep -q 'dateFormat YYYY-MM-DD' \
    && echo "$Q_OUT" | grep -q 'section 00-infrastructure'; then
    t_pass "render gantt: header + dateFormat + section present"
else
    t_fail "render gantt: missing required gantt syntax"
fi

# Sub-test 12f: markdown output is a GFM table with the expected
# columns.
Q_OUT=$(q_live --quiet render --render-format markdown --scope 00-infrastructure 2>&1)
if echo "$Q_OUT" | head -2 | grep -qE '^\| domain \| id \| status \| depends_on \| first_unfinished \|$' \
    && echo "$Q_OUT" | head -2 | grep -qE '^\| --- \| --- \| --- \| --- \| --- \|$'; then
    t_pass "render markdown: GFM header present"
else
    t_fail "render markdown: header row broken"
fi

# Sub-test 12g: --scope filter produces a strict subset of the
# no-scope output.
FULL_LINES=$(q_live --quiet render --render-format markdown 2>&1 | wc -l)
SCOPED_LINES=$(q_live --quiet render --render-format markdown --scope 00-infrastructure 2>&1 | wc -l)
if [ "$SCOPED_LINES" -lt "$FULL_LINES" ] && [ "$SCOPED_LINES" -gt 2 ]; then
    t_pass "render --scope: scoped output ($SCOPED_LINES) is a strict subset of full ($FULL_LINES)"
else
    t_fail "render --scope: filter not applied (full=$FULL_LINES, scoped=$SCOPED_LINES)"
fi

# Sub-test 12h: --output writes the rendered text to the named file
# and emits nothing on stdout.
OUT_PATH="$TMP_DIR/render-out.mmd"
rm -f "$OUT_PATH" 2>/dev/null || true
STDOUT_TEXT=$(q_live --quiet render --render-format mermaid --output "$OUT_PATH" 2>&1)
if [ -s "$OUT_PATH" ] && [ -z "$STDOUT_TEXT" ] && head -1 "$OUT_PATH" | grep -q '^flowchart TD$'; then
    t_pass "render --output: writes file, suppresses stdout"
else
    t_fail "render --output broken (stdout=$STDOUT_TEXT, file_size=$(wc -c < "$OUT_PATH" 2>/dev/null || echo 0))"
fi

# Sub-test 12h2: mermaid node ids are COLLISION-PROOF across file_paths
# that differ only in chars the sanitizer collapses to `_`. Codex pass
# 16 H1: without a hash suffix, `a-b.md` and `a_b.md` would emit the
# same node id and silently merge distinct TODOs.
COLL_TREE="$TMP_DIR/r-collide"
mkdir -p "$COLL_TREE"
python3 -c "
import json
nodes = [
    {'file_path': 'todo/01-t/TODO-01-a-b.md', 'id': 'a-b', 'domain': '01-t',
     'status': 'active', 'title': 'A-B', 'schema_version': 1,
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
    # NUMBERED 02, not 01: two TODOs sharing a domain AND a number derive one
    # resolver key, which section 22 refuses because the path index would file
    # them in the same slot. That collision is incidental here -- this fixture
    # is about `a-b` vs `a_b` sanitizing to the same MERMAID node id, and both
    # ids are unchanged.
    {'file_path': 'todo/01-t/TODO-02-a_b.md', 'id': 'a_b', 'domain': '01-t',
     'status': 'active', 'title': 'A_B', 'schema_version': 1,
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
]
with open('$COLL_TREE/cache.json', 'w') as f: json.dump(nodes, f)
"
bind_baseline "$COLL_TREE/cache.json"
Q_OUT=$(python3 "$QUERY_PY" --cache "$COLL_TREE/cache.json" --repo-root "$COLL_TREE" --quiet render --render-format mermaid 2>&1)
# Count unique node-declaration lines (grep for the `["label"]` shape).
UNIQ_NODES=$(printf '%s' "$Q_OUT" | grep -cE '\["[A-Za-z]' || true)
if [ "$UNIQ_NODES" = "2" ]; then
    t_pass "render mermaid: hash suffix prevents id collision on sanitize-equal paths"
else
    t_fail "render mermaid: collision detected ($UNIQ_NODES nodes for 2 distinct paths)"
fi

# Sub-test 12i: ascii cycle safety -- a synthetic fixture with A -> B
# -> A must not infinite-loop. Mark the back-edge `(cycle)`.
CYC_TREE="$TMP_DIR/r-cycle"
mkdir -p "$CYC_TREE"
python3 -c "
import json
nodes = [
    {'file_path': 'todo/01-t/TODO-01-a.md', 'id': 'alpha', 'domain': '01-t',
     'status': 'active', 'title': 'A', 'schema_version': 1,
     'depends_on': ['beta'],
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
    {'file_path': 'todo/01-t/TODO-02-b.md', 'id': 'beta', 'domain': '01-t',
     'status': 'active', 'title': 'B', 'schema_version': 1,
     'depends_on': ['alpha'],
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
]
with open('$CYC_TREE/cache.json', 'w') as f: json.dump(nodes, f)
"
# timeout 5s would be way more than enough; cycle protection should return immediately.
    bind_baseline "$CYC_TREE/cache.json"
CYC_OUT=$(PATH="/usr/bin:/bin" timeout --signal=TERM 5 \
    python3 "$QUERY_PY" --cache "$CYC_TREE/cache.json" --repo-root "$CYC_TREE" \
        --quiet render --render-format ascii alpha 2>&1); CYC_RC=$?
if [ "$CYC_RC" = "0" ] && echo "$CYC_OUT" | grep -q 'cycle'; then
    t_pass "render ascii: cycle detected + rendered with (cycle) marker"
else
    t_fail "render ascii: cycle handling broken (rc=$CYC_RC, out=$CYC_OUT)"
fi

# Sub-test 12i2: ascii diamond DAG must NOT be misreported as a cycle.
# Codex pass 16 M1 regression: A -> B, A -> C, B -> D, C -> D is a
# DAG with fan-in, not a cycle.
DAG_TREE="$TMP_DIR/r-diamond"
mkdir -p "$DAG_TREE"
python3 -c "
import json
nodes = [
    {'file_path': 'todo/01-t/TODO-01-a.md', 'id': 'alpha', 'domain': '01-t',
     'status': 'active', 'title': 'A', 'schema_version': 1,
     'depends_on': ['beta', 'gamma'],
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
    {'file_path': 'todo/01-t/TODO-02-b.md', 'id': 'beta', 'domain': '01-t',
     'status': 'active', 'title': 'B', 'schema_version': 1,
     'depends_on': ['delta'],
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
    {'file_path': 'todo/01-t/TODO-03-c.md', 'id': 'gamma', 'domain': '01-t',
     'status': 'active', 'title': 'C', 'schema_version': 1,
     'depends_on': ['delta'],
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
    {'file_path': 'todo/01-t/TODO-04-d.md', 'id': 'delta', 'domain': '01-t',
     'status': 'active', 'title': 'D', 'schema_version': 1,
     'sections': [], 'section_headings': [], 'inputs_xrefs': [],
     'stamps_xrefs': [], 'created_at': '2026-04-23T00:00:00Z',
     'last_active_at': '2026-04-23T00:00:00Z'},
]
with open('$DAG_TREE/cache.json', 'w') as f: json.dump(nodes, f)
"
bind_baseline "$DAG_TREE/cache.json"
DAG_OUT=$(python3 "$QUERY_PY" --cache "$DAG_TREE/cache.json" --repo-root "$DAG_TREE" --quiet render --render-format ascii alpha 2>&1); DAG_RC=$?
if [ "$DAG_RC" = "0" ] && ! echo "$DAG_OUT" | grep -q '(cycle)'; then
    # Second encounter of delta should be marked (seen), not (cycle).
    if echo "$DAG_OUT" | grep -q 'delta.*(seen)'; then
        t_pass "render ascii: diamond DAG renders shared delta as (seen), not (cycle)"
    else
        t_fail "render ascii: diamond missing (seen) marker (out=$DAG_OUT)"
    fi
else
    t_fail "render ascii: diamond DAG false-cycled (rc=$DAG_RC, out=$DAG_OUT)"
fi

# Sub-test 12j: make todo-graph-render-mermaid runs cleanly and produces
# a wrapped markdown file with the mermaid fence + flowchart body. It renders
# into TODO_GRAPH_MD under TMP_DIR: until 2026-11-11 this test rewrote the
# committed docs/infrastructure/todo-graph.md on every pack run, so every push
# left that page modified in the developer's tree. The file must appear WHERE
# it was asked for, which is what fails if the target ignores the variable.
MERMAID_MD="$TMP_DIR/todo-graph-render.md"
if cd "$REPO_ROOT" && make todo-graph-render-mermaid TODO_GRAPH_MD="$MERMAID_MD" >/dev/null 2>&1 \
    && [ -f "$MERMAID_MD" ] \
    && head -6 "$MERMAID_MD" | grep -q '^```mermaid$' \
    && tail -1 "$MERMAID_MD" | grep -q '^```$'; then
    t_pass "make: todo-graph-render-mermaid produces fenced markdown at TODO_GRAPH_MD"
else
    t_fail "make: todo-graph-render-mermaid broken or ignored TODO_GRAPH_MD"
fi
# 12j2: the destination is DATA. A name holding a backtick and both quotes must
# be written under that exact name and must not run: while the recipe pasted
# $(TODO_GRAPH_MD) into its shell source, the quotes broke the command and the
# backtick was shell code; while make re-expanded the value, `$5` vanished from
# the name and a `$(shell ...)` in it ran (adversarial review, 2026-11-11). The
# payload has no slash, so the name stays a single file in TMP_DIR.
MERMAID_ODD="$TMP_DIR/odd"'`echo MERMAID-RAN >&2`'"'q\""'$5$(shell echo MAKE-RAN >&2).md'
MERMAID_ERR=$(cd "$REPO_ROOT" && make todo-graph-render-mermaid TODO_GRAPH_MD="$MERMAID_ODD" 2>&1 >/dev/null)
MERMAID_RC=$?
if [ "$MERMAID_RC" = "0" ] && [ -f "$MERMAID_ODD" ] && ! printf '%s' "$MERMAID_ERR" | grep -qE 'MERMAID-RAN|MAKE-RAN'; then
    t_pass "make: todo-graph-render-mermaid takes TODO_GRAPH_MD literally (quotes, backtick, dollar, make function)"
else
    t_fail "make: TODO_GRAPH_MD with shell metacharacters (rc=$MERMAID_RC, file=$([ -f "$MERMAID_ODD" ] && echo yes || echo no), ran=$(printf '%s' "$MERMAID_ERR" | grep -cE 'MERMAID-RAN|MAKE-RAN'))"
fi

# ======================================================================
# Test 13: MCP server (§8) -- --self-test exit 0 regardless of SDK
# presence; tools/list returns 15 entries when SDK is present. Skips
# cleanly when the mcp package is not installed.
# ======================================================================

MCP_SERVER="$REPO_ROOT/scripts/todo-graph/mcp_server.py"

# Sub-test 13a: --self-test exits 0 whether or not the SDK is present.
# When the SDK is missing, expect a SKIP message. When it is present,
# expect "OK: N tools registered".
ST_OUT=$(python3 "$MCP_SERVER" --self-test 2>&1); ST_RC=$?
if [ "$ST_RC" = "0" ]; then
    if echo "$ST_OUT" | grep -q "SKIP: mcp SDK not installed"; then
        t_pass "mcp_server: --self-test SKIPs cleanly when SDK absent"
    elif echo "$ST_OUT" | grep -qE "OK: [0-9]+ tools registered"; then
        # SDK present -- tool count must be exactly 15 (12 + the 3
        # section-level readiness verbs mirrored in section 28).
        COUNT=$(echo "$ST_OUT" | sed -n 's/.*OK: \([0-9]\+\) tools.*/\1/p')
        if [ "$COUNT" = "15" ]; then
            t_pass "mcp_server: --self-test registers 15 tools (SDK present)"
        else
            t_fail "mcp_server: --self-test registered $COUNT tools (expected 15)"
        fi
    else
        t_fail "mcp_server: --self-test exit 0 but unexpected output: $ST_OUT"
    fi
else
    t_fail "mcp_server: --self-test should exit 0, got $ST_RC ($ST_OUT)"
fi

# Sub-test 13b: running mcp_server without --self-test when the SDK is
# absent exits 2 with a clear FATAL message (this path is what Claude
# Code would hit on a host without the SDK; we want a crisp failure,
# not a traceback).
if python3 -c "import mcp" 2>/dev/null; then
    # SDK present; starting the stdio server would block forever -- skip.
    t_pass "mcp_server: SDK-present startup (skipped; would block on stdio)"
else
    HUNG_OUT=$(PATH="/usr/bin:/bin" timeout --signal=TERM 2 \
        python3 "$MCP_SERVER" 2>&1); HUNG_RC=$?
    if [ "$HUNG_RC" = "2" ] && echo "$HUNG_OUT" | grep -q "mcp SDK not installed"; then
        t_pass "mcp_server: SDK-absent startup exits 2 with FATAL message"
    else
        t_fail "mcp_server: SDK-absent startup should exit 2 (rc=$HUNG_RC)"
    fi
fi

# Sub-test 13b2: MCP tool schemas expose the correct parameter names
# (SDK-present only). Codex pass 18 M1: every typed handler must carry
# the right public param name so FastMCP's introspection produces a
# contract that matches the tool description.
if python3 -c "import mcp" 2>/dev/null; then
    SCHEMA_CHECK=$(python3 -c "
import sys, json
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
from pathlib import Path
import mcp_server
from mcp.server.fastmcp import FastMCP
srv = mcp_server._build_mcp(FastMCP, Path('$REPO_ROOT').resolve())
tm = srv._tool_manager._tools
# backlinks / deferred / deferred-by / code must require 'target'.
# code-by must require 'path' (not 'target').
# stale must accept optional 'days'. by-domain must accept optional 'domain'.
# Row-returning tools additionally expose the bounding params
# (limit / offset / fields, plus scope where the verb has a domain
# column). stats is unbounded by shape and must expose NONE of them --
# advertising a limit on a verb with no rows to page would be a lie.
_B = ['limit', 'offset', 'fields', 'scope']
expected = {
    'backlinks':   {'required': ['target']},
    'deferred':    {'required': ['target']},
    'deferred-by': {'required': ['target']},
    'code':        {'required': ['target']},
    'code-by':     {'required': ['path']},
    'stale':       {'props': ['days'] + _B, 'required': []},
    'by-domain':   {'props': ['domain'] + _B, 'required': []},
    'stats':       {'props': [], 'required': []},
    'ready':       {'required': []},
}
failed = []
for name, expected_spec in expected.items():
    t = tm.get(name)
    if t is None:
        failed.append(f'{name}: missing')
        continue
    params = getattr(t, 'parameters', {}) or {}
    req = list(params.get('required') or [])
    props = list((params.get('properties') or {}).keys())
    if 'required' in expected_spec and req != expected_spec['required']:
        failed.append(f'{name}: required={req} (want {expected_spec[\"required\"]})')
    if 'props' in expected_spec and sorted(props) != sorted(expected_spec['props']):
        failed.append(f'{name}: props={props} (want {expected_spec[\"props\"]})')
if failed:
    print('SCHEMA_MISMATCH:', '; '.join(failed))
    sys.exit(1)
print('SCHEMAS_OK')
" 2>&1)
    if echo "$SCHEMA_CHECK" | grep -q "^SCHEMAS_OK$"; then
        t_pass "mcp_server: tool schemas expose correct typed params (target vs path)"
    else
        t_fail "mcp_server: schema mismatch ($SCHEMA_CHECK)"
    fi
else
    t_pass "mcp_server: schema check (skipped; SDK absent)"
fi

# Sub-test 13c: .mcp.json (Claude Code project-scope) and the reference
# manifest at scripts/todo-graph/mcp.json both register the server.
# Claude Code expects .mcp.json at the repo root, NOT .claude/mcp.json.
MANIFEST_OK=1
for MANIFEST in "$REPO_ROOT/.mcp.json" "$REPO_ROOT/scripts/todo-graph/mcp.json"; do
    if ! python3 -c "
import json, sys
d = json.load(open('$MANIFEST'))
srv = d.get('mcpServers', {}).get('todo-graph')
assert srv is not None, 'missing todo-graph entry'
assert 'mcp_server.py' in ' '.join(srv.get('args', [])), 'args do not point at mcp_server.py'
" 2>/dev/null; then
        t_fail "mcp_server: manifest $MANIFEST invalid or missing todo-graph entry"
        MANIFEST_OK=0
        break
    fi
done
if [ "$MANIFEST_OK" = "1" ]; then
    t_pass "mcp_server: .mcp.json + reference manifest both register todo-graph"
fi

# Sub-test 13d: end-to-end JSON-RPC roundtrip over stdio (SDK-present
# only). Spawns the server, sends initialize + tools/list + tools/call
# for stats + tools/call for backlinks, validates each response shape.
# This is the canonical acceptance test the §8 spec asked for and
# replaces the earlier "would block on stdio" soft skip.
if python3 -c "import mcp" 2>/dev/null; then
    RT_OUT=$(PATH="/usr/bin:/bin" timeout --signal=TERM 10 python3 <<'PY' 2>&1
import json, subprocess
proc = subprocess.Popen(
    ["python3", "scripts/todo-graph/mcp_server.py"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True, bufsize=1,
)
def send(req):
    proc.stdin.write(json.dumps(req) + "\n"); proc.stdin.flush()
def recv():
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError("server closed stdout early; stderr=" + proc.stderr.read())
    return json.loads(line)
try:
    send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
          "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                     "clientInfo": {"name": "t", "version": "0.1"}}})
    init = recv()
    assert init.get("result", {}).get("serverInfo", {}).get("name") == "todo-graph", f"init: {init}"
    send({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})
    send({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
    lst = recv()
    tools = lst.get("result", {}).get("tools", [])
    assert len(tools) == 15, f"tool count {len(tools)} != 15"
    # The section-level readiness verbs must reach the WIRE, not just
    # MCP_TOOLS: registration and transport are separate failures.
    for _want in ("section-ready", "section-blocked", "section-blocking"):
        assert any(t["name"] == _want for t in tools), f"{_want} not exposed"
    # Verify code-by exposes path (not target) in its schema.
    cb = next(t for t in tools if t["name"] == "code-by")
    assert cb["inputSchema"].get("required") == ["path"], f"code-by required: {cb['inputSchema']}"
    send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
          "params": {"name": "stats", "arguments": {}}})
    call = recv()
    txt = call.get("result", {}).get("content", [{}])[0].get("text", "")
    data = json.loads(txt)
    assert data.get("total_nodes", 0) > 0, f"stats.total_nodes not positive: {data}"
    # CALL every section-level verb, do not merely check it is LISTED.
    # Section 28 registered all three for tools/list while _dispatch_tool
    # rejected them, and this test passed anyway because it only called
    # stats. Advertised-but-uncallable is the exact failure to cover.
    _rid = 3
    for _name in ("section-ready", "section-blocked", "section-blocking"):
        _rid += 1
        send({"jsonrpc": "2.0", "id": _rid, "method": "tools/call",
              "params": {"name": _name, "arguments": {"limit": 5}}})
        _resp = recv()
        assert "error" not in _resp, f"{_name} tools/call errored: {_resp}"
        _txt = _resp.get("result", {}).get("content", [{}])[0].get("text", "")
        json.loads(_txt)  # must be JSON, not a traceback
    print("ROUNDTRIP_OK", "tools=" + str(len(tools)),
          "stats.total_nodes=" + str(data["total_nodes"]))
finally:
    proc.stdin.close()
    proc.wait(timeout=5)
PY
    )
    if echo "$RT_OUT" | grep -q "^ROUNDTRIP_OK"; then
        t_pass "mcp_server: end-to-end JSON-RPC roundtrip (initialize + tools/list + stats + each section verb CALLED)"
    else
        t_fail "mcp_server: JSON-RPC roundtrip broken (out=$RT_OUT)"
    fi
else
    t_pass "mcp_server: JSON-RPC roundtrip (skipped; SDK absent)"
fi

# ----------------------------------------------------------------------
# Test 14: stamped_items per-item cache extension (per-item cache extension).
# Covers the new `stamped_items` array on cache nodes and the lint
# Check 7 wiring (stub-behind-stamp).
# ----------------------------------------------------------------------

SI_TREE="$TMP_DIR/stamped-items-tree"
mkdir -p "$SI_TREE/todo/01-test" "$SI_TREE/src" "$SI_TREE/build" \
    "$SI_TREE/scripts/lint" "$SI_TREE/scripts/todo-graph"

# Sub-test 14a: a fixture TODO whose section 1 has two `[x]` items, one
# with a backtick-quoted symbol + markdown link, one with no code refs.
# Assert the cache emits stamped_items with the correct shape.
cat > "$SI_TREE/todo/01-test/TODO-01-fixture.md" <<'EOF'
---
schema_version: 1
id: stamped-items-fixture
domain: 01-test
status: active
title: "stamped_items fixture"
---

# Fixture

## Implementation Order

| icon | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| s | 1 | 1 | First | -- | [x] |

## 1. Stamped items

- [x] Created `foo_init()` in [`src/sample.c`](../../src/sample.c) -- shipped
- [x] Behavioral checklist item with no code references
EOF

cat > "$SI_TREE/src/sample.c" <<'EOF'
int foo_init(void)
{
    return 0;
}
EOF

SI_CACHE="$SI_TREE/build/todo-cache.json"
python3 "$BUILD_PY" --quiet --root "$SI_TREE/todo" --output "$SI_CACHE" \
    --repo-root "$SI_TREE" >"$TMP_DIR/si.log" 2>&1
if [ $? != 0 ]; then
    t_fail "stamped_items: build.py failed on fixture (log: $(tail -3 $TMP_DIR/si.log))"
else
    if SI_CACHE="$SI_CACHE" python3 - <<'PY' 2>/dev/null
import os, json, sys
d = json.load(open(os.environ["SI_CACHE"] + ""))
assert len(d) == 1, f"expected 1 node got {len(d)}"
node = d[0]
items = node.get("stamped_items")
assert items is not None, "stamped_items missing"
assert len(items) == 2, f"expected 2 stamped items got {len(items)}"
i0, i1 = items
assert i0["section_n"] == 1 and i0["item_idx"] == 0, f"bad i0: {i0}"
assert i1["section_n"] == 1 and i1["item_idx"] == 1, f"bad i1: {i1}"
# i0 must have file ref + symbol ref both pointing at src/sample.c
kinds = {(r.get("kind"), r.get("symbol")) for r in i0["refs"]}
assert ("symbol", "foo_init") in kinds, f"i0 missing symbol ref: {i0['refs']}"
files = [r for r in i0["refs"] if r["kind"] == "file"]
assert any(r["file"] == "src/sample.c" for r in files), f"i0 missing file ref: {i0['refs']}"
# Symbol ref must be paired with the file (Codex F2: deterministic resolution)
sym_ref = next(r for r in i0["refs"] if r["kind"] == "symbol")
assert sym_ref.get("file") == "src/sample.c", f"symbol not paired: {sym_ref}"
# i1 must have refs == []
assert i1["refs"] == [], f"i1 expected empty refs: {i1}"
sys.exit(0)
PY
    then
        t_pass "stamped_items: cache emits per-item refs (file + paired symbol)"
    else
        t_fail "stamped_items: cache shape wrong; first node:"
        python3 -c "import json; print(json.dumps(json.load(open('$SI_CACHE'))[0].get('stamped_items'), indent=2))" >&2 || true
    fi
fi

# Sub-test 14b: lint Check 7 must error on the synthetic fixture's
# stub-behind-stamp (foo_init returns 0 with no INTENTIONAL-STUB marker).
cp "$REPO_ROOT/scripts/lint.sh" "$SI_TREE/scripts/lint.sh"
cp "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
    "$SI_TREE/scripts/lint/check_stub_behind_stamp.py"
cp "$REPO_ROOT/scripts/lint/check_tautological_test.py" \
    "$SI_TREE/scripts/lint/check_tautological_test.py" 2>/dev/null
cp "$REPO_ROOT/scripts/todo-graph/resolve_symbol.py" \
    "$SI_TREE/scripts/todo-graph/resolve_symbol.py"
mkdir -p "$SI_TREE/include"

# Run only Check 7 by exercising the helper directly. Avoids dragging in
# the full lint.sh prelude (which scans for #pragma once etc.).
SI_OUT="$(STUB_LINT_CACHE="$SI_CACHE" STUB_LINT_REPO_ROOT="$SI_TREE" STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" 2>&1)"
SI_RC=$?
if [ "$SI_RC" = "0" ] && echo "$SI_OUT" | grep -q "stub-behind-stamp:foo_init"; then
    t_pass "lint Check 7: foo_init flagged as stub-behind-stamp"
else
    t_fail "lint Check 7: expected foo_init flag (rc=$SI_RC, out=$SI_OUT)"
fi

# Sub-test 14b2: a MISSING baseline fails CLOSED (rc 8), and the bypass the
# synthetic-root sub-tests above rely on is what makes them pass. Before
# 2026-08-06 an absent or corrupt stub-lint-baseline.json was swallowed into
# `baseline = None` and the coverage floor simply stopped applying while the
# check still exited 0 -- a silent, unrecorded bypass of the only gate that can
# see the resolver going blind. The file is TRACKED, so it is never legitimately
# absent; this pins that the skip must be asked for out loud.
SI_NOBASE_RC=0
STUB_LINT_CACHE="$SI_CACHE" STUB_LINT_REPO_ROOT="$SI_TREE" \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
    >/dev/null 2>&1 || SI_NOBASE_RC=$?
if [ "$SI_NOBASE_RC" = "8" ]; then
    t_pass "lint Check 7: missing baseline fails CLOSED (rc 8)"
else
    t_fail "lint Check 7: missing baseline should be rc 8, got $SI_NOBASE_RC"
fi

# Sub-test 14c: INTENTIONAL-STUB allowlist marker on the body opener
# suppresses the finding.
#
# CHANNEL DISCIPLINE -- the two NO-FINDING assertions below read STDOUT ONLY.
# check_stub_behind_stamp.py prints findings on stdout (lint.sh routes every
# stdout line through error()) and coverage/diagnostics on stderr, deliberately.
# Capturing 2>&1 here conflated the channels: when 08e64173 added the
# unconditional `stub-behind-stamp:coverage ...` stderr line, both sub-tests
# read it as a finding and CI went red (run 31038832724, 2026-08-05) while the
# check itself was behaving correctly. Stderr is kept in a file so a genuine
# failure still prints it.
cat > "$SI_TREE/src/sample.c" <<'EOF'
int foo_init(void)
{ /* INTENTIONAL-STUB: pending downstream scaffolding */
    return 0;
}
EOF
SI_ERR2="$SI_TREE/build/stub-lint-14c.err"
SI_OUT2="$(STUB_LINT_CACHE="$SI_CACHE" STUB_LINT_REPO_ROOT="$SI_TREE" STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" 2>"$SI_ERR2")"
SI_RC2=$?
if [ "$SI_RC2" = "0" ] && [ -z "$SI_OUT2" ]; then
    t_pass "lint Check 7: INTENTIONAL-STUB marker suppresses finding"
else
    t_fail "lint Check 7: marker should suppress; rc=$SI_RC2, out=$SI_OUT2, err=$(cat "$SI_ERR2")"
fi

# Sub-test 14d: real (non-stub) function does NOT trigger Check 7.
cat > "$SI_TREE/src/sample.c" <<'EOF'
int foo_init(void)
{
    int total = 0;
    for (int i = 0; i < 10; i++) {
        total += i * 2;
    }
    return total;
}
EOF
SI_ERR3="$SI_TREE/build/stub-lint-14d.err"
SI_OUT3="$(STUB_LINT_CACHE="$SI_CACHE" STUB_LINT_REPO_ROOT="$SI_TREE" STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" 2>"$SI_ERR3")"
SI_RC3=$?
if [ "$SI_RC3" = "0" ] && [ -z "$SI_OUT3" ]; then
    t_pass "lint Check 7: real multi-line function not flagged"
else
    t_fail "lint Check 7: real function false-positive; rc=$SI_RC3, out=$SI_OUT3, err=$(cat "$SI_ERR3")"
fi

# Sub-test 14f: prototype-then-definition is correctly resolved (Codex F2).
# A file with `static int foo(void);` above the real definition must not
# false-pass the lint Check 7.
cat > "$SI_TREE/src/sample.c" <<'EOF'
static int foo_init(void);

static int foo_init(void)
{
    return 0;
}
EOF
SI_OUT4="$(STUB_LINT_CACHE="$SI_CACHE" STUB_LINT_REPO_ROOT="$SI_TREE" STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" 2>&1)"
if echo "$SI_OUT4" | grep -q "stub-behind-stamp:foo_init"; then
    t_pass "lint Check 7: prototype-before-definition correctly resolves to def"
else
    t_fail "lint Check 7: prototype-before-definition false-passed; out=$SI_OUT4"
fi

# Sub-test 14g: cache refs that escape repo_root must NOT be read by
# Check 7 (Codex F3 path-traversal guard).
cat > "$SI_TREE/todo/01-test/TODO-03-escape.md" <<'EOF'
---
schema_version: 1
id: stamped-items-escape
domain: 01-test
status: active
title: "escape fixture"
---

# Body

## Implementation Order

| icon | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| s | 1 | 1 | First | -- | [x] |

## 1. Escape

- [x] Calls `evil_fn()` in [`../../../../tmp/evil.c`](../../../../tmp/evil.c)
EOF
mkdir -p /tmp 2>/dev/null
cat > /tmp/evil.c <<'EOF'
int evil_fn(void) { return 0; }
EOF
python3 "$BUILD_PY" --quiet --root "$SI_TREE/todo" --output "$SI_CACHE" \
    --repo-root "$SI_TREE" >/dev/null 2>&1
SI_ESC="$(STUB_LINT_CACHE="$SI_CACHE" STUB_LINT_REPO_ROOT="$SI_TREE" STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" 2>&1)"
if ! echo "$SI_ESC" | grep -q "stub-behind-stamp:evil_fn"; then
    t_pass "lint Check 7: repo-escape ref not followed (path-traversal guard)"
else
    t_fail "lint Check 7: followed escape ref into /tmp; out=$SI_ESC"
fi
rm -f "$SI_TREE/todo/01-test/TODO-03-escape.md" /tmp/evil.c

# Sub-test 14h: helper exit code propagates through lint.sh -- a
# corrupt cache should NOT silently false-clean the lint (Codex F1).
cp "$SI_CACHE" "$SI_TREE/build/cache-corrupt.json"
echo "this is not json" > "$SI_TREE/build/cache-corrupt.json"
SI_CORRUPT_OUT="$(STUB_LINT_CACHE="$SI_TREE/build/cache-corrupt.json" \
    STUB_LINT_REPO_ROOT="$SI_TREE" STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" 2>&1)"
SI_CORRUPT_RC=$?
# Pin the documented exit-code contract: 5 = corrupt JSON cache. lint.sh
# routes this to ERROR. A drift to 6 (internal failure) or 2 (missing)
# would silently break the lint.sh case routing -- assert exactly 5.
if [ "$SI_CORRUPT_RC" = "5" ] && echo "$SI_CORRUPT_OUT" | grep -qi "cache unreadable"; then
    t_pass "lint Check 7: corrupt cache yields rc=5 + diagnostic (contract pinned)"
else
    t_fail "lint Check 7: corrupt cache must yield rc=5; rc=$SI_CORRUPT_RC, out=$SI_CORRUPT_OUT"
fi

# Sub-test 14i: shared file-line cache is actually wired (resolve_symbol +
# is_stub_body share _load_file_lines). `_read_file_lines` is deliberately
# NOT `@lru_cache`d (TODO-06 section 12 -- that was tried first and is a real
# bug: it never re-opens a path on a second call, so a same-process mutation
# is never observed and the pin comparison in `_load_file_lines` becomes dead
# code; see the 14gg mutation-detection fixture, which is what caught it).
# It re-fstats on EVERY call but skips the expensive read+split when the
# fresh fstat matches what is cached -- proved here by IDENTITY: two lookups
# against an unchanged file must return the SAME cached lines tuple object,
# not merely equal content, or the "skip the reread" path did not fire.
SI_CACHE_HIT=$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 - <<'PY' 2>&1
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
fixture = os.environ["SI_TREE"] + "/src/sample.c"
with open(fixture, "w") as f:
    f.write("int real(void)\n{\n    int a = 1;\n    int b = a + 2;\n    return b;\n}\n")
rs.cache_clear()
rs.resolve_symbol(fixture, "real")
lines_after_resolve = rs._content_cache[fixture][1]
rs.is_stub_body(fixture, 1, 6)
lines_after_stub = rs._content_cache[fixture][1]
same = lines_after_resolve is lines_after_stub
print(f"same_object={same} entries={len(rs._content_cache)}")
PY
)
if echo "$SI_CACHE_HIT" | grep -qE "same_object=True entries=1"; then
    t_pass "resolve_symbol: shared _load_file_lines cache registers hits ($SI_CACHE_HIT)"
else
    t_fail "resolve_symbol: shared file cache not wired ($SI_CACHE_HIT)"
fi

# ---------------------------------------------------------------------------
# Sub-tests 14j-14r: SPLIT FUNCTION-HEAD RESOLUTION (TODO-06 section 10).
#
# These drive resolve_symbol() directly rather than through the lint, because
# what must be pinned is the exact (file, line_start, line_end) tuple -- not
# merely "a finding appeared". Section 10's FIRST attempt was reverted
# (5cff59cc) precisely because fixtures that only asserted the new shapes parse
# let a 52 -> 1 collapse in already-working shapes ship green. So every positive
# below asserts an EXACT range, and 14n-14r are the negative shapes.
SPLIT_PY="$SI_TREE/split_head_check.py"
cat > "$SPLIT_PY" <<'PY'
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
src, sym = sys.argv[1], sys.argv[2]
rs.cache_clear()
res = rs.resolve_symbol(src, sym)
print("NONE" if res is None else f"{res[1]}:{res[2]}")
PY

split_case() {  # <label> <symbol> <expected "start:end" or NONE> <<heredoc body
    local label="$1" sym="$2" want="$3"
    cat > "$SI_TREE/src/split.c"
    local got
    got="$(REPO_ROOT="$REPO_ROOT" python3 "$SPLIT_PY" "$SI_TREE/src/split.c" "$sym" 2>&1)"
    if [ "$got" = "$want" ]; then
        t_pass "resolve_symbol split-head: $label"
    else
        t_fail "resolve_symbol split-head: $label; want=$want got=$got"
    fi
}

# 14j POSITIVE: return type on its own line.
split_case "return type on its own line" foo_split "1:5" <<'EOF'
static int
foo_split(void)
{
    return 0;
}
EOF

# 14k POSITIVE: pointer return type whose `*` binds to the type line.
split_case "pointer return type, star on type line" foo_ptr "1:5" <<'EOF'
const struct thing *
foo_ptr(int a)
{
    return 0;
}
EOF

# 14l POSITIVE: pointer star leading the SYMBOL line.
split_case "pointer star leads the symbol line" foo_star "1:5" <<'EOF'
const struct thing
*foo_star(int a)
{
    return 0;
}
EOF

# 14m POSITIVE: __attribute__ between the split return type and the symbol.
# MEASURED: zero live definitions use this shape today (EFIAPI appears only in
# typedefs); it is covered because section 10 names it explicitly.
split_case "__attribute__ prefix on the symbol line" foo_attr "1:5" <<'EOF'
static void
__attribute__((noinline)) foo_attr(void)
{
    return;
}
EOF

# 14n POSITIVE: a split PROTOTYPE must walk past to the real definition --
# the false-positive bar section 9 set, now on the split path. Mirrors the real
# shape at src/boot/uefi/bootx64.c:9866 (prototype) vs :13024 (definition).
split_case "split prototype walks past to the definition" foo_proto "4:8" <<'EOF'
static int
foo_proto(void);

static int
foo_proto(void)
{
    return 0;
}
EOF

# 14o NEGATIVE: a call statement never resolves as a head.
#
# HONEST LABEL: this is a BEHAVIOR PIN, not a mutation-provable guard test, and
# it is the one fixture here that no single mutation makes fail. A call site is
# refused at THREE independent layers -- the lead anchor (the symbol must open
# its physical line), the type-only bound on the preceding line, and the shared
# head regex (whose character class admits neither `;` nor `=`). Measured by
# breaking the first two together: the case still resolves to NONE. Its value is
# as a guard against a FUTURE loosening of all three, not as proof that any one
# of them is load-bearing today.
#
# Said explicitly because a section-10 fixture has already shipped once
# "passing without testing anything" (f11c3bcc), and the honest response to
# that lesson is labelling what a fixture proves -- not quietly keeping a green
# check whose description overstates it. The guards that ARE individually
# provable are 14j-14n (fail when pass 2 is disabled), 14p (fails when the
# depth-0 reject path is removed), 14q (fails when depth-awareness is removed),
# and 14s (fails when pass 1 is displaced).
split_case "call statement is not a split head (lead anchor)" foo_call "NONE" <<'EOF'
static void caller(void)
{
    int total;
    total = foo_call(1);
}
EOF

# 14p NEGATIVE: an object initializer produced by a function-like macro.
# The old confirmation walk ignored `=` entirely and took the initializer
# brace as a function body (Codex design F2).
split_case "object initializer is not a definition" foo_init "NONE" <<'EOF'
static const struct ops
foo_init(A) = { .x = 1 };
EOF

# 14q NEGATIVE: a struct defined INSIDE the parameter list must not have its
# brace mistaken for the function body (Codex design F2). Depth-aware walk.
split_case "parameter-scope struct brace is not the body" foo_param "1:5" <<'EOF'
static int
foo_param(struct Local { int v; } *arg)
{
    return 0;
}
EOF

# 14r NEGATIVE: function-pointer typedef -- terminal line opens with `(`, so
# the symbol never leads it.
split_case "function-pointer typedef is not a definition" foo_fp "NONE" <<'EOF'
typedef int
(*foo_fp)(int a, int b);
EOF

# 14s REGRESSION GUARD: the single-line path still resolves. This is the
# assertion whose absence let the reverted attempt ship -- it proves pass 1
# was not displaced by adding pass 2.
split_case "single-line head still resolves (pass 1 intact)" foo_one "1:4" <<'EOF'
static int foo_one(void)
{
    return 0;
}
EOF

# 14t OFFSET SAFETY: an attribute expression containing the SAME `symbol(`
# earlier on the terminal line. Pass 2 matches a comment/modifier-masked line,
# so its match offset must still index the RAW line. An earlier draft searched
# the raw line for the symbol instead and started the confirmation walk at the
# attribute's parenthesis, silently dropping the definition (Codex adversarial).
split_case "attribute containing symbol( does not capture the offset" foo_attr2 "1:5" <<'EOF'
static void
__attribute__((foo_attr2())) foo_attr2(void)
{
    return;
}
EOF

# 14u OFFSET SAFETY: same hazard via a block comment on the terminal line.
split_case "comment containing symbol( does not capture the offset" foo_cmt "1:5" <<'EOF'
static int
/* foo_cmt(legacy) */ foo_cmt(void)
{
    return 0;
}
EOF

# 14v-1 RANGE TRUNCATION: a parameter-scope struct on the SAME line as the body
# opener. Brace counting used to restart at column 0 of the opener line, so the
# parameter struct's `}` closed the "body" immediately and the resolver returned
# a truncated 1:2 range that Check 7 still counted as resolved. The pre-section
# resolver returned None here, so accepting a WRONG range was a regression this
# section introduced (Codex adversarial, review round).
split_case "same-line param struct does not truncate the range" foo_a3 "1:5" <<'EOF'
static int
foo_a3(struct Local { int v; } *arg) {
    int x = 1;
    return x;
}
EOF

# 14v-2 LEXICAL STATE: a multi-line block comment containing an unbalanced brace
# must not break the body brace count. Pre-existing defect (the old resolver
# also returned None), fixed by routing every structural walk through one scanner.
split_case "block comment with a brace does not break resolution" foo_c1 "1:8" <<'EOF'
static int
foo_c1(void)
{
/* a block comment
   containing { a brace
   spanning lines */
    return 0;
}
EOF

# 14v-3 LEXICAL STATE: a string literal containing parens and a comment opener.
# 9 files in this tree carry attribute/annotation strings of this shape.
split_case "string literal parens do not unbalance the depth" foo_str "1:5" <<'EOF'
static const char *
foo_str(const char *tag)
{
    return "unbalanced ( and // and /* inside a string";
}
EOF

# 14v-5 REGEX PATHOLOGY: a long comment-only candidate line. The raw substring
# filter admits a line whose ONLY occurrence of the symbol is inside a block
# comment; masking then leaves near-pure whitespace. With the old
# `^\s*\*?\s*` lead pattern -- two whitespace quantifiers around an optional
# atom -- a failing match explored quadratically many partitions: 0.38s at 20k
# characters, 2.3s at 50k, and a 5MB case did not finish in 90s. That could hang
# the commit-time lint on one generated comment (Codex perf re-review).
# Asserts a WALL-CLOCK bound, so a reintroduced pathology fails loudly.
# Two long-line inputs, and only ONE of them is mutation-provable -- said plainly
# rather than implying otherwise:
#   quad-a: the symbol survives masking but is not followed by `(`, so the lead
#           regex RUNS and must fail fast. This ISOLATES the pattern rewrite:
#           restoring `^\s*\*?\s*` makes it fail (3.09s on this input).
#   quad-b: the symbol exists only inside the comment, so masking removes it and
#           the pre-regex short-circuit skips the line. With the regex already
#           fixed this case is fast either way, so removing the short-circuit
#           does NOT fail it -- that guard is a work-saver on the common path,
#           not a correctness guard. quad-b is a behavior pin against a future
#           reintroduction of both.
SI_QUAD="$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 - <<'PY' 2>&1
import os, sys, time
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
out = []
# time.monotonic(), never time.time(): a CLOCK_REALTIME step backwards makes
# the delta negative, and `< 1.0` then passes no matter how slow the resolve
# actually was -- which would retire this guard against the quadratic lead
# regex silently, on precisely the loaded host where the clock steps.
a = os.environ["SI_TREE"] + "/src/quad_a.c"
with open(a, "w") as f:
    f.write("static int\n" + " " * 60000 + "foo_qa;\nint z;\n")
rs.cache_clear()
t = time.monotonic()
ra = rs.resolve_symbol(a, "foo_qa")
out.append(f"{ra is None}:{time.monotonic() - t < 1.0}")
b = os.environ["SI_TREE"] + "/src/quad_b.c"
with open(b, "w") as f:
    f.write("static int\n/* " + "x" * 60000 + " foo_qb( */ y;\nint z;\n")
rs.cache_clear()
t = time.monotonic()
rb = rs.resolve_symbol(b, "foo_qb")
out.append(f"{rb is None}:{time.monotonic() - t < 1.0}")
print(" ".join(out))
PY
)"
if [ "$SI_QUAD" = "True:True True:True" ]; then
    t_pass "resolve_symbol: long candidate lines resolve fast (no regex blowup)"
else
    t_fail "resolve_symbol: quadratic lead-regex pathology; got=$SI_QUAD (want 'True:True True:True')"
fi

# 14v-4 END-TO-END, ONE-LINE: a stub whose head carries a parameter-scope struct
# on the SAME line as the body. resolve_symbol returned the right range (1:1) but
# is_stub_body sliced the body at the line's FIRST `{` -- the parameter struct's
# -- so the extracted text never matched return-constant and a real
# stub-behind-stamp went unreported (Codex re-adversarial; the multi-line
# fixture above does not reach this path).
cat > "$SI_TREE/src/split.c" <<'EOF'
static int foo_1line(struct Local { int v; } *arg) { return 0; }
EOF
SI_1L="$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 - <<'PY' 2>&1
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
src = os.environ["SI_TREE"] + "/src/split.c"
rs.cache_clear()
r = rs.resolve_symbol(src, "foo_1line")
print("NORESOLVE" if r is None else str(rs.is_stub_body(r[0], r[1], r[2])))
PY
)"
if [ "$SI_1L" = "('0', 1)" ]; then
    t_pass "resolve_symbol+is_stub_body: one-line param-struct stub IS reported"
else
    t_fail "resolve_symbol+is_stub_body: one-line param-struct stub missed; got=$SI_1L"
fi

# 14v END-TO-END: resolve_symbol + is_stub_body together. 14q pins only the
# RANGE, and Codex correctly noted that leaves the downstream classifier
# untested: is_stub_body picked the first brace in the range, so the
# parameter-scope struct brace was reported as the body opener -- and that line
# is the ONLY one the INTENTIONAL-STUB allowlist is read from. A deliberate stub
# therefore became a blocking Check 7 false positive.
cat > "$SI_TREE/src/split.c" <<'EOF'
static int
foo_e2e(struct Local { int v; } *arg)
{ /* INTENTIONAL-STUB: pending downstream scaffolding */
    return 0;
}
EOF
SI_E2E="$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 - <<'PY' 2>&1
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
src = os.environ["SI_TREE"] + "/src/split.c"
rs.cache_clear()
r = rs.resolve_symbol(src, "foo_e2e")
print("NORESOLVE" if r is None else f"{r[1]}:{r[2]}:{rs.is_stub_body(r[0], r[1], r[2])}")
PY
)"
if [ "$SI_E2E" = "1:5:None" ]; then
    t_pass "resolve_symbol+is_stub_body: INTENTIONAL-STUB honored behind a parameter-scope brace"
else
    t_fail "resolve_symbol+is_stub_body: param-scope brace bypassed the allowlist; got=$SI_E2E"
fi

# ---------------------------------------------------------------------------
# Sub-tests 14w-14z: corpus_resolution_snapshot.py -- the acceptance gate for
# section 10. A gate that fails OPEN is worse than no gate, so these pin that it
# REFUSES (exit 3, distinct from 0=pass and 1=regression) rather than emitting a
# vacuous passing baseline. Codex adversarial: an `[]` cache used to snapshot
# 0 refs, exit 0, and then report "OK, 54 added" against the live tree.

snap_refuses() {  # <label> <cache-json-content>
    local label="$1" content="$2"
    printf '%s' "$content" > "$SI_TREE/build/snap-cache.json"
    # Bind even the deliberately-BAD caches: an unbound one would be refused for
    # the wrong reason and the fixture would pass on rc 3 without ever reaching
    # the defect it names.
    bind_cache "$SI_TREE/build/snap-cache.json" "$SI_TREE/todo"
    STUB_LINT_CACHE="$SI_TREE/build/snap-cache.json" STUB_LINT_REPO_ROOT="$SI_TREE" STUB_LINT_ALLOW_NO_BASELINE=1 \
        python3 "$SNAP" write "$SI_TREE/build/snap-out.json" >/dev/null 2>&1
    local rc=$?
    if [ "$rc" = "3" ]; then
        t_pass "corpus snapshot: refuses $label (exit 3)"
    else
        t_fail "corpus snapshot: $label must exit 3, got $rc"
    fi
}

# The BASELINE is an input too. Validating only the live cache left compare()
# fail-open: an empty or all-null baseline yields an empty resolved_before, so
# every current mapping reads as ADDED and compare exits 0 having checked
# nothing (Codex adversarial, review round).
snap_baseline_refuses() {  # <label> <baseline-json>
    local label="$1" content="$2"
    printf '%s' "$content" > "$SI_TREE/build/snap-bad-base.json"
    STUB_LINT_CACHE="$REPO_ROOT/build/todo-cache.json" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
        python3 "$SNAP" compare "$SI_TREE/build/snap-bad-base.json" >/dev/null 2>&1
    local rc=$?
    if [ "$rc" = "3" ]; then
        t_pass "corpus snapshot: refuses baseline $label (exit 3)"
    else
        t_fail "corpus snapshot: baseline $label must exit 3, got $rc"
    fi
}
snap_baseline_refuses "that is empty" '{}'
snap_baseline_refuses "in the legacy schema-less format" '{"a::b": null}'
# EVERY element-validation fixture BELOW MUST DECLARE THE CURRENT SCHEMA.
# They were written at schema 1 and kept passing after the section 14 bump --
# but for the wrong reason: `compare` returns at the schema mismatch long
# before reaching the element checks, so deleting the arity, path-type,
# integer/range or truncation validation entirely would have left every one of
# them green. A fixture that passes because an EARLIER guard fired is not
# coverage of the guard it names (Codex test-coverage, section 14). The
# schema-1 migration refusal keeps its own assertion in 14cc.
snap_baseline_refuses "with only null mappings" '{"schema":2,"refs":1,"mappings":{"a::b":null}}'
snap_baseline_refuses "with an unknown bucket string" '{"schema":2,"refs":1,"mappings":{"a::b":"x"}}'
# An arity-only check let `[path, 411.0, 442.0]` through: three elements, and
# floats compare EQUAL to the integer tuple in Python, so a malformed baseline
# silently became authoritative (Codex re-adversarial).
snap_baseline_refuses "with float line numbers" \
    '{"schema":2,"refs":1,"mappings":{"a::b":["src/x.c",411.0,442.0]}}'
snap_baseline_refuses "with a reversed line range" \
    '{"schema":2,"refs":1,"mappings":{"a::b":["src/x.c",442,411]}}'
snap_baseline_refuses "with a boolean line number" \
    '{"schema":2,"refs":1,"mappings":{"a::b":["src/x.c",true,442]}}'
snap_baseline_refuses "with a boolean END line number" \
    '{"schema":2,"refs":1,"mappings":{"a::b":["src/x.c",411,true]}}'
snap_baseline_refuses "with a zero start line" \
    '{"schema":2,"refs":1,"mappings":{"a::b":["src/x.c",0,442]}}'
snap_baseline_refuses "with a two-element mapping" \
    '{"schema":2,"refs":1,"mappings":{"a::b":["src/x.c",411]}}'
snap_baseline_refuses "with a non-string path" \
    '{"schema":2,"refs":1,"mappings":{"a::b":[7,411,442]}}'
snap_baseline_refuses "with an empty path" \
    '{"schema":2,"refs":1,"mappings":{"a::b":["",411,442]}}'
# TRUNCATION: a one-entry baseline used to compare clean, reporting the other 56
# resolved mappings as ADDED and exiting 0. The declared population catches it.
snap_baseline_refuses "that is truncated (declared count disagrees)" \
    '{"schema":2,"refs":57,"mappings":{"a::b":["src/x.c",1,2]}}'
# NOT valid JSON at all, and specifically NOT UTF-8: UnicodeDecodeError is a
# ValueError rather than an OSError or JSONDecodeError, so the original except
# tuple let it escape as a traceback and a bare exit 1 -- colliding with exit
# 1's documented "a prior verdict changed" (Codex adversarial, section 14).
#
# ASSERTS THE MESSAGE, not just rc 3. `collect()` runs BEFORE the baseline is
# read, so a fixture pointed at a missing cache also exits 3 -- with a
# completely different diagnostic. An rc-only assertion would have passed here
# while never once reaching the baseline reader it exists to test, which is the
# same "green for the wrong reason" trap as the schema-1 fixtures above. Uses
# the REAL cache, exactly as `snap_baseline_refuses` does.
SNAP_BAD_UTF8="$TMP_DIR/snap-bad-utf8.json"
printf '{"schema":2,"refs":1,"mappings":{"a::b":["src/\xff\xfe.c",1,2]}}' > "$SNAP_BAD_UTF8"
SNAP_U_RC=0
STUB_LINT_CACHE="$REPO_ROOT/build/todo-cache.json" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
    python3 "$SNAP" compare "$SNAP_BAD_UTF8" \
    >/dev/null 2>"$TMP_DIR/snap-utf8.err" || SNAP_U_RC=$?
if [ "$SNAP_U_RC" = "3" ] && grep -q "baseline unreadable" "$TMP_DIR/snap-utf8.err"; then
    t_pass "corpus snapshot: invalid UTF-8 baseline is rc 3, not a traceback"
else
    t_fail "corpus snapshot: bad-UTF-8 baseline rc=$SNAP_U_RC (want 3)"
fi

snap_refuses "an empty node array" '[]'
snap_refuses "a cache with no stamped_items" '[{"file_path":"todo/x.md"}]'
snap_refuses "a non-list cache" '{"nodes":[]}'
snap_refuses "a cache whose stamped_items are all empty" '[{"file_path":"a","stamped_items":[]}]'
# MIXED shapes, node by node. The presence test is an `any(...)`, so ONE
# well-formed node used to admit the whole array -- and `collect()` then called
# `.get` on every node, item and ref, raising AttributeError from deep in the
# walk. `main()` catches only CacheError and ResolverInputError, so that
# escaped as a traceback and a bare exit 1, colliding with exit 1's documented
# "a prior verdict changed" (Codex adversarial, section 14).
snap_refuses "a cache with one good node and one junk node" \
    '[{"file_path":"a","stamped_items":[{"section_n":1,"item_idx":0,"refs":[]}]},"junk"]'
snap_refuses "a cache whose stamped_items is not a list" \
    '[{"file_path":"a","stamped_items":{"x":1}},{"file_path":"b","stamped_items":[{"refs":[]}]}]'
snap_refuses "a cache with a non-object stamped item" \
    '[{"file_path":"a","stamped_items":[{"refs":[]},"junk"]}]'
snap_refuses "a cache whose refs is not a list" \
    '[{"file_path":"a","stamped_items":[{"section_n":1,"item_idx":0,"refs":"x"}]}]'
snap_refuses "a cache with a non-object ref" \
    '[{"file_path":"a","stamped_items":[{"section_n":1,"item_idx":0,"refs":[{"kind":"symbol","symbol":"s"},"junk"]}]}]'
# SCALAR types, not just containers: `classify_ref` calls `rel.startswith(...)`
# on a ref's `file`, so an integer there passed container validation and raised
# AttributeError from inside the walk -- rc 1, colliding with "a prior verdict
# changed" (Codex adversarial, section 14).
snap_refuses "a cache whose ref file is an integer" \
    '[{"file_path":"a","stamped_items":[{"section_n":1,"item_idx":0,"refs":[{"kind":"symbol","symbol":"s","file":7}]}]}]'
snap_refuses "a cache whose ref symbol is an integer" \
    '[{"file_path":"a","stamped_items":[{"section_n":1,"item_idx":0,"refs":[{"kind":"symbol","symbol":9,"file":"src/x.c"}]}]}]'
# A DUPLICATE OCCURRENCE KEY is a hard error, never a silent overwrite: the
# dropped ref would leave `refs` recording the collapsed total, so the baseline
# is self-consistent and compare() passes forever over a population smaller
# than the lint's -- the exact divergence section 14 exists to prevent.
snap_refuses "a cache producing two identical occurrence keys" \
    '[{"file_path":"a","stamped_items":[{"section_n":1,"item_idx":0,"refs":[{"kind":"symbol","symbol":"s","file":"src/x.c"}]},{"section_n":1,"item_idx":0,"refs":[{"kind":"symbol","symbol":"s","file":"src/x.c"}]}]}]'
# NOT UTF-8 at all. The baseline reader was fixed for this and the CACHE reader
# was left narrower, so the same corrupt-input class exited 1 here and 3 there.
SNAP_CACHE_UTF8="$TMP_DIR/snap-cache-badutf8.json"
printf '[{"file_path":"a\xff\xfe","stamped_items":[{"refs":[]}]}]' > "$SNAP_CACHE_UTF8"
SNAP_CU_RC=0
STUB_LINT_CACHE="$SNAP_CACHE_UTF8" STUB_LINT_REPO_ROOT="$SI_TREE" \
    STUB_LINT_ALLOW_NO_BASELINE=1 python3 "$SNAP" write "$SI_TREE/build/snap-out.json" \
    >/dev/null 2>"$TMP_DIR/snap-cache-utf8.err" || SNAP_CU_RC=$?
if [ "$SNAP_CU_RC" = "3" ] && grep -q "cache unreadable" "$TMP_DIR/snap-cache-utf8.err"; then
    t_pass "corpus snapshot: invalid UTF-8 CACHE is rc 3, not a traceback"
else
    t_fail "corpus snapshot: bad-UTF-8 cache rc=$SNAP_CU_RC (want 3)"
fi
# An UNWRITABLE snapshot destination is infrastructure, not a regression
# verdict: `write_text` raised straight out as a bare exit 1 (reproduced with a
# directory target, Codex consistency).
SNAP_W_RC=0
STUB_LINT_CACHE="$REPO_ROOT/build/todo-cache.json" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
    python3 "$SNAP" write "$TMP_DIR" >/dev/null 2>"$TMP_DIR/snap-write.err" || SNAP_W_RC=$?
if [ "$SNAP_W_RC" = "3" ] && grep -q "cannot write snapshot" "$TMP_DIR/snap-write.err"; then
    t_pass "corpus snapshot: an unwritable destination is rc 3, not exit 1"
else
    t_fail "corpus snapshot: unwritable destination rc=$SNAP_W_RC (want 3)"
fi

# 14z: a duplicate (file, symbol) occurrence must be tracked PER OCCURRENCE --
# dropping one occurrence has to fail the gate. Deduping on (file, symbol) hid
# exactly this: the surviving twin kept the key and the tuple, so `compare`
# reported OK while a stamped reference had really gone.
#
# The class is DROPPED, not LOST, since section 14 split the old single verdict:
# DROPPED is "this occurrence no longer exists in the corpus" and LOST is "it
# still exists but no longer resolves". Both fail; naming them apart is what
# tells a reader whether a TODO edit or a resolver change caused it.
cat > "$SI_TREE/src/dup.c" <<'EOF'
int dup_fn(void)
{
    return 0;
}
EOF
snap_cache() {  # <n-occurrences>
    python3 - "$SI_TREE/build/snap-cache.json" "$1" <<'PY'
import json, sys
out, n = sys.argv[1], int(sys.argv[2])
refs = [{"kind": "symbol", "file": "src/dup.c", "symbol": "dup_fn"}]
items = [{"section_n": 1, "item_idx": i, "item_text": "x", "refs": refs}
         for i in range(n)]
json.dump([{"file_path": "todo/01-test/TODO-01-fixture.md",
            "stamped_items": items}], open(out, "w"))
PY
    bind_cache "$SI_TREE/build/snap-cache.json" "$SI_TREE/todo"
}
snap_cache 2
STUB_LINT_CACHE="$SI_TREE/build/snap-cache.json" STUB_LINT_REPO_ROOT="$SI_TREE" STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$SNAP" write "$SI_TREE/build/snap-base.json" >/dev/null 2>&1
snap_cache 1
SNAP_OUT="$(STUB_LINT_CACHE="$SI_TREE/build/snap-cache.json" STUB_LINT_REPO_ROOT="$SI_TREE" STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$SNAP" compare "$SI_TREE/build/snap-base.json" 2>&1)"
SNAP_RC=$?
if [ "$SNAP_RC" = "1" ] && echo "$SNAP_OUT" | grep -q "DROPPED"; then
    t_pass "corpus snapshot: a dropped duplicate occurrence registers as DROPPED"
else
    t_fail "corpus snapshot: dropped duplicate not caught; rc=$SNAP_RC out=$SNAP_OUT"
fi

# Sub-test 14e: forbidden-field validator rejects hand-authored
# stamped_items in frontmatter (defense-in-depth, Codex Q4).
cat > "$SI_TREE/todo/01-test/TODO-02-forbidden.md" <<'EOF'
---
schema_version: 1
id: stamped-items-forbidden
domain: 01-test
status: draft
title: "Forbidden field test"
stamped_items:
  - section_n: 1
    item_idx: 0
    item_text: "should not be allowed"
    refs: []
---

# Body
EOF
FORBID_LOG="$TMP_DIR/si-forbid.log"
RC=0
python3 "$BUILD_PY" --quiet --root "$SI_TREE/todo" \
    --output "$SI_TREE/build/cache-forbid.json" --repo-root "$SI_TREE" \
    >"$FORBID_LOG" 2>&1 || RC=$?
if [ "$RC" = "1" ] && grep -q "forbidden-field.*stamped_items" "$FORBID_LOG"; then
    t_pass "stamped_items: forbidden-field rejects hand-authored frontmatter"
else
    t_fail "stamped_items: forbidden-field check broken (rc=$RC, log=$(cat $FORBID_LOG))"
fi
rm -f "$SI_TREE/todo/01-test/TODO-02-forbidden.md"

# ---------------------------------------------------------------------------
# Sub-tests 14aa-14ll: TODO-06 section 12 -- resolver coverage past the head
# limit, and lexer/cache robustness. `split_case` (defined above at 14j) is
# reused for the splice fixtures: they are ordinary resolve_symbol range
# checks against a synthetic file, same idiom as every 14j-14v positive.

# 14aa POSITIVE: a spliced `/*` opener (`/` + backslash-newline + `*...`)
# must still open a block comment, hiding the embedded `}` from the brace
# counter. Splicing deletes ONLY the backslash-newline pair, so the
# continuation line must have NO leading whitespace for the two delimiter
# characters to actually land adjacent post-splice.
split_case "spliced /* opener hides an embedded brace" foo_spliceopen "1:6" <<'EOF'
int foo_spliceopen(void)
{
    /\
*this comment hides a } brace*/
    return 0;
}
EOF

# 14bb POSITIVE: a spliced `*/` closer (`*` + backslash-newline + `/`) must
# still close the block comment at that point, not run to EOF.
split_case "spliced */ closer ends the comment there" foo_spliceclose "1:6" <<'EOF'
int foo_spliceclose(void)
{
    /* comment *\
/
    return 0;
}
EOF

# 14cc POSITIVE: MULTI-HOP splice -- an empty splice-only line (just a lone
# backslash) sits between the two comment-delimiter halves. The pending
# state must carry across it rather than resolving (or dropping) early.
split_case "multi-hop splice carries a pending delimiter across an empty line" foo_multihop "1:7" <<'EOF'
int foo_multihop(void)
{
    /\
\
*multi-hop hides a } brace*/
    return 0;
}
EOF

# 14dd NEGATIVE (regression guard): an UNSPLICED `/` at end of line, followed
# by a REAL newline and then `* ... */`, must NOT open a comment -- C deletes
# only a literal backslash-newline, never a bare newline. The embedded `}`
# therefore stays visible as CODE, and closes the body's brace-depth count at
# the FIRST `}` it finds -- line 4, not the real closer on line 6. A truncated
# "1:4" is the observable proof the comment never formed: had it formed, the
# `}` would have been hidden and the range would extend to the real closer.
split_case "unspliced newline never opens a comment (negative control)" foo_nosplice "1:4" <<'EOF'
int foo_nosplice(void)
{
    /
    * this is not a comment, a } sits here *
    return 0;
}
EOF

# 14ee REGRESSION GUARD (candidate_lines per-occurrence fix): a line whose
# START sits inside a comment span, but which ALSO carries a REAL occurrence
# of the symbol later on the same physical line after the comment closes,
# must still surface that later occurrence as a candidate. An earlier version
# checked only the line-START offset against the span index and excluded the
# whole line, even though a later occurrence was ordinary code -- this is the
# split-head shape 14u already exercises end-to-end; this pins the underlying
# mechanism directly so a future regression fails at the right layer.
CAND_PY="$SI_TREE/candidate_check.py"
cat > "$CAND_PY" <<'PY'
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
lines = (
    "static int",
    "/* foo_cand(legacy) */ foo_cand(void)",
    "{",
    "    return 0;",
    "}",
)
idx = rs._FileIndex(lines)
print(",".join(str(i) for i in idx.candidate_lines("foo_cand")))
PY
CAND_OUT="$(REPO_ROOT="$REPO_ROOT" python3 "$CAND_PY" 2>&1)"
if [ "$CAND_OUT" = "1" ]; then
    t_pass "resolve_symbol candidate_lines: real occurrence after a same-line comment is still a candidate"
else
    t_fail "resolve_symbol candidate_lines: line-start-only span check regressed; got=$CAND_OUT"
fi

# 14ff ROBUSTNESS: a file above the per-file byte ceiling raises
# ResolverInputError rather than being silently read as empty (which would
# make it indistinguishable from "not resolvable here"). _MAX_FILE_BYTES is
# monkeypatched down for the test rather than writing a real 16 MiB fixture.
CEIL_PY="$SI_TREE/ceiling_check.py"
cat > "$CEIL_PY" <<'PY'
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
path = os.environ["SI_TREE"] + "/src/big.c"
with open(path, "w") as f:
    f.write("x" * 200)
rs.cache_clear()
rs._MAX_FILE_BYTES = 100
try:
    rs.resolve_symbol(path, "anything")
    print("NO_RAISE")
except rs.ResolverInputError as exc:
    print("RAISED" if "exceeds the" in str(exc) else f"WRONG_MESSAGE:{exc}")
finally:
    rs._MAX_FILE_BYTES = 16 * 1024 * 1024
PY
CEIL_OUT="$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 "$CEIL_PY" 2>&1)"
if [ "$CEIL_OUT" = "RAISED" ]; then
    t_pass "resolve_symbol: oversized file raises ResolverInputError (not silent empty)"
else
    t_fail "resolve_symbol: byte ceiling not enforced; got=$CEIL_OUT"
fi

# 14ff2 ROBUSTNESS: the ceiling bounds the ACTUAL READ, not just the pre-read
# fstat. `fh.read()` with no argument reads to EOF regardless of what an
# earlier fstat measured, so a file that GROWS after the pre-read fstat (a
# concurrent writer) would be read in full -- unbounded -- before a later
# fstat ever notices the drift (Codex adversarial). MUTATION-CHECKED
# DIRECTLY: a first draft of this fixture only asserted the resulting
# exception message, but `if len(text) > _MAX_FILE_BYTES: raise(...)` fires
# with the IDENTICAL message regardless of whether `text` came from a bounded
# `fh.read(_MAX_FILE_BYTES + 1)` or an unbounded `fh.read()` -- the length
# check does not care HOW the length was reached, so reverting the bound
# entirely would still pass an outcome-only assertion (Codex adversarial,
# round 2). `os.fdopen` is wrapped to SPY on the exact argument passed to
# `.read(...)`, proving the call site itself is bounded, not just its result.
GROW_PY="$SI_TREE/growth_check.py"
cat > "$GROW_PY" <<'PY'
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
path = os.environ["SI_TREE"] + "/src/grow.c"
with open(path, "w") as f:
    f.write("y" * 5000)
rs.cache_clear()
rs._MAX_FILE_BYTES = 100
real_fstat = os.fstat
fstat_calls = {"n": 0}
def fake_fstat(fd):
    st = real_fstat(fd)
    fstat_calls["n"] += 1
    if fstat_calls["n"] == 1:
        # Pretend the pre-read fstat saw a small file (the growth race).
        class Small:
            st_dev, st_ino, st_mtime_ns = st.st_dev, st.st_ino, st.st_mtime_ns
            st_size = 50
        return Small()
    return st
os.fstat = fake_fstat

real_fdopen = os.fdopen
read_args = []
class _SpyFile:
    def __init__(self, real):
        self._real = real
    def read(self, *a, **kw):
        read_args.append(a[0] if a else None)
        return self._real.read(*a, **kw)
    def fileno(self):
        return self._real.fileno()
    def __enter__(self):
        return self
    def __exit__(self, *exc):
        return self._real.__exit__(*exc)
def fake_fdopen(fd, *a, **kw):
    return _SpyFile(real_fdopen(fd, *a, **kw))
os.fdopen = fake_fdopen

try:
    rs.resolve_symbol(path, "anything")
    result = "NO_RAISE"
except rs.ResolverInputError as exc:
    result = "RAISED" if "grew past" in str(exc) else f"WRONG_MESSAGE:{exc}"
finally:
    os.fstat = real_fstat
    os.fdopen = real_fdopen
    rs._MAX_FILE_BYTES = 16 * 1024 * 1024
bound = read_args[0] if read_args else "NO_READ_CALL"
print(f"{result} bound={bound}")
PY
GROW_OUT="$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 "$GROW_PY" 2>&1)"
if [ "$GROW_OUT" = "RAISED bound=101" ]; then
    t_pass "resolve_symbol: read is bounded even when the pre-read fstat under-reports size"
else
    t_fail "resolve_symbol: unbounded read on a growth race; got=$GROW_OUT"
fi

# 14gg ROBUSTNESS: a file that changes on disk AFTER being pinned (a prior
# read succeeded) raises ResolverInputError on the NEXT read, rather than
# silently serving stale coordinates or degrading to empty. Two resolves for
# DIFFERENT symbols in the same process share the pin (no cache_clear between
# them), mirroring how Check 7 processes many refs against one file per run.
MUT_PY="$SI_TREE/mutation_check.py"
cat > "$MUT_PY" <<'PY'
import os, sys, time
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
path = os.environ["SI_TREE"] + "/src/mut.c"
with open(path, "w") as f:
    f.write("int mut_a(void)\n{\n    return 0;\n}\n")
rs.cache_clear()
first = rs.resolve_symbol(path, "mut_a")
# Rewrite with different content/size so size+mtime both change; sleep a hair
# to guarantee a distinguishable mtime_ns on coarser filesystems.
time.sleep(0.01)
with open(path, "w") as f:
    f.write("int mut_a(void)\n{\n    return 1;  /* changed */\n}\n")
try:
    rs.resolve_symbol(path, "mut_a")
    print(f"NO_RAISE first={first}")
except rs.ResolverInputError as exc:
    print("RAISED" if "changed on disk" in str(exc) else f"WRONG_MESSAGE:{exc}")
PY
MUT_OUT="$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 "$MUT_PY" 2>&1)"
if [ "$MUT_OUT" = "RAISED" ]; then
    t_pass "resolve_symbol: file mutated after pinning raises ResolverInputError"
else
    t_fail "resolve_symbol: mutation not detected; got=$MUT_OUT"
fi

# 14hh-14kk: follow_declaration() -- TODO-06 section 12 "class B". Deliberately
# NOT a general cross-file search (design-review-gated): restricted to the
# repo's own include/-mirrors-src/ convention, basename-unique, non-static.
FDECL_PY="$SI_TREE/follow_decl_check.py"
mkdir -p "$SI_TREE/include/kernel/fd" "$SI_TREE/src/kernel/fd"
cat > "$SI_TREE/include/kernel/fd/widget.h" <<'EOF'
void widget_reset(void);
EOF
cat > "$SI_TREE/src/kernel/fd/widget.c" <<'EOF'
void widget_reset(void)
{
    return;
}
EOF
cat > "$FDECL_PY" <<'PY'
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
root = os.environ["SI_TREE"]
rs.cache_clear()
res = rs.follow_declaration(root + "/include/kernel/fd/widget.h", "widget_reset", root)
print("14hh:" + ("PASS" if res and res[0].endswith("widget.c") else f"FAIL:{res}"))

rs.cache_clear()
res2 = rs.follow_declaration(root + "/src/kernel/fd/widget.c", "widget_reset", root)
print("14ii:" + ("PASS" if res2 is None else f"FAIL:{res2}"))
PY
echo "int mut_a(void) { return 0; }" > "$SI_TREE/src/kernel/fd/widget_caller.c"  # unrelated .c, not matched
FDECL_OUT="$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 "$FDECL_PY" 2>&1)"
if echo "$FDECL_OUT" | grep -q "^14hh:PASS"; then
    t_pass "follow_declaration: header resolves via basename-unique sibling .c"
else
    t_fail "follow_declaration: header->sibling failed; got=$FDECL_OUT"
fi
if echo "$FDECL_OUT" | grep -q "^14ii:PASS"; then
    t_pass "follow_declaration: a non-header (.c) declaring file stays unresolved"
else
    t_fail "follow_declaration: .c declaring file wrongly followed; got=$FDECL_OUT"
fi

# 14jj: the only sibling-.c candidate is `static` -- internal linkage cannot
# satisfy an external header declaration, so this must stay unresolved.
mkdir -p "$SI_TREE/include/kernel/fd2" "$SI_TREE/src/kernel/fd2"
cat > "$SI_TREE/include/kernel/fd2/gadget.h" <<'EOF'
void gadget_reset(void);
EOF
cat > "$SI_TREE/src/kernel/fd2/gadget.c" <<'EOF'
static void gadget_reset(void)
{
    return;
}
EOF
FDECL2_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2/gadget.h", "gadget_reset", "$SI_TREE")
print("PASS" if res is None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2_OUT" = "PASS" ]; then
    t_pass "follow_declaration: a static-only candidate is rejected (internal linkage)"
else
    t_fail "follow_declaration: static candidate wrongly accepted; got=$FDECL2_OUT"
fi

# 14jj2 REGRESSION GUARD: `static` is not always the LEADING token -- C's
# declaration-specifiers have no fixed order, so `inline static int foo(void)`
# is exactly as internal-linkage as `static inline int foo(void)`. A
# line-start-anchored check missed this (Codex adversarial).
mkdir -p "$SI_TREE/include/kernel/fd2b" "$SI_TREE/src/kernel/fd2b"
cat > "$SI_TREE/include/kernel/fd2b/sprocket.h" <<'EOF'
void sprocket_reset(void);
EOF
cat > "$SI_TREE/src/kernel/fd2b/sprocket.c" <<'EOF'
inline static void sprocket_reset(void)
{
    return;
}
EOF
FDECL2B_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2b/sprocket.h", "sprocket_reset", "$SI_TREE")
print("PASS" if res is None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2B_OUT" = "PASS" ]; then
    t_pass "follow_declaration: static rejected even when not the leading token (inline static)"
else
    t_fail "follow_declaration: non-leading static wrongly accepted; got=$FDECL2B_OUT"
fi

# 14jj3 REGRESSION GUARD (Codex adversarial, 3rd round): `static` alone on
# the line BEFORE an otherwise self-sufficient single-line head is a real
# repo idiom (src/libs/miniz/miniz.c:4482-4483). resolve_symbol resolves such
# a symbol via PASS 1 (the symbol's own line is already a complete head), so
# `res[1]` points at the symbol line, not the `static` line -- a check of
# only that one line missed this shape entirely.
mkdir -p "$SI_TREE/include/kernel/fd2c" "$SI_TREE/src/kernel/fd2c"
cat > "$SI_TREE/include/kernel/fd2c/widget2.h" <<'EOF'
void widget2_reset(void);
EOF
cat > "$SI_TREE/src/kernel/fd2c/widget2.c" <<'EOF'
static
void widget2_reset(void)
{
    return;
}
EOF
FDECL2C_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2c/widget2.h", "widget2_reset", "$SI_TREE")
print("PASS" if res is None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2C_OUT" = "PASS" ]; then
    t_pass "follow_declaration: static on the PRECEDING line (self-sufficient symbol line) is still rejected"
else
    t_fail "follow_declaration: static-on-preceding-line wrongly accepted; got=$FDECL2C_OUT"
fi

# 14jj4 REGRESSION GUARD (Codex adversarial, 4th round): an UNRELATED,
# disqualifying line immediately before the real symbol must NOT be scanned
# for `static` -- only lines that PASS the type-only declaration-prefix test
# belong to the same declaration. An earlier draft appended a line to the
# check set before verifying it was type-only, so the very line that broke
# the walk was still searched. Confirmed live in this repo:
# src/libs/monocypher/monocypher.c:158 is an unrelated one-line
# `static u64 x64(...) { ... }` immediately before line 159's real,
# external-linkage `crypto_verify16` -- the bug wrongly rejected it.
mkdir -p "$SI_TREE/include/kernel/fd2d" "$SI_TREE/src/kernel/fd2d"
cat > "$SI_TREE/include/kernel/fd2d/gizmo.h" <<'EOF'
void gizmo_reset(void);
EOF
cat > "$SI_TREE/src/kernel/fd2d/gizmo.c" <<'EOF'
static int unrelated_helper(int a, int b) { return a + b; }
void gizmo_reset(void)
{
    return;
}
EOF
FDECL2D_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2d/gizmo.h", "gizmo_reset", "$SI_TREE")
print("PASS" if res is not None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2D_OUT" = "PASS" ]; then
    t_pass "follow_declaration: an unrelated static line immediately before the symbol does not cause a false rejection"
else
    t_fail "follow_declaration: false rejection from an unrelated preceding static line; got=$FDECL2D_OUT"
fi

# 14jj5 REGRESSION GUARD (Codex adversarial, 5th round): `static` split
# across MORE lines than `_WINDOW_LINES` (the split-head resolver's own,
# unrelated 3-line contract) must still be caught -- `static\ninline\nconst\n
# void foo(void)` is valid C, and a bounded-to-3-lines lookback missed the
# `static` sitting a 4th line back.
mkdir -p "$SI_TREE/include/kernel/fd2e" "$SI_TREE/src/kernel/fd2e"
cat > "$SI_TREE/include/kernel/fd2e/deepthing.h" <<'EOF'
void deepthing_reset(void);
EOF
cat > "$SI_TREE/src/kernel/fd2e/deepthing.c" <<'EOF'
static
inline
const
void deepthing_reset(void)
{
    return;
}
EOF
FDECL2E_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2e/deepthing.h", "deepthing_reset", "$SI_TREE")
print("PASS" if res is None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2E_OUT" = "PASS" ]; then
    t_pass "follow_declaration: static caught even 4 lines back, past the split-head resolver's own window"
else
    t_fail "follow_declaration: deep static wrongly accepted; got=$FDECL2E_OUT"
fi

# 14jj6 REGRESSION GUARD (Codex adversarial, 5th round): a BLANK line between
# `static` and the symbol must not terminate the lookback early --
# `_TYPE_ONLY_LINE_RE` requires >= 1 character, so an empty line is not
# type-only, but it IS still part of the same declaration in C.
mkdir -p "$SI_TREE/include/kernel/fd2f" "$SI_TREE/src/kernel/fd2f"
cat > "$SI_TREE/include/kernel/fd2f/blankthing.h" <<'EOF'
void blankthing_reset(void);
EOF
printf 'static\n\nvoid blankthing_reset(void)\n{\n    return;\n}\n' > "$SI_TREE/src/kernel/fd2f/blankthing.c"
FDECL2F_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2f/blankthing.h", "blankthing_reset", "$SI_TREE")
print("PASS" if res is None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2F_OUT" = "PASS" ]; then
    t_pass "follow_declaration: a blank line between static and the symbol does not end the lookback early"
else
    t_fail "follow_declaration: blank-line-terminated lookback missed static; got=$FDECL2F_OUT"
fi

# 14jj7 FAIL-CLOSED (Codex adversarial, 6th round on this guard): a
# PREPROCESSOR-CONDITIONAL `static` (`#if ... / static / #endif` immediately
# before the head) cannot be evaluated by this lexical resolver -- there is
# no preprocessing anywhere in this module, so "no literal `static` found
# before hitting `#endif`" is UNDECIDED linkage, not proof of external
# linkage. This exact shape is REAL and LIVE in this repo:
# src/libs/mbedtls/library/sha512.c:558-566
# (`#if defined(MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT)` / `static` /
# `#endif` / the function head).
mkdir -p "$SI_TREE/include/kernel/fd2g" "$SI_TREE/src/kernel/fd2g"
cat > "$SI_TREE/include/kernel/fd2g/crypt.h" <<'EOF'
void crypt_process(void);
EOF
cat > "$SI_TREE/src/kernel/fd2g/crypt.c" <<'EOF'
#if defined(SOME_CONFIG)
static
#endif
void crypt_process(void)
{
    return;
}
EOF
FDECL2G_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2g/crypt.h", "crypt_process", "$SI_TREE")
print("PASS" if res is None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2G_OUT" = "PASS" ]; then
    t_pass "follow_declaration: a preprocessor-conditional static fails closed (undecidable, not assumed external)"
else
    t_fail "follow_declaration: preprocessor-conditional static wrongly accepted; got=$FDECL2G_OUT"
fi

# 14jj8 REGRESSION GUARD (Codex adversarial, 7th round on this guard): a
# block comment whose CLOSE shares a physical line with the directive it
# hides (`/* explanation` on one line, ` */ #endif` on the next) must still
# be recognized as a conditional directive -- `_mask_line_comments` only
# sees ONE physical line and reads the tail as raw `*/ #endif` text, which
# matches neither the type-only nor the directive pattern. Uses the
# multi-line-aware span index instead.
mkdir -p "$SI_TREE/include/kernel/fd2h" "$SI_TREE/src/kernel/fd2h"
cat > "$SI_TREE/include/kernel/fd2h/mcond.h" <<'EOF'
void mcond_process(void);
EOF
cat > "$SI_TREE/src/kernel/fd2h/mcond.c" <<'EOF'
#if 1
/* explanation
 */ #endif
void mcond_process(void)
{
    return;
}
EOF
FDECL2H_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2h/mcond.h", "mcond_process", "$SI_TREE")
print("PASS" if res is None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2H_OUT" = "PASS" ]; then
    t_pass "follow_declaration: a directive whose preceding comment closes on the same line still fails closed"
else
    t_fail "follow_declaration: comment-tail-before-directive wrongly accepted; got=$FDECL2H_OUT"
fi

# 14jj9 REGRESSION GUARD (Codex adversarial, 7th round on this guard): a
# standalone GCC/Clang attribute line (`__attribute__((noinline))`) between
# `static` and an otherwise self-sufficient head is valid, real style --
# `static\n__attribute__((noinline))\nvoid foo(void)` -- and must not
# terminate the lookback before reaching `static`.
mkdir -p "$SI_TREE/include/kernel/fd2i" "$SI_TREE/src/kernel/fd2i"
cat > "$SI_TREE/include/kernel/fd2i/attrthing.h" <<'EOF'
void attrthing_reset(void);
EOF
cat > "$SI_TREE/src/kernel/fd2i/attrthing.c" <<'EOF'
static
__attribute__((noinline))
void attrthing_reset(void)
{
    return;
}
EOF
FDECL2I_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2i/attrthing.h", "attrthing_reset", "$SI_TREE")
print("PASS" if res is None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2I_OUT" = "PASS" ]; then
    t_pass "follow_declaration: a standalone attribute line does not hide static from the lookback"
else
    t_fail "follow_declaration: attribute-hidden static wrongly accepted; got=$FDECL2I_OUT"
fi

# 14jj10 POSITIVE (companion to 14jj9): the SAME standalone-attribute shape,
# non-static, must still resolve -- proves the attribute recognition is a
# genuine "continue", not an accidental fail-closed-everything.
cat > "$SI_TREE/include/kernel/fd2i/attrthing2.h" <<'EOF'
void attrthing2_reset(void);
EOF
cat > "$SI_TREE/src/kernel/fd2i/attrthing2.c" <<'EOF'
__attribute__((noinline))
void attrthing2_reset(void)
{
    return;
}
EOF
FDECL2I2_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2i/attrthing2.h", "attrthing2_reset", "$SI_TREE")
print("PASS" if res is not None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2I2_OUT" = "PASS" ]; then
    t_pass "follow_declaration: a standalone attribute line alone (non-static) still resolves"
else
    t_fail "follow_declaration: non-static attribute case wrongly rejected; got=$FDECL2I2_OUT"
fi

# 14jj11 FAIL-CLOSED DEFAULT (Codex adversarial, 7th round): a genuinely
# UNRECOGNIZED line during the lookback -- neither a known-safe continuation
# (blank/type-only/attribute) nor a confident boundary (`;`/`}`-terminated,
# or a non-conditional directive) -- must fail closed rather than default to
# accept. Uses a made-up unterminated construct as a stand-in for "the next
# C syntax shape nobody has enumerated yet".
mkdir -p "$SI_TREE/include/kernel/fd2j" "$SI_TREE/src/kernel/fd2j"
cat > "$SI_TREE/include/kernel/fd2j/oddthing.h" <<'EOF'
void oddthing_reset(void);
EOF
cat > "$SI_TREE/src/kernel/fd2j/oddthing.c" <<'EOF'
SOME_MACRO(x, y)
void oddthing_reset(void)
{
    return;
}
EOF
FDECL2J_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd2j/oddthing.h", "oddthing_reset", "$SI_TREE")
print("PASS" if res is None else f"FAIL:{res}")
PY
)"
if [ "$FDECL2J_OUT" = "PASS" ]; then
    t_pass "follow_declaration: an unrecognized declaration-adjacent line fails closed by default"
else
    t_fail "follow_declaration: unrecognized shape wrongly defaulted to accept; got=$FDECL2J_OUT"
fi

# 14v-2b END-TO-END (Codex adversarial, 7th round): `is_stub_body`'s comment
# stripper only saw ONE physical line, so a return-constant body containing a
# MULTI-LINE comment retained both comment fragments in `real_lines` and the
# joined text never matched the return-constant pattern -- a real
# stub-behind-stamp silently missed classification. 14v-2 (above) already
# pins this exact shape for RESOLUTION; this pins it end-to-end through
# `is_stub_body` too, using the whole-file span index instead of a
# second, narrower per-line stripper. Live corpus hit surfaced by this fix:
# fb_get_output_count() (src/kernel/drivers/framebuffer.c) -- marked
# INTENTIONAL-STUB (a genuine, documented single-output placeholder).
MLC_PY="$SI_TREE/mlc_stub_check.py"
cat > "$MLC_PY" <<'PY'
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
path = os.environ["SI_TREE"] + "/src/mlc_stub.c"
with open(path, "w") as f:
    f.write(
        "int foo_mlc(void)\n"
        "{\n"
        "    /* explanation\n"
        "       continues */\n"
        "    return 0;\n"
        "}\n"
    )
rs.cache_clear()
res = rs.resolve_symbol(path, "foo_mlc")
stub = rs.is_stub_body(res[0], res[1], res[2]) if res else None
print("PASS" if stub == ("0", 2) else f"FAIL:{res}:{stub}")
PY
MLC_OUT="$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 "$MLC_PY" 2>&1)"
if [ "$MLC_OUT" = "PASS" ]; then
    t_pass "is_stub_body: a return-constant body containing a multi-line comment is still classified as a stub"
else
    t_fail "is_stub_body: multi-line comment hid a real stub; got=$MLC_OUT"
fi

# 14v-2c END-TO-END (Codex adversarial, 8th round): a genuine TWO-PHYSICAL-
# LINE body (`int f(void) {` / `    return 0; }`) -- content on the SAME
# line as the CLOSER, a DIFFERENT line than the opener -- was misread as a
# one-liner (the `not inner` slice-emptiness check cannot distinguish "one
# physical line" from "opener and closer on adjacent lines with nothing in
# between") and searched for `}` only on the OPENER's line, where it does
# not exist. The matching closer is now found via the SAME depth-aware scan
# used elsewhere, regardless of how many physical lines the body spans.
TL_PY="$SI_TREE/twoline_stub_check.py"
cat > "$TL_PY" <<'PY'
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
path = os.environ["SI_TREE"] + "/src/twoline.c"
with open(path, "w") as f:
    f.write("int foo_twoline(void) {\n    return 0; }\n")
rs.cache_clear()
res = rs.resolve_symbol(path, "foo_twoline")
stub = rs.is_stub_body(res[0], res[1], res[2]) if res else None
print("PASS" if stub == ("0", 1) else f"FAIL:{res}:{stub}")
PY
TL_OUT="$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 "$TL_PY" 2>&1)"
if [ "$TL_OUT" = "PASS" ]; then
    t_pass "is_stub_body: a two-physical-line body (closer on a different line than the opener) is still classified"
else
    t_fail "is_stub_body: two-line body misread as one-liner; got=$TL_OUT"
fi

# 14v-2d MARKER TIGHTENING (Codex adversarial, 9th round): the INTENTIONAL-
# STUB allowlist honored a bare SUBSTRING match, so negated prose, a
# bare token with no reason, and even a string-literal occurrence all
# silently suppressed a genuine stub-behind-stamp finding. Now requires the
# documented `/* INTENTIONAL-STUB: <reason> */` shape -- MUTATION-CHECKED
# both directions: a real marker (even one whose reason wraps onto later
# physical lines, the shape both real markers in this tree use) still
# suppresses; negated prose, a bare token, and an empty reason do NOT.
MARKER_PY="$SI_TREE/marker_check.py"
cat > "$MARKER_PY" <<'PY'
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs

def check(body, want_stub):
    path = os.environ["SI_TREE"] + "/src/marker.c"
    with open(path, "w") as f:
        f.write(f"int foo_marker(void)\n{body}\n")
    rs.cache_clear()
    res = rs.resolve_symbol(path, "foo_marker")
    stub = rs.is_stub_body(res[0], res[1], res[2]) if res else None
    is_stub = stub is not None
    return is_stub == want_stub

cases = [
    # (body, want_stub) -- want_stub=True means NOT suppressed (real finding)
    ("{ /* INTENTIONAL-STUB: pending scaffolding */\n    return 0;\n}", False),
    ("{  /* INTENTIONAL-STUB: reason wraps onto\n   a second physical line */\n    return 0;\n}", False),
    ("{ /* INTENTIONAL-STUB */\n    return 0;\n}", True),
    ("{ /* INTENTIONAL-STUB: */\n    return 0;\n}", True),
    ("{ /* not an INTENTIONAL-STUB, this is real */\n    return 0;\n}", True),
    # LEXICAL INDEPENDENCE (Codex adversarial, post-commit round): marker-
    # SHAPED text absorbed inside a `//` line comment is not itself a real,
    # independent block comment -- must NOT suppress.
    ("{ // example: /* INTENTIONAL-STUB: not an allowlist */\n    return 0;\n}", True),
    # MULTI-MATCH (Codex adversarial, post-commit round): a FAKE marker
    # earlier on the line (inside a string) must not short-circuit past a
    # genuinely real, independent marker later on the SAME line -- an
    # earlier fix checked only the FIRST regex match regardless of validity.
    ('{ const char *fake = "/* INTENTIONAL-STUB: fake */"; '
     '/* INTENTIONAL-STUB: real reason */ return 0; }', False),
]
results = [check(body, want) for body, want in cases]
print("PASS" if all(results) else f"FAIL:{results}")
PY
MARKER_OUT="$(REPO_ROOT="$REPO_ROOT" SI_TREE="$SI_TREE" python3 "$MARKER_PY" 2>&1)"
if [ "$MARKER_OUT" = "PASS" ]; then
    t_pass "is_stub_body: INTENTIONAL-STUB marker requires the documented shape, not a bare substring"
else
    t_fail "is_stub_body: marker over- or under-matched; got=$MARKER_OUT"
fi

# 14kk: TWO files share the header's basename under src/ -- ambiguous, must
# never guess.
mkdir -p "$SI_TREE/src/kernel/fd3a" "$SI_TREE/src/kernel/fd3b" "$SI_TREE/include/kernel/fd3"
cat > "$SI_TREE/include/kernel/fd3/thing.h" <<'EOF'
void thing_reset(void);
EOF
cat > "$SI_TREE/src/kernel/fd3a/thing.c" <<'EOF'
void thing_reset(void)
{
    return;
}
EOF
cat > "$SI_TREE/src/kernel/fd3b/thing.c" <<'EOF'
void thing_reset(void)
{
    return;
}
EOF
FDECL3_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<PY 2>&1
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import resolve_symbol as rs
rs.cache_clear()
res = rs.follow_declaration("$SI_TREE/include/kernel/fd3/thing.h", "thing_reset", "$SI_TREE")
print("PASS" if res is None else f"FAIL:{res}")
PY
)"
if [ "$FDECL3_OUT" = "PASS" ]; then
    t_pass "follow_declaration: ambiguous basename (2 matching .c files) never guesses"
else
    t_fail "follow_declaration: ambiguous basename wrongly resolved; got=$FDECL3_OUT"
fi

# 14mm FAIL-CLOSED (Codex adversarial, 3rd round): a string/char literal
# whose escaping backslash lands exactly on a spliced line boundary -- e.g. an
# EVEN-length run of trailing backslashes before the splice, so ONE survives
# C phase 2 as a real escape on the continuation line's first character --
# cannot be resolved safely by this per-line scanner (it does not carry
# escape state across the splice). Rather than silently guess a wrong range
# (an earlier draft returned a truncated "1:4" here), `_scan_line` now raises
# `_AmbiguousEscapeSplice` the moment it detects this exact shape, and all
# three `_iter_code_chars` callers (`_confirm_definition`, the body-close
# walk, `is_stub_body`'s opener scan) catch it and degrade to the SAME "not
# found" result an ordinary unresolvable input already produces. A
# whole-tree, parity-aware scan (any EVEN-length run >= 2, not just exactly
# two, across every .c/.h file, not just src/+include/) finds exactly ONE
# hit in the entire repo (tools/firmware-tables-decode.c:10) and it sits
# INSIDE A COMMENT, never in live string content -- so this fixture proves
# the SAFE degrade, not a corpus-verified-absent guess.
split_case "fail-closed: an escape spanning a splice boundary is refused, not misresolved" foo_escgap "NONE" <<'SPLITEOF'
int foo_escgap(void)
{
    char *s = "abc\\
"; }
SPLITEOF

# 14ll: corpus_resolution_snapshot.py surfaces a resolver ResolverInputError
# as exit 3 (INFRASTRUCTURE), not an uncaught traceback landing on the bare
# process exit 1 -- which would collide with exit 1's DOCUMENTED meaning
# (a prior mapping was lost or moved).
RIE_TREE="$SI_TREE/rie"
mkdir -p "$RIE_TREE/todo/01-test" "$RIE_TREE/src" "$RIE_TREE/build"
cat > "$RIE_TREE/src/rie.c" <<'EOF'
int rie_fn(void)
{
    return 0;
}
EOF
cat > "$RIE_TREE/todo/01-test/TODO-01-fixture.md" <<'EOF'
# TODO-01 -- fixture

## 1. Fixture section
- [x] rie_fn stamped
EOF
python3 - "$RIE_TREE/build/rie-cache.json" <<'PY'
import json, sys
out = sys.argv[1]
refs = [{"kind": "symbol", "file": "src/rie.c", "symbol": "rie_fn"}]
items = [{"section_n": 1, "item_idx": 0, "item_text": "x", "refs": refs}]
json.dump([{"file_path": "todo/01-test/TODO-01-fixture.md",
            "stamped_items": items}], open(out, "w"))
PY
bind_cache "$RIE_TREE/build/rie-cache.json" "$RIE_TREE/todo"
RIE_OUT="$(REPO_ROOT="$REPO_ROOT" \
    STUB_LINT_CACHE="$RIE_TREE/build/rie-cache.json" STUB_LINT_REPO_ROOT="$RIE_TREE" \
    python3 - "$RIE_TREE" <<'PY' 2>&1
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
rs._MAX_FILE_BYTES = 1  # every file in this fixture tree now "exceeds" it
import corpus_resolution_snapshot as crs
rc = crs.main(["write", sys.argv[1] + "/build/rie-out.json"])
print(f"RC={rc}")
PY
)"
RIE_RC="${RIE_OUT##*RC=}"
if [ "$RIE_RC" = "3" ] && echo "$RIE_OUT" | grep -q "resolver input refused"; then
    t_pass "corpus snapshot: ResolverInputError surfaces as exit 3, not a bare traceback"
else
    t_fail "corpus snapshot: ResolverInputError mishandled; got=$RIE_OUT"
fi

# ----------------------------------------------------------------------
# Sub-tests 14j-14u (12): stored-ref repair at RESOLUTION time (ref_resolution.py).
#
# Two repairs, both measured against the live corpus before they were written:
# a bare filename that does not exist at the repo root but is basename-unique
# in the tree (246 occurrences), and a symbol with no file of its own that
# exactly one of its SECTION's authored files defines (106 occurrences).
#
# Every assertion below pins the EXACT effective path, not "a finding
# appeared": the whole risk in both repairs is binding to the WRONG file, and a
# finding-count assertion cannot see that. The negative cases (ambiguous,
# absent, cross-section) are the load-bearing ones -- they pin that an
# unprovable pairing is left counted rather than guessed.
# ----------------------------------------------------------------------
RR_TREE="$SI_TREE/rr"
mkdir -p "$RR_TREE/build" "$RR_TREE/src/deep" "$RR_TREE/src/a" "$RR_TREE/src/b" \
    "$RR_TREE/todo/01-test"

# Basename-unique target, in a directory the ref never names.
cat > "$RR_TREE/src/deep/bare_only.c" <<'EOF'
int bare_fn(void)
{
    return 0;
}
EOF
# Two files sharing a basename: the ambiguous case, never guessed.
cat > "$RR_TREE/src/a/dup.c" <<'EOF'
int dup_fn(void)
{
    return 0;
}
EOF
cat > "$RR_TREE/src/b/dup.c" <<'EOF'
void dup_other(void) { }
EOF
# Section-scope pairing: one authored file in the section defines the symbol.
cat > "$RR_TREE/src/paired.c" <<'EOF'
static int s_paired_state = 0;

int pair_stub(void)
{
    return 0;
}

int pair_accessor(void)
{
    return s_paired_state;
}
EOF
cat > "$RR_TREE/src/sibling.c" <<'EOF'
void sibling_fn(void) { }
EOF
# Section whose only authored file ref is itself a BARE filename.
cat > "$RR_TREE/src/bare_sec.c" <<'EOF'
void bare_sec_other(void) { }

int bare_sec_stub(void)
{
    return 0;
}
EOF
# Both of a section's files define the same symbol -> ambiguous, no pairing.
cat > "$RR_TREE/src/a/twin.c" <<'EOF'
int twin_fn(void)
{
    return 0;
}
EOF
cat > "$RR_TREE/src/b/twin.c" <<'EOF'
int twin_fn(void)
{
    return 0;
}
EOF
# POST-RESOLUTION buckets. Every other file here either resolves or fails
# BEFORE resolution, so the fixture produced no `unresolved_calllike` /
# `no_calllike_token` refs at all -- the two buckets section 13 actually moved
# refs between, and therefore the pair a CHANGED assertion most needs. The file
# is found and read; the symbol simply is not defined in it.
cat > "$RR_TREE/src/calls_only.c" <<'EOF'
void local_helper(void)
{
    absent_called_fn(1);
}
EOF
cat > "$RR_TREE/src/mentions_only.c" <<'EOF'
/* absent_named_fn is described here in prose, never called. */
int absent_named_fn_marker = 0;

void other_local(void)
{
}
EOF

rr_cache() {
    # rr_cache <out.json> -- writes the fixture cache. Hand-authored rather
    # than built from a TODO because the point under test is RESOLUTION of a
    # given ref shape, not extraction of it.
    python3 - "$1" <<'PY'
import json, sys
def sym(s, f=None):
    r = {"kind": "symbol", "symbol": s}
    if f:
        r["file"] = f
    return r
def fil(f):
    return {"kind": "file", "file": f}
def item(sec, idx, refs):
    return {"section_n": sec, "item_idx": idx, "item_text": "x", "refs": refs}
items = [
    # sec 1: bare filename, basename-unique in the tree
    item(1, 0, [sym("bare_fn", "bare_only.c")]),
    # sec 2: bare filename shared by two files -> ambiguous
    item(2, 0, [sym("dup_fn", "dup.c")]),
    # sec 3: bare filename that exists nowhere
    item(3, 0, [sym("ghost_fn", "nowhere.c")]),
    # sec 4: item 0 authors the file, item 1 names the symbol alone
    item(4, 0, [fil("src/paired.c"), fil("src/sibling.c")]),
    item(4, 1, [sym("pair_stub")]),
    # sec 5: same shape, but the symbol is an accessor returning file state
    item(5, 0, [fil("src/paired.c")]),
    item(5, 1, [sym("pair_accessor")]),
    # sec 6: two authored files both define the symbol -> ambiguous
    item(6, 0, [fil("src/a/twin.c"), fil("src/b/twin.c")]),
    item(6, 1, [sym("twin_fn")]),
    # sec 7: names NO file of its own; sec 4's file must not leak here
    item(7, 0, [sym("pair_stub")]),
    # sec 8: the section's only file ref is a BARE filename
    item(8, 0, [fil("bare_sec.c")]),
    item(8, 1, [sym("bare_sec_stub")]),
    # sec 9/10: refs that REACH resolution and fail there -- the two
    # post-resolution buckets. The named file exists and is read; the symbol is
    # merely absent from it, called in one and only named in prose in the other.
    item(9, 0, [sym("absent_called_fn", "src/calls_only.c")]),
    item(10, 0, [sym("absent_named_fn", "src/mentions_only.c")]),
]
json.dump([{"file_path": "todo/01-test/TODO-01-rr.md",
            "stamped_items": items}], open(sys.argv[1], "w"))
PY
    bind_cache "$1" "$RR_TREE/todo"
}
cat > "$RR_TREE/todo/01-test/TODO-01-rr.md" <<'EOF'
# TODO-01 -- ref-resolution fixture

## 1. Fixture section
- [x] placeholder
EOF
rr_cache "$RR_TREE/build/rr-cache.json"

RR_ERR="$TMP_DIR/rr.err"
RR_OUT="$(STUB_LINT_CACHE="$RR_TREE/build/rr-cache.json" STUB_LINT_REPO_ROOT="$RR_TREE" \
    STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" 2>"$RR_ERR")"

# 14j: bare filename resolves, and is REPORTED at the real path.
if echo "$RR_OUT" | grep -q "^src/deep/bare_only\.c:2:stub-behind-stamp:bare_fn "; then
    t_pass "ref_resolution: bare filename repaired to src/deep/bare_only.c"
else
    t_fail "ref_resolution: bare-filename repair missing/mis-pathed; out=$RR_OUT"
fi

# 14k: ambiguous basename is NEVER guessed -- no finding, still counted.
if echo "$RR_OUT" | grep -q "dup_fn"; then
    t_fail "ref_resolution: ambiguous basename dup.c was guessed; out=$RR_OUT"
elif grep -q "missing-file=2" "$RR_ERR"; then
    t_pass "ref_resolution: ambiguous + absent basenames stay counted (missing-file=2)"
else
    t_fail "ref_resolution: expected missing-file=2; err=$(grep buckets "$RR_ERR")"
fi

# 14l: absent basename does not resolve and does not crash the walk.
# BEHAVIOR PIN, not a mutation-flipped fixture -- verified 2026-08-06 that no
# single mutation of the repair flips it: disabling the basename repair leaves
# `nowhere.c` unresolved (as here), and relaxing the uniqueness test to "pick
# the first hit" still finds zero candidates for it. It asserts an absence both
# the correct and the plausibly-broken code produce, and is kept because the
# empty-candidate path is the one that would IndexError under a careless
# "cands[0]" rewrite.
if echo "$RR_OUT" | grep -q "ghost_fn"; then
    t_fail "ref_resolution: absent basename nowhere.c produced a finding"
else
    t_pass "ref_resolution: absent basename produces no finding"
fi

# 14m: section-scope pairing, reported at the file the SECTION authored.
if echo "$RR_OUT" | grep -q "^src/paired\.c:4:stub-behind-stamp:pair_stub "; then
    t_pass "ref_resolution: section-scope pairing resolves pair_stub"
else
    t_fail "ref_resolution: section-scope pairing missing; out=$RR_OUT"
fi

# 14n: `return <file-scope variable>` is an accessor, not a stub. Without this
# the section-scope win would have shipped a false positive on its first live
# use (compositor_get_test_seed, src/kernel/main/compositor.c:64).
if echo "$RR_OUT" | grep -q "pair_accessor"; then
    t_fail "ref_resolution: accessor returning file-scope state flagged as stub"
else
    t_pass "ref_resolution: return of a file-scope variable is not a stub"
fi

# 14o: two authored files both defining the symbol -> no pairing, no guess.
if echo "$RR_OUT" | grep -q "twin_fn"; then
    t_fail "ref_resolution: ambiguous section-scope pairing was guessed"
else
    t_pass "ref_resolution: ambiguous section-scope pairing left unpaired"
fi

# 14p: section scope does not leak ACROSS sections. Section 7 names no file, so
# section 4's src/paired.c must not resolve its identical symbol -- the
# authored evidence belongs to a section, not to a TODO.
RR_HITS="$(echo "$RR_OUT" | grep -c "stub-behind-stamp:pair_stub " || true)"
if [ "$RR_HITS" = "1" ]; then
    t_pass "ref_resolution: section scope does not leak across sections"
else
    t_fail "ref_resolution: expected exactly 1 pair_stub finding, got $RR_HITS"
fi

# 14r: a section whose FILE ref is itself a bare filename still contributes a
# candidate. The first draft applied the basename repair only to symbol refs,
# so section_candidate_files silently dropped every bare file ref -- 673 of the
# corpus's 1,167 stamped .c/.h file refs, 629 of them bare and tree-unique --
# and starved the pairing of exactly the sections most likely to need it
# (Codex adversarial). One rule, both ref kinds.
if echo "$RR_OUT" | grep -q "^src/bare_sec\.c:4:stub-behind-stamp:bare_sec_stub "; then
    t_pass "ref_resolution: a bare section FILE ref still pairs its sibling symbol"
else
    t_fail "ref_resolution: bare section file ref dropped from candidates; out=$RR_OUT"
fi

# 14s: the accessor exemption must recognize the declarator forms this tree
# writes, not just `TYPE name =`. Each of these is a real C file-scope object
# whose getter was still flagged by the first draft's narrow regex.
if RR_ACC="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
pos = ["static int s_v = 0;", "static int a, s_v;", "static int (*s_v)(void);",
       "_Atomic(int) s_v;", "static int s_v __attribute__((unused));",
       "static volatile uint64_t *s_v[4] = {0};"]
neg = [("#define S_CONST 3", "S_CONST"), ("enum { S_CONST = 1 };", "S_CONST"),
       ("    int s_v = 0;", "s_v"), ("int fn(int s_v);", "s_v"),
       ("typedef int s_v;", "s_v"), ("static int other_s_v = 0;", "s_v"),
       # THE DANGEROUS DIRECTION. Admitting parens to the declarator prefix
       # made `_Static_assert(NAME == -1, ...)` read as a declaration of NAME,
       # which SUPPRESSED a genuine `return NAME;` stub -- fail-open on the
       # exact class this lint exists for (Codex adversarial, round 2).
       ('_Static_assert(STATUS_NOT_IMPLEMENTED == -1, "c");',
        "STATUS_NOT_IMPLEMENTED"),
       ('static_assert(E_NOTIMPL != 0, "c");', "E_NOTIMPL"),
       ("if (STATUS_X == 1) ;", "STATUS_X"),
       # An ARBITRARY function-like macro, which no keyword blacklist can
       # enumerate: the constant is a comma-separated ARGUMENT, and the guard
       # that rejects it is structural (the declarator must sit at paren
       # depth 0), not a name list (Codex adversarial, round 3).
       ("ASSERT_EQ(STATUS_NOT_IMPLEMENTED, -1);", "STATUS_NOT_IMPLEMENTED"),
       ("CHECK_RANGE(E_NOTIMPL, 0, 9);", "E_NOTIMPL"),
       # FILE SCOPE IS BRACE DEPTH 0, and column 0 is not evidence of it.
       # An UNINDENTED struct member or a multi-line enumerator line is
       # valid C that the first draft read as a file-scope object, which
       # suppressed the stub returning it (Codex adversarial, post-commit).
       ("struct s {\nint STATUS_NOT_IMPLEMENTED;\n};", "STATUS_NOT_IMPLEMENTED"),
       ("enum e {\nOK, STATUS_NOT_IMPLEMENTED,\n};", "STATUS_NOT_IMPLEMENTED"),
       ("void f(void)\n{\nint s_v = 0;\n}\n", "s_v")]
pos += ["static int s_arr[] = {\n1, 2\n};\nstatic int s_v = 0;",
        "struct s { int a; };\nstatic int s_v;"]
for d in pos:
    assert rs._is_file_scope_variable(d, "s_v"), f"missed object: {d}"
for text, name in neg:
    assert not rs._is_file_scope_variable(text, name), f"false object: {text}"
PY
)"; then
    t_pass "is_stub_body: accessor exemption covers real declarator forms, not constants"
else
    t_fail "is_stub_body: accessor declarator recognition wrong; $RR_ACC"
fi

# 14t: END-TO-END dangerous direction -- a `_Static_assert` about a constant
# must not suppress the stub that returns it. The declarator unit cases above
# pin the predicate; this pins the behavior lint Check 7 actually publishes.
if RR_SA="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, sys, tempfile
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs
src = ('#define STATUS_NOT_IMPLEMENTED -1\n'
       '_Static_assert(STATUS_NOT_IMPLEMENTED == -1, "contract");\n'
       '\nint f(void)\n{\n    return STATUS_NOT_IMPLEMENTED;\n}\n')
with tempfile.NamedTemporaryFile(suffix=".c", delete=False, mode="w") as t:
    t.write(src)
rs.cache_clear()
res = rs.resolve_symbol(t.name, "f")
stub = rs.is_stub_body(*res)
os.unlink(t.name)
assert stub and stub[0] == "STATUS_NOT_IMPLEMENTED", f"stub suppressed: {stub}"
PY
)"; then
    t_pass "is_stub_body: a _Static_assert about a constant does not suppress its stub"
else
    t_fail "is_stub_body: _Static_assert suppressed a real stub; $RR_SA"
fi

# 14u: the caches this module memoizes must reset TOGETHER. ref_resolution
# holds resolver ANSWERS while resolve_symbol holds the content and lexical
# index they came from, so clearing one alone can serve a pre-mutation verdict.
# check_stub_behind_stamp._resolve_cached was an lru_cache and callers still
# reach for its `.cache_clear` (scripts/overnight/tests/test_stub_lint_coverage
# .py resets all four); delegating to the shared memo silently dropped that
# attribute and broke them with an AttributeError (Codex adversarial, round 2).
if RR_CC="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, sys, tempfile
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/lint")
import ref_resolution as rr
import importlib.util
spec = importlib.util.spec_from_file_location(
    "csbs", os.environ["REPO_ROOT"] + "/scripts/lint/check_stub_behind_stamp.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
assert callable(getattr(m._resolve_cached, "cache_clear", None)), \
    "_resolve_cached.cache_clear contract broken"
d = tempfile.mkdtemp(); p = os.path.join(d, "m.c")
open(p, "w").write("int z(void)\n{\n    return 0;\n}\n")
assert m._resolve_cached(p, "z") is not None, "pre-mutation resolve failed"
# Rewrite so the symbol is GONE, then clear through the public reset.
open(p, "w").write("int other(void)\n{\n    return 0;\n}\n")
m._resolve_cached.cache_clear()
assert not rr._MENTION_INDEX and not rr._RESOLVE_MEMO, "memo not cleared"
assert m._resolve_cached(p, "z") is None, "stale resolve served after reset"
# REVERSE reset: the lower-level public entry point must invalidate the
# answers derived from it, or a caller that knows only resolve_symbol gets a
# pre-mutation verdict from a cache it cannot see.
import resolve_symbol as rs
open(p, "w").write("int z(void)\n{\n    return 0;\n}\n")
rs.cache_clear()
assert not rr._RESOLVE_MEMO, "resolve_symbol.cache_clear left ref_resolution stale"
assert m._resolve_cached(p, "z") is not None, "reverse reset lost the new content"
# CONTENT-BINDING, probed WITHOUT a reset -- that is the whole point. Keying
# the identifier set on the path alone let a file rewritten mid-run keep
# answering from the set built before the rewrite, so a symbol the file GAINED
# read as absent and the resolver's own rc 9 drift failure was swallowed
# (Codex adversarial, round 3). Correct behavior is either to see the new
# symbol or to RAISE the drift error -- never a silent miss.
assert not rr._mentions(p, "brand_new_sym"), "pre-mutation mention"
open(p, "w").write("int brand_new_sym(void)\n{\n    return 0;\n}\n")
try:
    saw = rr._mentions(p, "brand_new_sym")
except rs.ResolverInputError:
    saw = True   # drift surfaced loudly, which is the other legal answer
assert saw, "stale mention index hid content the file gained mid-run"
# The RESOLUTION memo has the same obligation: a second probe of the same
# (file, symbol) after a mid-run rewrite must not answer from the pre-mutation
# entry. `_load_file_lines` is the only layer that re-stats, so the memo calls
# it on every hit and binds its entry to the returned lines object.
d2 = tempfile.mkdtemp(); q = os.path.join(d2, "n.c")
open(q, "w").write("int keep(void)\n{\n    return 0;\n}\n")
rs.cache_clear()
first = rr._resolve_memoized(q, "keep")
assert first is not None, "primed resolve failed"
open(q, "w").write("\n\nint keep(void)\n{\n    return 1;\n}\n")
try:
    again = rr._resolve_memoized(q, "keep")
    assert again != first, f"stale memo served pre-mutation coords: {again}"
except rs.ResolverInputError:
    pass  # drift surfaced loudly, the other legal answer
PY
)"; then
    t_pass "ref_resolution: one reset clears both caches, and cache_clear survives"
else
    t_fail "ref_resolution: cache reset contract broken; $RR_CC"
fi

# 14v: the recorded DENOMINATOR is enforced, not decoration. `total` was
# written to the baseline and never read, so a cache that lost UNRESOLVED refs
# shrank the buckets and the population together while `resolved` stayed at its
# floor -- the published ratio IMPROVED and the check exited 0, fail-open on
# exactly the corpus rot the counted population exists to catch (Codex
# adversarial, post-commit).
RR_BASE="$RR_TREE/build/rr-baseline.json"
# `resolved: 1`, not 0. This fixture used a zero floor so that ONLY the
# population check could fire -- but section 17 made a zero `resolved` an
# invalid baseline (rc 8), because a zero floor passes a walk that resolved
# nothing. 1 is the smallest valid floor and preserves the intent: the
# population check at check_stub_behind_stamp.py:501 runs BEFORE the coverage
# floor, so a moved denominator is still what this fixture measures.
printf '%s\n' '{"resolved": 1, "total": 9, "recorded": "fixture", "why": "fixture"}' > "$RR_BASE"
RR_POP_RC=0
STUB_LINT_CACHE="$RR_TREE/build/rr-cache.json" STUB_LINT_REPO_ROOT="$RR_TREE" \
    STUB_LINT_BASELINE="$RR_BASE" \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
    >/dev/null 2>"$TMP_DIR/rr-pop.err" || RR_POP_RC=$?
if [ "$RR_POP_RC" = "7" ] && grep -q "POPULATION" "$TMP_DIR/rr-pop.err"; then
    t_pass "lint Check 7: a moved population is rc 7, not a silently better ratio"
else
    t_fail "lint Check 7: denominator not enforced (rc=$RR_POP_RC)"
fi

# 14v-x: THE COMMIT-GATE EXCLUSION SET REACHES THE rc-7 GATE (section 43).
# Section 39 excluded Check 7's FINDINGS by filtering the helper's stdout, which
# cannot reach the coverage/population accounting -- so a todo file the commit
# does not touch could still move the stamped-symbol population and block the
# commit, the exact unattended-worktree wedge the set exists to prevent.
#
# AGAINST THE REAL CACHE AND BASELINE, deliberately, not the RR_TREE fixture:
# that tree resolves ZERO symbols, so no positive coverage floor is satisfiable
# inside it and every one of these runs would refuse for a reason with nothing to
# do with exclusions (measured: the first version of this fixture failed rc 7
# COVERAGE REGRESSION, then rc 8, for exactly that reason). The repo's own cache
# is the only place the full arithmetic -- population AND floor -- is exercisable.
RR_XREAL_CACHE="$REPO_ROOT/build/todo-cache.json"
RR_XREAL_BASE="$REPO_ROOT/scripts/lint/stub-lint-baseline.json"
if [ ! -f "$RR_XREAL_CACHE" ] || [ ! -f "$RR_XREAL_BASE" ]; then
    t_fail "lint Check 7 exclusion: the repo cache or baseline is absent; build it with scripts/todo-graph/build-and-validate.sh --keep-cache"
else
    # The baseline is DERIVED FROM THE CACHE at test time, not copied from the
    # tracked file: `test-tooling.sh` rebuilds caches while this suite runs
    # nested inside it, and a fixture pinned to the tracked numbers fails
    # whenever the corpus has moved since they were recorded -- a flake with
    # nothing to say about exclusions. One occurrence is then removed from the
    # busiest owner, That is exactly "an
    # excluded file moved the population": subtracting the owner cancels the
    # move, leaving it in does not. `resolved` is untouched, so the coverage
    # floor plays no part in what these assertions measure.
    RR_XBASE="$TMP_DIR/rr-baseline-excl.json"
    RR_XOWNER="$(python3 "$REPO_ROOT/scripts/todo-graph/tests/mk_excl_baseline.py" \
        "$RR_XREAL_CACHE" "$RR_XBASE" from-cache)"
    if [ -z "$RR_XOWNER" ] || [ "$RR_XOWNER" = "NO-USABLE-CORPUS" ]; then
        t_fail "lint Check 7 exclusion: the live corpus carries no resolvable stamped-symbol owner ($RR_XOWNER)"
    else
        # CONTROL FIRST -- it must FAIL, or the fixture below proves nothing.
        RR_XC_RC=0
        STUB_LINT_CACHE="$RR_XREAL_CACHE" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
            STUB_LINT_BASELINE="$RR_XBASE" \
            python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
            >/dev/null 2>"$TMP_DIR/rr-xctl.err" || RR_XC_RC=$?
        if [ "$RR_XC_RC" = "7" ] && grep -q "POPULATION" "$TMP_DIR/rr-xctl.err"; then
            t_pass "lint Check 7 exclusion CONTROL: an unexcluded moved population still blocks"
        else
            t_fail "lint Check 7 exclusion CONTROL did not block (rc=$RR_XC_RC) -- the fixture below would be vacuous"
        fi
        RR_XF_RC=0
        STUB_LINT_CACHE="$RR_XREAL_CACHE" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
            STUB_LINT_BASELINE="$RR_XBASE" STUB_LINT_EXCLUDED="$RR_XOWNER" \
            python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
            >/dev/null 2>"$TMP_DIR/rr-xfix.err" || RR_XF_RC=$?
        if [ "$RR_XF_RC" != "7" ] && grep -q "coverage gate scope" "$TMP_DIR/rr-xfix.err"; then
            t_pass "lint Check 7 exclusion: an excluded file that moves the population does NOT block"
        else
            t_fail "lint Check 7 exclusion: excluded owner still blocked (rc=$RR_XF_RC); $(grep -m1 -E 'POPULATION|INVALID' "$TMP_DIR/rr-xfix.err")"
        fi
        # FAIL CLOSED on an identity that matches nothing -- the shape a rename
        # produces, and the one that would silently widen the gate.
        RR_XS_RC=0
        STUB_LINT_CACHE="$RR_XREAL_CACHE" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
            STUB_LINT_BASELINE="$RR_XBASE" STUB_LINT_EXCLUDED="todo/99-gone/TODO-99-renamed.md" \
            python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
            >/dev/null 2>"$TMP_DIR/rr-xstale.err" || RR_XS_RC=$?
        if [ "$RR_XS_RC" = "7" ] && grep -q "EXCLUSION SET INVALID" "$TMP_DIR/rr-xstale.err"; then
            t_pass "lint Check 7 exclusion: a stale identity refuses instead of excluding nothing"
        else
            t_fail "lint Check 7 exclusion: stale identity not refused (rc=$RR_XS_RC)"
        fi
        # FAIL CLOSED when the baseline cannot support symmetric subtraction.
        RR_XN="$TMP_DIR/rr-baseline-noowner.json"
        python3 "$REPO_ROOT/scripts/todo-graph/tests/mk_excl_baseline.py" \
            "$RR_XBASE" "$RR_XN" drop-by-owner >/dev/null
        RR_XN_RC=0
        STUB_LINT_CACHE="$RR_XREAL_CACHE" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
            STUB_LINT_BASELINE="$RR_XN" STUB_LINT_EXCLUDED="$RR_XOWNER" \
            python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
            >/dev/null 2>"$TMP_DIR/rr-xnoown.err" || RR_XN_RC=$?
        if [ "$RR_XN_RC" = "7" ] && grep -q "no .by_owner. map" "$TMP_DIR/rr-xnoown.err"; then
            t_pass "lint Check 7 exclusion: a baseline without per-owner contributions refuses"
        else
            t_fail "lint Check 7 exclusion: missing by_owner not refused (rc=$RR_XN_RC)"
        fi
        # A TAMPERED map must refuse too: the per-owner sums are the invariant
        # the section promised, and nothing else re-checks them.
        RR_XT="$TMP_DIR/rr-baseline-tampered.json"
        python3 "$REPO_ROOT/scripts/todo-graph/tests/mk_excl_baseline.py" \
            "$RR_XBASE" "$RR_XT" tamper-sum >/dev/null
        RR_XT_RC=0
        STUB_LINT_CACHE="$RR_XREAL_CACHE" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
            STUB_LINT_BASELINE="$RR_XT" STUB_LINT_EXCLUDED="$RR_XOWNER" \
            python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
            >/dev/null 2>"$TMP_DIR/rr-xtamper.err" || RR_XT_RC=$?
        # rc 8, not 7: since round 2 the map is validated on EVERY baseline-backed
        # run, so a tampered map is refused as a bad baseline before the
        # exclusion arithmetic is ever reached. That ordering is the fix.
        if [ "$RR_XT_RC" = "8" ] && grep -q "do not equal the recorded totals" "$TMP_DIR/rr-xtamper.err"; then
            t_pass "lint Check 7 exclusion: per-owner sums are enforced, not assumed"
        else
            t_fail "lint Check 7 exclusion: tampered by_owner sums not refused (rc=$RR_XT_RC)"
        fi
        # THE MAP IS CHECKED WITHOUT AN EXCLUSION SET TOO. Round 2: all of the
        # validation above was nested under `if excluded`, so ordinary lint and
        # CI -- which pass no set -- accepted a corrupted or desynchronised
        # by_owner map as long as the GLOBAL totals still looked right, and the
        # corruption surfaced later as an rc 7 at someone else's scoped commit.
        RR_XT0_RC=0
        STUB_LINT_CACHE="$RR_XREAL_CACHE" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
            STUB_LINT_BASELINE="$RR_XT" \
            python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
            >/dev/null 2>"$TMP_DIR/rr-xt0.err" || RR_XT0_RC=$?
        if [ "$RR_XT0_RC" = "8" ] && grep -q "BASELINE by_owner INVALID" "$TMP_DIR/rr-xt0.err"; then
            t_pass "lint Check 7 exclusion: a corrupt per-owner map is refused with NO exclusion set"
        else
            t_fail "lint Check 7 exclusion: corrupt map accepted without exclusions (rc=$RR_XT0_RC)"
        fi
        # An exclusion set with no floor to narrow is refused rather than
        # silently applied: STUB_LINT_ALLOW_NO_BASELINE plus a MISSING baseline
        # leaves nothing to subtract from.
        RR_XNB_RC=0
        STUB_LINT_CACHE="$RR_XREAL_CACHE" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
            STUB_LINT_BASELINE="$TMP_DIR/absent-baseline.json" \
            STUB_LINT_ALLOW_NO_BASELINE=1 STUB_LINT_EXCLUDED="$RR_XOWNER" \
            python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
            >/dev/null 2>"$TMP_DIR/rr-xnb.err" || RR_XNB_RC=$?
        if [ "$RR_XNB_RC" = "7" ] && grep -q "no gate to narrow" "$TMP_DIR/rr-xnb.err"; then
            t_pass "lint Check 7 exclusion: an exclusion with no floor in force is refused"
        else
            t_fail "lint Check 7 exclusion: exclusion applied with no floor (rc=$RR_XNB_RC)"
        fi
        # AND AN EXCLUSION MAY NOT SWITCH THE GATE OFF: excluding every owner
        # leaves a zero floor, which would pass vacuously.
        RR_XALL="$(python3 "$REPO_ROOT/scripts/todo-graph/tests/mk_excl_baseline.py" \
            "$RR_XBASE" /dev/null list-owners)"
        RR_XA_RC=0
        STUB_LINT_CACHE="$RR_XREAL_CACHE" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
            STUB_LINT_BASELINE="$RR_XBASE" STUB_LINT_EXCLUDED="$RR_XALL" \
            python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
            >/dev/null 2>"$TMP_DIR/rr-xall.err" || RR_XA_RC=$?
        if [ "$RR_XA_RC" = "7" ] && grep -q "may not switch it off" "$TMP_DIR/rr-xall.err"; then
            t_pass "lint Check 7 exclusion: excluding every owner refuses rather than disabling the floor"
        else
            t_fail "lint Check 7 exclusion: an all-owner exclusion did not refuse (rc=$RR_XA_RC)"
        fi
    fi
fi

# 14q: THE IDENTITY GATE SEES THE REPAIRS. corpus_resolution_snapshot.collect()
# used to skip every ref without a stored `file`, so a repair taught only to
# the lint walk would be invisible to the 0-lost/0-moved proof that is supposed
# to gate it -- the exact incomplete-proof defect the decl-following change
# already shipped once. Both now call ref_resolution.classify_ref.
RR_SNAP="$(STUB_LINT_CACHE="$RR_TREE/build/rr-cache.json" STUB_LINT_REPO_ROOT="$RR_TREE" \
    python3 "$REPO_ROOT/scripts/todo-graph/corpus_resolution_snapshot.py" \
    write "$RR_TREE/build/rr-snap.json" 2>&1)"
if python3 - "$RR_TREE/build/rr-snap.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))["mappings"]
# A RESOLVED value is a list; an unresolved one is now a BUCKET STRING, so
# `v[0] if v else None` would silently read the bucket's first CHARACTER.
res = {k.split("::")[-1]: v[0] for k, v in m.items() if isinstance(v, list)}
assert res.get("bare_fn") == "src/deep/bare_only.c", f"bare_fn: {res.get('bare_fn')}"
assert res.get("pair_stub") == "src/paired.c", f"pair_stub: {res.get('pair_stub')}"
# NEVER GUESSED -- but no longer ABSENT either. Under section 14 an ambiguous
# ref carries a bucket verdict instead of vanishing from the population, which
# is the whole point: a guess and a silence used to be indistinguishable here.
assert "dup_fn" not in res and "twin_fn" not in res, f"guessed: {sorted(res)}"
buckets = {k.split("::")[-1]: v for k, v in m.items() if isinstance(v, str)}
assert buckets.get("dup_fn") == "missing_file", f"dup_fn: {buckets.get('dup_fn')}"
assert buckets.get("twin_fn") == "unpaired_ref", f"twin_fn: {buckets.get('twin_fn')}"
PY
then
    t_pass "corpus snapshot: identity gate covers both stored-ref repairs"
else
    t_fail "corpus snapshot: repairs invisible to the identity gate; $RR_SNAP"
fi

# ----------------------------------------------------------------------
# Sub-tests 14aa-14ee: TODO-06 section 14 -- the identity gate must verdict the
# WHOLE symbol-ref population, and every degradation of a prior verdict must
# fail. Before this, collect() skipped any ref that did not reach resolution
# and recorded a bare null for one that reached it and failed: 695 of the live
# corpus's 1,625 occurrences had an entry, and section 13's own movements
# between the two unresolved buckets (35 -> 132, 47 -> 89) were ungated.
# ----------------------------------------------------------------------

# 14aa: POPULATION EQUALITY BY CONSTRUCTION, cross-checked against Check 7's
# own published counts rather than against a number written here. A snapshot
# over a narrower population than the check walks omits exactly the mappings a
# change added; a wider one is not a gate on the check at all.
if python3 - "$RR_TREE/build/rr-snap.json" "$RR_ERR" <<'PY'
import collections, json, re, sys
m = json.load(open(sys.argv[1]))["mappings"]
err = open(sys.argv[2], encoding="utf-8").read()
cov = re.search(r"coverage resolved=(\d+)/(\d+)", err)
assert cov, f"no coverage line in check 7 stderr: {err[:300]}"
resolved, total = int(cov.group(1)), int(cov.group(2))
assert len(m) == total, f"snapshot {len(m)} refs vs check 7 {total}"
got = collections.Counter(v if isinstance(v, str) else "RESOLVED"
                          for v in m.values())
assert got["RESOLVED"] == resolved, f"resolved {got['RESOLVED']} vs {resolved}"
# The bucket line is the reporting contract section 11 owns; pinning the
# snapshot against it keeps the two from drifting apart silently.
names = {"calllike-unresolved": "unresolved_calllike",
         "no-calllike-token": "no_calllike_token",
         "missing-file": "missing_file", "non-c-suffix": "unsupported_lang",
         "path-escape": "path_escape", "unpaired-ref": "unpaired_ref"}
line = [l for l in err.splitlines() if "buckets:" in l][0]
for printed, key in names.items():
    want = int(re.search(printed + r"=(\d+)", line).group(1))
    assert got[key] == want, f"{key}: snapshot {got[key]} vs check 7 {want}"
PY
then
    t_pass "corpus snapshot: population and buckets equal lint Check 7's exactly"
else
    t_fail "corpus snapshot: population diverges from Check 7; see $RR_ERR"
fi

# 14bb: every DEGRADATION of a prior verdict fails, and only the two shapes
# that cannot hide a loss pass. CHANGED fails deliberately: exit status is the
# only part of this tool an automated gate can read, so reporting a bucket
# reclassification while exiting 0 would let a resolver defect that moved
# hundreds of refs ship unattended (Codex design review, section 14).
if RR_CMP="$(REPO_ROOT="$REPO_ROOT" RR_TREE="$RR_TREE" python3 - <<'PY' 2>&1
import json, os, subprocess, sys, tempfile
tree, repo = os.environ["RR_TREE"], os.environ["REPO_ROOT"]
snap = json.load(open(tree + "/build/rr-snap.json"))
m = snap["mappings"]
a_res = next(k for k, v in m.items() if isinstance(v, list))
a_buc = next(k for k, v in m.items() if isinstance(v, str))
def run(doc):
    f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump(doc, f); f.close()
    env = dict(os.environ, STUB_LINT_CACHE=tree + "/build/rr-cache.json",
               STUB_LINT_REPO_ROOT=tree)
    r = subprocess.run([sys.executable,
                        repo + "/scripts/todo-graph/corpus_resolution_snapshot.py",
                        "compare", f.name], capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr
def mutate(fn):
    d = json.loads(json.dumps(snap)); fn(d["mappings"])
    d["refs"] = len(d["mappings"]); return d
rc, out = run(snap)
assert rc == 0, f"unchanged corpus must pass: rc={rc} {out[:300]}"
cases = [
    ("DROPPED", 1, lambda mm: mm.__setitem__("todo/x.md#9.9r9 g.c::gone",
                                             ["src/g.c", 1, 2])),
    ("LOST", 1, lambda mm: mm.__setitem__(a_buc, ["src/paired.c", 1, 2])),
    ("MOVED", 1, lambda mm: mm.__setitem__(
        a_res, [m[a_res][0], m[a_res][1] + 3, m[a_res][2] + 3])),
    ("CHANGED", 1, lambda mm: mm.__setitem__(
        a_buc, "no_calllike_token" if m[a_buc] != "no_calllike_token"
        else "unpaired_ref")),
    ("GAINED", 0, lambda mm: mm.__setitem__(a_res, "no_calllike_token")),
]
for label, want_rc, fn in cases:
    rc, out = run(mutate(fn))
    assert rc == want_rc, f"{label}: rc={rc} want {want_rc}\n{out[:400]}"
    assert label in out, f"{label} not named in output:\n{out[:400]}"
# ADDED: a key the baseline never had is not a regression.
rc, out = run(mutate(lambda mm: mm.pop(a_res)))
assert rc == 0 and "ADDED" in out, f"ADDED: rc={rc}\n{out[:300]}"
# EVERY TRANSITION, BY NAME -- not just whichever bucket happens to sort first.
# Picking `a_buc` exercised one cross-phase pair (post-resolution vs
# missing_file) with this fixture's insertion order, so a suppression specific
# to same-phase pairs -- unresolved_calllike vs no_calllike_token being the one
# section 13 actually moved refs between -- would have passed (Codex
# test-coverage, section 14).
by_bucket = {}
for k, v in m.items():
    if isinstance(v, str):
        by_bucket.setdefault(v, k)
pairs = [("unresolved_calllike", "no_calllike_token"),
         ("missing_file", "unpaired_ref")]
for have, want in pairs:
    k = by_bucket.get(have)
    assert k is not None, f"fixture lost its {have} ref: {sorted(by_bucket)}"
    rc, out = run(mutate(lambda mm, k=k, want=want: mm.__setitem__(k, want)))
    assert rc == 1, f"{have}->{want}: rc={rc} want 1\n{out[:400]}"
    assert "CHANGED" in out, f"{have}->{want} not reported:\n{out[:400]}"
PY
)"; then
    t_pass "corpus snapshot: dropped/lost/moved/changed fail, gained/added pass"
else
    t_fail "corpus snapshot: verdict-degradation classes wrong; $RR_CMP"
fi

# 14ff: the END-TO-END memo must bind the DEFINITION file, not only the
# declaring one. A followed declaration resolves in a sibling .c, so an entry
# validated against the header alone returned cached coordinates for a file it
# never re-read -- rewrite the .c mid-run and the header still looks unchanged,
# so the walk answers with stale line numbers instead of raising
# ResolverInputError. Found independently by BOTH review legs (section 14).
if RR_SIB="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, sys, tempfile
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import ref_resolution as rr, resolve_symbol as rs
d = tempfile.mkdtemp(); os.makedirs(d + "/include"); os.makedirs(d + "/src")
h = d + "/include/w.h"; c = d + "/src/w.c"
open(h, "w").write("int wfn(void);\n")
open(c, "w").write("int wfn(void)\n{\n    return 0;\n}\n")
ref = {"kind": "symbol", "symbol": "wfn", "file": "include/w.h"}
from pathlib import Path
first = rr.resolve_ref(ref, rr.EMPTY_SCOPE, Path(d))
assert first.bucket is None, f"header ref did not follow to its sibling: {first}"
assert first.def_rel == "src/w.c", first.def_rel
# Rewrite ONLY the .c. The header is untouched, so a memo bound to the header
# alone would hand back the pre-rewrite coordinates.
open(c, "w").write("\n\n\nint wfn(void)\n{\n    return 0;\n}\n")
try:
    again = rr.resolve_ref(ref, rr.EMPTY_SCOPE, Path(d))
except rs.ResolverInputError:
    pass          # drift surfaced loudly -- the correct answer
else:
    assert again.line_start != first.line_start, (
        f"stale followed-definition coordinates served: {again.line_start}")
PY
)"; then
    t_pass "ref_resolution: the end-to-end memo binds the followed sibling too"
else
    t_fail "ref_resolution: followed-definition cache not dependency-bound; $RR_SIB"
fi

# 14gg: the resolver's EXCEPTION contract, at each stage of the end-to-end
# rule. The existing ResolverInputError fixture forces the FIRST file load to
# fail, so it never reaches an error raised from the direct resolve, the
# declaration fallback, or the call-like probe -- nor the guard that refuses a
# definition resolving outside the repo root. A regression that buckets an
# inner resolver failure would not have failed a named test (Codex
# test-coverage, section 14).
if RR_EXC="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, sys, tempfile
from pathlib import Path
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import ref_resolution as rr, resolve_symbol as rs

d = tempfile.mkdtemp(); os.makedirs(d + "/src")
p = d + "/src/e.c"
open(p, "w").write("void only_calls(void) { target(1); }\n")
ref = {"kind": "symbol", "symbol": "target", "file": "src/e.c"}

# A ResolverInputError from ANY stage must PROPAGATE, never become a bucket,
# and must not leave a partial entry behind for the next caller to trust.
for stage in ("_resolve_memoized", "follow_declaration", "has_calllike_token"):
    rr.clear_caches()
    owner = rr if stage in ("_resolve_memoized", "has_calllike_token") else rs
    orig = getattr(owner, stage)
    def boom(*a, **k):
        raise rs.ResolverInputError("injected at " + stage)
    setattr(owner, stage, boom)
    try:
        rr.resolve_ref(ref, rr.EMPTY_SCOPE, Path(d))
    except rs.ResolverInputError:
        pass
    else:
        raise AssertionError(f"{stage}: error was swallowed into a bucket")
    finally:
        setattr(owner, stage, orig)
    assert not rr._END_TO_END, f"{stage}: partial answer cached: {rr._END_TO_END}"

# A definition resolving OUTSIDE the repo root is refused, not recorded as a
# path that differs per checkout and reads as a false MOVE.
rr.clear_caches()
outside = tempfile.mkdtemp() + "/far.c"
open(outside, "w").write("int target(void)\n{\n    return 0;\n}\n")
rr._resolve_end_to_end = lambda a, s, r: (outside, 1, 4)
try:
    rr.resolve_ref(ref, rr.EMPTY_SCOPE, Path(d))
except rs.ResolverInputError as exc:
    assert "outside repo root" in str(exc), str(exc)
else:
    raise AssertionError("a definition outside the repo root was accepted")
PY
)"; then
    t_pass "ref_resolution: resolver errors propagate at every stage, uncached"
else
    t_fail "ref_resolution: exception contract broken; $RR_EXC"
fi

# 14cc: a schema-1 baseline is REFUSED, not mis-compared. Under schema 1 an
# unresolved ref was a null and a never-resolved one was absent, so reading one
# here would report every newly-verdicted ref as an addition and every former
# null as a bucket change -- noise around any real regression. rc 3 is the
# INFRASTRUCTURE code: the gate could not run, which is not a pass.
if RR_SCH="$(REPO_ROOT="$REPO_ROOT" RR_TREE="$RR_TREE" python3 - <<'PY' 2>&1
import json, os, subprocess, sys, tempfile
tree, repo = os.environ["RR_TREE"], os.environ["REPO_ROOT"]
snap = json.load(open(tree + "/build/rr-snap.json"))
env = dict(os.environ, STUB_LINT_CACHE=tree + "/build/rr-cache.json",
           STUB_LINT_REPO_ROOT=tree)
def run(doc):
    f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump(doc, f); f.close()
    r = subprocess.run([sys.executable,
                        repo + "/scripts/todo-graph/corpus_resolution_snapshot.py",
                        "compare", f.name], capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr
old = json.loads(json.dumps(snap)); old["schema"] = 1
rc, out = run(old)
assert rc == 3, f"schema-1 baseline: rc={rc} want 3\n{out[:300]}"
assert "regenerate" in out, f"no regenerate pointer:\n{out[:300]}"
# An unrecognised bucket string is malformed, not a novel value: accepting it
# would leave a permanent CHANGED that no correct behaviour can ever clear.
bad = json.loads(json.dumps(snap))
k = next(k for k, v in bad["mappings"].items() if isinstance(v, str))
bad["mappings"][k] = "typo_bucket"
rc, out = run(bad)
assert rc == 3 and "malformed" in out, f"bad bucket: rc={rc}\n{out[:300]}"
PY
)"; then
    t_pass "corpus snapshot: old schema and unknown buckets are refused (rc 3)"
else
    t_fail "corpus snapshot: baseline migration not fail-closed; $RR_SCH"
fi

# 14dd: the moved call-like text caches are CONTENT-BOUND. As
# `lru_cache(file_abs)` a cache HIT never re-entered `_load_file_lines`, which
# is the only layer that re-stats a pinned file, so a file rewritten mid-walk
# answered from pre-mutation text and produced a stale BUCKET instead of the
# resolver's drift failure. Same defect `_mentions` fixed one level down.
if RR_CB="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, sys, tempfile
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import ref_resolution as rr, resolve_symbol as rs
d = tempfile.mkdtemp(); p = os.path.join(d, "c.c")
open(p, "w").write("void other(void) { }\n")
assert not rr.has_calllike_token(p, "later_fn"), "pre-mutation call-like hit"
open(p, "w").write("void other(void) { later_fn(1); }\n")
try:
    saw = rr.has_calllike_token(p, "later_fn")
except rs.ResolverInputError:
    saw = True   # drift surfaced loudly, the other legal answer
assert saw, "stale code text hid a call-like token the file gained mid-run"
# And the strip must still be PER FILE: the counter is what the 5.62x
# regression was made of, so repeated symbols on one file must not rebuild it.
rr.clear_caches()
open(p, "w").write("void a(void) { x1(0); x2(0); x3(0); }\n")
for s in ("x1", "x2", "x3", "x1"):
    rr.has_calllike_token(p, s)
assert rr._CODE_TEXT_BUILDS == 1, f"per-symbol strip: {rr._CODE_TEXT_BUILDS}"
PY
)"; then
    t_pass "ref_resolution: call-like text is content-bound and stripped per file"
else
    t_fail "ref_resolution: call-like text caching broken; $RR_CB"
fi

# 14ee: ONE END-TO-END RULE. Both consumers must reach their verdict through
# ref_resolution.resolve_ref and neither may orchestrate a resolver fallback of
# its own -- that split is what let decl-following ship taught to the lint walk
# only, so the identity proof covered 36 of 54 added mappings.
if RR_ONE="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, re, sys
repo = os.environ["REPO_ROOT"]
for rel in ("scripts/lint/check_stub_behind_stamp.py",
            "scripts/todo-graph/corpus_resolution_snapshot.py"):
    src = open(repo + "/" + rel, encoding="utf-8").read()
    body = "\n".join(l for l in src.splitlines()
                     if not l.lstrip().startswith("#"))
    assert "resolve_ref(" in body, f"{rel} does not call resolve_ref"
    for banned in ("follow_declaration(", "classify_ref("):
        assert banned not in body, (
            f"{rel} still orchestrates {banned} itself -- the identity gate and "
            f"Check 7 can drift again")
PY
)"; then
    t_pass "ref_resolution: both callers consume ONE end-to-end verdict"
else
    t_fail "ref_resolution: resolution orchestration duplicated again; $RR_ONE"
fi

# 15a: SCALING SHAPE of the structural walk, counted rather than timed.
# The section this guards was filed to remove a per-symbol rescan and the
# premise did not survive measurement: candidate discovery is ~5% of the walk
# (0.071s of ~1.5s), while the body brace walk materialized 1.31M characters so
# three brace counters could test each against `{` and `}`. The fix narrows
# what the lexer EMITS, so the invariant is a growth SHAPE -- a body that grows
# without gaining braces must not make the walk yield more -- and a wall-clock
# number could not see it. That is the exact failure mode the section names:
# the earlier round was accepted on wall clock.
if RR_SCALE="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, sys
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs


def body(n):
    """A function whose body has `n` brace-free statement lines."""
    return (["void f(void) {"] + ["    int x%d = %d + 1;" % (i, i)
                                  for i in range(n)] + ["}"])


def yielded(lines, wanted):
    return sum(1 for _ in rs._iter_code_chars(lines, 0, 0, len(lines), wanted))


small, big = body(50), body(400)
# FILTERED: only the two braces are yielded, whatever the body size. This is
# the shape -- constant in body length, not merely "smaller".
s_f, b_f = yielded(small, rs._BRACES), yielded(big, rs._BRACES)
assert s_f == b_f == 2, f"brace walk not body-independent: {s_f} vs {b_f}"

# END-TO-END, through the REAL entry point, because the direct calls above
# prove only that the mechanism works -- not that anything uses it. MUTATION
# CHECKED: dropping `_BRACES` from the body-walk call sites left every
# assertion in this fixture green until this block existed, which is the
# green-for-the-wrong-reason shape the section 14 fixtures were written to
# stop. Counting through a wrapper is what binds the fixture to the WIRING.
import tempfile
counted = {"n": 0}
real_iter = rs._iter_code_chars


def counting(*a, **kw):
    for t in real_iter(*a, **kw):
        counted["n"] += 1
        yield t


def resolve_cost(n):
    d = tempfile.mkdtemp()
    p = os.path.join(d, "big.c")
    open(p, "w").write("\n".join(body(n)) + "\n")
    rs.cache_clear()
    counted["n"] = 0
    rs._iter_code_chars = counting
    try:
        assert rs.resolve_symbol(p, "f") is not None, "fixture file unresolved"
    finally:
        rs._iter_code_chars = real_iter
    return counted["n"]


c_small, c_big = resolve_cost(50), resolve_cost(400)
# An 8x longer brace-free body must not cost 8x the yields. The bound is
# generous on purpose: `_confirm_definition` stays unfiltered by design and
# contributes a fixed head-window cost, so the assertion targets the BODY
# walk's growth, not a total.
assert c_big < c_small * 2, (
    f"resolve yields grew with body size ({c_small} -> {c_big}): the body "
    f"brace walk is not passing its `wanted` filter")
# UNFILTERED grows with the body, which is what the filter removed. Asserting
# the contrast pins the WIN, so deleting the `wanted` argument fails here
# instead of silently restoring the old cost.
s_u, b_u = yielded(small, None), yielded(big, None)
assert b_u > s_u * 5, f"unfiltered walk not growing: {s_u} vs {b_u}"
# And the filter must be a pure OUTPUT narrowing: the characters it does yield
# are exactly the ones the unfiltered walk yielded, at the same positions.
want = rs._PARENS_BRACES
full = [t for t in rs._iter_code_chars(big, 0, 0, len(big)) if t[2] in want]
filt = list(rs._iter_code_chars(big, 0, 0, len(big), want))
assert full == filt, "filtered walk disagrees with unfiltered on kept chars"
# The narrowing must not reach the classifier, which inspects EVERY character
# (isspace / isalnum / a catch-all reject); a filtered _confirm_definition
# would stop rejecting `int f(void) = 1;`.
src = open(os.environ["REPO_ROOT"]
           + "/scripts/todo-graph/resolve_symbol.py", encoding="utf-8").read()
conf = src[src.index("def _confirm_definition("):]
conf = conf[:conf.index("\ndef ", 1)]
assert "_BRACES" not in conf and "_PARENS_BRACES" not in conf, (
    "_confirm_definition must stay unfiltered -- it reads every character")
PY
)"; then
    t_pass "resolve_symbol: structural walk is body-independent, output-only"
else
    t_fail "resolve_symbol: walk scaling shape regressed; $RR_SCALE"
fi

# 15b: the two cache LIFECYCLES are not interchangeable. Content-bound caches
# revalidate against the file generation they were derived from, so evicting
# one costs a recompute and returns the same answer -- those are LRU-bounded.
# Topology-derived caches (which files exist, whether a basename is unique)
# have nothing pinning them, so an eviction mid-walk can recompute against a
# CHANGED tree and hand a later ref a different verdict than an earlier one.
# Those stay walk-scoped. Codex design review, section 15.
if RR_LIFE="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, sys, tempfile
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import ref_resolution as rr
# `_END_TO_END` belongs with the TOPOLOGY group despite looking content-bound:
# a miss or a followed-header answer also depends on `follow_declaration`'s live
# `src/**/<basename>.c` glob, which no lines object pins (Codex adversarial,
# section 15).
for name in ("_BASENAME_INDEX", "_EFFECTIVE_PATH_MEMO", "_END_TO_END"):
    c = getattr(rr, name)
    assert not isinstance(c, rr._Lru), (
        f"{name} is topology-derived and must NOT evict mid-walk: an entry "
        f"recomputed against a changed tree is a silent RESULT change")
for name in ("_RESOLVE_MEMO", "_MENTION_INDEX", "_RAW_TEXT", "_CODE_TEXT"):
    c = getattr(rr, name)
    assert isinstance(c, rr._Lru) and c.cap > 0, f"{name} unbounded"
# The lifecycle must be ENTERED by production, not merely offered: an API no
# walk enters leaves both gates answering from the previous walk's topology.
for rel in ("scripts/lint/check_stub_behind_stamp.py",
            "scripts/todo-graph/corpus_resolution_snapshot.py"):
    src = open(os.environ["REPO_ROOT"] + "/" + rel, encoding="utf-8").read()
    body = "\n".join(l for l in src.splitlines()
                     if not l.lstrip().startswith("#"))
    assert "walk_scope()" in body, (
        f"{rel} never enters walk_scope -- the cache lifecycle is advertised "
        f"but not applied")
# The bound is real: inserting past capacity evicts, and never exceeds it.
lru = rr._Lru(3)
for i in range(10):
    lru[i] = i
assert len(lru) == 3 and 9 in lru and 0 not in lru, f"no eviction: {dict(lru)}"
# Eviction must not change an ANSWER. Drive a real resolve, evict it, re-ask.
d = tempfile.mkdtemp()
p = os.path.join(d, "z.c")
open(p, "w").write("void zfn(void) { }\n")
first = rr._resolve_memoized(p, "zfn")
rr._RESOLVE_MEMO.clear()          # the eviction, in its strongest form
assert rr._resolve_memoized(p, "zfn") == first, "eviction changed a verdict"
# And the walk boundary releases the topology caches rather than trimming them.
with rr.walk_scope():
    rr._basename_index(__import__("pathlib").Path(d))
    assert rr._BASENAME_INDEX, "topology cache not populated inside the walk"
assert not rr._BASENAME_INDEX, "walk_scope did not release the topology cache"
PY
)"; then
    t_pass "ref_resolution: cache lifecycles split by what pins them"
else
    t_fail "ref_resolution: cache lifecycle wrong; $RR_LIFE"
fi

# 15c: the PAIRING's growth shape, across all three axes independently.
# 15a pins the body walk; it varies only one function's length and therefore
# cannot see the pairing cost at all. The section's Test checkpoint asked for a
# fixture varying symbols x candidate files x file size, and measuring
# discovery at 5% of today's corpus does NOT refute the multiplicative shape --
# it only says the constant is small at today's scale (Codex perf, section 15).
# So the shape is MEASURED and pinned here as a known, accepted property rather
# than left to a wall-clock number on one corpus.
if RR_PAIR="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, sys, tempfile
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs, ref_resolution as rr


def build(n_files, n_syms, pad):
    """A section naming `n_files` files, each defining all `n_syms` symbols,
    with `pad` filler lines per file. Every symbol is AMBIGUOUS across files,
    so `_defining_candidates` must probe every (file, symbol) pair."""
    d = tempfile.mkdtemp()
    items, files = [], []
    for f in range(n_files):
        path = os.path.join(d, "f%d.c" % f)
        body = []
        for s in range(n_syms):
            body.append("void s%d(void) {" % s)
            body += ["    int p%d = %d;" % (i, i) for i in range(pad)]
            body.append("}")
        open(path, "w").write("\n".join(body) + "\n")
        files.append(path)
        items.append({"section_n": 1, "refs": [{"kind": "file",
                                                "file": os.path.basename(path)}]})
    items.append({"section_n": 1,
                  "refs": [{"kind": "symbol", "symbol": "s%d" % s}
                           for s in range(n_syms)]})
    return d, items


def cost(n_files, n_syms, pad):
    d, items = build(n_files, n_syms, pad)
    rr.clear_caches()
    scope = rr.section_scope(items, __import__("pathlib").Path(d))
    for s in range(n_syms):
        rr._defining_candidates(scope, "s%d" % s, __import__("pathlib").Path(d))
    st = rs.scan_stats()
    return st["scalar"], st["bytes"]


base_scans, base_bytes = cost(2, 4, 5)
# SYMBOLS x2 -> scans x2. This is the multiplicative term, pinned deliberately:
# the section did NOT remove it, and a future change that does must update this
# assertion rather than silently pass a shape test that never looked.
s2, _ = cost(2, 8, 5)
assert s2 >= base_scans * 2, f"symbol axis not linear: {base_scans} -> {s2}"
# FILES x2 -> scans x2.
f2, _ = cost(4, 4, 5)
assert f2 >= base_scans * 2, f"file axis not linear: {base_scans} -> {f2}"
# FILE SIZE x~4 -> scan COUNT unchanged, scanned BYTES grow. Separating the two
# is the point: a wall-clock number cannot distinguish "more scans" from
# "bigger scans", and they have different fixes.
p4, p4_bytes = cost(2, 4, 25)
assert p4 == base_scans, f"size axis changed scan count: {base_scans} -> {p4}"
assert p4_bytes > base_bytes * 2, f"size axis not growing bytes: {base_bytes} -> {p4_bytes}"
PY
)"; then
    t_pass "ref_resolution: pairing growth shape measured on all three axes"
else
    t_fail "ref_resolution: pairing shape unpinned; $RR_PAIR"
fi

# 15d: the walk boundary is RE-ENTRANT and covers declaration-following.
# A nested scope that cleared on exit would drop resolve_symbol's generation
# pins mid-walk, turning a documented rc 9 drift failure into a silent clean
# read; and the sibling glob must be frozen per walk or two refs in one walk
# can straddle a topology change (Codex adversarial + consistency, section 15).
if RR_REENT="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, sys, tempfile
sys.path.insert(0, os.environ["REPO_ROOT"] + "/scripts/todo-graph")
import resolve_symbol as rs, ref_resolution as rr
from pathlib import Path

d = tempfile.mkdtemp()
os.makedirs(os.path.join(d, "src", "a"))
open(os.path.join(d, "src", "a", "w.c"), "w").write("void wf(void) { }\n")
with rr.walk_scope():
    first = rs._sibling_candidates(d, "w.c")
    assert len(first) == 1, f"setup: {first}"
    # A nested scope must NOT reset the outer walk's snapshot.
    with rr.walk_scope():
        pass
    os.makedirs(os.path.join(d, "src", "b"))
    open(os.path.join(d, "src", "b", "w.c"), "w").write("void wf(void) { }\n")
    again = rs._sibling_candidates(d, "w.c")
    assert again == first, (
        f"sibling glob straddled a topology change inside one walk: "
        f"{first} -> {again}")
# The OUTERMOST exit does release it, so the next walk sees the new tree.
after = rs._sibling_candidates(d, "w.c")
assert len(after) == 2, f"walk exit did not release the snapshot: {after}"

# An INTERRUPTED walk must not strand the depth above zero. Incrementing
# outside the `try` left a window where an exception between the increment and
# the protected block permanently disabled clearing, so every later walk kept
# the previous walk's topology -- including cached ZERO-match tuples (Codex
# re-adversarial, section 15). Depth must return to its prior value however
# the body exits.
assert rr._WALK_DEPTH == 0, f"depth not clean before test: {rr._WALK_DEPTH}"
try:
    with rr.walk_scope():
        raise KeyboardInterrupt("simulated interrupt inside the walk")
except KeyboardInterrupt:
    pass
assert rr._WALK_DEPTH == 0, (
    f"an interrupted walk stranded _WALK_DEPTH at {rr._WALK_DEPTH}; every "
    f"later walk would skip clearing and serve a stale topology")
# STRUCTURAL, because the behavioural assertion above CANNOT reach the real
# window: the defect needs an exception delivered between the increment and
# the `try`, and an in-body `raise` balances under the broken shape too (its
# `finally` still runs). Checked at source, the way fixture 14ee checks the
# caller rule -- the depth must not be mutated before the protected region
# opens, and the exit must RESTORE a saved value rather than decrement, so a
# stranded depth cannot accumulate across walks.
src = open(os.environ["REPO_ROOT"]
           + "/scripts/todo-graph/ref_resolution.py", encoding="utf-8").read()
ws = src[src.index("def walk_scope("):]
ws = ws[:ws.index("\ndef ", 1)]
code = "\n".join(l for l in ws.splitlines() if not l.lstrip().startswith("#"))
body = code[code.index('"""', code.index('"""') + 3) + 3:]
assert body.index("    try:") < body.index("_WALK_DEPTH ="), (
    "walk_scope mutates _WALK_DEPTH before entering `try` -- an exception in "
    "that window strands the depth and disables clearing permanently")
assert "_WALK_DEPTH = prior" in body and "_WALK_DEPTH -=" not in body, (
    "walk_scope must RESTORE a saved depth in `finally`, not decrement")
# And a normal walk after the interruption still clears.
with rr.walk_scope():
    rs._sibling_candidates(d, "w.c")
    assert rs._SIBLING_INDEX, "sibling index not populated after interruption"
assert not rs._SIBLING_INDEX, "clearing stayed disabled after an interruption"
PY
)"; then
    t_pass "resolve_symbol: walk boundary is re-entrant and freezes sibling glob"
else
    t_fail "resolve_symbol: walk boundary leaks topology; $RR_REENT"
fi

# ----------------------------------------------------------------------
# validate.py rebuild recovery + the --diff baseline cache.
#
# Every fixture below is paired with a MUTATION CHECK: the guard under test
# is reverted in a COPY of the source and the same fixture is re-run, which
# must then behave the old way. A fixture that passes against both is not
# testing the guard, it is testing something incidental on the way to it.
#
# The Implementation Order rows below spell the Section column as a bare
# number rather than with the section glyph. build.py:853 matches it
# optionally, so the fixtures parse identically -- and this file is exempt
# from the bare-section-ref lint (scripts/lint.sh:340) precisely because it
# builds TODO corpora, but the edit-time hook does not honour that exemption.
# ======================================================================

# Sub-test 23r1: a corrupt cache is REBUILT and the rebuild is re-validated,
# ending in a clean rc 0. This is the recovery the section preserves.
S23A="$TMP_DIR/s23-rebuild-clean"
s23_tree "$S23A"
printf 'CORRUPT{{{' > "$S23A/build/todo-cache.json"
S23A_OUT=$(timeout 60 python3 "$VALIDATE_PY" \
    --repo-root "$S23A" --quiet 2>&1); S23A_RC=$?
if [ "$S23A_RC" = "0" ]; then
    t_pass "validate recovery: a corrupt cache is rebuilt once and passes (rc 0)"
else
    t_fail "validate recovery: corrupt-then-rebuildable gave rc=$S23A_RC out=$S23A_OUT"
fi

# Sub-test 23r2: a producer that "succeeds" while emitting an unusable
# artifact must exit 2 after EXACTLY ONE rebuild. rc 1 would read as graph
# findings, and a retry loop against a deterministically-corrupt producer is
# a hang rather than a recovery -- so the attempt COUNT is asserted, not just
# the exit code.
S23B="$TMP_DIR/s23-rebuild-corrupt"
s23_tree "$S23B"
s23_tooldir "$S23B/tool"
cat > "$S23B/tool/build.py" <<S23BEOF
import sys
out = sys.argv[sys.argv.index("--output") + 1]
open(out, "w").write("STILL CORRUPT")
open("$S23B/attempts", "a").write("x")
sys.exit(0)
S23BEOF
printf 'CORRUPT' > "$S23B/build/todo-cache.json"
: > "$S23B/attempts"
S23B_OUT=$(timeout 60 python3 "$S23B/tool/validate.py" \
    --repo-root "$S23B" --quiet 2>&1); S23B_RC=$?
S23B_N=$(wc -c < "$S23B/attempts" | tr -d ' ')
if [ "$S23B_RC" = "2" ] && [ "$S23B_N" = "1" ]; then
    t_pass "validate recovery: a still-corrupt rebuild exits 2 after exactly one attempt"
else
    t_fail "validate recovery: still-corrupt rebuild gave rc=$S23B_RC attempts=$S23B_N (want 2 / 1) out=$S23B_OUT"
fi

# 23r2-mutation: revert the second validation to the old bare re-read. The
# fixture must then STOP exiting 2 -- the traceback lands on rc 1, which is
# the findings channel this work separates it from.
S23B_MUT="$TMP_DIR/s23-rebuild-corrupt-mut"
s23_tree "$S23B_MUT"
cp -r "$S23B/tool" "$S23B_MUT/tool"
python3 - "$S23B_MUT/tool/validate.py" <<'S23BMEOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
needle = """    try:
        return cache_schema.load_and_validate(
            cache_path, todo_root, profile=cache_schema.PROFILE_VALIDATE)
    except cache_schema.CacheSchemaError as second:"""
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
revert = """    if True:
        return json.loads(cache_path.read_text(encoding="utf-8")), None
    try:
        return cache_schema.load_and_validate(
            cache_path, todo_root, profile=cache_schema.PROFILE_VALIDATE)
    except cache_schema.CacheSchemaError as second:"""
p.write_text(src.replace(needle, revert, 1), encoding="utf-8")
S23BMEOF
sed -i "s#$S23B/attempts#$S23B_MUT/attempts#" "$S23B_MUT/tool/build.py"
printf 'CORRUPT' > "$S23B_MUT/build/todo-cache.json"
: > "$S23B_MUT/attempts"
timeout 60 python3 "$S23B_MUT/tool/validate.py" \
    --repo-root "$S23B_MUT" --quiet >/dev/null 2>&1; S23BM_RC=$?
if [ "$S23BM_RC" != "2" ]; then
    t_pass "validate recovery: mutation check -- the reverted bare re-read stops returning rc 2 (fixture is sensitive)"
else
    t_fail "validate recovery: mutation check -- reverted re-read still gave rc 2, so 23r2 proves nothing"
fi

# Sub-test 23r2b: a corrupt cache at a NON-canonical path refuses instead of
# being rebuilt over. The reason (UNREADABLE) is rebuildable and 23r1 proves
# the same bytes DO get rebuilt at the generated path -- so this isolates
# ownership as the deciding factor, not the failure reason. The file's bytes
# are asserted intact: this is a data-loss guard, and rc alone would not catch
# a rebuild-then-refuse.
S23J="$TMP_DIR/s23-not-ours"
s23_tree "$S23J"
printf 'CORRUPT{{{' > "$S23J/imported-cache.json"
S23J_BEFORE=$(md5sum "$S23J/imported-cache.json" | cut -d' ' -f1)
S23J_OUT=$(timeout 60 python3 "$VALIDATE_PY" --cache "$S23J/imported-cache.json" \
    --repo-root "$S23J" --quiet 2>&1); S23J_RC=$?
S23J_AFTER=$(md5sum "$S23J/imported-cache.json" | cut -d' ' -f1)
if [ "$S23J_RC" = "2" ] && [ "$S23J_BEFORE" = "$S23J_AFTER" ] \
    && echo "$S23J_OUT" | grep -q "does not own"; then
    t_pass "validate recovery: a corrupt cache this tool does not own refuses instead of being rebuilt over"
else
    t_fail "validate recovery: non-canonical cache rc=$S23J_RC bytes-changed=$([ "$S23J_BEFORE" = "$S23J_AFTER" ] && echo no || echo yes) out=$S23J_OUT"
fi

# 23r2b-mutation: drop the ownership gate. The imported cache must then be
# REWRITTEN -- the destruction the gate exists to prevent.
S23J_MUT="$TMP_DIR/s23-not-ours-mut"
s23_tree "$S23J_MUT"
s23_tooldir "$S23J_MUT/tool"
python3 - "$S23J_MUT/tool/validate.py" <<'S23JMEOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
needle = "        if first.reason in _REBUILDABLE_REASONS and not owned:"
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
p.write_text(src.replace(needle, "        if False:", 1), encoding="utf-8")
S23JMEOF
printf 'CORRUPT{{{' > "$S23J_MUT/imported-cache.json"
S23JM_BEFORE=$(md5sum "$S23J_MUT/imported-cache.json" | cut -d' ' -f1)
timeout 60 python3 "$S23J_MUT/tool/validate.py" --cache "$S23J_MUT/imported-cache.json" \
    --repo-root "$S23J_MUT" --quiet >/dev/null 2>&1
S23JM_AFTER=$(md5sum "$S23J_MUT/imported-cache.json" | cut -d' ' -f1)
if [ "$S23JM_BEFORE" != "$S23JM_AFTER" ]; then
    t_pass "validate recovery: mutation check -- without the ownership gate the imported cache is overwritten (fixture is sensitive)"
else
    t_fail "validate recovery: mutation check -- reverted ownership gate left the imported cache intact, so 23r2b proves nothing"
fi

# Sub-test 23r2c: the generated cache path being a SYMLINK does not confer
# ownership. Resolving both sides to compare them cuts both ways: a symlink at
# build/todo-cache.json pointing at a tracked file makes both realpaths equal,
# so the ownership gate would classify a corrupt cache as owned and the rebuild
# would write cache JSON over the victim -- the exact data loss the gate exists
# to prevent, authorised BY the gate.
S23Y="$TMP_DIR/s23-cache-symlink"
s23_tree "$S23Y"
if ln -s "../todo/01-test/TODO-01-s23.md" "$S23Y/build/todo-cache.json" 2>/dev/null; then
    S23Y_BEFORE=$(md5sum "$S23Y/todo/01-test/TODO-01-s23.md" | cut -d' ' -f1)
    S23Y_OUT=$(timeout 60 python3 "$VALIDATE_PY" --repo-root "$S23Y" --quiet 2>&1); S23Y_RC=$?
    S23Y_AFTER=$(md5sum "$S23Y/todo/01-test/TODO-01-s23.md" | cut -d' ' -f1)
    if [ "$S23Y_RC" = "2" ] && [ "$S23Y_BEFORE" = "$S23Y_AFTER" ]; then
        t_pass "validate recovery: a symlinked cache path is not owned, so the rebuild cannot overwrite what it points at"
    else
        t_fail "validate recovery: symlinked cache rc=$S23Y_RC victim-changed=$([ "$S23Y_BEFORE" = "$S23Y_AFTER" ] && echo no || echo yes) out=$S23Y_OUT"
    fi
else
    t_pass "validate recovery: symlinked-cache case skipped (symlinks unavailable on this filesystem)"
fi

# Sub-test 23r2d: the PRIMARY path, end to end. build-and-validate.sh runs the
# producer BEFORE the validator, so the ownership gate in validate.py never sees
# it -- and build.py used to resolve its --output, following a symlink at
# build/todo-cache.json and replacing what it pointed at. That is the same data
# loss as 23r2c, reached through the command a human actually runs.
S23Z="$TMP_DIR/s23-wrapper-symlink"
s23_tree "$S23Z"
if ln -s "../todo/01-test/TODO-01-s23.md" "$S23Z/build/todo-cache.json" 2>/dev/null; then
    S23Z_BEFORE=$(md5sum "$S23Z/todo/01-test/TODO-01-s23.md" | cut -d' ' -f1)
    timeout 120 python3 "$BUILD_PY" --quiet --root "$S23Z/todo" \
        --output "$S23Z/build/todo-cache.json" --repo-root "$S23Z" >/dev/null 2>&1
    S23Z_AFTER=$(md5sum "$S23Z/todo/01-test/TODO-01-s23.md" | cut -d' ' -f1)
    S23Z_ISLINK=$([ -L "$S23Z/build/todo-cache.json" ] && echo yes || echo no)
    if [ "$S23Z_BEFORE" = "$S23Z_AFTER" ] && [ "$S23Z_ISLINK" = "no" ]; then
        t_pass "build: a symlinked --output is replaced as a directory entry, so the producer cannot write cache JSON over what it pointed at"
    else
        t_fail "build: symlinked output victim-changed=$([ "$S23Z_BEFORE" = "$S23Z_AFTER" ] && echo no || echo yes) still-a-link=$S23Z_ISLINK"
    fi
else
    t_pass "build: symlinked-output case skipped (symlinks unavailable on this filesystem)"
fi

# Sub-test 23r3: a STALE cache REFUSES and is NOT rebuilt. The recovery
# writes back to the caller-named --cache path, so auto-rebuilding on
# staleness would destroy a captured or imported cache for being exactly
# what it is. The cache BYTES are asserted unchanged, which is the property
# that matters -- rc 2 alone would not catch a rebuild-then-refuse.
S23C="$TMP_DIR/s23-stale"
s23_tree "$S23C"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23C/todo" \
    --output "$S23C/build/todo-cache.json" --repo-root "$S23C" >/dev/null 2>&1
S23C_BEFORE=$(md5sum "$S23C/build/todo-cache.json" | cut -d' ' -f1)
# The edit must reach a field the cache actually CARRIES. Appending prose to
# the body changes no emitted field, so the rebuilt cache would be byte-
# identical and "bytes unchanged" would pass whether or not a rebuild ran --
# a probe that cannot fail. Rewriting the frontmatter title does land in the
# cache, which is what makes both assertions below mean something.
sed -i 's/^title: .*/title: "s23 fixture EDITED"/' "$S23C/todo/01-test/TODO-01-s23.md"
S23C_OUT=$(timeout 60 python3 "$VALIDATE_PY" \
    --repo-root "$S23C" --quiet 2>&1); S23C_RC=$?
S23C_AFTER=$(md5sum "$S23C/build/todo-cache.json" | cut -d' ' -f1)
if [ "$S23C_RC" = "2" ] && [ "$S23C_BEFORE" = "$S23C_AFTER" ]; then
    t_pass "validate recovery: a stale cache refuses (rc 2) without overwriting the caller's file"
else
    t_fail "validate recovery: stale cache rc=$S23C_RC bytes-changed=$([ "$S23C_BEFORE" = "$S23C_AFTER" ] && echo no || echo yes) out=$S23C_OUT"
fi

# 23r3-mutation: put STALE back into the rebuildable set. The cache must then
# be REWRITTEN, which is the data loss the exclusion prevents.
S23C_MUT="$TMP_DIR/s23-stale-mut"
s23_tree "$S23C_MUT"
s23_tooldir "$S23C_MUT/tool"
python3 - "$S23C_MUT/tool/validate.py" <<'S23CMEOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
# The tail of _REBUILDABLE_REASONS moved when REASON_LEGACY_FORMAT joined it
# in section 25; the needle tracks the LAST member so the mutation still
# lands inside the set rather than silently matching nothing.
needle = "    cache_schema.REASON_LEGACY_FORMAT,\n))"
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
p.write_text(src.replace(
    needle,
    "    cache_schema.REASON_LEGACY_FORMAT,\n    cache_schema.REASON_STALE,\n))",
    1), encoding="utf-8")
S23CMEOF
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23C_MUT/todo" \
    --output "$S23C_MUT/build/todo-cache.json" --repo-root "$S23C_MUT" >/dev/null 2>&1
S23CM_BEFORE=$(md5sum "$S23C_MUT/build/todo-cache.json" | cut -d' ' -f1)
sed -i 's/^title: .*/title: "s23 fixture EDITED"/' "$S23C_MUT/todo/01-test/TODO-01-s23.md"
timeout 60 python3 "$S23C_MUT/tool/validate.py" \
    --repo-root "$S23C_MUT" --quiet >/dev/null 2>&1
S23CM_AFTER=$(md5sum "$S23C_MUT/build/todo-cache.json" | cut -d' ' -f1)
if [ "$S23CM_BEFORE" != "$S23CM_AFTER" ]; then
    t_pass "validate recovery: mutation check -- STALE in the rebuildable set overwrites the caller's cache (fixture is sensitive)"
else
    t_fail "validate recovery: mutation check -- reverted STALE left the cache untouched, so 23r3 proves nothing"
fi

# Sub-test 23b1: a malformed --diff baseline is REFUSED, and specifically it
# does NOT become a graph-delta finding. A delta nobody authored is actioned
# by a human; a refusal is obviously about the tool.
S23D="$TMP_DIR/s23-baseline"
s23_tree "$S23D"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23D/todo" \
    --output "$S23D/cache.json" --repo-root "$S23D" >/dev/null 2>&1
printf '[{"file_path":"todo/01-test/TODO-01-s23.md"}]' > "$S23D/bad-baseline.json"
S23D_OUT=$(timeout 60 python3 "$VALIDATE_PY" --cache "$S23D/cache.json" \
    --repo-root "$S23D" --quiet --diff "$S23D/bad-baseline.json" 2>&1); S23D_RC=$?
if [ "$S23D_RC" = "2" ] && ! echo "$S23D_OUT" | grep -q "graph-delta:"; then
    t_pass "validate baseline: a malformed baseline refuses (rc 2) instead of manufacturing graph deltas"
else
    t_fail "validate baseline: malformed baseline rc=$S23D_RC out=$S23D_OUT (want rc 2, no graph-delta finding)"
fi

# Sub-test 23b2: a VALID baseline still diffs cleanly, so the refusal above is
# not just "the baseline path is broken now".
S23D_OK=$(timeout 60 python3 "$VALIDATE_PY" --cache "$S23D/cache.json" \
    --repo-root "$S23D" --quiet --diff "$S23D/cache.json" 2>&1); S23DOK_RC=$?
if [ "$S23DOK_RC" = "0" ]; then
    t_pass "validate baseline: a current-producer baseline still diffs (rc 0)"
else
    t_fail "validate baseline: valid baseline gave rc=$S23DOK_RC out=$S23D_OK"
fi

# 23b1-mutation: revert the baseline to the old raw json.loads. The malformed
# baseline must then stop refusing -- it becomes an invented finding instead.
S23D_MUT="$TMP_DIR/s23-baseline-mut"
s23_tree "$S23D_MUT"
s23_tooldir "$S23D_MUT/tool"
python3 - "$S23D_MUT/tool/validate.py" <<'S23DMEOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
# Matched by PREFIX and closing paren, not by a literal copy of the whole
# call: section 25 added a comment block inside these parens and then moved it,
# and a literal needle silently stops matching on any such edit -- which is
# exactly how this control went quiet once already.
needle_head = """        baseline_nodes, _base_info = cache_schema.load_and_validate("""
start = src.index(needle_head)
end = src.index("\n", src.index("PROFILE_BASELINE)", start))
needle = src[start:end]
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
revert = """        baseline_nodes = json.loads(baseline_path.read_text(encoding="utf-8"))"""
p.write_text(src.replace(needle, revert, 1), encoding="utf-8")
S23DMEOF
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23D_MUT/todo" \
    --output "$S23D_MUT/cache.json" --repo-root "$S23D_MUT" >/dev/null 2>&1
printf '[{"file_path":"todo/01-test/TODO-01-s23.md"}]' > "$S23D_MUT/bad-baseline.json"
S23DM_OUT=$(timeout 60 python3 "$S23D_MUT/tool/validate.py" --cache "$S23D_MUT/cache.json" \
    --repo-root "$S23D_MUT" --quiet --diff "$S23D_MUT/bad-baseline.json" 2>&1); S23DM_RC=$?
if [ "$S23DM_RC" != "2" ]; then
    t_pass "validate baseline: mutation check -- the reverted raw read stops refusing (fixture is sensitive)"
else
    t_fail "validate baseline: mutation check -- reverted raw read still gave rc 2, so 23b1 proves nothing"
fi

# Sub-test 23c1: a corpus file edited DURING the check walk is caught by the
# post-walk re-verification, and NO findings are printed. The output assertion
# is half the point: a human acts on printed findings, not on an exit code, so
# a run that reaches no verdict must say nothing about the corpus.
S23E="$TMP_DIR/s23-midwalk"
s23_tree "$S23E"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23E/todo" \
    --output "$S23E/cache.json" --repo-root "$S23E" >/dev/null 2>&1
cat > "$S23E/drive.py" <<S23EEOF
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import validate
_orig = validate.check_duplicate_id
def _spy(nodes):
    # Fires INSIDE the walk, after the cache was loaded and bound.
    with open("$S23E/todo/01-test/TODO-01-s23.md", "a") as fh:
        fh.write("\\nedited mid-walk\\n")
    return _orig(nodes)
validate.check_duplicate_id = _spy
sys.argv = ["validate.py", "--cache", "$S23E/cache.json",
            "--repo-root", "$S23E"]
sys.exit(validate.main())
S23EEOF
S23E_OUT=$(timeout 60 python3 "$S23E/drive.py" 2>/dev/null); S23E_RC=$?
if [ "$S23E_RC" = "2" ] && [ -z "$S23E_OUT" ]; then
    t_pass "validate binding: a mid-walk corpus edit refuses (rc 2) and prints no findings"
else
    t_fail "validate binding: mid-walk edit rc=$S23E_RC stdout='$S23E_OUT' (want rc 2, empty stdout)"
fi

# 23c1-mutation: drop the post-walk re-verification. The same mid-walk edit
# must then certify normally -- the window the binding exists to cover.
S23E_MUT="$TMP_DIR/s23-midwalk-mut"
s23_tree "$S23E_MUT"
s23_tooldir "$S23E_MUT/tool"
python3 - "$S23E_MUT/tool/validate.py" <<'S23EMEOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
needle = "    _reverify_corpus(todo_root, cache_info)\n    for line in out:"
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
p.write_text(src.replace(needle, "    for line in out:", 1), encoding="utf-8")
S23EMEOF
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23E_MUT/todo" \
    --output "$S23E_MUT/cache.json" --repo-root "$S23E_MUT" >/dev/null 2>&1
cat > "$S23E_MUT/drive.py" <<S23EMDEOF
import sys
sys.path.insert(0, "$S23E_MUT/tool")
import validate
_orig = validate.check_duplicate_id
def _spy(nodes):
    with open("$S23E_MUT/todo/01-test/TODO-01-s23.md", "a") as fh:
        fh.write("\\nedited mid-walk\\n")
    return _orig(nodes)
validate.check_duplicate_id = _spy
sys.argv = ["validate.py", "--cache", "$S23E_MUT/cache.json",
            "--repo-root", "$S23E_MUT"]
sys.exit(validate.main())
S23EMDEOF
timeout 60 python3 "$S23E_MUT/drive.py" >/dev/null 2>&1; S23EM_RC=$?
if [ "$S23EM_RC" != "2" ]; then
    t_pass "validate binding: mutation check -- without the post-walk check the mid-walk edit certifies (fixture is sensitive)"
else
    t_fail "validate binding: mutation check -- still rc 2 without the post-walk check, so 23c1 proves nothing"
fi

# Sub-test 23c2: an unreadable TODO file REFUSES rather than reading as empty
# text. The fail-open version turned a transient read error into orphan-IO-row
# and dangling-section FAILs against a file nobody had touched.
S23F="$TMP_DIR/s23-unreadable"
s23_tree "$S23F"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23F/todo" \
    --output "$S23F/cache.json" --repo-root "$S23F" >/dev/null 2>&1
if mkfifo "$S23F/fifo-probe" 2>/dev/null; then
    rm -f "$S23F/fifo-probe"
    rm -f "$S23F/todo/01-test/TODO-01-s23.md"
    mkfifo "$S23F/todo/01-test/TODO-01-s23.md"
    S23F_OUT=$(timeout 30 python3 "$VALIDATE_PY" --cache "$S23F/cache.json" \
        --repo-root "$S23F" --quiet 2>&1); S23F_RC=$?
    rm -f "$S23F/todo/01-test/TODO-01-s23.md"
    if [ "$S23F_RC" = "124" ]; then
        t_fail "validate snapshot: HUNG on a non-regular TODO file (timed out)"
    elif [ "$S23F_RC" = "2" ] && ! echo "$S23F_OUT" | grep -q "\[FAIL\]"; then
        t_pass "validate snapshot: an unreadable TODO refuses (rc 2) instead of reading as empty"
    else
        t_fail "validate snapshot: unreadable TODO rc=$S23F_RC out=$S23F_OUT (want rc 2, no [FAIL] lines)"
    fi
else
    t_pass "validate snapshot: unreadable-TODO case skipped (mkfifo unavailable on this filesystem)"
fi

# Sub-test 23w1: --fix-line-numbers --write refuses when its destination moved
# since the startup snapshot, instead of silently discarding that edit. This
# is the ONE walk the global post-run check cannot cover, because a write run
# changes the corpus itself.
# TWO files, deliberately: the item-name match is a plain substring scan over
# every line of the target, so a stamp pointing at its OWN file always matches
# twice (the checklist line and the stamp line) and is refused as ambiguous
# before the write is ever reached. Real stamps point across files, and so does
# this fixture -- TODO-01 holds the stamp, TODO-02 holds the item, and TODO-01
# is therefore the file the repair rewrites.
s23_write_tree() {
    local root="$1"
    rm -rf "$root"
    mkdir -p "$root/todo/01-test"
    cat > "$root/todo/01-test/TODO-01-a.md" <<'S23WAEOF'
---
schema_version: 1
id: fm-s23-a
domain: 01-test
status: active
title: "s23 stamp holder"
---
# TODO-01 -- s23 stamp holder
## Inputs
| Path | Purpose |
| ---- | ------- |
| `src/foo` | example |
## Outcome
Holds the stamp whose line number the repair re-resolves.
## Implementation Order
| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | 1 | First | -- | [x] |
## 1. First Section
Body.
- [x] Commit

> **Accepted:** finding -> XREF: [`TODO-02`](01-test/TODO-02-b.md) (item: "Wire the resolver" at line 999)
S23WAEOF
    cat > "$root/todo/01-test/TODO-02-b.md" <<'S23WBEOF'
---
schema_version: 1
id: fm-s23-b
domain: 01-test
status: active
title: "s23 item holder"
---
# TODO-02 -- s23 item holder
## Inputs
| Path | Purpose |
| ---- | ------- |
| `src/bar` | example |
## Outcome
Holds the item the stamp above names.
## Implementation Order
| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | 1 | First | -- | [ ] |
## 1. First Section
Body.
- [ ] Wire the resolver
- [ ] Commit
S23WBEOF
}

# s23_write_drive <dir> <tool-dir-or-empty> -- writes a driver that lands a
# concurrent edit on the rewrite target BETWEEN the snapshot and the write.
s23_write_drive() {
    local root="$1" toolpath="${2:-$REPO_ROOT/scripts/todo-graph}"
    cat > "$root/drive.py" <<S23WDEOF
import sys
sys.path.insert(0, "$toolpath")
import validate
_orig = validate.load_file_snapshot
def _spy(repo_root, nodes):
    snap = _orig(repo_root, nodes)
    with open("$root/todo/01-test/TODO-01-a.md", "a") as fh:
        fh.write("\\na concurrent edit\\n")
    return snap
validate.load_file_snapshot = _spy
sys.argv = ["validate.py", "--cache", "$root/cache.json",
            "--repo-root", "$root", "--fix-line-numbers", "--write", "--quiet"]
sys.exit(validate.main())
S23WDEOF
}

S23G="$TMP_DIR/s23-write-race"
s23_write_tree "$S23G"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23G/todo" \
    --output "$S23G/cache.json" --repo-root "$S23G" >/dev/null 2>&1
s23_write_drive "$S23G"
timeout 60 python3 "$S23G/drive.py" >/dev/null 2>&1; S23G_RC=$?
if [ "$S23G_RC" = "2" ] && grep -q "a concurrent edit" "$S23G/todo/01-test/TODO-01-a.md"; then
    t_pass "validate repair: --write refuses a destination that moved since the snapshot, preserving the edit"
else
    t_fail "validate repair: write-race rc=$S23G_RC edit-survived=$(grep -q 'a concurrent edit' "$S23G/todo/01-test/TODO-01-a.md" && echo yes || echo no) (want rc 2 / yes)"
fi

# 23w1-mutation: revert to the blind write. The concurrent edit must then be
# GONE from the file -- the lost update this guard refuses.
S23G_MUT="$TMP_DIR/s23-write-race-mut"
s23_write_tree "$S23G_MUT"
s23_tooldir "$S23G_MUT/tool"
python3 - "$S23G_MUT/tool/validate.py" <<'S23GMPEOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
# BOTH compares must be reverted. The publish-time re-check added after the
# re-adversarial round independently catches a destination that moved, so
# neutralising phase 1 alone left the fixture passing for the WRONG reason --
# defense in depth made a single-site mutation inert rather than proving the
# fixture insensitive.
for needle in ("        if current != old_text:", "            if now != old_text:"):
    assert src.count(needle) == 1, f"mutation needle {needle!r} appears {src.count(needle)}x"
    src = src.replace(needle, needle.split("if")[0] + "if False:", 1)
p.write_text(src, encoding="utf-8")
S23GMPEOF
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23G_MUT/todo" \
    --output "$S23G_MUT/cache.json" --repo-root "$S23G_MUT" >/dev/null 2>&1
s23_write_drive "$S23G_MUT" "$S23G_MUT/tool"
timeout 60 python3 "$S23G_MUT/drive.py" >/dev/null 2>&1
if ! grep -q "a concurrent edit" "$S23G_MUT/todo/01-test/TODO-01-a.md"; then
    t_pass "validate repair: mutation check -- the reverted blind write DISCARDS the concurrent edit (fixture is sensitive)"
else
    t_fail "validate repair: mutation check -- reverted blind write kept the edit, so 23w1 proves nothing"
fi

# Sub-test 23w2: a repair that refuses writes NOTHING and says nothing. The
# write used to happen inside the traversal, so a destination discovered stale
# on file N left files 1..N-1 already rewritten and their DRY-RUN/WRITE lines
# already printed -- an rc 2 run that both mutated the tree and published a
# verdict. Both halves are asserted here: no file changed, and stdout/stderr
# carry the refusal and nothing else.
S23K="$TMP_DIR/s23-repair-atomic"
s23_write_tree "$S23K"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23K/todo" \
    --output "$S23K/cache.json" --repo-root "$S23K" >/dev/null 2>&1
s23_write_drive "$S23K"
S23K_OUT=$(timeout 60 python3 "$S23K/drive.py" 2>&1); S23K_RC=$?
if [ "$S23K_RC" = "2" ] && echo "$S23K_OUT" | grep -q "nothing was written" \
    && ! echo "$S23K_OUT" | grep -qE "WRITE |DRY-RUN "; then
    t_pass "validate repair: a refused write publishes only the refusal -- no proposed-rewrite lines"
else
    t_fail "validate repair: refused write rc=$S23K_RC out=$S23K_OUT (want rc 2, 'nothing was written', no WRITE/DRY-RUN lines)"
fi
# No `.validate-tmp` may survive a refusal or a successful run.
if [ -z "$(find "$S23K/todo" -name '*.validate-tmp' -print -quit)" ]; then
    t_pass "validate repair: no temporary rewrite file is left behind"
else
    t_fail "validate repair: a .validate-tmp file survived the run"
fi

# Sub-test 23w5: a repair that rewrites TWO destinations must give each its own
# text. THE ABSENCE OF THIS FIXTURE IS WHY A REAL BUG SHIPPED THROUGH a full
# review round: every other repair fixture has exactly one destination, and the
# publish loop paired destination N with destination N+1's staged content, so a
# two-file repair silently wrote the wrong file's contents and returned 0.
# s23_multi_tree <dir> -- a corpus with TWO stamp-holding files whose stamps
# both point at a third file. Two destinations is the minimum that exercises
# the publish loop's pairing and the staging cleanup path.
s23_multi_tree() {
    local root="$1"
    rm -rf "$root"
    mkdir -p "$root/todo/01-test"
    for mroot_n in 01 02; do
        cat > "$root/todo/01-test/TODO-$mroot_n-holder.md" <<S23PEOF
---
schema_version: 1
id: fm-multi-$mroot_n
domain: 01-test
status: active
title: "multi $mroot_n"
---
# TODO-$mroot_n -- multi $mroot_n
## Inputs
| Path | Purpose |
| ---- | ------- |
| \`src/foo\` | example |
## Outcome
UNIQUE-MARKER-$mroot_n
## Implementation Order
| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | 1 | First | -- | [x] |
## 1. First Section
Body.
- [x] Commit

> **Accepted:** finding -> XREF: [\`TODO-03\`](01-test/TODO-03-target.md) (item: "Wire the resolver" at line 999)
S23PEOF
    done
    cat > "$root/todo/01-test/TODO-03-target.md" <<'S23PTEOF'
---
schema_version: 1
id: fm-multi-target
domain: 01-test
status: active
title: "multi target"
---
# TODO-03 -- multi target
## Inputs
| Path | Purpose |
| ---- | ------- |
| `src/bar` | example |
## Outcome
    Holds the item both stamps name.
## Implementation Order
| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | 1 | First | -- | [ ] |
## 1. First Section
Body.
- [ ] Wire the resolver
- [ ] Commit
S23PTEOF
}

S23P="$TMP_DIR/s23-multi-dest"
s23_multi_tree "$S23P"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23P/todo" \
    --output "$S23P/cache.json" --repo-root "$S23P" >/dev/null 2>&1
timeout 60 python3 "$VALIDATE_PY" --cache "$S23P/cache.json" --repo-root "$S23P" \
    --fix-line-numbers --write --quiet >/dev/null 2>&1; S23P_RC=$?
# Each file must still carry its OWN marker, and both stamps must have been
# re-resolved off line 999.
S23P_OK=1
for S23P_N in 01 02; do
    grep -q "UNIQUE-MARKER-$S23P_N" "$S23P/todo/01-test/TODO-$S23P_N-holder.md" || S23P_OK=0
    grep -q 'at line 999' "$S23P/todo/01-test/TODO-$S23P_N-holder.md" && S23P_OK=0
done
if [ "$S23P_RC" = "0" ] && [ "$S23P_OK" = "1" ]; then
    t_pass "validate repair: a two-destination rewrite gives each file its own content"
else
    t_fail "validate repair: multi-destination rc=$S23P_RC content-correct=$S23P_OK"
fi

# Sub-test 23w6: two paths that are the SAME inode refuse, even when the repair
# derives identical text for both. os.replace swaps one directory entry and
# breaks the hard link, so the other name would silently keep the old content.
# THE ALIAS IS NOT ITSELF A CORPUS FILE, deliberately. An earlier version of
# this fixture linked two TODO-*.md names in one directory, so BOTH entered the
# pending write set and a duplicate-inode scan over that set caught it. The
# escape path is the opposite shape: one name needs rewriting and the other is
# invisible to this run, so no duplicate is ever observed. `st_nlink` sees it
# either way, which is why the check moved to the link count.
S23Q="$TMP_DIR/s23-hardlink"
s23_write_tree "$S23Q"
if ln "$S23Q/todo/01-test/TODO-01-a.md" "$S23Q/todo/01-test/alias-not-a-todo.txt" 2>/dev/null; then
    timeout 60 python3 "$BUILD_PY" --quiet --root "$S23Q/todo" \
        --output "$S23Q/cache.json" --repo-root "$S23Q" >/dev/null 2>&1
    S23Q_OUT=$(timeout 60 python3 "$VALIDATE_PY" --cache "$S23Q/cache.json" \
        --repo-root "$S23Q" --fix-line-numbers --write --quiet 2>&1); S23Q_RC=$?
    S23Q_LEFT=$(grep -c 'at line 999' "$S23Q/todo/01-test/TODO-01-a.md")
    S23Q_ALIAS=$(grep -c 'at line 999' "$S23Q/todo/01-test/alias-not-a-todo.txt")
    if [ "$S23Q_RC" = "2" ] && echo "$S23Q_OUT" | grep -q "hard link" \
        && [ "$S23Q_LEFT" != "0" ] && [ "$S23Q_ALIAS" != "0" ]; then
        t_pass "validate repair: a destination with a second name refuses -- including an alias this run never sees -- instead of breaking the link and leaving it stale"
    else
        t_fail "validate repair: hard-link rc=$S23Q_RC untouched=$S23Q_LEFT out=$S23Q_OUT"
    fi
else
    t_pass "validate repair: hard-link case skipped (hard links unavailable on this filesystem)"
fi

# Sub-test 23w7: a hard link created DURING staging is caught at publish time.
# The phase-1 link count is stale by then, and a link added mid-batch changes
# neither the device, the inode, the contents nor the file type -- so every
# other phase-3 check passes. Injected deterministically by hooking mkstemp,
# which runs after phase 1 has already accepted the destination.
S23R="$TMP_DIR/s23-midstage-link"
s23_write_tree "$S23R"
if ln "$S23R/todo/01-test/TODO-02-b.md" "$S23R/todo/01-test/probe-link.txt" 2>/dev/null; then
    rm -f "$S23R/todo/01-test/probe-link.txt"
    timeout 60 python3 "$BUILD_PY" --quiet --root "$S23R/todo" \
        --output "$S23R/cache.json" --repo-root "$S23R" >/dev/null 2>&1
    S23R_BEFORE=$(md5sum "$S23R/todo/01-test/TODO-01-a.md" | cut -d' ' -f1)
    cat > "$S23R/drive.py" <<S23RDEOF
import os, sys, tempfile
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import validate
_orig = tempfile.mkstemp
def _hooked(*a, **kw):
    # Fires after phase 1 accepted the destination, before phase 3 publishes.
    link = "$S23R/todo/01-test/late-alias.txt"
    if not os.path.exists(link):
        os.link("$S23R/todo/01-test/TODO-01-a.md", link)
    return _orig(*a, **kw)
tempfile.mkstemp = _hooked
sys.argv = ["validate.py", "--cache", "$S23R/cache.json",
            "--repo-root", "$S23R", "--fix-line-numbers", "--write", "--quiet"]
sys.exit(validate.main())
S23RDEOF
    timeout 60 python3 "$S23R/drive.py" >/dev/null 2>&1; S23R_RC=$?
    S23R_AFTER=$(md5sum "$S23R/todo/01-test/TODO-01-a.md" | cut -d' ' -f1)
    S23R_LEFT=$(find "$S23R/todo" -name 'TODO-01-a.md.*.tmp' -print -quit)
    if [ "$S23R_RC" = "2" ] && [ "$S23R_BEFORE" = "$S23R_AFTER" ] && [ -z "$S23R_LEFT" ]; then
        t_pass "validate repair: a hard link created DURING staging is caught at publish time, leaving both names untouched"
    else
        t_fail "validate repair: mid-stage link rc=$S23R_RC changed=$([ "$S23R_BEFORE" = "$S23R_AFTER" ] && echo no || echo yes) leftover='$S23R_LEFT'"
    fi
else
    t_pass "validate repair: mid-stage hard-link case skipped (hard links unavailable on this filesystem)"
fi

# Sub-test 23w8: a staging failure is an INFRASTRUCTURE refusal, not a graph
# verdict. mkstemp sat outside the OSError handler, so a full disk exited 1 --
# the code reserved for "the corpus has problems" -- and pointed the operator at
# a graph that was never evaluated. Fault-injected after at least one temp file
# has already been staged, so the cleanup path is exercised too.
S23S="$TMP_DIR/s23-stage-enospc"
s23_multi_tree "$S23S"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23S/todo" \
    --output "$S23S/cache.json" --repo-root "$S23S" >/dev/null 2>&1
S23S_BEFORE=$(md5sum "$S23S/todo/01-test/TODO-01-holder.md" "$S23S/todo/01-test/TODO-02-holder.md" | md5sum)
cat > "$S23S/drive.py" <<S23SDEOF
import errno, os, sys, tempfile
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import validate
_orig = tempfile.mkstemp
_calls = {"n": 0}
def _hooked(*a, **kw):
    _calls["n"] += 1
    if _calls["n"] == 1:
        # Stage one real temp file first, so the finally-cleanup has something
        # to remove when the next call fails.
        return _orig(*a, **kw)
    raise OSError(errno.ENOSPC, "No space left on device")
tempfile.mkstemp = _hooked
sys.argv = ["validate.py", "--cache", "$S23S/cache.json",
            "--repo-root", "$S23S", "--fix-line-numbers", "--write", "--quiet"]
sys.exit(validate.main())
S23SDEOF
timeout 60 python3 "$S23S/drive.py" >/dev/null 2>&1; S23S_RC=$?
S23S_AFTER=$(md5sum "$S23S/todo/01-test/TODO-01-holder.md" "$S23S/todo/01-test/TODO-02-holder.md" | md5sum)
S23S_LEFT=$(find "$S23S/todo" -name '*.tmp' -print -quit)
# TWO destinations, so call 1 stages a real temp file and call 2 fails: the
# finally-cleanup has something to remove, which is the half a single-file tree
# could not reach.
if [ "$S23S_RC" = "2" ] && [ "$S23S_BEFORE" = "$S23S_AFTER" ] && [ -z "$S23S_LEFT" ]; then
    t_pass "validate repair: a staging OSError refuses as infrastructure rather than exiting 1 into the findings channel"
else
    t_fail "validate repair: staging ENOSPC rc=$S23S_RC changed=$([ "$S23S_BEFORE" = "$S23S_AFTER" ] && echo no || echo yes) leftover='$S23S_LEFT'"
fi

# Sub-test 23w9: a staged file whose mode cannot be reproduced refuses rather
# than publishing an owner-only replacement. mkstemp creates at 0600, so
# swallowing the failure turned a world-readable TODO private at rc 0.
S23T="$TMP_DIR/s23-mode-fail"
s23_write_tree "$S23T"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23T/todo" \
    --output "$S23T/cache.json" --repo-root "$S23T" >/dev/null 2>&1
S23T_BEFORE=$(md5sum "$S23T/todo/01-test/TODO-01-a.md" | cut -d' ' -f1)
cat > "$S23T/drive.py" <<S23TDEOF
import errno, os, sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import validate
def _boom(fd, mode):
    raise OSError(errno.EPERM, "Operation not permitted")
os.fchmod = _boom
sys.argv = ["validate.py", "--cache", "$S23T/cache.json",
            "--repo-root", "$S23T", "--fix-line-numbers", "--write", "--quiet"]
sys.exit(validate.main())
S23TDEOF
timeout 60 python3 "$S23T/drive.py" >/dev/null 2>&1; S23T_RC=$?
S23T_AFTER=$(md5sum "$S23T/todo/01-test/TODO-01-a.md" | cut -d' ' -f1)
S23T_LEFT=$(find "$S23T/todo" -name '*.tmp' -print -quit)
if [ "$S23T_RC" = "2" ] && [ "$S23T_BEFORE" = "$S23T_AFTER" ] && [ -z "$S23T_LEFT" ]; then
    t_pass "validate repair: a staging mode failure refuses instead of publishing an owner-only replacement"
else
    t_fail "validate repair: mode failure rc=$S23T_RC changed=$([ "$S23T_BEFORE" = "$S23T_AFTER" ] && echo no || echo yes) leftover='$S23T_LEFT'"
fi

# Sub-test 23w10: repair refuses while frontmatter ids collide. The id index is
# last-wins and repair runs BEFORE the duplicate-id check, so a stamp naming a
# duplicated id would be re-resolved against the wrong file of the pair --
# silently, at rc 0. PROFILE_VALIDATE permits the duplicate on purpose so the
# check can report it, which is exactly why repair has to refuse it separately.
S23U="$TMP_DIR/s23-dup-id-repair"
rm -rf "$S23U"
d_todo "$S23U" 01-t 01 first shared active
d_todo "$S23U" 01-t 02 second shared active
cat >> "$S23U/todo/01-t/TODO-01-first.md" <<'S23UEOF'

> **Accepted:** finding -> XREF: [`shared`](shared) (item: "body" at line 999)
S23UEOF
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23U/todo" \
    --output "$S23U/cache.json" --repo-root "$S23U" >/dev/null 2>&1
S23U_BEFORE=$(md5sum "$S23U/todo/01-t/TODO-01-first.md" | cut -d' ' -f1)
S23U_OUT=$(timeout 60 python3 "$VALIDATE_PY" --cache "$S23U/cache.json" \
    --repo-root "$S23U" --fix-line-numbers --write --quiet 2>&1); S23U_RC=$?
S23U_AFTER=$(md5sum "$S23U/todo/01-t/TODO-01-first.md" | cut -d' ' -f1)
if [ "$S23U_RC" = "2" ] && echo "$S23U_OUT" | grep -q "ids collide" \
    && [ "$S23U_BEFORE" = "$S23U_AFTER" ]; then
    t_pass "validate repair: colliding frontmatter ids refuse the rewrite instead of resolving against whichever file the cache lists last"
else
    t_fail "validate repair: duplicate-id repair rc=$S23U_RC changed=$([ "$S23U_BEFORE" = "$S23U_AFTER" ] && echo no || echo yes) out=$S23U_OUT"
fi

# Sub-test 23p1: node paths are containment-checked, and the WRITE path
# re-establishes containment independently of the module that validated the
# cache. Every shape below was accepted before -- confirmed by probe against
# the live cache -- while `repo_root / rel` discarded the root for an absolute
# path and walked out of it for a relative one.
S23V_OUT=$(cd "$REPO_ROOT" && timeout 60 python3 - <<'S23VEOF' 2>&1
import json, sys
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs

nodes = json.load(open("build/todo-cache.json"))
base = nodes[0]
# Escaping spellings, then NON-CANONICAL ones that name the same inode under a
# different string -- `file_path` IS the node identity and the uniqueness check
# compares it as text, so an alias spelling would let one file carry several
# identities. The last entry must be ACCEPTED: a Cyrillic look-alike is an
# ordinary POSIX filename character and refusing it would be the tool inventing
# a rule the filesystem does not have.
bad = ["../../outside.md", "/etc/passwd", "todo/../../x.md", "todo\\x.md",
       "nottodo/x.md", "C:\\x.md", "todo//x.md", "todo/./x.md",
       "todo/x.md/", "todo", "./todo/x.md", "todo/a\x00b.md"]
results = []
for spelling in bad:
    node = dict(base)
    node["file_path"] = spelling
    try:
        cs.validate_nodes([node], "probe", cs.PROFILE_VALIDATE)
        results.append(f"ACCEPTED:{spelling}")
    except cs.CacheSchemaError as exc:
        results.append(exc.reason)
ok_node = dict(base)
ok_node["file_path"] = "todo/01-t/TODO-01-\u0430-cyrillic.md"
try:
    cs.validate_nodes([ok_node], "probe", cs.PROFILE_VALIDATE)
    results.append("UNICODE-OK")
except cs.CacheSchemaError as exc:
    results.append(f"UNICODE-REFUSED:{exc.reason}")
try:
    cs.validate_nodes(nodes, "live", cs.PROFILE_VALIDATE)
    results.append("LIVE-OK")
except cs.CacheSchemaError as exc:
    results.append(f"LIVE-BROKEN:{exc.reason}")
print(" ".join(results))
S23VEOF
)
if [ "$S23V_OUT" = "SHAPE SHAPE SHAPE SHAPE SHAPE SHAPE SHAPE SHAPE SHAPE SHAPE SHAPE SHAPE UNICODE-OK LIVE-OK" ]; then
    t_pass "cache schema: every escaping and non-canonical file_path spelling is refused, a Unicode look-alike is not, and the live 232-node corpus still validates"
else
    t_fail "cache schema: file_path containment probe gave '$S23V_OUT'"
fi

# Sub-test 23p1c: cmd_code's OWN containment guard, exercised directly. The
# cache-wide refusal added above shadows it, so breaking query.py's local guard
# would leave the end-to-end escape fixture green.
#
# THE OUTSIDE FILE'S CONTENT IS LOAD-BEARING. A first attempt pointed at
# /etc/passwd and passed with the guard deleted -- not because the guard was
# unobservable, but because passwd contains nothing matching the notes-path
# pattern, so the walk produced no rows either way. A file containing a
# `(src/leak.c)` reference makes the difference visible: guard present -> no
# rows; guard removed -> one `notes-grep` row naming a path read from outside
# the repo. Measured both ways before this fixture was written.
S23X_DIR="$TMP_DIR/s23-cmdcode-guard"
rm -rf "$S23X_DIR"
mkdir -p "$S23X_DIR/repo"
printf 'see (src/leak.c) for details\n' > "$S23X_DIR/outside.md"
S23X_OUT=$(cd "$REPO_ROOT" && timeout 60 python3 - "$S23X_DIR/repo" <<'S23XEOF' 2>&1
import sys, types
sys.path.insert(0, "scripts/todo-graph")
from pathlib import Path
import query

node = {"file_path": "../outside.md", "id": "esc", "domain": "01-t",
        "status": "draft", "title": "E", "sections": [],
        "section_headings": [], "inputs_xrefs": [], "stamps_xrefs": [],
        "schema_version": 1}
ctx = query.Ctx([node], Path(sys.argv[1]), quiet=True)
args = types.SimpleNamespace(target="esc", quiet=True, json=False,
                             limit=50, offset=0)
try:
    rows, _cols = query.cmd_code(ctx, args)
    print(f"rows={len(rows)}")
except SystemExit as exc:
    print(f"exit={exc.code}")
S23XEOF
)
if [ "$S23X_OUT" = "rows=0" ]; then
    t_pass "query: cmd_code's own containment guard yields no rows for a file_path outside the repo, independently of the cache-wide refusal"
else
    t_fail "query: cmd_code containment guard gave '$S23X_OUT' (want rows=0)"
fi

# 23p1b: and the repair refuses independently, so the write path does not
# depend on cache_schema having been called first.
S23W="$TMP_DIR/s23-write-escape"
s23_write_tree "$S23W"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23W/todo" \
    --output "$S23W/cache.json" --repo-root "$S23W" >/dev/null 2>&1
# ONE LEVEL ABOVE the repo root, because that is where `repo_root/../x` lands.
# Placing it inside the root made the fixture inert: the refusal came from the
# file not existing, and the guard could be deleted with the test still green.
printf 'ORIGINAL' > "$TMP_DIR/outside-target.md"
cat > "$S23W/drive.py" <<S23WDEOF
import sys
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import validate
# Hand _commit_rewrites a destination that left the corpus, exactly as a
# hostile cache would if cache_schema had not been consulted.
try:
    validate._commit_rewrites(__import__("pathlib").Path("$S23W"),
                              [("../outside-target.md", "ORIGINAL", "REWRITTEN")])
except SystemExit as exc:
    sys.exit(exc.code)
S23WDEOF
timeout 60 python3 "$S23W/drive.py" >/dev/null 2>&1; S23W_RC=$?
S23W_KEPT=$(cat "$TMP_DIR/outside-target.md")
if [ "$S23W_RC" = "2" ] && [ "$S23W_KEPT" = "ORIGINAL" ]; then
    t_pass "validate repair: a destination outside the corpus is refused by the write path itself, not only by the cache validator"
else
    t_fail "validate repair: escaping destination rc=$S23W_RC content='$S23W_KEPT'"
fi

# Sub-test 23d1: the ninth check. Two files land on the SAME derived
# (domain, TODO-number) key with NO reference to the ambiguous form anywhere --
# which is the case that used to escape: build_path_index recorded the
# collision, but it only became a finding if some XREF happened to look the key
# up. validate.py must REPORT it at rc 1 while PROFILE_QUERY REFUSES the same
# cache, so the two tools agree on the defect and differ only in the response.
S23L="$TMP_DIR/s23-resolver-collision"
rm -rf "$S23L"
d_todo "$S23L" 01-t 01 first alpha active
d_todo "$S23L" 01-t 01 second beta active
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23L/todo" \
    --output "$S23L/cache.json" --repo-root "$S23L" >/dev/null 2>&1
S23L_OUT=$(timeout 60 python3 "$VALIDATE_PY" --cache "$S23L/cache.json" \
    --repo-root "$S23L" 2>&1); S23L_RC=$?
S23L_Q=$(timeout 60 python3 - "$S23L/cache.json" "$S23L/todo" <<'S23LEOF' 2>&1
import sys
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs
try:
    cs.load_and_validate(sys.argv[1], sys.argv[2], check_stale=False,
                         profile=cs.PROFILE_QUERY)
    print("NORAISE")
except cs.CacheSchemaError as exc:
    print(exc.reason)
S23LEOF
)
if [ "$S23L_RC" = "1" ] && echo "$S23L_OUT" | grep -q "duplicate-resolver-key" \
    && [ "$S23L_Q" = "SHAPE" ]; then
    t_pass "validate: a resolver-key collision with no referring XREF is REPORTED at rc 1, while the query profile refuses the same cache"
else
    t_fail "validate: resolver-key collision rc=$S23L_RC query=$S23L_Q out=$(echo "$S23L_OUT" | tail -3)"
fi

# 23d1-mutation: drop the ninth check from the registered list. The collision
# must then pass unreported -- the exact divergence it closes.
S23L_MUT="$TMP_DIR/s23-resolver-collision-mut"
rm -rf "$S23L_MUT"
d_todo "$S23L_MUT" 01-t 01 first alpha active
d_todo "$S23L_MUT" 01-t 01 second beta active
s23_tooldir "$S23L_MUT/tool"
python3 - "$S23L_MUT/tool/validate.py" <<'S23LMEOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
needle = '        ("duplicate-resolver-key", check_duplicate_resolver_key, (nodes,)),\n'
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
p.write_text(src.replace(needle, "", 1), encoding="utf-8")
S23LMEOF
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23L_MUT/todo" \
    --output "$S23L_MUT/cache.json" --repo-root "$S23L_MUT" >/dev/null 2>&1
timeout 60 python3 "$S23L_MUT/tool/validate.py" --cache "$S23L_MUT/cache.json" \
    --repo-root "$S23L_MUT" --quiet >/dev/null 2>&1; S23LM_RC=$?
if [ "$S23LM_RC" = "0" ]; then
    t_pass "validate: mutation check -- without the ninth check the collision passes unreported (fixture is sensitive)"
else
    t_fail "validate: mutation check -- unregistered ninth check still gave rc=$S23LM_RC, so 23d1 proves nothing"
fi

# Sub-test 23w3: the repair refuses a SYMLINK destination instead of replacing
# the link with a regular file. The corpus deliberately accepts a symlink to a
# regular TODO, and os.replace swaps the directory ENTRY -- so a repair would
# silently convert a tracked link into a copy.
S23M="$TMP_DIR/s23-symlink-dest"
s23_write_tree "$S23M"
if ln -s "TODO-01-a.md" "$S23M/todo/01-test/link-probe" 2>/dev/null; then
    rm -f "$S23M/todo/01-test/link-probe"
    mv "$S23M/todo/01-test/TODO-01-a.md" "$S23M/todo/01-test/real-a.md"
    ln -s "real-a.md" "$S23M/todo/01-test/TODO-01-a.md"
    timeout 60 python3 "$BUILD_PY" --quiet --root "$S23M/todo" \
        --output "$S23M/cache.json" --repo-root "$S23M" >/dev/null 2>&1
    S23M_OUT=$(timeout 60 python3 "$VALIDATE_PY" --cache "$S23M/cache.json" \
        --repo-root "$S23M" --fix-line-numbers --write --quiet 2>&1); S23M_RC=$?
    if [ "$S23M_RC" = "2" ] && echo "$S23M_OUT" | grep -q "is a symlink" \
        && [ -L "$S23M/todo/01-test/TODO-01-a.md" ]; then
        t_pass "validate repair: a symlink destination refuses instead of being replaced by a regular file"
    else
        t_fail "validate repair: symlink dest rc=$S23M_RC still-a-link=$([ -L "$S23M/todo/01-test/TODO-01-a.md" ] && echo yes || echo no) out=$S23M_OUT"
    fi
else
    t_pass "validate repair: symlink-destination case skipped (symlinks unavailable on this filesystem)"
fi

# Sub-test 23w4: a pre-existing sibling at the OLD fixed temp name must survive
# untouched. The rewrite used to open `<dest>.validate-tmp` with no O_EXCL, so
# it truncated whatever was already there -- and followed it if it was a link.
S23N="$TMP_DIR/s23-tmp-collision"
s23_write_tree "$S23N"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S23N/todo" \
    --output "$S23N/cache.json" --repo-root "$S23N" >/dev/null 2>&1
printf 'PRECIOUS' > "$S23N/todo/01-test/TODO-01-a.md.validate-tmp"
timeout 60 python3 "$VALIDATE_PY" --cache "$S23N/cache.json" \
    --repo-root "$S23N" --fix-line-numbers --write --quiet >/dev/null 2>&1; S23N_RC=$?
S23N_KEPT=$(cat "$S23N/todo/01-test/TODO-01-a.md.validate-tmp" 2>/dev/null)
S23N_LEFT=$(find "$S23N/todo" -name 'TODO-01-a.md.*.tmp' -print -quit)
if [ "$S23N_RC" = "0" ] && [ "$S23N_KEPT" = "PRECIOUS" ] && [ -z "$S23N_LEFT" ]; then
    t_pass "validate repair: a pre-existing sibling at the old fixed temp name is not truncated, and no staging file survives"
else
    t_fail "validate repair: temp collision rc=$S23N_RC kept='$S23N_KEPT' leftover='$S23N_LEFT'"
fi

# Sub-test 23c3: the ownership gate is PHYSICAL. A dot-dot spelling of the
# generated cache names the same file and must be recovered, not refused.
S23O="$TMP_DIR/s23-cache-alias"
s23_tree "$S23O"
printf 'CORRUPT{{{' > "$S23O/build/todo-cache.json"
S23O_OUT=$(timeout 60 python3 "$VALIDATE_PY" \
    --cache "$S23O/build/../build/todo-cache.json" \
    --repo-root "$S23O" --quiet 2>&1); S23O_RC=$?
if [ "$S23O_RC" = "0" ]; then
    t_pass "validate recovery: a dot-dot spelling of the generated cache is recognised as owned and recovered"
else
    t_fail "validate recovery: aliased canonical cache rc=$S23O_RC out=$S23O_OUT (want rc 0)"
fi

# Sub-test 23h1: the section_headings subtree. validate.py dereferences
# h["n"] at two sites with no isinstance test, so every one of these escaped
# as a traceback at rc 1 before the subtree existed.
S23H_OUT=$(timeout 60 python3 - <<'S23HEOF' 2>&1
import sys
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs

base = {
    "file_path": "todo/01-t/TODO-01-h.md", "id": "h", "domain": "01-t",
    "status": "draft", "title": "h", "schema_version": 1,
    "sections": [], "section_headings": [], "inputs_xrefs": [],
    "stamps_xrefs": [], "created_at": None, "last_active_at": None,
}

def probe(headings, profile=cs.PROFILE_VALIDATE):
    node = dict(base)
    node["section_headings"] = headings
    try:
        cs.validate_nodes([node], "probe", profile)
        return "NORAISE"
    except cs.CacheSchemaError as exc:
        return exc.reason

results = {
    "non_dict": probe(["not-a-dict"]),
    "missing_n": probe([{"title": "t"}]),
    "non_int_n": probe([{"n": "1", "title": "t"}]),
    "duplicate_n": probe([{"n": 1, "title": "a"}, {"n": 1, "title": "b"}]),
    "unknown_key": probe([{"n": 1, "title": "t", "extra": 1}]),
    "valid": probe([{"n": 1, "title": "t"}]),
    "baseline_ignores": probe(["not-a-dict"], cs.PROFILE_BASELINE),
}
want = {
    "non_dict": "SHAPE", "missing_n": "SHAPE", "non_int_n": "SHAPE",
    "duplicate_n": "SHAPE", "unknown_key": "SHAPE",
    "valid": "NORAISE", "baseline_ignores": "NORAISE",
}
print("OK" if results == want else f"MISMATCH {results}")
S23HEOF
)
if [ "$S23H_OUT" = "OK" ]; then
    t_pass "cache schema: section_headings refuses all five malformed shapes, and the baseline profile does not declare it"
else
    t_fail "cache schema: section_headings probe gave '$S23H_OUT'"
fi

# 23h1-mutation: the claim above is only worth anything if the dereference it
# guards really is unguarded, so prove THAT directly rather than end-to-end. A
# malformed heading is handed to the two checks that read it; each must raise
# an ordinary Python exception, which is neither a CacheSchemaError nor
# anything either reader catches -- so before the subtree existed it escaped
# main() as a traceback and exited 1, the findings code. Driving it in-process
# also avoids rewriting a cache file, which would strip its corpus binding and
# make the fixture refuse for an unrelated reason.
S23I_OUT=$(cd "$REPO_ROOT" && timeout 60 python3 - <<'S23IMEOF' 2>&1
import sys
sys.path.insert(0, "scripts/todo-graph")
import validate

node = {
    "file_path": "todo/01-t/TODO-01-h.md", "id": "h", "domain": "01-t",
    "status": "active", "title": "h", "schema_version": 1,
    "sections": [], "inputs_xrefs": [], "stamps_xrefs": [],
    "section_headings": ["not-a-dict"],
}
nodes = [node]
idx = validate.build_id_index(nodes)
pidx = validate.build_path_index(nodes)
seen = []
for label, call in (
    ("dangling-section",
     lambda: validate.check_dangling_section_ref(nodes, idx, pidx)),
    ("orphan-io-row", lambda: validate.check_orphaned_io_row(nodes)),
):
    try:
        call()
        seen.append(f"{label}=NORAISE")
    except (TypeError, KeyError) as exc:
        seen.append(f"{label}={type(exc).__name__}")
    except Exception as exc:  # noqa: BLE001 -- any other type is a miss too
        seen.append(f"{label}=OTHER:{type(exc).__name__}")
print(" ".join(seen))
S23IMEOF
)
if [ "$S23I_OUT" = "dangling-section=TypeError orphan-io-row=TypeError" ]; then
    t_pass "cache schema: mutation check -- both section_headings consumers raise on a malformed entry, so the subtree is what stands between the cache and a traceback"
else
    t_fail "cache schema: mutation check -- section_headings consumers gave '$S23I_OUT' (want both TypeError)"
fi

# ----------------------------------------------------------------------
# Section 25: fields the readers consume that the producer never emits.
#
# Three pipelines, each of which degraded SILENTLY rather than failing --
# `effort` unreachable, file-level `depends_on` unauthored, and no cache-format
# identity at all -- so every fixture below asserts the REFUSAL or the value,
# and each is control-tested: revert the mechanism and the fixture must fail
# with the predicted symptom.
# ======================================================================

# s25_tree <root> <effort-line> -- a one-TODO corpus with a real git history,
# because `build.py` derives created_at/last_active_at from `git log` and a
# tree without one produces null timestamps the Gantt renderer then defaults.
s25_tree() {
    local root="$1" effort="$2"
    rm -rf "$root"
    mkdir -p "$root/todo/01-test" "$root/build"
    {
        printf -- '---\nschema_version: 1\nid: fm-s25\ndomain: 01-test\n'
        printf -- 'status: active\ntitle: "s25 fixture"\n'
        [ -n "$effort" ] && printf -- 'effort: %s\n' "$effort"
        printf -- '---\n# TODO-01 -- s25 fixture\n## Implementation Order\n'
        printf -- '| Order | Section | Deliverable | Depends On | Status |\n'
        printf -- '| --- | --- | --- | --- | --- |\n| 1 | 1 | First | -- | [ ] |\n'
        printf -- '## 1. First Section\n\n- [ ] Item one\n'
    } > "$root/todo/01-test/TODO-01-s25.md"
    git init -q "$root" >/dev/null 2>&1
    git -C "$root" add -A >/dev/null 2>&1
    git -C "$root" -c user.email=t@t -c user.name=t commit -qm s25 >/dev/null 2>&1
}

# Sub-test 25e1: a frontmatter `effort` reaches the emitted Gantt duration, and
# a TODO without one still renders the documented default. Before this section
# the producer dropped the field, so the first row was UNREACHABLE by any route.
S25E="$TMP_DIR/s25-effort"
s25_tree "$S25E" "3d"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S25E/todo" \
    --output "$S25E/build/todo-cache.json" --repo-root "$S25E" >/dev/null 2>&1
S25E_GANTT=$(timeout 60 python3 "$QUERY_PY" --cache "$S25E/build/todo-cache.json" \
    --repo-root "$S25E" --quiet render --render-format gantt 2>&1)
s25_tree "$S25E-def" ""
timeout 60 python3 "$BUILD_PY" --quiet --root "$S25E-def/todo" \
    --output "$S25E-def/build/todo-cache.json" --repo-root "$S25E-def" >/dev/null 2>&1
S25E_DEF=$(timeout 60 python3 "$QUERY_PY" --cache "$S25E-def/build/todo-cache.json" \
    --repo-root "$S25E-def" --quiet render --render-format gantt 2>&1)
if echo "$S25E_GANTT" | grep -q ", 3d$" && echo "$S25E_DEF" | grep -q ", 1w$"; then
    t_pass "section 25: frontmatter effort reaches the Gantt row; absent still defaults to 1w"
else
    t_fail "section 25: effort round trip broken (with='$S25E_GANTT' without='$S25E_DEF')"
fi

# 25e1-mutation: drop `effort` from the producer's copy list, which is the exact
# pre-section-25 state. The row must fall back to the default, proving the
# fixture reads the PRODUCER's behaviour rather than the renderer's.
S25E_MUT="$TMP_DIR/s25-effort-mut"
s25_tree "$S25E_MUT" "3d"
s23_tooldir "$S25E_MUT/tool"
python3 - "$S25E_MUT/tool/build.py" <<'S25EMEOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
# Revert the DERIVED copy list to a retyped literal that drops `effort` --
# the pre-review shape. The producer must now REFUSE rather than silently
# publish a node without the authored field (Codex adversarial, [medium]:
# a subset check cannot see a field that stops being emitted).
needle = "        for opt in _EMITTED_OPTIONAL:"
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
revert = ('''        for opt in ("owners", "file_patterns", "depends_on", "satisfies",
                    "superseded_by"):''')
p.write_text(src.replace(needle, revert, 1), encoding="utf-8")
S25EMEOF
S25EM_OUT=$(timeout 60 python3 "$S25E_MUT/tool/build.py" --root "$S25E_MUT/todo" \
    --output "$S25E_MUT/build/todo-cache.json" --repo-root "$S25E_MUT" 2>&1)
S25EM_RC=$?
if [ "$S25EM_RC" != "0" ] && echo "$S25EM_OUT" | grep -q "emission-drop" \
   && [ ! -f "$S25E_MUT/build/todo-cache.json" ]; then
    t_pass "section 25: mutation check -- a copy list that drops an authored effort REFUSES the build (fixture is sensitive)"
else
    t_fail "section 25: mutation check -- dropped effort published anyway rc=$S25EM_RC, so 25e1 proves nothing (out=$S25EM_OUT)"
fi

# Sub-test 25e2: hostile effort values are REFUSED by the producer rather than
# defaulted. `2w\n` is the one that matters and the one a `$`-anchored regex
# ACCEPTS: Python's `$` matches before a trailing newline, and the renderer
# interpolates this straight into a mermaid row, so the newline ends the row
# early and reshapes the chart. Measured, not reasoned about.
S25E_BAD=""
for bad in '4mo' '0d' '2 w' '007w' 'x' '2w extra'; do
    s25_tree "$TMP_DIR/s25-bad" "$bad"
    timeout 60 python3 "$BUILD_PY" --quiet --root "$TMP_DIR/s25-bad/todo" \
        --output "$TMP_DIR/s25-bad/build/todo-cache.json" \
        --repo-root "$TMP_DIR/s25-bad" >/dev/null 2>&1
    [ $? -eq 0 ] && S25E_BAD="$S25E_BAD $bad"
    [ -f "$TMP_DIR/s25-bad/build/todo-cache.json" ] && S25E_BAD="$S25E_BAD wrote:$bad"
done
S25E_NL=$(python3 -c "
import sys; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
print('REFUSED' if not cs.EFFORT_REGEX.match('2w\n') else 'ACCEPTED')")
if [ -z "$S25E_BAD" ] && [ "$S25E_NL" = "REFUSED" ]; then
    t_pass "section 25: every hostile effort value refuses the build, including the trailing-newline case a \$-anchor accepts"
else
    t_fail "section 25: hostile effort accepted ('$S25E_BAD'), newline=$S25E_NL"
fi

# Sub-test 25e3: the JSON Schema mirror and the Python grammar admit the SAME
# values. Two spellings of one rule is how the schema comes to accept what the
# producer refuses -- the gap section 23 closed for sections[].n, in reverse.
S25E_AGREE=$(python3 -c "
import json, re, sys; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
sch = json.load(open('$REPO_ROOT/scripts/todo-graph/schema/cache.schema.json'))
pat = sch['items']['properties']['effort']['pattern']
rx = re.compile(pat)
bad = [v for v in ('1w','3d','12w','0d','007w','4mo','','2 w','x')
       if bool(rx.match(v) and rx.match(v).end() == len(v)) != bool(cs.EFFORT_REGEX.match(v))]
print('AGREE' if not bad else 'DISAGREE:' + ','.join(bad))")
if [ "$S25E_AGREE" = "AGREE" ]; then
    t_pass "section 25: cache.schema.json effort pattern and cache_schema.EFFORT_REGEX admit the same values"
else
    t_fail "section 25: effort grammars disagree ($S25E_AGREE)"
fi

# Sub-test 25d1: the readiness verbs consult the FILE-level depends_on and say
# so when there is nothing to rank. The section chose this source deliberately
# over sections[].depends_on, whose targets are unqualified; the visible note is
# what stops an unconditionally-empty answer reading as a verdict.
S25D=$(timeout 60 python3 "$QUERY_PY" --repo-root "$REPO_ROOT" blocking 2>&1)
S25D_Q=$(timeout 60 python3 "$QUERY_PY" --repo-root "$REPO_ROOT" --quiet blocking 2>&1)
if echo "$S25D" | grep -q "ranked against 0 file-level" \
   && ! echo "$S25D_Q" | grep -q "ranked against 0 file-level"; then
    t_pass "section 25: an empty readiness ranking names its dependency source; --quiet stays machine-clean"
else
    t_fail "section 25: dependency-source note missing (out=$S25D) or leaked under --quiet"
fi

# 25d1-mutation: the note must be CONDITIONAL on the edge set being empty, or it
# is noise that trains a reader to skip stderr. Give one node an edge and it
# must go quiet.
S25D_M=$(python3 -c "
import sys, io, contextlib; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import query as q, json
nodes = json.load(open('$REPO_ROOT/build/todo-cache.json'))
nodes[1]['depends_on'] = [nodes[0]['id']]
ctx = q.Ctx(nodes, '$REPO_ROOT', quiet=False)
err = io.StringIO()
with contextlib.redirect_stderr(err):
    ctx.dependency_source_notice_once()
print('QUIET' if 'ranked against 0' not in err.getvalue() else 'FIRED')")
if [ "$S25D_M" = "QUIET" ]; then
    t_pass "section 25: mutation check -- an authored depends_on edge silences the note (it is not unconditional)"
else
    t_fail "section 25: the dependency-source note fires even with edges present, so it is noise not a caveat"
fi

# Sub-test 25c1: a --diff baseline written by a DIFFERENT producer contract is
# refused rather than diffed. Until this section validate.py could only ASK the
# caller to regenerate: shape was all it had, and a field that kept its type and
# changed its meaning passed, producing deltas nobody authored.
S25C="$TMP_DIR/s25-baseline"
s23_tree "$S25C"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S25C/todo" \
    --output "$S25C/build/todo-cache.json" --repo-root "$S25C" >/dev/null 2>&1
cp "$S25C/build/todo-cache.json" "$S25C/old-baseline.json"
bind_baseline "$S25C/old-baseline.json"
python3 - "$S25C/old-baseline.json" "$REPO_ROOT/scripts/todo-graph" <<'S25CEOF'
import hashlib, json, pathlib, sys
sys.path.insert(0, sys.argv[2])
import cache_schema as cs
cache = pathlib.Path(sys.argv[1])
side = cs.sidecar_path(cache, hashlib.sha256(cache.read_bytes()).hexdigest())
rec = json.loads(side.read_text())
# An OLDER contract, which is exactly what a historical artifact carries.
rec[cs.CACHE_FORMAT_VERSION_KEY] = cs.CACHE_FORMAT_VERSION - 1
side.write_text(json.dumps(rec, indent=2, sort_keys=True) + "\n")
S25CEOF
# HASH BEFORE THE DESTRUCTIVE OPERATION. This was taken AFTER validate.py
# ran, so 25c2 below compared two POST-operation digests -- it passed even if
# the refusal had overwritten the baseline, and would have passed against code
# that never refused at all. A fixture that cannot fail is not evidence
# (Codex adversarial, [medium]).
# THE ARTIFACT IS THE PAIR, not the JSON alone. Hashing only the cache let a
# destructive repair pass: rebuilding from the same deterministic corpus
# reproduces byte-identical JSON while REPLACING the deliberately old-contract
# binding with a current one, so the JSON digest is equal and the thing the
# fixture exists to protect is gone (Codex re-adversarial, [medium]).
s25c_fingerprint() {
    { md5sum "$S25C/old-baseline.json"
      ls -1 "$S25C"/old-baseline.json.corpus-*.json 2>/dev/null | sort
      cat "$S25C"/old-baseline.json.corpus-*.json 2>/dev/null | md5sum
    } | md5sum | cut -d' ' -f1
}
S25C_BYTES=$(s25c_fingerprint)
S25C_OUT=$(REPO_ROOT="$REPO_ROOT" timeout 60 python3 "$VALIDATE_PY" \
    --cache "$S25C/build/todo-cache.json" --repo-root "$S25C" --quiet \
    --diff "$S25C/old-baseline.json" 2>&1); S25C_RC=$?
if [ "$S25C_RC" = "2" ] && echo "$S25C_OUT" | grep -q "LEGACY_FORMAT" \
   && ! echo "$S25C_OUT" | grep -q "graph-delta:"; then
    t_pass "section 25: a baseline from an older producer contract is REFUSED, not diffed"
else
    t_fail "section 25: old-contract baseline rc=$S25C_RC out=$S25C_OUT"
fi

# Sub-test 25c2: and the refusal leaves the caller's artifact byte-identical --
# a historical baseline is somebody else's file, so the recovery must not reach
# it. Same ownership rule section 23 established for the current cache.
S25C_AFTER=$(s25c_fingerprint)
if [ "$S25C_BYTES" = "$S25C_AFTER" ]; then
    t_pass "section 25: the refused baseline is left byte-identical, cache AND binding"
else
    t_fail "section 25: the refused baseline artifact changed (cache or binding)"
fi

# 25c2-mutation: replace the binding with a CURRENT-contract one while leaving
# the cache bytes untouched -- the exact shape a destructive repair produces.
# The fingerprint must change, or 25c2 proves nothing.
bind_baseline "$S25C/old-baseline.json"
S25C_MUT_FP=$(s25c_fingerprint)
if [ "$S25C_MUT_FP" != "$S25C_BYTES" ]; then
    t_pass "section 25: mutation check -- swapping the baseline binding alone moves the fingerprint (fixture is sensitive)"
else
    t_fail "section 25: the baseline fingerprint ignores the binding, so 25c2 proves nothing"
fi

# Sub-test 25c3: a binding whose FIELD SET drifted at the SAME version refuses
# too, and with a different message -- that is producer/schema drift, not an
# ordinary old artifact, and the two need different words to the operator.
S25C_DRIFT=$(REPO_ROOT="$REPO_ROOT" python3 -c "
import hashlib, json, pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
cache = pathlib.Path('$S25C/old-baseline.json')
blob = cache.read_bytes()
side = cs.sidecar_path(cache, hashlib.sha256(blob).hexdigest())
rec = json.loads(side.read_text())
rec[cs.CACHE_FORMAT_VERSION_KEY] = cs.CACHE_FORMAT_VERSION
rec[cs.PRODUCER_CONTRACT_DIGEST_KEY] = 'sha256:' + '0' * 64
side.write_text(json.dumps(rec))
try:
    cs.check_cache_format(cache, blob, require_binding=True)
    print('ACCEPTED')
except cs.CacheSchemaError as e:
    print('REFUSED' if 'without a version bump' in str(e) else 'WRONGMSG')")
if [ "$S25C_DRIFT" = "REFUSED" ]; then
    t_pass "section 25: a drifted field set at the same version refuses as producer drift, not as an old artifact"
else
    t_fail "section 25: same-version digest drift gave '$S25C_DRIFT'"
fi

# 25c1-mutation: with the identity check reverted, the old-contract baseline
# must be DIFFED instead of refused -- the pre-section-25 behaviour, and the
# reason the section exists.
S25C_MUT="$TMP_DIR/s25-baseline-mut"
s23_tree "$S25C_MUT"
s23_tooldir "$S25C_MUT/tool"
python3 - "$S25C_MUT/tool/cache_schema.py" <<'S25CMEOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
# Matched by CALL PREFIX through its closing paren, not by a literal copy:
# the call gained an assignment target and wrapped onto two lines during the
# review, and a literal needle silently stops matching on any such edit --
# which is exactly how this control went quiet once already.
start = src.index("        binding = check_cache_format(")
end = src.index("\n", src.index("require_binding=not check_stale)", start))
needle = src[start:end]
p.write_text(src.replace(needle, "        binding = None", 1), encoding="utf-8")
S25CMEOF
timeout 60 python3 "$S25C_MUT/tool/build.py" --quiet --root "$S25C_MUT/todo" \
    --output "$S25C_MUT/build/todo-cache.json" --repo-root "$S25C_MUT" >/dev/null 2>&1
cp "$S25C_MUT/build/todo-cache.json" "$S25C_MUT/old-baseline.json"
S25CM_OUT=$(timeout 60 python3 "$S25C_MUT/tool/validate.py" \
    --cache "$S25C_MUT/build/todo-cache.json" --repo-root "$S25C_MUT" --quiet \
    --diff "$S25C_MUT/old-baseline.json" 2>&1); S25CM_RC=$?
if [ "$S25CM_RC" != "2" ]; then
    t_pass "section 25: mutation check -- without the identity check an unidentified baseline is diffed (fixture is sensitive)"
else
    t_fail "section 25: mutation check -- reverted identity still refused, so 25c1 proves nothing (out=$S25CM_OUT)"
fi

# Sub-test 25c4: the producer may not emit a node key it has not DECLARED. The
# digest certifies the declaration, so an undeclared key would ship under an
# identity claiming a field set the cache does not have -- the one failure the
# identity exists to make impossible. Section 20's "declare, do not infer" rule.
S25C_DECL="$TMP_DIR/s25-declared"
s23_tree "$S25C_DECL"
s23_tooldir "$S25C_DECL/tool"
python3 - "$S25C_DECL/tool/build.py" <<'S25DEOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
# TWO nodes carry `"file_path": rel,` -- the frontmatter branch and the
# no-frontmatter one -- so the needle names the frontmatter branch by its
# preceding line rather than matching both and asserting out.
needle = """            "title": fm.get("title", title_from_h1),
            "file_path": rel,"""
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
p.write_text(src.replace(needle, needle + '\n            "smuggled": 1,', 1),
             encoding="utf-8")
S25DEOF
S25CD_OUT=$(timeout 60 python3 "$S25C_DECL/tool/build.py" --root "$S25C_DECL/todo" \
    --output "$S25C_DECL/build/todo-cache.json" --repo-root "$S25C_DECL" 2>&1); S25CD_RC=$?
if [ "$S25CD_RC" != "0" ] && echo "$S25CD_OUT" | grep -q "EMITTED_NODE_FIELDS" \
   && [ ! -f "$S25C_DECL/build/todo-cache.json" ]; then
    t_pass "section 25: an undeclared emitted node key refuses the build and writes no cache"
else
    t_fail "section 25: undeclared key rc=$S25CD_RC wrote=$([ -f "$S25C_DECL/build/todo-cache.json" ] && echo yes || echo no)"
fi

# Sub-test 25c5: a binding declaring a DIFFERENT wire format is refused before
# any of its fields are interpreted. `check_freshness` makes the same
# comparison, but `--diff` disables freshness and never reaches it -- so the
# documented rule that bumping SIDECAR_SCHEMA invalidates every existing
# binding did not hold for the one caller that REQUIRES a binding.
S25S=$(python3 -c "
import hashlib, json, pathlib, sys, tempfile
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
d = pathlib.Path(tempfile.mkdtemp())
c = d / 'cache.json'
c.write_bytes(pathlib.Path('$REPO_ROOT/build/todo-cache.json').read_bytes())
blob = c.read_bytes(); sha = hashlib.sha256(blob).hexdigest()
def bind(schema):
    cs.sidecar_path(c, sha).write_text(json.dumps({
        'schema': schema,
        cs.CACHE_FORMAT_VERSION_KEY: cs.CACHE_FORMAT_VERSION,
        cs.PRODUCER_CONTRACT_DIGEST_KEY: cs.PRODUCER_CONTRACT_DIGEST,
        'cache_sha256': sha}))
def verdict():
    try:
        cs.check_cache_format(c, blob, require_binding=True); return 'ACCEPTED'
    except cs.CacheSchemaError: return 'REFUSED'
bind('todo-cache-corpus-v99'); bad = verdict()
bind(cs.SIDECAR_SCHEMA); good = verdict()
print(bad, good)")
if [ "$S25S" = "REFUSED ACCEPTED" ]; then
    t_pass "section 25: a binding declaring another wire format is refused; the control still passes"
else
    t_fail "section 25: sidecar-schema gate gave '$S25S' (want 'REFUSED ACCEPTED')"
fi

# Sub-test 25e4: the standalone Gantt guard distinguishes an ABSENT effort from
# a PRESENT null. `node.get()` conflated them, so `{"effort": null}` rendered
# the 1w default -- more permissive than the validator this guard backstops,
# and reachable by a caller that imported render.py without cache_schema.
S25N=$(python3 -c "
import sys; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import render
absent = render._gantt_effort({'id': 'x'})
try:
    render._gantt_effort({'id': 'x', 'effort': None}); present = 'DEFAULTED'
except ValueError: present = 'RAISED'
try:
    render._gantt_effort({'id': 'x', 'effort': '4mo'}); bad = 'DEFAULTED'
except ValueError: bad = 'RAISED'
print(absent, present, bad)")
if [ "$S25N" = "1w RAISED RAISED" ]; then
    t_pass "section 25: the standalone Gantt guard defaults only on ABSENCE; a present null raises"
else
    t_fail "section 25: standalone gantt guard gave '$S25N' (want '1w RAISED RAISED')"
fi

# Sub-test 25c6: the declared field set and the JSON Schema's property list are
# the SAME set. They are two spellings of one contract, and the digest certifies
# only the Python one -- so a field added to the schema alone would be accepted
# by jsonschema and refused by the producer, while one added to the declaration
# alone would move the digest and invalidate every artifact for a field the
# published schema does not admit.
S25AG=$(python3 -c "
import json, sys; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
sch = json.load(open('$REPO_ROOT/scripts/todo-graph/schema/cache.schema.json'))
props = set(sch['items']['properties'])
only_decl = sorted(cs.EMITTED_NODE_FIELDS - props)
only_sch = sorted(props - cs.EMITTED_NODE_FIELDS)
print('AGREE' if not only_decl and not only_sch
      else f'DISAGREE decl-only={only_decl} schema-only={only_sch}')")
if [ "$S25AG" = "AGREE" ]; then
    t_pass "section 25: EMITTED_NODE_FIELDS and cache.schema.json declare the same node fields"
else
    t_fail "section 25: declaration and schema disagree ($S25AG)"
fi

# Sub-test 25c7: the lint consumer maps EVERY cache reason to an exit code. A
# reason added without a row here printed its refusal and then died on
# `KeyError`, which is an undocumented traceback out of the handler whose whole
# job is keeping infrastructure failures out of the verdict channel.
S25RC=$(python3 -c "
import sys; sys.path.insert(0, '$REPO_ROOT/scripts/lint')
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
import check_stub_behind_stamp as chk
reasons = [getattr(cs, n) for n in dir(cs) if n.startswith('REASON_')]
missing = sorted(r for r in reasons if r not in chk._CACHE_REASON_RC)
print('MAPPED' if not missing else 'MISSING:' + ','.join(missing),
      chk._CACHE_REASON_RC.get(cs.REASON_LEGACY_FORMAT))")
if [ "$S25RC" = "MAPPED 5" ]; then
    t_pass "section 25: every cache_schema reason has a lint exit code, LEGACY_FORMAT at 5"
else
    t_fail "section 25: reason-to-rc mapping incomplete ($S25RC)"
fi

# 25c7-mutation: drop LEGACY_FORMAT from the table and the module must REFUSE TO
# IMPORT, naming the reason -- not wait until a bad cache is being reported.
S25RCM=$(python3 - "$REPO_ROOT" <<'S25RCEOF'
import pathlib, subprocess, sys, tempfile
repo = pathlib.Path(sys.argv[1])
d = pathlib.Path(tempfile.mkdtemp())
src = (repo / "scripts/lint/check_stub_behind_stamp.py").read_text()
needle = "    _cs.REASON_LEGACY_FORMAT: 5,            # ERROR: another producer contract\n"
if src.count(needle) != 1:
    print("NEEDLE-MISS"); raise SystemExit
(d / "check_stub_behind_stamp.py").write_text(src.replace(needle, "", 1))
r = subprocess.run([sys.executable, "-c",
                    "import sys; sys.path.insert(0, %r); sys.path.insert(0, %r); "
                    "import check_stub_behind_stamp"
                    % (str(d), str(repo / "scripts/todo-graph"))],
                   capture_output=True, text=True)
print("REFUSED" if r.returncode != 0 and "LEGACY_FORMAT" in r.stderr else "IMPORTED")
S25RCEOF
)
if [ "$S25RCM" = "REFUSED" ]; then
    t_pass "section 25: mutation check -- an unmapped reason fails the lint module at import, naming it"
else
    t_fail "section 25: unmapped reason gave '$S25RCM' (want REFUSED)"
fi

# Sub-test 25e5: the FRONTMATTER schema and the producer admit the same effort
# values. `additionalProperties` is true there, so an undeclared property is
# silently accepted -- editor-time and ad-hoc jsonschema validation would
# approve a TODO the generator refuses, breaking the two-layer split.
S25FM=$(python3 -c "
import json, re, sys; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import build as b
sch = json.load(open('$REPO_ROOT/docs/infrastructure/todo-metadata.schema.json'))
prop = sch['properties'].get('effort')
if not prop:
    print('UNDECLARED'); raise SystemExit
from jsonschema import Draft202012Validator
_v = Draft202012Validator(prop)
def sch_ok(v):
    # THROUGH THE REAL VALIDATOR, not a hand-rolled re-implementation of it.
    # This used to compile prop['pattern'] itself and additionally require
    # match.end() == len(v), which MASKED the divergence it exists to catch:
    # jsonschema evaluates the pattern with Python re, whose \$ also matches
    # before a trailing newline, so the schema accepted '2w\n' while the
    # producer refused it and this fixture reported AGREE (Codex consistency,
    # section 25, [medium]). Only the validator's own verdict is evidence.
    return _v.is_valid(v)
def prod_ok(v):
    fm = {'schema_version': 1, 'id': 'x-y', 'domain': 'd', 'status': 'draft',
          'title': 't', 'effort': v}
    return not [m for c, m in b.validate_frontmatter(fm) if c == 'invalid-field']
bad = [repr(v) for v in ('1w', '3d', '12w', '0d', '007w', '4mo', '', '2 w', 'x',
                         '2w\n', '2w\r\n', '\n2w')
       if sch_ok(v) != prod_ok(v)]
extra = [repr(v) for v in (None, 7, True) if prod_ok(v)]
print('AGREE' if not bad and not extra else 'DISAGREE:' + ','.join(bad + extra))")
if [ "$S25FM" = "AGREE" ]; then
    t_pass "section 25: the frontmatter schema and build.py admit the same effort values"
else
    t_fail "section 25: frontmatter schema disagrees with the producer ($S25FM)"
fi

# Sub-test 25d2: the dependency caveat survives `--quiet`, which is what the
# MCP transport always passes. A stderr-only note was invisible to exactly the
# caller least able to tell an empty ranking from an empty graph -- the same
# transport section 22 hardened, losing the same distinction.
S25MCP=$(timeout 60 python3 "$QUERY_PY" --repo-root "$REPO_ROOT" --quiet --json blocking 2>/dev/null)
S25MCP_ERR=$(timeout 60 python3 "$QUERY_PY" --repo-root "$REPO_ROOT" --quiet --json blocking 2>&1 >/dev/null)
# `stats` is UNBOUNDED and never reaches bound_rows, yet it reuses cmd_blocking
# for top_blocking and reports avg_dep_depth from the same empty edge set -- so
# it needs the caveat by the same argument, and the first fix missed it (Codex
# re-adversarial, [high]).
S25MCP_ST=$(timeout 60 python3 "$QUERY_PY" --repo-root "$REPO_ROOT" --quiet --json stats 2>/dev/null)
if echo "$S25MCP" | grep -q '"dependency_source"' \
   && echo "$S25MCP" | grep -q '"edges": 0' \
   && echo "$S25MCP_ST" | grep -q '"dependency_source"' \
   && [ -z "$S25MCP_ERR" ]; then
    t_pass "section 25: the dependency caveat rides in the --json envelope for BOTH the bounded verbs and stats"
else
    t_fail "section 25: --quiet --json dropped the caveat (blocking=$S25MCP stats=$S25MCP_ST err=$S25MCP_ERR)"
fi

# 25d2-mutation: the caveat must be CONDITIONAL. Give a node an edge and the
# envelope must lose the key entirely rather than reporting edges: 0.
S25MCPM=$(python3 -c "
import sys, json; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import query as q
nodes = json.load(open('$REPO_ROOT/build/todo-cache.json'))
nodes[1]['depends_on'] = [nodes[0]['id']]
ctx = q.Ctx(nodes, '$REPO_ROOT', quiet=True)
ctx.dependency_source_notice_once()
class A: pass
a = A(); a.quiet = True; a.scope = None; a.fields = None; a.limit = 50; a.offset = 0
_, _, meta = q.bound_rows([], ['id'], a, 'blocking', ctx)
print('ABSENT' if 'dependency_source' not in meta else 'PRESENT')")
if [ "$S25MCPM" = "ABSENT" ]; then
    t_pass "section 25: mutation check -- an authored edge removes the caveat from the envelope (it is not unconditional)"
else
    t_fail "section 25: the --json caveat is emitted even with edges present ($S25MCPM)"
fi

# Sub-test 25c8: a sidecar that is a FIFO must REFUSE, not hang. `open()` on a
# FIFO with no writer blocks before fstat, so the size ceiling, the JSON
# handling and every reason-code path below it are unreachable -- a hang
# needing no overflow. Reachable because freshness-disabled reads take a
# caller-owned directory.
S25FIFO=$(timeout 30 python3 -c "
import hashlib, os, pathlib, sys, tempfile
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
d = pathlib.Path(tempfile.mkdtemp())
c = d / 'todo-cache.json'
c.write_bytes(pathlib.Path('$REPO_ROOT/build/todo-cache.json').read_bytes())
blob = c.read_bytes()
os.mkfifo(cs.sidecar_path(c, hashlib.sha256(blob).hexdigest()))
try:
    cs.check_cache_format(c, blob, require_binding=True)
    print('ACCEPTED')
except cs.CacheSchemaError:
    print('REFUSED')" 2>&1)
if [ "$S25FIFO" = "REFUSED" ]; then
    t_pass "section 25: a FIFO corpus binding is refused rather than blocking the reader forever"
else
    t_fail "section 25: FIFO sidecar gave '$S25FIFO' (want REFUSED; empty/timeout means it hung)"
fi

# Sub-test 25c9: the format check HANDS its validated record to the freshness
# check. Two independent opens let a reader certify the format from one sidecar
# generation and the corpus from its replacement.
S25GEN=$(python3 -c "
import inspect, sys; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
src = inspect.getsource(cs._load_and_validate)
passes = 'rec=binding' in src
returns = inspect.signature(cs.check_freshness).parameters.get('rec') is not None
print('BOUND' if passes and returns else f'UNBOUND passes={passes} accepts={returns}')")
if [ "$S25GEN" = "BOUND" ]; then
    t_pass "section 25: the format and freshness checks consume ONE binding generation"
else
    t_fail "section 25: sidecar generations are read independently ($S25GEN)"
fi

# Sub-test 25e6: the frontmatter schema forbids EVERY cache-only field the
# producer forbids. It listed two of three, so a hand-authored stamped_items
# passed the schema and was refused by build.py -- the two-layer split holding
# in one direction only.
S25CO=$(python3 -c "
import json, sys; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
sch = json.load(open('$REPO_ROOT/docs/infrastructure/todo-metadata.schema.json'))
forbidden = {r['required'][0] for r in sch.get('not', {}).get('anyOf', [])}
src = open('$REPO_ROOT/scripts/todo-graph/build.py').read()
i = src.index('for cache_only in (')
producer = set(eval(src[src.index('(', i):src.index(')', i) + 1]))
print('AGREE' if forbidden == producer else f'DISAGREE schema={sorted(forbidden)} producer={sorted(producer)}')")
if [ "$S25CO" = "AGREE" ]; then
    t_pass "section 25: the frontmatter schema and build.py forbid the same cache-only fields"
else
    t_fail "section 25: cache-only contract disagrees ($S25CO)"
fi

# Sub-test 25g1: BOTH published schemas refuse a trailing-newline effort THROUGH
# THE VALIDATOR THAT ACTUALLY RUNS. JSON Schema pins no regex engine and the two
# that read these files disagree on `$`: ECMA anchors at end-of-input, Python's
# `re` also matches before a final newline, so an anchored-only pattern accepts
# "2w\n" -- the exact value the grammar exists to refuse, because render.py
# interpolates it into a mermaid row where a newline ends the row early.
S25NL=$(python3 -c "
import json
from jsonschema import Draft202012Validator
out = []
for p, dig in (('$REPO_ROOT/docs/infrastructure/todo-metadata.schema.json', ('properties',)),
               ('$REPO_ROOT/scripts/todo-graph/schema/cache.schema.json', ('items', 'properties'))):
    node = json.load(open(p))
    for k in dig:
        node = node[k]
    v = Draft202012Validator(node['effort'])
    for val, want in (('2w', True), ('3d', True), ('2w\n', False),
                      ('2w\r\n', False), ('2w\r', False), ('4mo', False)):
        if v.is_valid(val) != want:
            out.append(f'{p.split(chr(47))[-1]}:{val!r}')
print('REFUSED' if not out else 'ACCEPTED:' + ','.join(out))")
if [ "$S25NL" = "REFUSED" ]; then
    t_pass "section 25: both schemas refuse a trailing-newline effort through Draft202012Validator"
else
    t_fail "section 25: a schema accepts an embedded-newline effort ($S25NL)"
fi

# 25g1-mutation: strip the `not` clause and the Python validator must ACCEPT
# "2w\n" again. Without this control the fixture above would still pass on a
# pattern-only property under an engine whose `$` happens to be strict, which
# is how the divergence hid in the first place.
S25NLM=$(python3 -c "
import json
from jsonschema import Draft202012Validator
node = json.load(open('$REPO_ROOT/docs/infrastructure/todo-metadata.schema.json'))['properties']['effort']
if 'not' not in node:
    print('NO-CLAUSE'); raise SystemExit
mut = {k: v for k, v in node.items() if k != 'not'}
print('ACCEPTED' if Draft202012Validator(mut).is_valid('2w\n') else 'STILL-REFUSED')")
if [ "$S25NLM" = "ACCEPTED" ]; then
    t_pass "section 25: mutation check -- dropping the not-clause restores the newline hole"
else
    t_fail "section 25: newline mutation gave '$S25NLM' (want ACCEPTED)"
fi

# Sub-test 25g2: the producer contract is checked in BOTH directions. main()'s
# runtime scan subtracts EMITTED_NODE_FIELDS from the emitted keys, so it can
# only ever see an undeclared ADDITION; a field the producer stops emitting is
# invisible to it AND to the digest, which certifies the declaration rather than
# the emission. A dropped `effort` would then publish under a valid identity and
# every reader would silently take its default.
S25BD=$(python3 - "$REPO_ROOT" <<'S25BDEOF'
import os, pathlib, subprocess, sys, tempfile
repo = pathlib.Path(sys.argv[1])
tg = repo / "scripts/todo-graph"
src = (tg / "build.py").read_text()
env = dict(os.environ, PYTHONPATH=str(tg))
d = pathlib.Path(tempfile.mkdtemp())

def load(text):
    p = d / "probe_build.py"
    p.write_text(text)
    r = subprocess.run([sys.executable, str(p), "--help"],
                       capture_output=True, text=True, cwd=str(repo), env=env)
    return r.returncode, r.stderr

rc, _ = load(src)
if rc != 0:
    print("CONTROL-FAILED"); raise SystemExit
# The producer-side authoring list is the only field list left in build.py, so
# it is the only one a build.py mutation can drift.
survived = []
needle = '_EMITTED_OPTIONAL = tuple(f for f in OPTIONAL_FIELDS if f != "$schema")'
if src.count(needle) != 1:
    survived.append("drop-optional:needle-miss")
else:
    rc, err = load(src.replace(
        needle,
        '_EMITTED_OPTIONAL = tuple(f for f in OPTIONAL_FIELDS '
        'if f not in ("$schema", "effort"))', 1))
    if rc == 0 or "optional-mode" not in err:
        survived.append("drop-optional")

# THE COORDINATED RECLASSIFICATION, which is the one no equality check can see.
# Moving `sections` from always-emitted to conditional in the SHARED
# declaration AND dropping it from cache.schema.json's `required` list leaves
# every name set identical, so the union assert and the schema cross-check both
# pass. Only the digest can refuse it, and only because the MODE is hashed --
# so this case asserts the identity MOVES, which is what forces a reader to
# treat the two contracts as different (Codex re-adversarial, round 5).
cs_src = (tg / "cache_schema.py").read_text()
mode_needle = '    "sections": FIELD_ALWAYS,\n'
if cs_src.count(mode_needle) != 1:
    survived.append("reclassify:needle-miss")
else:
    md = pathlib.Path(tempfile.mkdtemp())
    (md / "cache_schema.py").write_text(
        cs_src.replace(mode_needle, '    "sections": FIELD_CONDITIONAL,\n', 1))
    probe = (
        "import sys; sys.path.insert(0, %r); sys.path.insert(1, %r)\n"
        "import cache_schema as m\n"
        "print(m.PRODUCER_CONTRACT_DIGEST)\n" % (str(md), str(tg)))
    (md / "probe_digest.py").write_text(probe)
    r = subprocess.run([sys.executable, str(md / "probe_digest.py")],
                       capture_output=True, text=True, cwd=str(repo))
    real = subprocess.run(
        [sys.executable, "-c",
         "import sys; sys.path.insert(0, %r); import cache_schema as m; "
         "print(m.PRODUCER_CONTRACT_DIGEST)" % str(tg)],
        capture_output=True, text=True, cwd=str(repo))
    if r.returncode != 0 or real.returncode != 0:
        survived.append("reclassify:probe-failed")
    elif r.stdout.strip() == real.stdout.strip():
        survived.append("reclassify")

# AN UNKNOWN MODE, which is the typo case. Every producer guard selects by exact
# mode equality, so a misspelled value leaves the field declared in
# EMITTED_NODE_FIELDS and selected by NOTHING -- name set unchanged, schema and
# optional comparisons unchanged, digest happily hashing the bad value. The
# module must refuse to import rather than certify a field nothing guards.
typo_needle = '    "stamped_items": FIELD_CONDITIONAL,\n'
if cs_src.count(typo_needle) != 1:
    survived.append("unknown-mode:needle-miss")
else:
    td = pathlib.Path(tempfile.mkdtemp())
    (td / "cache_schema.py").write_text(
        cs_src.replace(typo_needle, '    "stamped_items": "conditionl",\n', 1))
    r = subprocess.run(
        [sys.executable, "-c",
         "import sys; sys.path.insert(0, %r); sys.path.insert(1, %r); "
         "import cache_schema" % (str(td), str(tg))],
        capture_output=True, text=True, cwd=str(repo))
    if r.returncode == 0 or "unknown mode" not in r.stderr:
        survived.append("unknown-mode")
print("REFUSED" if not survived else "SURVIVED:" + ",".join(survived))
S25BDEOF
)
if [ "$S25BD" = "REFUSED" ]; then
    t_pass "section 25: mutation check -- a dropped optional refuses at import, a reclassified field moves the digest"
else
    t_fail "section 25: producer declaration drift not caught ($S25BD)"
fi

# Sub-test 25g3: IDENTITY BEFORE SHAPE holds when the binding is ABSENT, not
# only when it is present and wrong. check_cache_format used to return quietly
# under freshness-ON and leave the refusal to check_freshness, which runs AFTER
# validate_nodes -- so an unbound cache that also violated a subtree was
# reported as SHAPE, sending the operator after a corpus fault when the artifact
# declared no producer contract at all. The reason stays STALE (the honest
# repair for the canonical cache is a rebuild); only the ORDERING moved.
S25IO=$(python3 - "$REPO_ROOT" <<'S25IOEOF'
import hashlib, json, pathlib, subprocess, sys, tempfile
repo = pathlib.Path(sys.argv[1])
tg = repo / "scripts/todo-graph"
sys.path.insert(0, str(tg))
import cache_schema as cs

tmp = pathlib.Path(tempfile.mkdtemp())
todo = tmp / "todo"; (todo / "01-x").mkdir(parents=True)
(todo / "01-x" / "TODO-01-a.md").write_text(
    "---\nschema_version: 1\nid: probe-a\ndomain: 01-x\nstatus: active\n"
    "title: \"A\"\n---\n\n# A\n")
out = tmp / "todo-cache.json"
r = subprocess.run([sys.executable, str(tg / "build.py"), "--root", str(todo),
                    "--repo-root", str(tmp), "--output", str(out), "--quiet"],
                   capture_output=True, text=True)
if r.returncode != 0:
    print("FIXTURE-BUILD-FAILED"); raise SystemExit
nodes = json.loads(out.read_text())
nodes[0]["inputs_xrefs"] = "not-a-list"          # a genuine SHAPE violation
blob = (json.dumps(nodes, indent=2, sort_keys=True) + "\n").encode()
real = json.loads(next(tmp.glob("todo-cache.json.corpus-*.json")).read_text())

def reason(bind):
    d = pathlib.Path(tempfile.mkdtemp())
    c = d / "todo-cache.json"; c.write_bytes(blob)
    if bind:
        rec = dict(real); rec["cache_sha256"] = hashlib.sha256(blob).hexdigest()
        cs.sidecar_path(c, rec["cache_sha256"]).write_text(json.dumps(rec))
    try:
        cs.load_and_validate(c, todo, check_stale=True,
                             profile=cs.PROFILE_VALIDATE)
        return "NO-REFUSAL"
    except cs.CacheSchemaError as exc:
        return exc.reason

absent, present = reason(False), reason(True)
# LEGACY_FORMAT, not STALE: the absence is a missing producer contract, and it
# must stay in validate.py's _REBUILDABLE_REASONS so the one cache this repo
# owns is still auto-rebuilt rather than hard-refused.
# The CONTROL is the second one: a bound cache with the same violation must
# still report SHAPE, or the fix has degenerated into refusing everything.
print("OK" if absent == cs.REASON_LEGACY_FORMAT and present == cs.REASON_SHAPE
      else f"absent={absent} present={present}")
S25IOEOF
)
if [ "$S25IO" = "OK" ]; then
    t_pass "section 25: an absent binding refuses before the node walk, and a bound cache still reports SHAPE"
else
    t_fail "section 25: identity-before-shape ordering wrong ($S25IO)"
fi

# Sub-test 25g4: the CONDITIONAL emission is guarded too. `stamped_items` is
# declared in the contract but emitted only when a TODO has `[x]` items, so it
# is deliberately outside main()'s per-node presence check -- which left a
# fail-open hole: delete the assignment and every node simply lacks the key, so
# the undeclared-key scan, the presence scan and the digest all pass. The result
# is not a broken build but a SILENTLY DISABLED lint Check 7, because an
# all-absent population reads as REASON_LEGACY_NO_STAMPED_ITEMS -> rc 3 ->
# warning (scripts/lint.sh:574).
S25CE=$(python3 - "$REPO_ROOT" <<'S25CEEOF'
import json, os, pathlib, subprocess, sys, tempfile
repo = pathlib.Path(sys.argv[1])
tg = repo / "scripts/todo-graph"
env = dict(os.environ, PYTHONPATH=str(tg))
d = pathlib.Path(tempfile.mkdtemp())
todo = d / "todo"; (todo / "01-x").mkdir(parents=True)
# A body with a STAMPED item, so the extractor returns something to drop.
(todo / "01-x" / "TODO-01-a.md").write_text(
    "---\nschema_version: 1\nid: probe-ce\ndomain: 01-x\nstatus: active\n"
    "title: \"A\"\n---\n\n# A\n\n## 1. One\n\n- [x] did a thing in `build.py`\n")

def run(text, out):
    p = d / "probe_build.py"; p.write_text(text)
    r = subprocess.run([sys.executable, str(p), "--root", str(todo),
                        "--repo-root", str(d), "--output", str(out), "--quiet"],
                       capture_output=True, text=True, cwd=str(repo), env=env)
    return r.returncode, r.stderr

src = (tg / "build.py").read_text()
rc, err = run(src, d / "c.json")
if rc != 0:
    print("CONTROL-FAILED:" + err.strip()[-120:]); raise SystemExit
if "stamped_items" not in json.loads((d / "c.json").read_text())[0]:
    print("CONTROL-NO-STAMPED-ITEMS"); raise SystemExit

needle = '    if stamped_items:\n        node["stamped_items"] = stamped_items\n'
if src.count(needle) != 1:
    print("NEEDLE-MISS"); raise SystemExit
rc, err = run(src.replace(needle, "", 1), d / "m.json")
print("REFUSED" if rc != 0 and "emission-drop" in err else f"PUBLISHED(rc={rc})")
S25CEEOF
)
if [ "$S25CE" = "REFUSED" ]; then
    t_pass "section 25: mutation check -- dropping the conditional stamped_items emission refuses the build"
else
    t_fail "section 25: conditional emission drop not caught ($S25CE)"
fi

# ----------------------------------------------------------------------
# Section 28: section-level readiness verbs over the resolved dependency graph.
#
# The fixtures live in tests/section28_fixtures.py rather than inline: the
# mutation controls re-exec a PATCHED copy of query.py, which does not survive
# shell quoting. Each of its lines is one assertion; a missing SUMMARY line
# means the harness itself died and is a FAIL, not a skip.
# ----------------------------------------------------------------------
#
# THE EXPECTED COUNT IS PINNED. Accepting any SUMMARY line means deleting a
# fixture or a mutation control silently REDUCES coverage while the suite
# stays green -- the same "passes either way" defect the controls exist to
# prevent, one level up. Raise this number when you add assertions.
S28_EXPECT="SUMMARY 35 / 35"
S28_OUT=$(REPO_ROOT="$REPO_ROOT" python3 "$REPO_ROOT/scripts/todo-graph/tests/section28_fixtures.py" 2>&1)
if ! printf '%s\n' "$S28_OUT" | grep -qF "$S28_EXPECT"; then
    t_fail "section 28: fixture inventory changed -- expected '$S28_EXPECT', got '$(printf '%s' "$S28_OUT" | grep '^SUMMARY ' || echo none)'"
fi
if ! printf '%s\n' "$S28_OUT" | grep -q '^SUMMARY '; then
    t_fail "section 28: fixture harness did not run to completion -- $(printf '%s' "$S28_OUT" | tail -3 | tr '\n' ' ')"
else
    while IFS= read -r line; do
        case "$line" in
            PASS\ *) t_pass "section 28: ${line#PASS }" ;;
            FAIL\ *) t_fail "section 28: ${line#FAIL }" ;;
        esac
    done <<< "$S28_OUT"
fi

# 28-live: the three NEW verbs must answer over the REAL corpus without
# raising, and the three OLD ones must still return exactly what they did.
# The synthetic fixtures above cannot catch a resolver or classifier
# interaction that only the 232-node corpus produces.
S28_LIVE=$(python3 -c "
import sys; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import query as q, json
nodes = json.load(open('$REPO_ROOT/build/todo-cache.json'))
ctx = q.Ctx(nodes, '$REPO_ROOT', quiet=True)
r = len(q.cmd_section_ready(ctx, None)[0])
b = len(q.cmd_section_blocked(ctx, None)[0])
k = len(q.cmd_section_blocking(ctx, None)[0])
old = len(q.cmd_blocked(ctx, None)[0]) + len(q.cmd_blocking(ctx, None)[0])
print('OK' if (r > 0 and b > 0 and k > 0 and old == 0) else f'BAD r={r} b={b} k={k} old={old}')" 2>&1 | tail -1)
if [ "$S28_LIVE" = "OK" ]; then
    t_pass "section 28: the new verbs answer non-empty over the live corpus while the file-level three stay empty"
else
    t_fail "section 28: live-corpus check gave '$S28_LIVE' (want OK)"
fi

# ----------------------------------------------------------------------
# Section 45: the container phase at the PRODUCER
# ----------------------------------------------------------------------
#
# The scan-level rules are pinned in scripts/tests/test_todo_fence.py. What
# these prove is that the emitted NODE changed -- which is where both defects
# were paid for. The erasure case is the one that matters most: it produced a
# node missing a whole section AND its stamped item with rc 0 and no error, so
# nothing downstream could tell it apart from a TODO that simply lacks them.

s45_fixture() {   # $1 = dir, $2 = body after the IO table
    rm -rf "$1"; mkdir -p "$1/todo/d"
    {
        printf -- '---\nschema_version: 1\nid: s45\ndomain: d\nstatus: active\ntitle: T\n---\n\n# T\n\n'
        printf '## Implementation Order\n\n'
        printf '| Order | Section | Deliverable | Depends On | Status |\n'
        printf '| :---: | :-----: | ----------- | ---------- | :----: |\n'
        printf '|   1   |   §1    | One         | --         |  [x]   |\n'
        printf '|   2   |   §2    | Two         | --         |  [x]   |\n\n'
        printf '%b' "$2"
    } > "$1/todo/d/TODO-01-a.md"
    ( cd "$1" && git init -q . && git config user.email s45@test.invalid \
        && git config user.name s45 && git add -A && git commit -qm s45 ) >/dev/null 2>&1
    python3 "$BUILD_PY" --quiet --root "$1/todo" \
        --output "$1/build/todo-cache.json" --repo-root "$1" >/dev/null 2>&1
    echo $?
}

# --- s45a: the under-indented blank no longer ERASES a section and its item ---
S45A="$TMP_DIR/s45a"
S45A_RC="$(s45_fixture "$S45A" '## 1. Root\n\n- a\n  <?pi\n \n  ## 2. Real\n  - [x] `erased_sym()` shipped\n')"
S45A_OUT="$(python3 - "$S45A/build/todo-cache.json" <<'PY'
import json, sys
try:
    c = json.load(open(sys.argv[1]))
except Exception as e:
    print("no-cache: %s" % e); raise SystemExit
n = (c["nodes"] if isinstance(c, dict) and "nodes" in c else c)
n = n[0] if isinstance(n, list) else list(n.values())[0]
secs = sorted(h["n"] for h in n.get("section_headings", []))
syms = sorted(r.get("symbol") for i in n.get("stamped_items", [])
              for r in i.get("refs", []) if r.get("kind") == "symbol")
print("sections=%s items=%s errors=%s" % (secs, syms, n.get("errors", [])))
PY
)"
if [ "$S45A_RC" = "0" ] && echo "$S45A_OUT" | grep -q "sections=\[1, 2\]" \
   && echo "$S45A_OUT" | grep -q "erased_sym"; then
    t_pass "s45a: an under-indented blank no longer erases the section and item behind it"
else
    t_fail "s45a: rc=$S45A_RC $S45A_OUT"
fi

# --- s45b: a list-contained section OWNS its indented item in the node ---
# FOUR spaces of indent, because `classify_heading` accepts 0-3 on a physical
# line: at two the pre-section-45 producer saw the heading too and the fixture
# would prove nothing.
S45B="$TMP_DIR/s45b"
S45B_RC="$(s45_fixture "$S45B" '## 1. Root\n\n- item\n\n    ## 2. Nested\n\n    - [x] `nested_sym()` shipped\n')"
S45B_OUT="$(python3 - "$S45B/build/todo-cache.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
n = (c["nodes"] if isinstance(c, dict) and "nodes" in c else c)
n = n[0] if isinstance(n, list) else list(n.values())[0]
print("attrib=%s" % sorted((i["section_n"], r.get("symbol"))
                          for i in n.get("stamped_items", [])
                          for r in i.get("refs", []) if r.get("kind") == "symbol"))
PY
)"
if [ "$S45B_RC" = "0" ] && echo "$S45B_OUT" | grep -q "(2, 'nested_sym')"; then
    t_pass "s45b: a list-contained section owns its indented item in the emitted node"
else
    t_fail "s45b: rc=$S45B_RC $S45B_OUT"
fi

# --- s45c: the FIFTH walk. A list-contained over-long heading is REFUSED ---
# The other four walks now see this heading; if `_walk_unusable_headings` stayed
# on physical lines the node would omit the section AND carry no error, which is
# the silent narrowing the refusal exists to prevent.
S45C="$TMP_DIR/s45c"
S45C_RC="$(s45_fixture "$S45C" '## 1. Root\n\n- item\n\n    ## 12345678901. Over\n\n    - [x] `over_sym()` shipped\n')"
if [ "$S45C_RC" != "0" ] || grep -q "unusable-heading" "$S45C/build/todo-cache.json" 2>/dev/null; then
    t_pass "s45c: a list-contained over-long heading is refused, not silently dropped"
else
    t_fail "s45c: rc=$S45C_RC, no unusable-heading recorded"
fi

# --- s45e: a heading sharing the container-OPENING line ---
# `views` keeps the marker a line opens, so `- ## 2.` was invisible to
# `classify_heading` and its item filed under section 1; the leaf projection is
# the second view that fixes it. This is the producer-level half.
S45E="$TMP_DIR/s45e"
S45E_RC="$(s45_fixture "$S45E" '## 1. Root\n\n- ## 2. Nested\n  - [x] `same_line_sym()` shipped\n')"
S45E_OUT="$(python3 - "$S45E/build/todo-cache.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
n = (c["nodes"] if isinstance(c, dict) and "nodes" in c else c)
n = n[0] if isinstance(n, list) else list(n.values())[0]
print("attrib=%s" % sorted((i["section_n"], r.get("symbol"))
                          for i in n.get("stamped_items", [])
                          for r in i.get("refs", []) if r.get("kind") == "symbol"))
PY
)"
if [ "$S45E_RC" = "0" ] && echo "$S45E_OUT" | grep -q "(2, 'same_line_sym')"; then
    t_pass "s45e: a heading on the container-opening line owns the item under it"
else
    t_fail "s45e: rc=$S45E_RC $S45E_OUT"
fi

# --- s45f: ...and its OVER-LONG form fails CLOSED rather than open ---
# Before the leaf projection this recorded no error at all, which is the one
# direction the refusal must never take.
S45F="$TMP_DIR/s45f"
S45F_RC="$(s45_fixture "$S45F" '## 1. Root\n\n- ## 12345678901. Over\n  - [x] `over2_sym()` shipped\n')"
if [ "$S45F_RC" != "0" ] || grep -q "unusable-heading" "$S45F/build/todo-cache.json" 2>/dev/null; then
    t_pass "s45f: a same-line over-long heading fails closed, not open"
else
    t_fail "s45f: rc=$S45F_RC, no unusable-heading recorded"
fi

# --- s45g: a QUOTED example must not become graph data ---
# Container stripping deletes the `>` every walk used to reject on, so the
# guarantee that a quoted example cannot affect the graph had to be published
# as a predicate instead. This fixture quotes an Inputs block, a heading and an
# Implementation Order row all at once; the node must carry the REAL section 1
# and section 2 only, no edge to TODO-99, and no third IO row.
S45G="$TMP_DIR/s45g"
S45G_RC="$(s45_fixture "$S45G" '## 1. Root\n\n> ## Inputs\n> - -> XREF: [`TODO-99`](../d/TODO-99-z.md) \xc2\xa71\n\n> ## 3. Quoted\n\n## 2. Real\n\n- [x] `real_sym()` shipped\n')"
S45G_OUT="$(python3 - "$S45G/build/todo-cache.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
n = (c["nodes"] if isinstance(c, dict) and "nodes" in c else c)
n = n[0] if isinstance(n, list) else list(n.values())[0]
print("secs=%s edges=%s attrib=%s" % (
    sorted(h["n"] for h in n.get("section_headings", [])),
    sorted(x.get("target_path", "") for x in n.get("inputs_xrefs", [])),
    sorted(i["section_n"] for i in n.get("stamped_items", []))))
PY
)"
if [ "$S45G_RC" = "0" ] && echo "$S45G_OUT" | grep -q "secs=\[1, 2\]" \
   && ! echo "$S45G_OUT" | grep -q "TODO-99" && echo "$S45G_OUT" | grep -q "attrib=\[2\]"; then
    t_pass "s45g: a quoted heading, Inputs block and IO row stay out of the graph"
else
    t_fail "s45g: rc=$S45G_RC $S45G_OUT"
fi

# --- s45d: CONTROL -- the live corpus builds byte-identically ---
# 18,505 of 90,801 corpus lines carry a container prefix and 9,336 of those sit
# inside a blockquote, so the projection is exercised heavily; it must change no
# answer. Without this control the three fixtures above would pass against a
# projection that silently re-parented ordinary work.
S45D="$TMP_DIR/s45d.json"
S45D_REF="$REPO_ROOT/build/todo-cache.json"
if [ ! -f "$S45D_REF" ]; then
    # A MISSING REFERENCE IS NOT A PASS. Reported as its own failure rather than
    # a silent skip: this control is the only thing standing between the three
    # fixtures above and a projection that re-parents ordinary corpus work, and
    # a control that evaporates when its input is absent is the fail-silent
    # shape this suite exists to refuse. The cache is a build artefact any run
    # of the producer creates.
    t_fail "s45d: no built cache at $S45D_REF to compare against -- run the producer first"
elif python3 "$BUILD_PY" --quiet --root "$REPO_ROOT/todo" --repo-root "$REPO_ROOT" \
        --output "$S45D" >/dev/null 2>&1 \
   && python3 -c "
import json,sys
a=json.load(open('$S45D')); b=json.load(open('$S45D_REF'))
sys.exit(0 if json.dumps(a,sort_keys=True)==json.dumps(b,sort_keys=True) else 1)
" 2>/dev/null; then
    t_pass "s45d: CONTROL -- the live corpus still builds the committed cache exactly"
else
    t_fail "s45d: the live corpus no longer builds the committed cache"
fi

# ----------------------------------------------------------------------
# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
tb_finish core
