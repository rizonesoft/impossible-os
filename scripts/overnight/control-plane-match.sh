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

# Load manifest entries (skip blanks + comments).
entries=()
while IFS= read -r line; do
  line="${line%%#*}"
  line="${line#"${line%%[![:space:]]*}"}"   # ltrim
  line="${line%"${line##*[![:space:]]}"}"   # rtrim
  [ -n "$line" ] && entries+=("$line")
done < "$MANIFEST"

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
  for e in "${entries[@]}"; do
    if [ "$c" = "$e" ]; then
      echo "$c"; matched=1; break
    elif [ "${e: -1}" = "/" ] && [ "${c#"$e"}" != "$c" ]; then
      echo "$c"; matched=1; break
    fi
  done
done

[ "$matched" -eq 1 ]
