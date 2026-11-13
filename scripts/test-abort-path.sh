#!/bin/bash
# =============================================================================
# test-abort-path.sh -- prove the user-mode suite's abort path on a live boot
#
# Boots the suite with the opt-in fixture named (UTEST_FILTER=
# test_optin_smoke_abort.exe), which adds a deliberately failing smoke ahead
# of the default plan (the user-mode test framework abort-path proof), and checks what a consumer of the
# artifacts alone would see:
#   - the run fails (non-zero exit);
#   - the fixture is published FAIL by name, and serial says
#     "suite ABORT (smoke failed)";
#   - build/test-results.json: summary.aborted is true and summary.not_run
#     equals every binary of the default plan (all of them were left behind);
#   - build/test-results.xml: the synthetic suite-abort <error> testcase.
#
# Usage: bash scripts/test-abort-path.sh        (exit 0 = the abort path holds)
# =============================================================================
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
FIXTURE="test_optin_smoke_abort.exe"
JSON="$PROJECT/build/test-results.json"
XML="$PROJECT/build/test-results.xml"
LOG="$PROJECT/build/test.log"

fail() { echo "[ABORT-PATH] FAIL: $*" >&2; exit 1; }

# Every binary the default plan runs: manifest names that are not opt-in.
expected=$(awk '!/^[[:space:]]*(#|$)/ && $1 !~ /^test_optin_/ {n++} END {print n+0}' \
    "$PROJECT/tests/usermode.manifest")
[ "$expected" -gt 0 ] || fail "no default binaries in tests/usermode.manifest"

rm -f "$JSON" "$XML"
bash "$SCRIPT_DIR/test.sh" QUIET=1 XML=1 JSON=1 UTEST_FILTER="$FIXTURE" >/dev/null 2>&1
rc=$?
[ "$rc" -ne 0 ] || fail "the run exited 0 with a failing smoke"

grep -aq "$FIXTURE: FAIL" "$LOG" || fail "the fixture was not published FAIL by name"
grep -aq 'suite ABORT (smoke failed)' "$LOG" || fail "serial does not name the smoke abort"

python3 - "$JSON" "$FIXTURE" "$expected" <<'PY' || exit 1
import json, sys
path, fixture, expected = sys.argv[1], sys.argv[2], int(sys.argv[3])
try:
    with open(path) as fh:
        doc = json.load(fh)
except (OSError, ValueError) as e:
    sys.exit(f"[ABORT-PATH] FAIL: {path} unreadable: {e}")
s = doc.get("summary", {})
cases = {c.get("name"): c.get("status") for c in doc.get("testcases", [])}
problems = []
if s.get("aborted") is not True:
    problems.append(f"summary.aborted={s.get('aborted')!r}")
if s.get("not_run") != expected:
    problems.append(f"summary.not_run={s.get('not_run')!r}, expected {expected}")
if cases.get(fixture) != "FAIL":
    problems.append(f"{fixture} status={cases.get(fixture)!r}")
if problems:
    sys.exit("[ABORT-PATH] FAIL: " + "; ".join(problems))
PY

python3 - "$XML" <<'PY' || exit 1
import sys
import xml.etree.ElementTree as ET
try:
    root = ET.parse(sys.argv[1]).getroot()
except (OSError, ET.ParseError) as e:
    sys.exit(f"[ABORT-PATH] FAIL: {sys.argv[1]} unreadable: {e}")
suites = [root] if root.tag == "testsuite" else root.findall("testsuite")
errors = [c for s in suites for c in s.findall("testcase") if c.find("error") is not None]
if not errors:
    sys.exit("[ABORT-PATH] FAIL: no <error> testcase in the JUnit artifact")
PY

echo "[ABORT-PATH] PASS: smoke FAIL aborted the run (rc=$rc), not_run=$expected, JUnit <error> present"
