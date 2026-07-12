#!/usr/bin/env bash
# control-plane-match.sh -- does any candidate path touch the runner control plane?
#
# Reads scripts/overnight/control-plane-manifest.txt (the single source of truth)
# and matches candidate repo-relative paths against it. A candidate MATCHES a
# manifest line if it equals the line, or -- when the line ends in "/" -- begins
# with it (directory prefix).
#
# Modes:
#   (default)         -- match against the FULL manifest. Used by the pre-commit /
#                        pre-push runner-suite gate and scripts/lint.sh: ANY
#                        control-plane change runs the deterministic suite.
#   --flow-critical   -- match against the full manifest MINUS the deterministic
#                        allowlist (control-plane-deterministic.txt). Prints only
#                        FLOW-CRITICAL candidates. Used by the arm-sequencer canary
#                        gate: a change limited to deterministically-covered files
#                        (metrics/receipts/reporting, all test-backed) does NOT
#                        re-arm the watched canary; a change touching any flow-
#                        critical file (arm/launch/watchdog, phase machine, gates,
#                        split, rotation) does. Canary tiering (P0.0, 2026-07-12).
#                        The allowlist is a strict SUBSET and DEFAULT-FLOW-CRITICAL:
#                        an unlisted control-plane file is always flow-critical.
#
# Usage:
#   git diff --cached --name-only | bash scripts/overnight/control-plane-match.sh
#   bash scripts/overnight/control-plane-match.sh [--flow-critical] path/a path/b ...
#
# Candidates come from ARGS if given, else from stdin (one per line).
# Prints each matching candidate to stdout. Exit 0 if >=1 matched, 1 if none.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="$HERE/control-plane-manifest.txt"
DET_LIST="$HERE/control-plane-deterministic.txt"

FLOW_CRITICAL=0
if [ "${1:-}" = "--flow-critical" ]; then
  FLOW_CRITICAL=1
  shift
fi

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
det_entries=()
if [ "$FLOW_CRITICAL" -eq 1 ]; then
  # Missing allowlist => empty => everything stays flow-critical (fail-safe).
  mapfile -t det_entries < <(read_entries "$DET_LIST")
fi

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
    # Flow-critical mode: drop candidates fully covered by the deterministic
    # allowlist -- they are proven by the deterministic suite, not the canary.
    if [ "$FLOW_CRITICAL" -eq 1 ] && matches "$c" ${det_entries[@]+"${det_entries[@]}"}; then
      continue
    fi
    echo "$c"; matched=1
  fi
done

[ "$matched" -eq 1 ]
