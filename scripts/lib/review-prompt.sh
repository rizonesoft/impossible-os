# shellcheck shell=bash
# Reads a review prompt's first line, its TODO path and its section, in memory.
# Sourced by scripts/codex-dispatch.sh, scripts/codex-bg-dispatch.sh and
# scripts/overnight/review-broker-codex-dispatch.sh, so all three attribute a
# prompt the same way. No pipeline: a reader that exits early (sed q, head -1)
# closes the pipe, and under pipefail the writer's SIGPIPE ends the dispatcher
# once the prompt outgrows the pipe buffer (2026-11-21, a whole-file inventory).

# The first line holding a non-space character, or nothing.
review_prompt_first_line() {
    local rest="$1" line
    while :; do
        line="${rest%%$'\n'*}"
        if [[ "$line" =~ [^[:space:]] ]]; then
            printf '%s' "$line"
            return 0
        fi
        [[ "$rest" == *$'\n'* ]] || return 0
        rest="${rest#*$'\n'}"
    done
}

# The first todo/...md path on the line, or nothing.
review_prompt_todo_path() {
    if [[ "$1" =~ (todo/[^[:space:]]+\.md) ]]; then
        printf '%s' "${BASH_REMATCH[1]}"
    fi
    return 0
}

# The section number after the TODO path: `section N` or the section glyph
# straight after it, else `sN` straight after it (the form item prompts use;
# anchored, so `x86 s3 sleep` later in the prompt is not a section), else
# `section N` anywhere after the path.
review_prompt_section_of() {
    local rest="${1#*.md}" found="" nocase=0
    local sect='(section[[:space:]_:-]*|§[[:space:]]*)([0-9]+)'
    shopt -q nocasematch && nocase=1
    shopt -s nocasematch
    if [[ "$rest" =~ ^[[:space:]]*$sect ]]; then
        found="${BASH_REMATCH[2]}"
    fi
    shopt -u nocasematch
    if [[ -z "$found" && "$rest" =~ ^[[:space:]]*s([0-9]+)([^[:alnum:]]|$) ]]; then
        found="${BASH_REMATCH[1]}"
    fi
    if [[ -z "$found" ]]; then
        shopt -s nocasematch
        [[ "$rest" =~ $sect ]] && found="${BASH_REMATCH[2]}"
        shopt -u nocasematch
    fi
    [[ "$nocase" -eq 1 ]] && shopt -s nocasematch
    printf '%s' "$found"
    return 0
}
