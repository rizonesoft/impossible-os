#!/usr/bin/env python3
"""Self-test for the boot certification matrix lint -- TODO-28.

Proves the lint is a real gate (rejects bad input with the SPECIFIC diagnostic),
not advisory. Every negative case asserts the targeted error substring in
stderr, not merely exit 1 -- a one-row fixture also trips the coverage gate, so
exit-1-alone would mask whether the intended check fired. Run by
scripts/test-tooling.sh. Exits 0 only if every assertion holds.
"""
import sys
import os
import json
import tempfile
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
LINT = os.path.join(HERE, "lint.py")

try:
    import jsonschema
except ImportError:
    sys.stderr.write("boot-cert test: jsonschema required\n")
    sys.exit(2)

GOOD_MATRIX = os.path.join(HERE, "boot-cert.yml")
RESULT_SCHEMA = os.path.join(HERE, "result.schema.json")

PASS = 0
FAIL = 0


def check(name, cond):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [PASS] {name}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name}")


def lint(*args):
    """Run lint.py; return (returncode, stderr)."""
    r = subprocess.run([sys.executable, LINT, "--quiet", *args],
                       capture_output=True, text=True)
    return r.returncode, r.stderr


def write_tmp(text, suffix):
    fd, path = tempfile.mkstemp(suffix=suffix, dir=HERE)
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        fh.write(text)
    return path


def reject(name, yaml_text, needle, extra_args=()):
    """Assert lint exits 1 AND stderr contains the targeted diagnostic `needle`
    -- so a coverage-gate exit-1 cannot masquerade as the targeted rejection."""
    p = write_tmp(yaml_text, ".yml")
    try:
        rc, err = lint("--matrix", p, *extra_args)
        check(name, rc == 1 and needle in err and "Traceback" not in err)
    finally:
        os.remove(p)


# 1. The shipped matrix passes (schema valid + every TODO has a gate).
rc, _ = lint("--matrix", GOOD_MATRIX)
check("shipped matrix passes lint", rc == 0)

# 2. coverage gap -- a TODO with zero rows.
reject("coverage gap (missing TODOs) rejected", """version: 1
platform_classes: [qemu-whpx]
tiers: [nightly, stable]
rows:
  - id: only-one
    feature: only one
    owner_todo: TODO-01
    gate_kind: boot
    platforms: [qemu-whpx]
    required_for: {}
    automation_level: auto
    source_artifact: x.json
""", "maps to zero certification rows")

# 3. D1: required auto row with empty source_artifact.
reject("required auto row without source_artifact rejected (D1)", """version: 1
platform_classes: [qemu-whpx]
tiers: [nightly, stable]
rows:
  - id: needs-evidence
    feature: required but no evidence
    owner_todo: TODO-01
    gate_kind: boot
    platforms: [qemu-whpx]
    required_for: { stable: [qemu-whpx] }
    automation_level: auto
    source_artifact: null
""", "source_artifact is empty")

# 4. required_for class not in the row's platforms.
reject("required_for class outside row.platforms rejected", """version: 1
platform_classes: [qemu-whpx, qemu-tcg]
tiers: [nightly, stable]
rows:
  - id: bad-subset
    feature: required on a platform it does not list
    owner_todo: TODO-01
    gate_kind: boot
    platforms: [qemu-whpx]
    required_for: { stable: [qemu-tcg] }
    automation_level: auto
    source_artifact: x.json
""", "not in this row's platforms")

# 4b. undeclared row platform (even optional).
reject("undeclared row platform rejected", """version: 1
platform_classes: [qemu-whpx]
tiers: [nightly, stable]
rows:
  - id: bad-row-platform
    feature: claims an unsupported platform
    owner_todo: TODO-01
    gate_kind: boot
    platforms: [qemu-whpx, not-a-real-platform]
    required_for: {}
    automation_level: auto
    source_artifact: x.json
""", "not in top-level platform_classes")

# 4c. required_for tier not in top-level tiers.
reject("required_for tier outside top-level tiers rejected", """version: 1
platform_classes: [qemu-whpx]
tiers: [nightly]
rows:
  - id: bad-tier
    feature: requires an undeclared tier
    owner_todo: TODO-01
    gate_kind: boot
    platforms: [qemu-whpx]
    required_for: { stable: [qemu-whpx] }
    automation_level: auto
    source_artifact: x.json
""", "required_for tier 'stable' not in top-level tiers")

# 4d. literal duplicate mapping key.
reject("duplicate mapping key rejected (strict loader)", """version: 1
platform_classes: [qemu-whpx]
tiers: [nightly, stable]
rows:
  - id: dup-key
    feature: duplicate required_for
    owner_todo: TODO-01
    gate_kind: boot
    platforms: [qemu-whpx]
    required_for: { stable: [qemu-whpx] }
    automation_level: auto
    source_artifact: x.json
    required_for: {}
""", "duplicate mapping key")

