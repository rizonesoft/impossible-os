#!/bin/bash
# =============================================================================
# test-identity-path.sh -- prove the loader's identity-mismatch branch live
#
# Boots the suite with the opt-in identity pair named
# (UTEST_FILTER=test_optin_swap.exe:test_optin_victim.exe). The plan freezes
# the victim's identity, then test_optin_swap.exe rewrites the victim in place
# (it flips the last byte, so every boot is a genuine mismatch), and the loader
# must refuse the victim rather than run it. Checks, from this run's own
# artifacts: the run fails; the victim has exactly one record, an ERROR named
# "planned binary replaced", in JSON and JUnit; the victim's marker never
# reached serial; the run was not aborted and its counts reconcile.
#
# Boots RUNS times (default 2) against the same image: the swap must keep
# every boot a mismatch, never a match on the second boot.
#
# Usage: bash scripts/test-identity-path.sh   (exit 0 = the branch holds)
# =============================================================================
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
FILTER="test_optin_swap.exe:test_optin_victim.exe"
RUNS="${RUNS:-2}"

. "$SCRIPT_DIR/lib/optin-path-run.sh"
optin_path_lock IDENTITY-PATH

expected=$(( $(optin_default_count) + 2 ))
for i in $(seq 1 "$RUNS"); do
    optin_path_run IDENTITY-PATH "UTEST_FILTER=$FILTER" || exit 1
    python3 "$SCRIPT_DIR/lib/optin_path_check.py" identity "$OPTIN_JSON" "$OPTIN_XML" \
        "$OPTIN_SLICE" "$OPTIN_RUN_ID" "$expected" || { echo "[IDENTITY-PATH] FAIL on boot $i" >&2; exit 1; }
    echo "[IDENTITY-PATH] boot $i: victim refused as planned binary replaced, never executed (rc=$OPTIN_RC)"
done
echo "[IDENTITY-PATH] PASS: $RUNS boots, every one a refused mismatch"
