# Private companion directories for review passes, shared by scripts/codex-dispatch.sh and the older-kind route
# of scripts/overnight/review-broker-codex-dispatch.sh. Sourced; defines functions only.
#
# A pass directory is .cp.<owner pid>.<random> inside a PARENT the dispatcher owns (the reviews directory, or for a
# review with no transcript pass_dir_parent's private directory under TMPDIR), never a directory shared with other
# programs: the sweep removes a .cp.* entry there whose owner is gone, and must never reach anything it did not
# make (round review, 2026-11-23: a sweep of TMPDIR itself could delete an operator's own .cp.<n>.* directory).

# The private parent for passes with no reviews directory: <TMPDIR>/codex-dispatch.<uid>, mode 700, ours.
pass_dir_parent() {
    local parent="${TMPDIR:-/tmp}/codex-dispatch.$(id -u)"
    mkdir -p -m 700 -- "$parent" 2>/dev/null || return 1
    [ -d "$parent" ] && [ -O "$parent" ] && [ ! -L "$parent" ] || return 1
    printf '%s\n' "$parent"
}

# Remove the pass directories under PARENT whose owner process is gone. A reused pid only keeps one longer.
sweep_pass_dirs() {
    local d owner
    for d in "$1"/.cp.*; do
        [ -d "$d" ] && [ ! -L "$d" ] && [ -O "$d" ] || continue
        owner="${d##*/.cp.}"; owner="${owner%%.*}"
        [[ "$owner" =~ ^[0-9]+$ ]] || continue
        kill -0 "$owner" 2>/dev/null || rm -rf -- "$d"
    done
}

# Sweep PARENT, then make and print a new pass directory in it owned by OWNER_PID (default: this shell).
new_pass_dir() {
    sweep_pass_dirs "$1"
    mktemp -d "$1/.cp.${2:-$$}.XXXXXX"
}
