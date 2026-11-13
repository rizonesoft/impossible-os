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
#   - the run's own test-results.json: summary.aborted is true and not_run
#     equals every binary of the default plan (all of them were left behind);
#   - its test-results.xml: the suite-abort <error>, the fixture <failure>, and
#     matching aborted/not_run properties; both carry this run's run_id.
#
# Usage: bash scripts/test-abort-path.sh        (exit 0 = the abort path holds)
# =============================================================================
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
FIXTURE="test_optin_smoke_abort.exe"

fail() { echo "[ABORT-PATH] FAIL: $*" >&2; exit 1; }

# The image lock is held for the whole proof and the artifacts checked are the
# ones THIS run published (scripts/lib/optin-path-run.sh).
. "$SCRIPT_DIR/lib/optin-path-run.sh"
optin_path_lock ABORT-PATH

# Every binary the default plan runs: all of them are left behind.
expected=$(optin_default_count)
[ "$expected" -gt 0 ] || fail "no default binaries in tests/usermode.manifest"

optin_path_run ABORT-PATH "UTEST_FILTER=$FIXTURE" || exit 1

# Serial evidence from the framed slice this run saved in its own record,
# not the shared build/test.log.
grep -aq "$FIXTURE: FAIL" "$OPTIN_SLICE" || fail "the fixture was not published FAIL by name"
grep -aq 'suite ABORT (smoke failed)' "$OPTIN_SLICE" || fail "serial does not name the smoke abort"

python3 "$SCRIPT_DIR/lib/abort_path_check.py" "$OPTIN_JSON" "$OPTIN_XML" "$FIXTURE" "$expected" "$OPTIN_RUN_ID" || exit 1

echo "[ABORT-PATH] PASS: smoke FAIL aborted the run (rc=$OPTIN_RC), not_run=$expected, suite-abort <error> present"
