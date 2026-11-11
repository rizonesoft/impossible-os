# isolate-git-env.sh -- sourced by the git hooks before they launch a test suite.
#
# Inside a hook git exports GIT_DIR, and from a linked worktree it is an ABSOLUTE
# path into the real repository (from the main checkout it is not exported at all).
# Every throwaway-repo fixture in the test pack inherits os.environ, so its `git init`
# re-initialised the real repository as bare and its commits landed on the worktree's
# HEAD, under a fixture identity (2026-11-09). A suite gets the environment of a
# plain shell, and finds its own repository from its working directory.
isolate_git_env() {
  unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR GIT_PREFIX \
        GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES
}

# The lint step keeps the hook's index (a path-limited commit stages into a temporary
# one), so it cannot be isolated. From a linked worktree git exports GIT_DIR with no
# GIT_WORK_TREE, and git then takes the CURRENT directory as the work tree root: a call
# made from todo/ scoped `.` to the whole repository, and the TODO graph build refused
# every commit there as "the corpus git history moved" (2026-11-11). A hook starts at
# the work tree root, so pin it before anything changes directory.
anchor_git_env() {
  if [ -n "${GIT_DIR:-}" ] && [ -z "${GIT_WORK_TREE:-}" ]; then
    GIT_WORK_TREE="$(pwd -P)"
    export GIT_WORK_TREE
  fi
}
