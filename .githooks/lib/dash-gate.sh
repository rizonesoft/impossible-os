# shellcheck shell=bash
# Sourced by .githooks/pre-commit: the ASCII-dash gate for every lane
# (TODO-08 section 48). Claude's edit hook binds only Claude sessions, so a
# line a commit ADDS with an en or em dash is refused here. Older files are
# not swept; generated files are exempt.
#
# The checker's status is captured with `|| rc=$?`, never read after a bare
# call: the hook runs under `set -e`, which would exit at the call and skip
# the guidance below.
dash_gate() {
    local rc=0
    python3 "$REPO_ROOT/scripts/staged_dash_check.py" || rc=$?
    if [ "$rc" -eq 1 ]; then
        echo -e "\033[31mPre-commit: a staged line adds a Unicode dash (above)\033[0m"
        echo "  Opt-out (last resort): SKIP_DASH_CHECK=1 git commit ..."
        return 1
    elif [ "$rc" -ne 0 ]; then
        echo -e "\033[31mPre-commit: the dash check could not read the staged changes (above); fix that and commit again\033[0m"
        return 1
    fi
}
