#!/usr/bin/env bash
# control-plane-match.sh -- does any candidate path touch the runner control plane?
#
# Reads scripts/overnight/control-plane-manifest.txt (the single source of truth)
# and matches candidate repo-relative paths against it. A candidate MATCHES a
# manifest line if it equals the line, or -- when the line ends in "/" -- begins
# with it (directory prefix).
#
# Usage:
#   git diff --cached --name-only | bash scripts/overnight/control-plane-match.sh
#   bash scripts/overnight/control-plane-match.sh path/a path/b ...
#
# Candidates come from ARGS if given, else from stdin (one per line).
# Prints each matching candidate to stdout. Exit 0 if >=1 matched, 1 if none.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="$HERE/control-plane-manifest.txt"

if [ ! -f "$MANIFEST" ]; then
  echo "control-plane-match: manifest missing: $MANIFEST" >&2
  exit 2
fi

# read_entries FILE -> stdout: trimmed, non-comment, non-blank lines.
read_entries() {
  local f="$1" line
  [ -f "$f" ] || return 0
  while IFS= read -r line; do
    line="${line%%#*}"
    line="${line#"${line%%[![:space:]]*}"}"   # ltrim
    line="${line%"${line##*[![:space:]]}"}"   # rtrim
    [ -n "$line" ] && printf '%s\n' "$line"
  done < "$f"
}

mapfile -t entries < <(read_entries "$MANIFEST")

# matches CANDIDATE ENTRY...  -> 0 if candidate equals an entry or sits under a
# "/"-suffixed entry (dir prefix); 1 otherwise.
matches() {
  local c="$1"; shift
  local e
  for e in "$@"; do
    if [ "$c" = "$e" ]; then
      return 0
    elif [ "${e: -1}" = "/" ] && [ "${c#"$e"}" != "$c" ]; then
      return 0
    fi
  done
  return 1
}

# Collect candidates from args or stdin.
candidates=()
if [ "$#" -gt 0 ]; then
  candidates=("$@")
else
  while IFS= read -r c; do
    [ -n "$c" ] && candidates+=("$c")
  done
fi

matched=0
for c in "${candidates[@]:-}"; do
  [ -n "$c" ] || continue
  c="${c#./}"
  if matches "$c" ${entries[@]+"${entries[@]}"}; then
    printf '%s\n' "$c"
    matched=1
  fi
done

[ "$matched" -eq 1 ]
