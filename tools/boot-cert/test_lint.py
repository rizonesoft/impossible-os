#!/usr/bin/env python3
"""Self-test for the boot certification matrix lint -- TODO-28.

Proves the lint is a real gate (rejects bad input), not advisory. Run by
scripts/test-tooling.sh. Exits 0 only if every assertion holds.

Covers the TODO Unit Tests cases:
  - test_boot_cert_schema_valid          -> the shipped matrix passes
  - test_boot_cert_every_todo_has_gate   -> coverage gap is rejected
  - test_boot_result_json_parse          -> result objects validate / reject
plus the D1 evidence-binding and required_for-subset cross-field rejections.
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
SCHEMA = os.path.join(HERE, "boot-cert.schema.json")
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


def run_lint(matrix_path):
    r = subprocess.run([sys.executable, LINT, "--matrix", matrix_path, "--quiet"],
                       capture_output=True, text=True)
    return r.returncode


def write_tmp(text, suffix):
    fd, path = tempfile.mkstemp(suffix=suffix, dir=HERE)
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        fh.write(text)
    return path


# 1. The shipped matrix passes (schema valid + every TODO has a gate).
check("shipped matrix passes lint", run_lint(GOOD_MATRIX) == 0)

# 2. A matrix missing the boot-cert-matrix row's owner (no TODO-28 row) fails
#    the every-TODO-has-a-row coverage check. Build a 1-row matrix.
ONE_ROW = """version: 1
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
"""
p = write_tmp(ONE_ROW, ".yml")
try:
    check("coverage gap (missing TODOs) rejected", run_lint(p) == 1)
finally:
    os.remove(p)

# 3. D1: a required auto row with empty source_artifact is rejected.
NO_EVIDENCE = """version: 1
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
"""
p = write_tmp(NO_EVIDENCE, ".yml")
try:
    check("required auto row without source_artifact rejected (D1)", run_lint(p) == 1)
finally:
    os.remove(p)

# 4. required_for class not in the row's platforms is rejected.
BAD_SUBSET = """version: 1
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
"""
p = write_tmp(BAD_SUBSET, ".yml")
try:
    check("required_for class outside row.platforms rejected", run_lint(p) == 1)
finally:
    os.remove(p)

# 4b. A row that lists an undeclared platform class is rejected (even optional).
BAD_ROW_PLATFORM = """version: 1
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
"""
p = write_tmp(BAD_ROW_PLATFORM, ".yml")
try:
    check("undeclared row platform rejected", run_lint(p) == 1)
finally:
    os.remove(p)

# 4c. A result whose platform_class is outside the row's platforms is rejected.
def run_lint_results(matrix_path, result_path):
    r = subprocess.run([sys.executable, LINT, "--matrix", matrix_path,
                        "--results", result_path, "--quiet"],
                       capture_output=True, text=True)
    return r.returncode

bad_pc = {
    "row_id": "boot-abi-handoff", "status": "pass", "build_id": "b1",
    "machine_id": "m1", "tier": "nightly", "platform_class": "not-a-real-platform",
    "ts": "2026-06-19T00:00:00Z",
}
rp = write_tmp(json.dumps(bad_pc), ".json")
try:
    check("result with out-of-row platform_class rejected", run_lint_results(GOOD_MATRIX, rp) == 1)
finally:
    os.remove(rp)

# 5. result.schema.json: a well-formed result validates; a bad one is rejected.
rschema = json.load(open(RESULT_SCHEMA, encoding="utf-8"))
rv = jsonschema.Draft202012Validator(rschema)
good_result = {
    "row_id": "boot-abi-handoff", "status": "pass", "build_id": "b123",
    "machine_id": "ci-qemu", "tier": "nightly", "platform_class": "qemu-whpx",
    "ts": "2026-06-19T00:00:00Z", "logs": ["serial.log"],
}
bad_result = dict(good_result, status="exploded")  # not an enum value
check("valid result object parses", not list(rv.iter_errors(good_result)))
check("invalid result status rejected", bool(list(rv.iter_errors(bad_result))))

print(f"boot-cert self-test: {PASS} passed, {FAIL} failed")
sys.exit(1 if FAIL else 0)