# 4e. duplicate inside an inline merge source (recursive scan).
reject("duplicate inside inline merge source rejected", """version: 1
platform_classes: [qemu-whpx]
tiers: [nightly, stable]
rows:
  - id: merge-src-dup
    feature: dup inside merge source
    owner_todo: TODO-01
    gate_kind: boot
    platforms: [qemu-whpx]
    automation_level: auto
    source_artifact: x.json
    <<: {required_for: {stable: [qemu-whpx]}, required_for: {}}
""", "duplicate mapping key")

# 4f. cyclic merge alias -> clean rejection, not RecursionError traceback.
reject("cyclic merge alias rejected without traceback", """version: 1
platform_classes: [qemu-whpx]
tiers: [nightly, stable]
rows:
  - &row
    id: cyclic
    <<: *row
    feature: self-referential merge
    owner_todo: TODO-01
    gate_kind: boot
    platforms: [qemu-whpx]
    required_for: {}
    automation_level: auto
    source_artifact: x.json
""", "recursive merge source")

# 4g. valid << merge + explicit override LOADS (failure, if any, is ONLY the
#     one-row coverage gate -- prove the loader did not reject the merge).
mp = write_tmp("""version: 1
platform_classes: [qemu-whpx, qemu-tcg]
tiers: [nightly, stable]
rows:
  - id: anchored
    feature: merge defaults then override
    owner_todo: TODO-01
    gate_kind: boot
    platforms: [qemu-whpx]
    automation_level: auto
    required_for: {}
    source_artifact: defaults.json
    <<: {source_artifact: overridden.json}
""", ".yml")
try:
    rc, err = lint("--matrix", mp)
    check("merge-key + override loads (no false duplicate)",
          "cannot load" not in err and "duplicate mapping key" not in err
          and "Traceback" not in err)
finally:
    os.remove(mp)

# 5. duplicate JSON key in --results (last-wins masking).
djson = write_tmp(
    '{"row_id":"boot-abi-handoff","status":"pass","build_id":"b","machine_id":"m",'
    '"tier":"nightly","platform_class":"not-a-real-platform",'
    '"platform_class":"qemu-whpx","ts":"2026-06-19T00:00:00Z"}', ".json")
try:
    rc, err = lint("--matrix", GOOD_MATRIX, "--results", djson)
    check("duplicate JSON key in --results rejected", rc == 1 and "duplicate JSON key" in err)
finally:
    os.remove(djson)

# 6. result whose platform_class is outside the row's platforms.
badpc = write_tmp(json.dumps({
    "row_id": "boot-abi-handoff", "status": "pass", "build_id": "b",
    "machine_id": "m", "tier": "nightly", "platform_class": "not-a-real-platform",
    "ts": "2026-06-19T00:00:00Z"}), ".json")
try:
    rc, err = lint("--matrix", GOOD_MATRIX, "--results", badpc)
    check("result out-of-matrix platform_class rejected",
          rc == 1 and "platform_class 'not-a-real-platform' not in matrix" in err)
finally:
    os.remove(badpc)

# 7. deeply nested JSON exits 1 cleanly (no RecursionError traceback).
deep = write_tmp("[" * 6000 + "0" + "]" * 6000, ".json")
try:
    rc, err = lint("--matrix", GOOD_MATRIX, "--result-schema", deep)
    check("deeply nested --result-schema exits 1 without traceback",
          rc == 1 and "Traceback" not in err)
    rc, err = lint("--matrix", GOOD_MATRIX, "--results", deep)
    check("deeply nested --results exits 1 without traceback",
          rc == 1 and "Traceback" not in err)
finally:
    os.remove(deep)

# 8. empty --results array is not a vacuous pass.
ej = write_tmp("[]", ".json")
try:
    rc, err = lint("--matrix", GOOD_MATRIX, "--results", ej)
    check("empty --results array rejected", rc == 1 and "empty result array" in err)
finally:
    os.remove(ej)

# 9. present-but-empty --results flag (zero evidence) is rejected.
rc, err = lint("--matrix", GOOD_MATRIX, "--results")
check("present-but-empty --results flag rejected",
      rc == 1 and "given with no files" in err)

# 10. result.schema.json: a valid result parses; a bad status is rejected.
rschema = json.load(open(RESULT_SCHEMA, encoding="utf-8"))
rv = jsonschema.Draft202012Validator(rschema)
good = {"row_id": "boot-abi-handoff", "status": "pass", "build_id": "b123",
        "machine_id": "ci", "tier": "nightly", "platform_class": "qemu-whpx",
        "ts": "2026-06-19T00:00:00Z", "logs": ["serial.log"]}
check("valid result object parses", not list(rv.iter_errors(good)))
check("invalid result status rejected", bool(list(rv.iter_errors(dict(good, status="boom")))))

print(f"boot-cert self-test: {PASS} passed, {FAIL} failed")
sys.exit(1 if FAIL else 0)
