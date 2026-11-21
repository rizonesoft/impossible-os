#!/bin/bash
# =============================================================================
# install-hooks.sh -- Configure the repo's git-hook install path and toggle
#                     the opt-in pre-push build/test gate.
#
# Canonical hook path: .githooks/ (set via core.hooksPath). Once configured,
# git runs every executable in .githooks/ that matches a hook name; the
# .git/hooks/ directory is ignored. The repo ships five hooks:
#   - .githooks/pre-commit   always-on: lints staged .c/.h; blocks commit
#                            on lint failure or a file that gains a dash.
#   - .githooks/commit-msg   always-on: the dash check for a merge that
#                            `git merge` commits without pre-commit.
#   - .githooks/prepare-commit-msg  always-on: refuses a git commit that
#                            skipped pre-commit (--no-verify in any spelling).
#   - .githooks/post-commit  always-on: regenerates COUNT.md (core code +
#                            supporting text counts), amends commit.
#   - .githooks/pre-push     always-on dash check over outgoing commits;
#                            opt-in: delegates to scripts/hooks/pre-push
#                            (full build + test.sh) when
#                            .git/.impossible-os-prepush sentinel exists;
#                            no-ops otherwise.
#
# Usage:
#   bash scripts/install-hooks.sh                # set core.hooksPath=.githooks
#                                                # (idempotent; does NOT enable
#                                                # pre-push).
#   bash scripts/install-hooks.sh --with-pre-push
#                                                # above + enable pre-push
#                                                # (creates sentinel).
#   bash scripts/install-hooks.sh --enable-pre-push
#                                                # just create the sentinel.
#   bash scripts/install-hooks.sh --disable-pre-push
#                                                # just remove the sentinel.
#   bash scripts/install-hooks.sh --remove       # unset core.hooksPath, drop
#                                                # sentinel, prune any legacy
#                                                # symlinks in .git/hooks/.
#   bash scripts/install-hooks.sh --status       # print current state.
#   bash scripts/install-hooks.sh --help         # show this help.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOOKS_DIR="$PROJECT/.githooks"
# Worktree-safe sentinel path: rev-parse --git-path resolves .git whether it
# is a directory (main checkout) or a file (linked worktree). The sentinel
# lives per-worktree so contributors using `git worktree add` can opt into
# pre-push independently of the primary checkout. --path-format=absolute:
# the plain form is relative to the CALLER's directory in the primary
# checkout, so running this from docs/ wrote docs/.git/.impossible-os-prepush.
SENTINEL="$(git -C "$PROJECT" rev-parse --path-format=absolute --git-path .impossible-os-prepush 2>/dev/null || echo "$PROJECT/.git/.impossible-os-prepush")"
# The legacy hooks directory is git's own, in the common dir: --git-path hooks
# follows core.hooksPath and would name .githooks while it is set, which is
# exactly when --remove reads it.
LEGACY_HOOKS="$(git -C "$PROJECT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$PROJECT/.git")/hooks"

usage() {
    sed -n '2,/^# =====/ { s/^# \{0,1\}//; p }' "$0" | sed '$d'
    exit "${1:-0}"
}

status() {
    local configured pre_push_state
    configured="$(git -C "$PROJECT" config --get core.hooksPath 2>/dev/null || echo '<unset>')"
    if [ -f "$SENTINEL" ]; then
        pre_push_state="ENABLED (sentinel: $SENTINEL)"
    else
        pre_push_state="disabled (no sentinel)"
    fi
    # Every hook in .githooks/ runs on every commit or push; pre-push also
    # carries an opt-in build/test step behind the sentinel.
    local always_on
    always_on="$(ls "$HOOKS_DIR" 2>/dev/null | grep -vxF 'lib' | tr '\n' ' ')"
    echo "Hooks path     : $configured (expected: .githooks)"
    echo "Always-on hooks: $always_on"
    echo "Pre-push build/test step: $pre_push_state"
    echo
    echo "Pre-push always runs: the dash check over outgoing commits, the identity check,"
    echo "  the branch policy and the conditional suites (see .githooks/pre-push)."
    echo "Pre-push build/test step runs (when enabled):"
    echo "  1. bash scripts/todo-graph/build-and-validate.sh --keep-cache (todo-graph CI gate)"
    echo "  2. bash scripts/build.sh (kernel + userland build)"
    echo "  3. bash scripts/test.sh (TCG unit tests)"
}

