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
        echo "  This check compares with HEAD (and MERGE_HEAD): git does not tell a hook that it is amending."
        echo "  If this is \`git commit --amend\` putting back a line the amended commit removed, its real parent"
        echo "  has it and pre-push judges against that parent: SKIP_DASH_CHECK=1 git commit --amend ..."
        echo "  Otherwise, opt-out (last resort): SKIP_DASH_CHECK=1 git commit ..."
        return 1
    elif [ "$rc" -ne 0 ]; then
        echo -e "\033[31mPre-commit: the dash check could not read the staged changes (above); fix that and commit again\033[0m"
        return 1
    fi
}

# The same rule over each outgoing commit, from .githooks/pre-push: a rebase
# that resolves a conflict commits without running pre-commit. $1 is the
# remote's old OID (all zeros for a new ref), $2 the local OID being pushed,
# $3 the remote's name (a new ref is judged against what that remote holds).
dash_push_gate() {
    local rc=0
    python3 "$REPO_ROOT/scripts/staged_dash_check.py" --range "$1" "$2" "${3:-}" || rc=$?
    if [ "$rc" -eq 1 ]; then
        echo -e "\033[31m[pre-push] an outgoing commit adds a Unicode dash (above); amend it before pushing\033[0m"
        echo "  Opt-out (last resort): SKIP_DASH_CHECK=1 git push ..."
        return 1
    elif [ "$rc" -ne 0 ]; then
        echo -e "\033[31m[pre-push] the dash check could not read the outgoing commits (above); fix that and push again\033[0m"
        return 1
    fi
}
