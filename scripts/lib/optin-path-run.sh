# shellcheck shell=bash
# optin-path-run.sh -- shared by the opt-in fixture proofs (test-abort-path.sh,
# test-identity-path.sh, test-timeout-path.sh). Source it; needs SCRIPT_DIR
# and PROJECT.
#
# optin_path_lock <TAG>     hold the image lock for the whole proof; the nested
#                           test.sh shares the hold, so no other run can boot or
#                           publish artifacts between this run and its checks.
# optin_default_count       binaries the default plan runs (non-opt-in names).
# optin_path_run <TAG> ARG.. run test.sh with ARGs; requires a non-zero exit and
#                           sets OPTIN_RC, OPTIN_RUN_ID, OPTIN_JSON, OPTIN_XML and
#                           OPTIN_SLICE to THIS run's own artifacts.

optin_path_lock() {
    . "$SCRIPT_DIR/lib/boot-conf-lock.sh"
    boot_conf_lock_hold "$PROJECT" 9 || { echo "[$1] FAIL: another run holds the image lock" >&2; exit 1; }
}

optin_default_count() {
    awk '!/^[[:space:]]*(#|$)/ && $1 !~ /^test_optin_/ {n++} END {print n+0}' \
        "$PROJECT/tests/usermode.manifest"
}

optin_path_run() {
    local tag="$1" out run_dir
    shift
    out=$(mktemp)
    bash "$SCRIPT_DIR/test.sh" QUIET=1 XML=1 JSON=1 "$@" >"$out" 2>&1
    OPTIN_RC=$?
    run_dir=$(sed 's/\x1b\[[0-9;]*m//g' "$out" | sed -n 's#^\[TEST\] JUnit XML written: \(.*\)/test-results\.xml .*#\1#p' | tail -1)
    rm -f "$out"
    if [ "$OPTIN_RC" -eq 0 ]; then
        echo "[$tag] FAIL: the run exited 0" >&2; return 1
    fi
    if [ -z "$run_dir" ] || [ ! -d "$run_dir" ]; then
        echo "[$tag] FAIL: the run did not publish a JUnit artifact (no run directory in its output)" >&2; return 1
    fi
    OPTIN_RUN_ID=$(basename "$run_dir")
    OPTIN_JSON="$run_dir/test-results.json"
    OPTIN_XML="$run_dir/test-results.xml"
    OPTIN_SLICE="$run_dir/runslice"
    if [ ! -s "$OPTIN_SLICE" ]; then
        echo "[$tag] FAIL: the run saved no serial slice ($OPTIN_SLICE)" >&2; return 1
    fi
}