set_hooks_path() {
    if [ ! -d "$HOOKS_DIR" ]; then
        echo "ERROR: $HOOKS_DIR not found -- are you in the repo root?"
        exit 1
    fi
    local current
    current="$(git -C "$PROJECT" config --get core.hooksPath 2>/dev/null || true)"
    if [ "$current" != ".githooks" ]; then
        git -C "$PROJECT" config core.hooksPath .githooks
        echo "Configured: core.hooksPath = .githooks"
    else
        echo "Already configured: core.hooksPath = .githooks"
    fi
}

enable_pre_push() {
    mkdir -p "$(dirname "$SENTINEL")"
    if [ -f "$SENTINEL" ]; then
        echo "Pre-push already enabled (sentinel exists)."
    else
        : > "$SENTINEL"
        echo "Pre-push enabled (sentinel: $SENTINEL)."
        echo "  Next 'git push' will run: bash scripts/build.sh && bash scripts/test.sh"
    fi
}

disable_pre_push() {
    if [ -f "$SENTINEL" ]; then
        rm -f "$SENTINEL"
        echo "Pre-push disabled (sentinel removed)."
    else
        echo "Pre-push already disabled (no sentinel)."
    fi
}

prune_legacy_symlinks() {
    # Early versions of this script symlinked hooks into .git/hooks/. Since
    # core.hooksPath=.githooks makes .git/hooks/ inert, leftover symlinks
    # are harmless but confusing -- clean them on --remove. The legacy
    # installer (399ee35b6) linked only pre-push, by absolute path, to
    # scripts/hooks/pre-push in whichever checkout ran it, so a link is ours
    # only when it names that file in a worktree of this repository: a link
    # to anything else is the operator's, and unsetting core.hooksPath gives
    # it back to them.
    [ -d "$LEGACY_HOOKS" ] || return 0
    local removed=0 target wt link
    target="$LEGACY_HOOKS/pre-push"
    if [ -L "$target" ]; then
        link="$(readlink -f "$target")"
        while IFS= read -r wt; do
            if [ "$link" = "$(readlink -f "${wt#worktree }/scripts/hooks/pre-push")" ]; then
                rm "$target"
                removed=1
                break
            fi
        done < <(git -C "$PROJECT" worktree list --porcelain 2>/dev/null | grep '^worktree ')
    fi
    [ "$removed" -gt 0 ] && echo "Pruned $removed legacy symlink(s) from .git/hooks/"
    return 0
}

remove_all() {
    local current
    current="$(git -C "$PROJECT" config --get core.hooksPath 2>/dev/null || true)"
    if [ -n "$current" ]; then
        git -C "$PROJECT" config --unset core.hooksPath || true
        echo "Unset: core.hooksPath (was: $current)"
    else
        echo "core.hooksPath already unset."
    fi
    disable_pre_push
    prune_legacy_symlinks
    echo ""
    echo "Git hooks removed. Always-on hooks in .githooks/ are inert until"
    echo "'bash scripts/install-hooks.sh' reconfigures core.hooksPath."
}

case "${1:-}" in
    --help|-h)
        usage 0
        ;;
    --status)
        status
        ;;
    --remove)
        remove_all
        ;;
    --enable-pre-push)
        # Gate on core.hooksPath -- without it, the sentinel is inert since
        # git never invokes .githooks/pre-push. Bootstrap first, then toggle.
        set_hooks_path
        enable_pre_push
        ;;
    --disable-pre-push)
        disable_pre_push
        ;;
    --with-pre-push)
        set_hooks_path
        enable_pre_push
        echo ""
        status
        ;;
    "")
        set_hooks_path
        echo ""
        status
        ;;
    *)
        echo "Unknown option: $1"
        usage 1
        ;;
esac
