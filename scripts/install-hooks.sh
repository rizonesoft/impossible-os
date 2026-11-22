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
#   - .githooks/pre-push     always-on: the branch policy, the review debt,
#                            the dash check over outgoing commits and the
#                            conditional suites; the sentinel
#                            .git/.impossible-os-prepush adds only the
#                            delegated build + test.sh (scripts/hooks/pre-push).
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
#   bash scripts/install-hooks.sh --pre-push-state
#                                                # one line: the build/test
#                                                # step's state (the doctor).
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
# a plain print would hide. With an argument, that value (as hooks_path_raw spells it) is shown without
# reading the configuration again, so a diagnostic names the value it was judged on.
hooks_path_shown() {
    local raw
    if [ $# -gt 0 ]; then raw=$1; else raw="$(hooks_path_raw)"; fi
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

pre_push_state() {
    if [ -f "$SENTINEL" ]; then
        echo "ENABLED (sentinel: $SENTINEL)"
    else
        echo "disabled (no sentinel)"
    fi
}

status() {
    local configured pre_push_state
    configured="$(hooks_path_shown)"
    pre_push_state="$(pre_push_state)"
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
        echo "core.hooksPath is $(hooks_path_shown "$HP_VALUE"$'\001') from the command line or environment, which outranks every config file; run the installer without it (nothing written)" >&2
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
    local landed=0
    hooks_path_scoped && [ "$HP_SCOPE" = "$scope" ] && [ "$HP_VALUE" = ".githooks" ] && landed=1
    if [ "$held" != $'.githooks\001' ] || [ "$landed" != 1 ]; then
        # Another value appeared beside the write, or git reads one from another scope first (even .githooks,
        # which would leave this repository-wide write latent): take out only what was added.
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

# Whether a failed stat's diagnostic reports the entry missing. GNU stat names the path quoted (in double
# quotes when it holds a single one, a quote of the other kind escaped) and the error after the closing quote: only that ending is read, so a path that
# merely contains the words cannot pass for an absent entry (LC_ALL=C on the stat call keeps them English).
stat_says_absent() {
    local last="${1##*$'\n'}"
    case "$last" in
        *[\'\"]": No such file or directory"|*[\'\"]": Not a directory") return 0 ;;
    esac
    return 1
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
    # Identity, not spelling: -ef holds only when the link resolves to that very file (a dangling link, or one
    # to another file whose name merely reads the same once a shell strips a newline, never does).
    # 0: ours, 1: not ours, 2: the worktrees could not be listed (the listing's own status ends it), 3: an
    # identity could not be read (a path stat cannot reach is not a different file: -ef alone would say no).
    # The whole listing is read before a match counts, so one cut short by a failure refuses.
    resolved_id() {   # RESOLVED: device and inode of what $1 names. 0 read, 1 definitely absent, 2 unknown
        local err
        RESOLVED="$(LC_ALL=C stat -L -c '%d:%i' -- "$1" 2>/dev/null)" && return 0
        err="$(LC_ALL=C stat -L -c '%d:%i' -- "$1" 2>&1 >/dev/null)"
        stat_says_absent "$err" && return 1
        return 2
    }
    legacy_link_is_ours() {
        local entry listed="" found=1 unknown=0 link_id rc=0
        resolved_id "$1" || rc=$?
        link_id=$RESOLVED
        [ "$rc" = 2 ] && unknown=1
        while IFS= read -r -d '' entry; do
            case "$entry" in
                "worktree "*)
                    [ "$rc" = 0 ] || continue                # a dangling link names no worktree's hook
                    local erc=0
                    resolved_id "${entry#worktree }/scripts/hooks/pre-push" || erc=$?
                    if [ "$erc" = 0 ] && [ "$RESOLVED" = "$link_id" ]; then
                        found=0
                    elif [ "$erc" = 2 ]; then
                        unknown=1
                    fi ;;
                "listed=0") listed=0 ;;
            esac
        done < <(git -C "$PROJECT" worktree list --porcelain -z && printf 'listed=0\0')
        [ "$listed" = 0 ] || return 2
        [ "$unknown" = 1 ] && return 3                   # a worktree not read could hold a second copy
        [ "$found" = 0 ] && return 0
        return 1
    }
    # The link's own identity: device and inode, then its text (the && keeps a failed readlink failing; the
    # x keeps a trailing newline the substitution would strip).
    link_identity() {
        local inode text
        inode="$(stat -c '%d:%i' -- "$1")" || return 1
        text="$(readlink -- "$1" && printf x)" || return 1
        printf '%s/%s' "$inode" "$text"
    }
    local common legacy
    common="$(git -C "$PROJECT" rev-parse --path-format=absolute --git-common-dir)" \
        || { echo "The repository's git directory could not be found, so no legacy link was judged" >&2; return 1; }
    legacy="$common/hooks"
    # Absent is known only from a directory that can be searched; one that cannot hides what it holds.
    if [ ! -e "$legacy" ] && [ ! -L "$legacy" ]; then
        [ -x "$common" ] && return 0
        echo "$common cannot be searched, so no legacy link in it was judged" >&2
        return 1
    fi
    if [ ! -d "$legacy" ] || [ ! -x "$legacy" ]; then
        echo "$legacy cannot be searched, so a legacy pre-push link in it was not judged" >&2
        return 1
    fi
    local removed=0 target
    target="$legacy/pre-push"
    # Judged in place first: a hook that is not ours is never moved, so no push in the meantime runs without
    # it. Ours is then renamed into a fresh directory and judged again there, so a hook another writer put
    # at that name in between is put back, never removed, and no earlier saved entry can be overwritten.
    # Absent or not a link is known only from an lstat that succeeded or reported the entry missing; any other
    # failure (an I/O error) leaves it unjudged.
    local lookup
    if ! lookup="$(LC_ALL=C stat -c '%F' -- "$target" 2>&1)"; then
        stat_says_absent "$lookup" && return 0
        echo "The legacy pre-push entry $target could not be looked up ($lookup): left unjudged" >&2
        return 1
    fi
    [ "$lookup" = "symbolic link" ] || return 0
    # The entry's own identity (the link itself, not what it names) and its text, so the entry moved aside
    # below is known to be the one judged: a relative link resolves differently once moved, so it is not
    # judged again there, and the text stands in where a freed inode number is given to a replacement.
    local id
    id="$(link_identity "$target")" \
        || { echo "The legacy link $target could not be read: left in place" >&2; return 1; }
    local ours=0
    legacy_link_is_ours "$target" || ours=$?
    if [ "$ours" = 2 ]; then
        echo "The worktrees could not be listed, so the legacy link $target was not judged: left in place" >&2
        return 1
    fi
    if [ "$ours" = 3 ]; then
        echo "The legacy link $target, or a worktree's hook it may name, could not be read, so it was not judged: left in place" >&2
        return 1
    fi
    if [ "$ours" = 0 ]; then
        local aside
        aside="$(mktemp -d "$legacy/.install-hooks.XXXXXX")" \
            || { echo "No private directory could be made in $legacy; the legacy link $target is left in place" >&2; return 1; }
        if ! mv -T -- "$target" "$aside/pre-push"; then
            rmdir -- "$aside" 2>/dev/null || true                         # fail-direction: empty, made above
            echo "The legacy link $target could not be moved aside (see above): it is left in place and still runs" >&2
            return 1
        else
            local now=""
            if [ -L "$aside/pre-push" ] && ! now="$(link_identity "$aside/pre-push")"; then
                # A link that cannot be read is not known to be the one judged: it is put back, and the
                # removal fails.
                mv -n -T -- "$aside/pre-push" "$target" \
                    || echo "It could not be put back either; it is kept at $aside/pre-push" >&2
                echo "The legacy link moved aside could not be read again, so it was not deleted" >&2
                return 1
            fi
            if [ -n "$now" ] && [ "$now" = "$id" ]; then
                rm -f -- "$aside/pre-push" || {
                    echo "The legacy link could not be deleted; it stays, inactive, at $aside/pre-push" >&2
                    return 1
                }
                removed=1
            else
                mv -n -T -- "$aside/pre-push" "$target" 2>/dev/null || true   # fail-direction: checked below
                if [ -e "$aside/pre-push" ] || [ -L "$aside/pre-push" ]; then
                    echo "A new pre-push hook appeared at $target meanwhile; the earlier entry is kept at $aside/pre-push" >&2
                    return 0
                fi
            fi
        fi
        rmdir -- "$aside" 2>/dev/null || true
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
        echo "core.hooksPath is $(hooks_path_shown "$HP_VALUE"$'\001') from the command line or environment, which outranks every config file; run --remove without it (nothing changed)" >&2
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
            echo "core.hooksPath is $(hooks_path_shown "$HP_VALUE"$'\001') in $HP_SCOPE config, not this installer's .githooks here: left as it is."
        fi
    fi
    disable_pre_push
    local pruned=0
    prune_legacy_symlinks || pruned=1
    echo ""
    hooks_path_scoped || return 1
    if hooks_run_from_repo; then
        echo "This checkout still runs .githooks/: core.hooksPath ($(hooks_path_shown "$HP_VALUE"$'\001')) comes from $HP_SCOPE config, which --remove does not change."
    else
        echo "Git hooks removed. Always-on hooks in .githooks/ are inert until"
        echo "'bash scripts/install-hooks.sh' reconfigures core.hooksPath."
    fi
    return "$pruned"
}

case "${1:-}" in
    --help|-h)
        usage 0
        ;;
    --status)
        status
        ;;
    --pre-push-state)
        pre_push_state
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
