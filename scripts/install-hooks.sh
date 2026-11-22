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
# One installer or remover at a time per repository: git's value checks keep another writer's value, but two
# of these adding at once would leave two entries that neither can take out by value alone.
lock_hooks_config() {
    command -v flock >/dev/null || { echo "flock (util-linux) is required to change core.hooksPath safely" >&2; return 1; }
    local lock
    lock="$(git -C "$PROJECT" rev-parse --path-format=absolute --git-common-dir)/impossible-os-hooks.lock" || return 1
    exec 9>"$lock"
    flock -w 30 9 || { echo "another install-hooks.sh holds $lock; run again once it finishes" >&2; return 1; }
}
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
# Whether git runs this checkout's .githooks/, however the value spells it (an absolute path, ./.githooks):
# the directory git resolves, compared by identity.
hooks_run_from_repo() {
    local dir
    dir="$(git -C "$PROJECT" rev-parse --path-format=absolute --git-path hooks 2>/dev/null)" || return 1
    [ "$dir" -ef "$HOOKS_DIR" ]
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
    # Written only into a scope that holds no core.hooksPath at all (its include files counted), so the one
    # entry there is this installer's and --remove takes out exactly that, leaving the config as it was. A
    # scope that already holds a value (the operator's own, several, or one from an include) refuses: writing
    # beside it would either lose to it or, removed later, take the operator's value with it. The scope is the
    # one git reads the setting from, decided before anything is written: a worktree-scoped value
    # (extensions.worktreeConfig) outranks the repository's, so writing the repository's alone would report
    # success while git keeps using the other directory; a value given on the command line or in the
    # environment outranks every file, so writing any would change what sibling worktrees and later commands
    # run and still not take effect here. The effective value is checked after the write, and a write git does
    # not then read is undone. Every change is one git decides under its own lock: the value is added (--add
    # never replaces another), and undone or removed by value (--fixed-value --unset takes out a single
    # .githooks entry and nothing else), so a writer changing the setting between the check and the write
    # loses nothing.
    lock_hooks_config || exit 1
    hooks_path_scoped || exit 1
    # Checked first: a value from the command line or the environment, even .githooks itself, says nothing
    # about what the next plain git command reads.
    if [ "$HP_SCOPE" = "command" ]; then
        echo "core.hooksPath is $(hooks_path_shown) from the command line or environment, which outranks every config file; run the installer without it (nothing written)" >&2
        exit 1
    fi
    if [ "$HP_VALUE" = ".githooks" ]; then
        echo "Already configured: core.hooksPath = .githooks"
        return 0
    fi
    local scope=local held rc=0
    [ "$HP_SCOPE" = "worktree" ] && scope=worktree
    held="$(git -C "$PROJECT" config "--$scope" --includes --null --get-all core.hooksPath | tr '\0' '\001')" || rc=$?
    if [ "$rc" -gt 1 ]; then
        echo "core.hooksPath could not be read from $scope config (git config exit $rc); nothing written" >&2
        exit 1
    fi
    if [ -n "$held" ]; then
        local shown="${held%$'\001'}"
        echo "core.hooksPath is already set in $scope config (or a file it includes) to ${shown//$'\001'/, }; this installer writes only where no value is set, so --remove can restore the config exactly. Remove or change that value, then run it again (nothing written)" >&2
        exit 1
    fi
    git -C "$PROJECT" config "--$scope" --add core.hooksPath .githooks \
        || { echo "core.hooksPath could not be written to $scope config; nothing enabled" >&2; exit 1; }
    held="$(git -C "$PROJECT" config "--$scope" --includes --null --get-all core.hooksPath | tr '\0' '\001')" || held=""
    if [ "$held" != $'.githooks\001' ] || ! hooks_path_is_ours; then
        # Another value appeared beside the write, or one git reads before it: take out only what was added.
        if git -C "$PROJECT" config "--$scope" --fixed-value --unset core.hooksPath .githooks; then
            echo "core.hooksPath is not this installer's alone after writing it (another value is set, or a higher-priority one wins); the write was undone, nothing enabled" >&2
        else
            echo "core.hooksPath changed while it was written, and no single .githooks entry was left to undo: check core.hooksPath in $scope config" >&2
        fi
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
        # Taken aside first, in one rename, and judged there: a hook another installer puts at that name
        # meanwhile is then never the one removed, and an entry that is not ours goes back without replacing
        # one that appeared since.
        local aside="$legacy/.pre-push.install-hooks.$$" ours=0
        mv -T -- "$target" "$aside" 2>/dev/null || return 0     # gone meanwhile: nothing of ours to remove
        if [ -L "$aside" ]; then
            # Identity, not spelling: -ef holds only when the link resolves to that very file (a dangling
            # link, or one to another file whose name merely reads the same once a shell strips a newline,
            # never does).
            while IFS= read -r -d '' wt; do
                if [ "$aside" -ef "${wt#worktree }/scripts/hooks/pre-push" ]; then
                    ours=1
                    break
                fi
            done < <(git -C "$PROJECT" worktree list --porcelain -z | grep -z '^worktree ')
        fi
        if [ "$ours" = 1 ]; then
            rm -f -- "$aside"
            removed=1
        else
            mv -n -T -- "$aside" "$target" 2>/dev/null || true   # fail-direction: the check below reports it
            if [ -e "$aside" ] || [ -L "$aside" ]; then
                echo "A new pre-push hook appeared at $target meanwhile; the earlier entry is kept at $aside" >&2
            fi
        fi
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
    lock_hooks_config || return 1
    hooks_path_scoped || return 1
    # Before any cleanup: under a command-line value, what the next plain git command runs is unknown, so
    # neither the setting, the pre-push sentinel nor a legacy link is touched.
    if [ "$HP_SCOPE" = "command" ]; then
        echo "core.hooksPath is $(hooks_path_shown) from the command line or environment, which outranks every config file; run --remove without it (nothing changed)" >&2
        return 1
    fi
    if [ -z "$HP_SCOPE" ]; then
        echo "core.hooksPath is not set."
    else
        local own="" own_rc=0
        if [ "$HP_SCOPE" = "local" ] || [ "$HP_SCOPE" = "worktree" ]; then
            # Exit 1 is "that file holds none" (the value comes from an include); anything else is an error.
            own="$(set -o pipefail; git -C "$PROJECT" config "--$HP_SCOPE" --no-includes --null --get-all core.hooksPath | tr '\0' '\001')" || own_rc=$?
            if [ "$own_rc" -gt 1 ]; then
                echo "core.hooksPath could not be read from $HP_SCOPE config (git config exit $own_rc)" >&2
                return 1
            fi
        fi
        # Only the single entry this installer writes: that scope's own file holds exactly .githooks. Several
        # entries, or a value from an include, are the operator's and are left as they are.
        if [ "$HP_VALUE" = ".githooks" ] && [ "$own" = $'.githooks\001' ]; then
            # By value, under git's lock: a value written meanwhile in its place is left as it is.
            git -C "$PROJECT" config "--$HP_SCOPE" --fixed-value --unset core.hooksPath .githooks \
                || { echo "core.hooksPath changed in $HP_SCOPE config while it was removed (it no longer holds a single .githooks entry): left as it is" >&2; return 1; }
            echo "Unset: core.hooksPath ($HP_SCOPE config, was: .githooks)"
        elif [ "$HP_VALUE" = ".githooks" ] && [ -n "$HP_SCOPE" ] && [ "$HP_SCOPE" != "global" ] && [ "$HP_SCOPE" != "system" ]; then
            echo "core.hooksPath is .githooks in $HP_SCOPE config, but not as this installer's single entry (several values, or one from an included file): left as it is."
        else
            echo "core.hooksPath is $(hooks_path_shown) in $HP_SCOPE config, not this installer's .githooks here: left as it is."
        fi
    fi
    disable_pre_push
    prune_legacy_symlinks
    echo ""
    if hooks_run_from_repo; then
        echo "This checkout still runs .githooks/: core.hooksPath ($(hooks_path_shown)) comes from $(git -C "$PROJECT" config --show-scope --get core.hooksPath | tail -1 | cut -f1) config, which --remove does not change."
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
