#!/usr/bin/env bash
# segment-sync.sh -- pull operator repairs into the run's tree at a segment start.
#
# WHY (2026-07-31): attended control-plane repairs land on `main` from the
# operator's repair worktree, and the run's PRIMARY checkout does not see them
# until it pulls. A segment start is the only safe moment: the previous segment
# exited at a verified rollover, so the tree is provably clean and a fresh
# worker has no in-flight work to disturb. Pulling mid-section would swap hooks
# underneath a running section -- the unrecoverable shape the self-modify
# boundary exists to prevent.
#
# CONTRACT, and every clause is load-bearing:
#   - ALWAYS exits 0. A sync failure must never stop a run from starting; being
#     one segment behind on repairs beats not running at all.
#   - SKIPS on a dirty tree. Dirt at segment start means a crashed segment left
#     WIP; pulling over it risks conflicts nobody is watching.
#   - --ff-only, never a merge. If local main has diverged from origin that is a
#     real condition needing a human, not something to silently reconcile at
#     03:00 -- a merge commit authored by an unattended run is exactly the kind
#     of history nobody can later explain.
#   - Prints ONE line describing what it did, so the run log answers "did this
#     segment pick up my fix?" with a grep instead of an inference.
#
# Usage: segment-sync.sh <project_dir>
set -uo pipefail

PROJECT_DIR="${1:?usage: segment-sync.sh <project_dir>}"

if [ ! -d "$PROJECT_DIR/.git" ] && [ ! -f "$PROJECT_DIR/.git" ]; then
  echo "segment-start sync: SKIPPED, not a git checkout ($PROJECT_DIR)"
  exit 0
fi

if [ -n "$(git -C "$PROJECT_DIR" status --porcelain 2>/dev/null)" ]; then
  echo "segment-start sync: SKIPPED, tree not clean (in-flight work from a crashed segment)"
  exit 0
fi

BEFORE="$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
if OUT="$(git -C "$PROJECT_DIR" pull --ff-only 2>&1)"; then
  AFTER="$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  if [ "$BEFORE" = "$AFTER" ]; then
    echo "segment-start sync: already current at $AFTER"
  else
    echo "segment-start sync: $BEFORE -> $AFTER (operator repairs picked up)"
  fi
else
  # Diverged, offline, or no upstream. Named explicitly so the log distinguishes
  # "nothing to do" from "could not do it".
  echo "segment-start sync: FAILED (diverged, offline, or no upstream) -- continuing on the local tree at $BEFORE: $(printf '%s' "$OUT" | tr '\n' ' ' | cut -c1-160)"
fi
exit 0
