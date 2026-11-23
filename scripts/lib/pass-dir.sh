# Private companion directories for review passes, shared by scripts/codex-dispatch.sh and the older-kind route
# of scripts/overnight/review-broker-codex-dispatch.sh. Sourced; defines functions only.
#
# Every pass directory is <pid>.<random> inside BASE/.cp, a directory this helper creates (mode 700, owned by this
# user, not a link) and uses for nothing else. The sweep removes entries there whose owner process is gone and
# never looks anywhere else, so no caller-chosen BASE (a reviews directory set to /tmp, a TMPDIR) can expose
# another program's files to it (round reviews, 2026-11-23: a sweep of TMPDIR, then of a reviews directory set to
# /tmp, could delete an operator's own directory). The names are short: a pass's broker socket,
# <pass>/cxc-XXXXXX/broker.sock, must stay under the 108-byte Unix limit.

# Print BASE/.cp once it is proven ours: made here (mode 700) with the marker file .codex-dispatch-passes, or found
# with that marker, owned by this user and not a link. A .cp without the marker is someone else's: never used.
PASS_DIR_MARKER=".codex-dispatch-passes"
pass_dir_parent() {
    local parent="$1/.cp"
    if mkdir -m 700 -- "$parent" 2>/dev/null; then
        : > "$parent/$PASS_DIR_MARKER" || return 1
    fi
    [ -d "$parent" ] && [ ! -L "$parent" ] && [ -O "$parent" ] && [ -f "$parent/$PASS_DIR_MARKER" ] || return 1
    printf '%s\n' "$parent"
}

# Remove the pass directories in PARENT (a pass_dir_parent) whose owner process is gone. A reused pid only keeps
# one longer.
sweep_pass_dirs() {
    local d owner
    for d in "$1"/*; do
        [ -d "$d" ] && [ ! -L "$d" ] && [ -O "$d" ] || continue
        owner="${d##*/}"; owner="${owner%%.*}"
        [[ "$owner" =~ ^[0-9]+$ ]] || continue
        kill -0 "$owner" 2>/dev/null || rm -rf -- "$d"
    done
}

# Make and print a new pass directory under BASE owned by OWNER_PID (default: this shell), sweeping first.
new_pass_dir() {
    local parent
    parent="$(pass_dir_parent "$1")" || return 1
    sweep_pass_dirs "$parent"
    mktemp -d "$parent/${2:-$$}.XXXXXX"
}
