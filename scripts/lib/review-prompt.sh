# shellcheck shell=bash
# Reads a review prompt's first line, its TODO path and its section, in memory.
# Sourced by scripts/codex-dispatch.sh, scripts/codex-bg-dispatch.sh and
# scripts/overnight/review-broker-codex-dispatch.sh, so all three attribute a
# prompt the same way. No pipeline: a reader that exits early (sed q, head -1)
# closes the pipe, and under pipefail the writer's SIGPIPE ends the dispatcher
# once the prompt outgrows the pipe buffer (2026-11-21, a whole-file inventory).

# The first line holding a non-space character (its leading spaces kept), or
# nothing. Two start-anchored regex matches, each one forward pass: the first
# takes every whitespace-only line before it (greedy whitespace, then the last
# newline of that run), the second takes the line. A per-line loop re-copies
# the rest of the prompt for each blank line, and the `%%`/`##` glob removals
# and end-anchored regexes retry from each position: all quadratic.
review_prompt_first_line() {
    local s="$1" line nl=$'\n'
    [[ "$s" =~ ^([[:space:]]*$nl)? ]]
    s="${s:${#BASH_REMATCH[0]}}"
    [[ "$s" =~ ^([^$nl]*) ]]
    line="${BASH_REMATCH[1]}"
    [[ "$line" =~ [^[:space:]] ]] && printf '%s' "$line"
    return 0
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
