#!/bin/bash
# =============================================================================
# test-timeout-path.sh -- prove the launcher's timeout reap live
#
# Boots the suite with test_optin_hang.exe named and a short per-binary limit
# (UTEST_TIMEOUT_MS, default 2000). The fixture reaches ring 3 and never
# exits. Checks, from this run's own artifacts: the run fails; the hang has
# exactly one record, a FAIL "timeout after <ms>ms", in JSON and JUnit, with
# its ring-3 marker captured; serial says it was reaped with the status named
# TASK_EXIT_UTEST_TIMEOUT, not SIGKILL's -9; the counts reconcile.
#
# What this proves is the FORCED reap (task_terminate_remote), the only branch
# a timeout takes today: signal_check() has no caller, so SIGKILL is never
# delivered cooperatively. The cooperative branch's -9 conversion is proven
# once signal delivery is wired (the process-model signal_check item).
#
# Usage: bash scripts/test-timeout-path.sh    (exit 0 = the reap holds)
# =============================================================================
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
MS="${UTEST_TIMEOUT_MS:-2000}"

. "$SCRIPT_DIR/lib/optin-path-run.sh"
optin_path_lock TIMEOUT-PATH

expected=$(( $(optin_default_count) + 1 ))
optin_path_run TIMEOUT-PATH "UTEST_FILTER=test_optin_hang.exe" "UTEST_TIMEOUT_MS=$MS" || exit 1
python3 "$SCRIPT_DIR/lib/optin_path_check.py" timeout "$OPTIN_JSON" "$OPTIN_XML" \
    "$OPTIN_SLICE" "$OPTIN_RUN_ID" "$expected" "$MS" || exit 1
echo "[TIMEOUT-PATH] PASS: the hang reached ring 3 and was reaped as TASK_EXIT_UTEST_TIMEOUT after ${MS}ms (rc=$OPTIN_RC)"
