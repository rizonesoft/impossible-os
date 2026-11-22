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
    python3 "$REPO_ROOT/scripts/staged_dash_check.py" "$@" || rc=$?
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
# that resolves a conflict commits without running pre-commit. $1 is the URL
# git pushes to (pre-push's second argument): the checker asks that
# repository what it holds. stdin is pre-push's own, one line per ref.
dash_push_gate() {
    local rc=0
    python3 "$REPO_ROOT/scripts/staged_dash_check.py" --push "${1:-}" || rc=$?
    if [ "$rc" -eq 1 ]; then
        echo -e "\033[31m[pre-push] an outgoing commit adds a Unicode dash (above); amend it before pushing\033[0m"
        echo "  Opt-out (last resort): SKIP_DASH_CHECK=1 git push ..."
        return 1
    elif [ "$rc" -ne 0 ]; then
        echo -e "\033[31m[pre-push] the dash check could not read the outgoing commits (above); fix that and push again\033[0m"
        return 1
    fi
}

# Whether a merge is being committed, bounded: the path lookup and the read of
# MERGE_HEAD each run under `timeout` (DASH_GATE_PROBE_SECS, 10 by default), so
# storage that stalls either refuses the commit rather than holding it. The
# read tells confirmed absence (ENOENT, ENOTDIR) from any other stat or read
# error, opens without blocking (a FIFO in its place never holds it) and takes
# only a regular file. Prints the other parents, one a line.
# 0: a merge; 1: not a merge; 2: unknown (refuse).
_DASH_MERGE_READ='
import errno, os, stat, sys
try:
    st = os.lstat(sys.argv[1])
except OSError as exc:
    sys.exit(1 if exc.errno in (errno.ENOENT, errno.ENOTDIR) else 2)
if not stat.S_ISREG(st.st_mode):
    sys.exit(2)
try:
    fd = os.open(sys.argv[1], os.O_RDONLY | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            sys.exit(2)
        data = os.read(fd, 1 << 16)
    finally:
        os.close(fd)
    parents = data.decode("ascii").split()
except (OSError, UnicodeDecodeError):
    sys.exit(2)
if not parents:
    sys.exit(2)
print("\n".join(parents))
'
dash_merge_head() {
    local secs="${DASH_GATE_PROBE_SECS:-10}" path rc=0
    path="$(timeout -k 1 "$secs" git rev-parse --git-path MERGE_HEAD)" || return 2
    timeout -k 1 "$secs" python3 -c "$_DASH_MERGE_READ" "$path" || rc=$?
    case "$rc" in
        0|1) return "$rc" ;;
        *) return 2 ;;
    esac
}

# pre-commit's call: a merge being committed is judged once, by commit-msg,
# which runs for a merge whether git or the user commits it; every other
# commit is judged here, before the message is written.
dash_commit_gate() {
    local rc=0
    dash_merge_head >/dev/null || rc=$?
    [ "$rc" -eq 0 ] && return 0
    if [ "$rc" -eq 2 ]; then
        echo -e "\033[31mPre-commit: whether this is a merge could not be read (storage stalled or failed); commit again\033[0m"
        return 1
    fi
    dash_gate --no-merge
}
