# staged-tree.sh -- sourced by the git hooks to run a suite over the commit.
#
# A suite that reads the working tree judges what is on disk, not what the
# commit records: an unstaged edit to a runner test refused a correct commit
# (2026-11-13), and an unstaged repair would let a broken one land. The commit's
# index (GIT_INDEX_FILE, which git sets for a path-limited commit) is exported to
# a scratch tree whose .git names this repository, so the tests that ask git
# about history still find it, and the suite runs from there.
#
# run_on_staged_tree SCRIPT [ARGS...]: runs `bash <tree>/SCRIPT ARGS...` with the
# git environment isolated and returns its status. The tree is removed however
# the hook ends, and an interrupted hook (Ctrl-C, SIGTERM) stops the suite's
# whole process tree first: the suite runs in its own session, so a signal to
# the hook alone would otherwise leave it running over a deleted tree.
_STAGED_TREE_LIB="$(dirname "${BASH_SOURCE[0]}")"

run_on_staged_tree() (
  script="$1"
  shift
  gitdir="$(git rev-parse --absolute-git-dir)" || exit 1
  tree="$(mktemp -d "${TMPDIR:-/tmp}/staged-tree.XXXXXX")" || exit 1
  suite=""
  stop_suite() {
    if [ -n "$suite" ]; then
      kill -TERM -- "-$suite" 2>/dev/null || kill -TERM "$suite" 2>/dev/null
      wait "$suite" 2>/dev/null
    fi
  }
  trap 'rm -rf "$tree"' EXIT
  trap 'stop_suite; exit 130' INT
  trap 'stop_suite; exit 143' TERM
  if ! git checkout-index -a --prefix="$tree/"; then
    echo "staged-tree: could not export the staged tree" >&2
    exit 1
  fi
  printf 'gitdir: %s\n' "$gitdir" > "$tree/.git"
  # shellcheck source=.githooks/lib/isolate-git-env.sh
  . "$_STAGED_TREE_LIB/isolate-git-env.sh"
  isolate_git_env
  cd "$tree" || exit 1
  if command -v setsid >/dev/null 2>&1; then
    setsid bash "$tree/$script" "$@" &
  else
    bash "$tree/$script" "$@" &
  fi
  suite=$!
  wait "$suite"
  rc=$?
  suite=""
  exit "$rc"
)
