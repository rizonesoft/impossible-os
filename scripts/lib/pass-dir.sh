# Private companion directories for review passes, shared by scripts/codex-dispatch.sh and the older-kind route
# of scripts/overnight/review-broker-codex-dispatch.sh. Sourced; defines functions only.
#
# Every pass directory is <pid>.<random> inside BASE/.cp, a directory this helper creates (mode 700, owned by this
# user, not a link) and uses for nothing else. The sweep removes entries there whose owner process is gone and
# never looks anywhere else, so no caller-chosen BASE (a reviews directory set to /tmp, a TMPDIR) can expose
# another program's files to it (round reviews, 2026-11-23: a sweep of TMPDIR, then of a reviews directory set to
# /tmp, could delete an operator's own directory). The names are short: a pass's broker socket,
# <pass>/cxc-XXXXXX/broker.sock, must stay under the 108-byte Unix limit.

# Print BASE/.cp once it is proven ours: owned by this user, not a link, and carrying the marker file
# .codex-dispatch-passes (written when this helper made it, or when it adopts an empty one).
PASS_DIR_MARKER=".codex-dispatch-passes"
pass_dir_parent() {
    local parent="$1/.cp"
    mkdir -m 700 -- "$parent" 2>/dev/null
    [ -d "$parent" ] && [ ! -L "$parent" ] && [ -O "$parent" ] || return 1
    if [ ! -f "$parent/$PASS_DIR_MARKER" ]; then
        # An EMPTY unmarked .cp is adopted: a first caller interrupted between its mkdir and its marker leaves
        # exactly that, and an empty directory holds nothing a sweep could remove. One with any entry is someone
        # else's and is never used, unless the marker is there by now: a first caller racing this one wrote it
        # between the two looks (nothing but a marker is written before it).
        # The listing must SUCCEED: an unreadable one prints nothing, like an empty one, and would adopt a
        # directory full of someone else's entries, which a later sweep would delete. One x per entry (so a name
        # that is only a newline counts), stopping at the first.
        local entries rc
        entries="$(find "$parent" -mindepth 1 -maxdepth 1 -printf x -quit 2>/dev/null)"
        rc=$?
        if [ "$rc" -ne 0 ]; then
            return 1
        fi
        if [ -n "$entries" ] && [ ! -f "$parent/$PASS_DIR_MARKER" ]; then
            return 1
        fi
        : > "$parent/$PASS_DIR_MARKER" || return 1
    fi
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
