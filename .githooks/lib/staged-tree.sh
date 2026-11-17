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
# index for the gates after it. The export and the suite share one deadline,
# STAGED_TREE_TIMEOUT seconds (default 1200; the suite takes about 4 minutes).
# However the hook ends -- normally, interrupted, or past the deadline -- every
# process either stage started is stopped, TERM then KILL after
# STAGED_TREE_GRACE seconds, and the tree is removed. A process belongs to a
# stage if it carries STAGED_TREE_RUN in its environment: fixtures that start
# their own session (tmux, start_new_session) leave the process group but keep
# it. Everything here is written to complete under the hooks' `set -euo pipefail`.
_STAGED_TREE_LIB="$(dirname "${BASH_SOURCE[0]}")"
_STAGED_TREE_LEADER=""

# The pids of every process carrying STAGED_TREE_RUN=$1, this shell excluded.
# One grep over every environ file (-z: NUL-separated entries); unreadable ones
# belong to other users and are skipped. Prints nothing and succeeds when none.
#
# STAGED_TREE_RUN lists every enclosing run, `:<tree>:<tree>:`, so a stage whose
# suite starts a nested supervisor (this helper's own tests do) still owns the
# nested stage's processes: overwriting the marker hid them from the outer
# cleanup, and a TERM-resistant inner child outlived it (2026-11-17).
_staged_tree_pids() {
  local pattern
  pattern="^STAGED_TREE_RUN=.*:$(printf '%s' "$1" | sed 's/[][\.*^$/]/\\&/g'):"
  { grep -lz -- "$pattern" /proc/[0-9]*/environ 2>/dev/null || true; } |
    { sed -n 's|^/proc/\([0-9]*\)/environ$|\1|p' || true; } | { grep -vx "$$" || true; }
}

# Stop a stage: TERM to the leader's group and every tagged process, KILL what
# is left after the grace period, then reap the leader. Always succeeds.
_staged_tree_stop() {
  local mark="$1" leader="$2" grace="${STAGED_TREE_GRACE:-10}" end pids
  if [ -n "$leader" ]; then kill -TERM -- "-$leader" 2>/dev/null || true; fi
  pids="$(_staged_tree_pids "$mark")"
  if [ -n "$pids" ]; then kill -TERM $pids 2>/dev/null || true; fi
  end=$((SECONDS + grace))
  while [ -n "$(_staged_tree_pids "$mark")" ] && [ "$SECONDS" -lt "$end" ]; do sleep 0.2; done
  if [ -n "$leader" ]; then kill -KILL -- "-$leader" 2>/dev/null || true; fi
  pids="$(_staged_tree_pids "$mark")"
  if [ -n "$pids" ]; then kill -KILL $pids 2>/dev/null || true; fi
  if [ -n "$leader" ]; then wait "$leader" 2>/dev/null || true; fi
  return 0
}

# _staged_tree_stage MARK END ISOLATE DIR CMD...: run CMD from DIR in its own
# session, tagged with MARK, until it exits or SECONDS reaches END (status 124,
# the stage stopped). ISOLATE=1 drops the git environment for CMD alone.
_staged_tree_stage() {
  local mark="$1" end="$2" isolate="$3" dir="$4" rc=0
  shift 4
  (
    if [ "$isolate" = 1 ]; then
      # shellcheck source=.githooks/lib/isolate-git-env.sh
      . "$_STAGED_TREE_LIB/isolate-git-env.sh"
      isolate_git_env
    fi
    export STAGED_TREE_RUN="${STAGED_TREE_RUN:-:}$mark:"
    cd "$dir" || exit 1
    # LIFECYCLE-WAIVER: a gate stage in its own group so the hook can stop it as
    # one; the hook supervises and reaps it, nothing outlives the commit.
    if command -v setsid >/dev/null 2>&1; then exec setsid "$@"; fi
    exec "$@"
  ) &
  _STAGED_TREE_LEADER=$!
  while kill -0 "$_STAGED_TREE_LEADER" 2>/dev/null; do
    if [ "$SECONDS" -ge "$end" ]; then
      echo "staged-tree: $* ran past the ${STAGED_TREE_TIMEOUT:-1200} s deadline; stopped" >&2
      _staged_tree_stop "$mark" "$_STAGED_TREE_LEADER"
      _STAGED_TREE_LEADER=""
      return 124
    fi
    sleep 0.5
  done
  wait "$_STAGED_TREE_LEADER" || rc=$?
  _STAGED_TREE_LEADER=""
  return "$rc"
}

run_on_staged_tree() {
  local script="$1" gitdir tree rc=0 end
  shift
  end=$((SECONDS + ${STAGED_TREE_TIMEOUT:-1200}))
  gitdir="$(git rev-parse --absolute-git-dir)" || return 1
  tree="$(mktemp -d "${TMPDIR:-/tmp}/staged-tree.XXXXXX")" || return 1
  # shellcheck disable=SC2064  # the tree is fixed now, on purpose
  trap "_staged_tree_stop '$tree' \"\$_STAGED_TREE_LEADER\"; rm -rf '$tree'" EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  # The export keeps the git environment: GIT_INDEX_FILE names the commit's index.
  _staged_tree_stage "$tree" "$end" 0 . git checkout-index -a --prefix="$tree/" || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "staged-tree: could not export the staged tree" >&2
  else
    printf 'gitdir: %s\n' "$gitdir" > "$tree/.git"
    _staged_tree_stage "$tree" "$end" 1 "$tree" bash "$tree/$script" "$@" || rc=$?
  fi
  _staged_tree_stop "$tree" ""
  rm -rf "$tree"
  trap - EXIT INT TERM
  return "$rc"
}
