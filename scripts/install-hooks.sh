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
# core.hooksPath as git reads it, exactly. --null keeps every byte, a trailing
# newline included, which a plain command substitution would strip; the NUL
# is turned into \001 so the substitution has nothing to strip. It is the
# effective value (the last one git reads). A value git cannot read compares
# unequal, so every caller fails closed.
hooks_path_raw() {
    git -C "$PROJECT" config --null --get core.hooksPath | tr '\0' '\001'
}
# The effective value and the scope git takes it from, in one read: sets
# HP_SCOPE (empty when the key is absent) and HP_VALUE. Exit 1 from git is
# "not set"; anything else is an error and returns non-zero.
hooks_path_scoped() {
    local scoped rc=0
    HP_SCOPE="" HP_VALUE=""
    scoped="$(set -o pipefail; git -C "$PROJECT" config --show-scope --null --get core.hooksPath | tr '\0' '\001')" || rc=$?
    if [ "$rc" -gt 1 ]; then
        echo "core.hooksPath could not be read (git config exit $rc)" >&2
        return 1
    fi
    [ -n "$scoped" ] || return 0
    HP_SCOPE="${scoped%%$'\001'*}"
    HP_VALUE="${scoped#*$'\001'}"
    HP_VALUE="${HP_VALUE%$'\001'}"
}
hooks_path_is_ours() {
    [ "$(hooks_path_raw)" = $'.githooks\001' ]
}
# The value for a person to read: the exact one, quoted when it holds bytes
# a plain print would hide.
hooks_path_shown() {
    local raw
    raw="$(hooks_path_raw)"
    if [ -z "$raw" ]; then
        echo '<unset>'                                     # no value at all: the key is absent
        return
    fi
    raw="${raw%$'\001'}"
    if [ -z "$raw" ]; then
        echo "''"                                         # set, to the empty string: git reads hooks from ./
    elif [[ "$raw" =~ ^[[:graph:]]+$ ]]; then
        printf '%s\n' "$raw"
    else
        printf '%q\n' "$raw"
    fi
}

usage() {
    sed -n '2,/^# =====/ { s/^# \{0,1\}//; p }' "$0" | sed '$d'
    exit "${1:-0}"
}

status() {
    local configured pre_push_state
    configured="$(hooks_path_shown)"
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
    # The setting is written where git reads it from, decided before anything is written: a worktree-scoped
    # value (extensions.worktreeConfig) outranks the repository's, so writing the repository's alone would
    # report success while git keeps using the other directory; a value given on the command line or in the
    # environment outranks every file, so writing any would change what sibling worktrees and later commands
    # run and still not take effect here. The effective value is checked after the write.
    hooks_path_scoped || exit 1
    if [ "$HP_VALUE" = ".githooks" ]; then
        echo "Already configured: core.hooksPath = .githooks"
        return 0
    fi
    case "$HP_SCOPE" in
        command)
            echo "core.hooksPath is $(hooks_path_shown) from the command line or environment, which outranks every config file; nothing written" >&2
            exit 1 ;;
        worktree)
            git -C "$PROJECT" config --worktree core.hooksPath .githooks ;;
        *)
            git -C "$PROJECT" config --local core.hooksPath .githooks ;;
    esac
    if ! hooks_path_is_ours; then
        echo "core.hooksPath is still not .githooks after writing it (a higher-priority value wins); nothing enabled" >&2
        exit 1
    fi
    echo "Configured: core.hooksPath = .githooks"
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
    # The legacy hooks directory is git's own, in the common dir: --git-path
    # hooks follows core.hooksPath and would name .githooks while it is set.
    local legacy
    legacy="$(git -C "$PROJECT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$PROJECT/.git")/hooks"
    [ -d "$legacy" ] || return 0
    local removed=0 target wt
    target="$legacy/pre-push"
    if [ -L "$target" ]; then
        # Identity, not spelling: -ef holds only when the link resolves to that very file (a dangling link,
        # or one to another file whose name merely reads the same once a shell strips a newline, never does).
        while IFS= read -r -d '' wt; do
            if [ "$target" -ef "${wt#worktree }/scripts/hooks/pre-push" ]; then
                rm "$target"
                removed=1
                break
            fi
        done < <(git -C "$PROJECT" worktree list --porcelain -z | grep -z '^worktree ')
    fi
    [ "$removed" -gt 0 ] && echo "Pruned $removed legacy symlink(s) from .git/hooks/"
    return 0
}

remove_all() {
    # Only the value this installer writes is removed, and only where git
    # reads it from: the effective core.hooksPath, when it is exactly
    # .githooks in this repository's or this worktree's config. A value at
    # another scope is left alone, so a shared repository value beneath this
    # worktree's own setting keeps sibling worktrees' gates; a custom
    # directory, or a value inherited from global or system config, is the
    # operator's.
    hooks_path_scoped || return 1
    if [ -z "$HP_SCOPE" ]; then
        echo "core.hooksPath is not set."
    else
        if [ "$HP_VALUE" = ".githooks" ] && { [ "$HP_SCOPE" = "local" ] || [ "$HP_SCOPE" = "worktree" ]; }; then
            git -C "$PROJECT" config "--$HP_SCOPE" --fixed-value --unset core.hooksPath .githooks \
                || { echo "core.hooksPath could not be unset ($HP_SCOPE config)" >&2; return 1; }
            echo "Unset: core.hooksPath ($HP_SCOPE config, was: .githooks)"
        else
            echo "core.hooksPath is $(hooks_path_shown) in $HP_SCOPE config, not this installer's .githooks here: left as it is."
        fi
    fi
    disable_pre_push
    prune_legacy_symlinks
    echo ""
    if hooks_path_is_ours; then
        echo "This checkout still runs .githooks/: core.hooksPath comes from $(git -C "$PROJECT" config --show-scope --get core.hooksPath | tail -1 | cut -f1) config, which --remove does not change."
    else
        echo "Git hooks removed. Always-on hooks in .githooks/ are inert until"
        echo "'bash scripts/install-hooks.sh' reconfigures core.hooksPath."
    fi
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
