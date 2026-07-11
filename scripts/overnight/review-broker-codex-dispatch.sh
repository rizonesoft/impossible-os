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

OUTPUT="$(node "$COMPANION" task --background --json "$PROMPT")"
JOB_ID="$(printf '%s' "$OUTPUT" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("jobId",""))' 2>/dev/null || true)"
LOG_FILE="$(printf '%s' "$OUTPUT" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("logFile",""))' 2>/dev/null || true)"
[[ -n "$JOB_ID" && -n "$LOG_FILE" ]] || { echo "ERROR: background dispatch returned no jobId/logFile: $OUTPUT" >&2; exit 1; }

MANIFEST_DIR=".claude/overnight/reviews"
mkdir -p "$MANIFEST_DIR"
MANIFEST="$MANIFEST_DIR/manifest.jsonl"
PROMPT_SHA="$(printf '%s' "$PROMPT" | sha256sum | cut -d' ' -f1)"
printf '{"ts": %s, "kind": "%s", "todo": "%s", "jobId": "%s", "logFile": "%s", "prompt_sha256": "%s"}\n' \
  "$(date +%s)" "$KIND" "$TODO_PATH" "$JOB_ID" "$LOG_FILE" "$PROMPT_SHA" >> "$MANIFEST"

printf '{"kind": "%s", "jobId": "%s", "logFile": "%s", "manifest": "%s"}\n' \
  "$KIND" "$JOB_ID" "$LOG_FILE" "$MANIFEST"
