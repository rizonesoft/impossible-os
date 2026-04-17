#!/bin/bash
# =============================================================================
# install-hooks.sh -- Configure the repo's git-hook install path and toggle
#                     the opt-in pre-push build/test gate.
#
# Canonical hook path: .githooks/ (set via core.hooksPath). Once configured,
# git runs every executable in .githooks/ that matches a hook name; the
# .git/hooks/ directory is ignored. The repo ships three hooks:
#   - .githooks/pre-commit   always-on: lints staged .c/.h; blocks commit
#                            on lint failure.
#   - .githooks/post-commit  always-on: regenerates COUNT.md, amends commit.
#   - .githooks/pre-push     opt-in: delegates to scripts/hooks/pre-push
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
# pre-push independently of the primary checkout.
SENTINEL="$(git -C "$PROJECT" rev-parse --git-path .impossible-os-prepush 2>/dev/null || echo "$PROJECT/.git/.impossible-os-prepush")"
LEGACY_HOOKS="$(git -C "$PROJECT" rev-parse --git-path hooks 2>/dev/null || echo "$PROJECT/.git/hooks")"
LEGACY_SRC="$SCRIPT_DIR/hooks"

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
    echo "Hooks path     : $configured (expected: .githooks)"
    echo "Pre-push gate  : $pre_push_state"
    echo "Always-on hooks: $(ls "$HOOKS_DIR" 2>/dev/null | tr '\n' ' ')"
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
    # are harmless but confusing -- clean them on --remove.
    [ -d "$LEGACY_HOOKS" ] || return 0
    local removed=0
    for hook in "$LEGACY_SRC"/*; do
        [ -e "$hook" ] || continue
        local name target
        name="$(basename "$hook")"
        target="$LEGACY_HOOKS/$name"
        if [ -L "$target" ]; then
            rm "$target"
            removed=$((removed + 1))
        fi
    done
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
