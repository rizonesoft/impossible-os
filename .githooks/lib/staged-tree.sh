# staged-tree.sh -- sourced by the git hooks to run a suite over the commit.
#
# A suite that reads the working tree judges what is on disk, not what the
# commit records: an unstaged edit to a runner test refused a correct commit
# (2026-11-13), and an unstaged repair would let a broken one land. The commit's
# index (GIT_INDEX_FILE, which git sets for a path-limited commit) is exported to
# a scratch tree whose .git names this repository, so the tests that ask git
# about history still find it, and the suite runs from there.
#
# run_on_staged_tree SCRIPT [ARGS...]: runs `bash <tree>/SCRIPT ARGS...` and
# returns its status. It runs in the hook's own process, so a signal to the hook
# alone reaches its traps (a subshell would leave the suite running, 2026-11-17).
# Only the suite's process loses the git environment; the hook keeps the commit's
# index for the gates after it. However the hook ends -- normally, interrupted,
# or past STAGED_TREE_TIMEOUT seconds (default 1200; the suite takes about 4
# minutes) -- every process the suite started is stopped, TERM then KILL after
# STAGED_TREE_GRACE seconds, and the tree is removed. A process is the suite's if
# it carries STAGED_TREE_RUN in its environment: fixtures that start their own
# session (tmux, start_new_session) leave the suite's process group but keep it.
_STAGED_TREE_LIB="$(dirname "${BASH_SOURCE[0]}")"

# The pids of every process carrying STAGED_TREE_RUN=$1, this shell excluded.
_staged_tree_pids() {
  # One grep over every environ file (-z: NUL-separated entries); unreadable
  # ones belong to other users and are skipped.
  grep -lzxF "STAGED_TREE_RUN=$1" /proc/[0-9]*/environ 2>/dev/null |
    sed -n 's|^/proc/\([0-9]*\)/environ$|\1|p' | grep -vx "$$"
}

# Stop the suite: TERM to its group and every tagged process, KILL whatever is
# still there after the grace period, then reap the leader.
_staged_tree_stop() {
  local mark="$1" leader="$2" grace="${STAGED_TREE_GRACE:-10}" end pids
  [ -n "$leader" ] && kill -TERM -- "-$leader" 2>/dev/null
  pids="$(_staged_tree_pids "$mark")"
  [ -n "$pids" ] && kill -TERM $pids 2>/dev/null
  end=$((SECONDS + grace))
  while [ -n "$(_staged_tree_pids "$mark")" ] && [ "$SECONDS" -lt "$end" ]; do sleep 0.2; done
  [ -n "$leader" ] && kill -KILL -- "-$leader" 2>/dev/null
  pids="$(_staged_tree_pids "$mark")"
  [ -n "$pids" ] && kill -KILL $pids 2>/dev/null
  [ -n "$leader" ] && wait "$leader" 2>/dev/null
  return 0
}

run_on_staged_tree() {
  local script="$1" gitdir tree mark rc end limit="${STAGED_TREE_TIMEOUT:-1200}"
  shift
  gitdir="$(git rev-parse --absolute-git-dir)" || return 1
  tree="$(mktemp -d "${TMPDIR:-/tmp}/staged-tree.XXXXXX")" || return 1
  mark="$tree"
  _STAGED_TREE_LEADER=""
  # shellcheck disable=SC2064  # the values are fixed now, on purpose
  trap "_staged_tree_stop '$mark' \"\$_STAGED_TREE_LEADER\"; rm -rf '$tree'" EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  if ! git checkout-index -a --prefix="$tree/"; then
    echo "staged-tree: could not export the staged tree" >&2
    return 1
  fi
  printf 'gitdir: %s\n' "$gitdir" > "$tree/.git"
  (
    # shellcheck source=.githooks/lib/isolate-git-env.sh
    . "$_STAGED_TREE_LIB/isolate-git-env.sh"
    isolate_git_env
    export STAGED_TREE_RUN="$mark"
    cd "$tree" || exit 1
    if command -v setsid >/dev/null 2>&1; then
      exec setsid bash "$tree/$script" "$@"
    fi
    exec bash "$tree/$script" "$@"
  ) &
  _STAGED_TREE_LEADER=$!
  end=$((SECONDS + limit))
  while kill -0 "$_STAGED_TREE_LEADER" 2>/dev/null; do
    if [ "$SECONDS" -ge "$end" ]; then
      echo "staged-tree: $script ran past ${limit} s; stopped" >&2
      _staged_tree_stop "$mark" "$_STAGED_TREE_LEADER"
      _STAGED_TREE_LEADER=""
      rc=124
      break
    fi
    sleep 0.5
  done
  if [ -n "$_STAGED_TREE_LEADER" ]; then
    wait "$_STAGED_TREE_LEADER"
    rc=$?
    _STAGED_TREE_LEADER=""
  fi
  _staged_tree_stop "$mark" ""
  rm -rf "$tree"
  trap - EXIT INT TERM
  return "$rc"
}
