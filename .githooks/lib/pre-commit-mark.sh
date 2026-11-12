# shellcheck shell=bash
# .githooks/lib/pre-commit-mark.sh -- prove to prepare-commit-msg that pre-commit ran.
#
# `git commit --no-verify` (or -n) skips pre-commit and commit-msg, and nothing in the
# command line can be trusted to show it: bash can spell -n in endless ways ($'\x2dn',
# a heredoc, a substitution), and a reading of the command is only ever a guess. git
# itself knows. prepare-commit-msg still runs under --no-verify, and both hooks are
# children of the same git process, so pre-commit records that process's id here and
# prepare-commit-msg refuses a `git commit` whose id is not on record.

pre_commit_mark_path() {
    printf '%s/pre-commit.ran' "$(git rev-parse --git-dir)"
}

# Called first thing in pre-commit, before any early exit: an empty commit runs it too.
mark_pre_commit_ran() {
    printf '%s\n' "$PPID" > "$(pre_commit_mark_path)" 2>/dev/null || true
}

# The one internal --no-verify commit: post-commit's own COUNT.md amend. It records its
# own pid; prepare-commit-msg accepts a commit whose git process that pid started.
allow_hook_amend() {
    printf '%s\n' "$$" > "$(git rev-parse --git-dir)/hook-amend.by" 2>/dev/null || true
}
