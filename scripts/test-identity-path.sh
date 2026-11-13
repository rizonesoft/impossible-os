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
# Boots RUNS times (default 4, the smoke matrix's leg count) on ONE image:
# the first boot builds it, every later boot is KEEP_DISK=1, so it boots the
# disk the previous boot rewrote. The swap must keep every boot a mismatch; a
# fixed replacement payload would pass boot 1 and run the victim on boot 2.
#
# Usage: bash scripts/test-identity-path.sh   (exit 0 = the branch holds)
# =============================================================================
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
FILTER="test_optin_swap.exe:test_optin_victim.exe"
RUNS="${RUNS:-4}"
if ! [[ "$RUNS" =~ ^[1-9][0-9]*$ ]]; then
    echo "[IDENTITY-PATH] FAIL: RUNS must be a positive integer (got '$RUNS')" >&2
    exit 2
fi

. "$SCRIPT_DIR/lib/optin-path-run.sh"
optin_path_lock IDENTITY-PATH

expected=$(( $(optin_default_count) + 2 ))
checked=0
for ((i = 1; i <= RUNS; i++)); do
    keep=()
    [ "$i" -gt 1 ] && keep=(KEEP_DISK=1)
    optin_path_run IDENTITY-PATH "UTEST_FILTER=$FILTER" "${keep[@]}" || exit 1
    python3 "$SCRIPT_DIR/lib/optin_path_check.py" identity "$OPTIN_JSON" "$OPTIN_XML" \
        "$OPTIN_SLICE" "$OPTIN_RUN_ID" "$expected" || { echo "[IDENTITY-PATH] FAIL on boot $i" >&2; exit 1; }
    checked=$((checked + 1))
    echo "[IDENTITY-PATH] boot $i: victim refused as planned binary replaced, never executed (rc=$OPTIN_RC)"
done
[ "$checked" -eq "$RUNS" ] || { echo "[IDENTITY-PATH] FAIL: checked $checked of $RUNS boots" >&2; exit 1; }
echo "[IDENTITY-PATH] PASS: $RUNS boots of one image, every one a refused mismatch"
