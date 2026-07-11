#!/usr/bin/env bash
# review-broker-codex-dispatch.sh -- broker leg of a multi-kind Codex review.
#
# One invocation dispatches ONE review kind in the background and registers it
# in the broker manifest; the session then declares ONE structural wait over
# all legs' log files and ends. When the artifacts complete, the woken session
# reads a single combined envelope via scripts/overnight/review-envelope.py.
#
#   bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: adversarial] <todo-path> <body>'
#
# NAMING IS LOAD-BEARING: the basename ends in "codex-dispatch.sh", the shape
# the shared hook parser (.claude/hooks/_codex_dispatch.py) already recognizes
# -- so each broker leg gets its own per-kind gate receipt exactly like a
# foreground scripts/codex-dispatch.sh call. Never bundle several dispatches
# behind one opaque shell command: the recorder can only attribute what is on
# the Bash command line.
#
# Output (stdout): one JSON line {kind, jobId, logFile, manifest} -- pass
# logFile to `run_phase_guard.py wait ... <logFile> "Turn completed"`.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 '<[review-kind: X] todo-path body>'" >&2
    exit 2
fi
PROMPT="$1"
FIRST_LINE="$(printf '%s\n' "$PROMPT" | sed -n '/[^[:space:]]/{p;q;}')"
if ! [[ "$FIRST_LINE" =~ ^\[review-kind:[[:space:]]*([a-zA-Z-]+)\][[:space:]]+todo/.+\.md([[:space:]]|$) ]]; then
    echo "ERROR: first nonblank prompt line must be '[review-kind: <kind>] <todo-path> ...'" >&2
    exit 2
fi
KIND="${BASH_REMATCH[1],,}"
TODO_PATH="$(printf '%s\n' "$FIRST_LINE" | grep -oE 'todo/[^[:space:]]+\.md' | head -1)"

COMPANION="${CODEX_COMPANION_PATH:-$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs}"
[[ -f "$COMPANION" ]] || { echo "ERROR: codex-companion.mjs not found at $COMPANION" >&2; exit 1; }
export CODEX_REVIEWER_DISPATCH=1

MANIFEST_DIR=".claude/overnight/reviews"
mkdir -p "$MANIFEST_DIR"
MANIFEST="$MANIFEST_DIR/manifest.jsonl"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="$PWD/$MANIFEST_DIR/${STAMP}-${KIND}.out"
UNIT="codex-rev-${KIND}-${STAMP}-$$"

# CGROUP SURVIVAL IS LOAD-BEARING (live incident 2026-07-11): a review
# spawned inside the headless session's systemd service dies with the
# session when it exits for a structural wait -- systemd kills every
# process left in the control group, and the wait then waits forever on a
# corpse (until expiry). systemd-run puts the review in its OWN transient
# scope; the sentinel line is what `run_phase_guard.py wait` watches for.
PROMPT_FILE="$(mktemp)"
printf '%s' "$PROMPT" > "$PROMPT_FILE"
if systemd-run --user --collect "--unit=$UNIT" \
     "--setenv=CODEX_REVIEWER_DISPATCH=1" "--setenv=HOME=$HOME" \
     "--setenv=PATH=$PATH" --same-dir \
     /bin/bash -c "node '$COMPANION' adversarial-review \"\$(cat '$PROMPT_FILE')\" > '$LOG_FILE' 2>&1; rc=\$?; echo \"Turn completed (rc=\$rc)\" >> '$LOG_FILE'; rm -f '$PROMPT_FILE'" \
     >/dev/null 2>&1; then
  DETACH="systemd-run:$UNIT"
else
  # Non-systemd fallback: setsid detach (survives plain shells; NOT a
  # systemd cgroup -- callers inside a service should have systemd-run).
  setsid bash -c "node '$COMPANION' adversarial-review \"\$(cat '$PROMPT_FILE')\" > '$LOG_FILE' 2>&1; rc=\$?; echo \"Turn completed (rc=\$rc)\" >> '$LOG_FILE'; rm -f '$PROMPT_FILE'" \
    < /dev/null >/dev/null 2>&1 &
  DETACH="setsid:$!"
fi

PROMPT_SHA="$(printf '%s' "$PROMPT" | sha256sum | cut -d' ' -f1)"
printf '{"ts": %s, "kind": "%s", "todo": "%s", "jobId": "%s", "logFile": "%s", "prompt_sha256": "%s"}\n' \
  "$(date +%s)" "$KIND" "$TODO_PATH" "$DETACH" "$LOG_FILE" "$PROMPT_SHA" >> "$MANIFEST"

printf '{"kind": "%s", "jobId": "%s", "logFile": "%s", "manifest": "%s"}\n' \
  "$KIND" "$DETACH" "$LOG_FILE" "$MANIFEST"
