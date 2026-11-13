#!/bin/bash
# =============================================================================
# check_make_duplicates.sh -- lint Check 38: no target has two recipes
#
# GNU make keeps the LAST recipe for a target and drops the earlier one with a
# warning, so a documented target silently does something else. Until
# 2026-11-13 run-test and run-debug were each defined twice, and the four
# warnings on every build trained readers to ignore make's warnings.
#
# make itself decides what a duplicate is: it parses a COPY of the Makefile in
# a temporary directory and runs one sentinel target appended to it. The
# directory mirrors the Makefile's own directory through symlinks, so source
# discovery ($(wildcard), $(shell find)) sees the real tree and a recipe over a
# discovered object is still caught, but build/ is left out: the root Makefile
# deletes a stale signing stamp under build/ at parse time, and that delete
# must not run from lint. The sentinel proves the parse finished: a $(error), a syntax error or a
# timeout is a failure to check, never a pass. Any "overriding recipe" or
# "ignoring old recipe" warning fails. Pattern and double-colon rules do not
# warn, so they are not refused.
#
# Usage: check_make_duplicates.sh [MAKEFILE]   (default: the root Makefile)
# Exit:  0 clean, 1 duplicates (printed), 2 the Makefile could not be checked
# =============================================================================

set -uo pipefail

MK="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/Makefile}"
for tool in make timeout; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "check_make_duplicates: $tool is not installed" >&2
        exit 2
    fi
done
if [ ! -f "$MK" ]; then
    echo "check_make_duplicates: no Makefile at $MK" >&2
    exit 2
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/make-dup.XXXXXX")" || exit 2
trap 'rm -rf "$TMP"' EXIT
SENTINEL="__check_make_duplicates_parsed__"
SRC_DIR="$(cd "$(dirname "$MK")" && pwd)"
MK_NAME="$(basename "$MK")"
for entry in "$SRC_DIR"/* "$SRC_DIR"/.[!.]* "$SRC_DIR"/..?*; do
    [ -e "$entry" ] || [ -L "$entry" ] || continue
    name="$(basename "$entry")"
    case "$name" in build|"$MK_NAME") continue ;; esac
    ln -s "$entry" "$TMP/$name" || { echo "check_make_duplicates: could not mirror $entry" >&2; exit 2; }
done
# Each write is checked: a copy that lost the source would hold only the
# sentinel and pass.
if ! cat "$MK" > "$TMP/Makefile" \
   || ! printf '\n.PHONY: %s\n%s:\n\t@echo %s\n' "$SENTINEL" "$SENTINEL" "$SENTINEL" >> "$TMP/Makefile"; then
    echo "check_make_duplicates: could not copy $MK" >&2
    exit 2
fi

# deadline: 60 s, far above the measured 0.2 s parse.
RC=0
# LC_ALL=C: the warnings matched below are make's English text, and a
# translated make would otherwise report a duplicate the grep cannot see.
OUT="$(cd "$TMP" && LC_ALL=C LANGUAGE= timeout 60 make --no-print-directory -f Makefile "$SENTINEL" 2>&1)" || RC=$?
DUP="$(printf '%s\n' "$OUT" | grep -E 'warning: (overriding|ignoring old) recipe for target')"
if [ -n "$DUP" ]; then
    printf '%s\n' "$DUP" | sed "s|$TMP/Makefile|$MK|g; s|^Makefile:|$MK:|"
    exit 1
fi
if [ "$RC" -ne 0 ] || ! printf '%s\n' "$OUT" | grep -qx "$SENTINEL"; then
    echo "check_make_duplicates: make did not finish parsing $MK (exit $RC):" >&2
    printf '%s\n' "$OUT" | tail -5 >&2
    exit 2
fi
exit 0
