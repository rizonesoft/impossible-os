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
LOG="$PROJECT/build/test.log"

fail() { echo "[ABORT-PATH] FAIL: $*" >&2; exit 1; }

# Hold the image for the whole proof: the nested test.sh shares this hold
# (BOOT_CONF_LOCK_HELD), so no other run can boot, publish artifacts or write
# build/test.log between this run's boot and the checks below.
. "$SCRIPT_DIR/lib/boot-conf-lock.sh"
boot_conf_lock_hold "$PROJECT" 9 || fail "another run holds the image lock"

# Every binary the default plan runs: manifest names that are not opt-in.
expected=$(awk '!/^[[:space:]]*(#|$)/ && $1 !~ /^test_optin_/ {n++} END {print n+0}' \
    "$PROJECT/tests/usermode.manifest")
[ "$expected" -gt 0 ] || fail "no default binaries in tests/usermode.manifest"

out=$(mktemp)
trap 'rm -f "$out"' EXIT
bash "$SCRIPT_DIR/test.sh" QUIET=1 XML=1 JSON=1 UTEST_FILTER="$FIXTURE" >"$out" 2>&1
rc=$?
[ "$rc" -ne 0 ] || fail "the run exited 0 with a failing smoke"

# The artifacts THIS invocation published, by the run directory it names.
run_dir=$(sed 's/\x1b\[[0-9;]*m//g' "$out" | sed -n 's#^\[TEST\] JUnit XML written: \(.*\)/test-results\.xml .*#\1#p' | tail -1)
[ -n "$run_dir" ] && [ -d "$run_dir" ] || fail "the run did not publish a JUnit artifact (no run directory in its output)"
JSON="$run_dir/test-results.json"
XML="$run_dir/test-results.xml"

grep -aq "$FIXTURE: FAIL" "$LOG" || fail "the fixture was not published FAIL by name"
grep -aq 'suite ABORT (smoke failed)' "$LOG" || fail "serial does not name the smoke abort"

python3 "$SCRIPT_DIR/lib/abort_path_check.py" "$JSON" "$XML" "$FIXTURE" "$expected" "$(basename "$run_dir")" || exit 1

echo "[ABORT-PATH] PASS: smoke FAIL aborted the run (rc=$rc), not_run=$expected, suite-abort <error> present"
