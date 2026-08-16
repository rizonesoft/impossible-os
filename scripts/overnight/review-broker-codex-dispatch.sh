#!/usr/bin/env bash
# review-broker-codex-dispatch.sh -- broker leg of a multi-kind Codex review.
#
# One invocation dispatches ONE review kind in the background and registers it
# in the broker manifest; the session then polls all legs' log files IN-SESSION
# (a single blocking Bash sleep loop, ~0 model tokens) and, when the artifacts
# complete, reads a single combined envelope via
# scripts/overnight/review-envelope.py. The session never exits to wait.
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
# Output (stdout): one JSON line {kind, jobId, logFile, manifest} -- poll
# logFile IN-SESSION for the "Turn completed" sentinel (blocking sleep loop).
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
# Section attribution (v14 close-out, 2026-08-16): the envelope was FILE-scoped
# only, so a section's review wave silently ingested an earlier section's legs
# from the same TODO. The prompt already names the section; record it so
# review-envelope.py --section can filter. Separator set matches the
# codex_review_completed.py recogniser (space, dash, underscore, colon).
SECTION="$(printf '%s\n' "$FIRST_LINE" | grep -oE "(section[[:space:]_:-]*|\xc2\xa7[[:space:]]*)[0-9]+" | head -1 | grep -oE '[0-9]+' | tail -1 || true)"

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
# session -- when the session ENDS (a verified rollover, a crash, or a
# usage-limit exit) systemd's KillMode=control-group reaps every process
# left in the service's cgroup, killing an in-flight review with it. A
# review in its OWN transient scope survives the session's death, so the
# NEXT session can still read a completed artifact. systemd-run --collect
# gives it that scope; the "Turn completed" sentinel is what the in-session
# poll (and any recovering next session) watches for.
PROMPT_FILE="$(mktemp)"
printf '%s' "$PROMPT" > "$PROMPT_FILE"
# LIFECYCLE-WAIVER: this detaches a REVIEW (Codex) into its own scope so it
# survives the session's rollover -- it does NOT relaunch or watch the session.
# The session still polls this review's logFile IN-SESSION; nothing here is the
# removed exit-and-relaunch apparatus.
if systemd-run --user --collect "--unit=$UNIT" \
     "--setenv=CODEX_REVIEWER_DISPATCH=1" "--setenv=HOME=$HOME" \
     "--setenv=PATH=$PATH" --same-dir \
     /bin/bash -c "node '$COMPANION' adversarial-review \"\$(cat '$PROMPT_FILE')\" > '$LOG_FILE' 2>&1; rc=\$?; echo \"Turn completed (rc=\$rc)\" >> '$LOG_FILE'; rm -f '$PROMPT_FILE'" \
     >/dev/null 2>&1; then
  DETACH="systemd-run:$UNIT"
else
  # Non-systemd fallback: setsid detach (survives plain shells; NOT a
  # systemd cgroup -- callers inside a service should have systemd-run).
  # LIFECYCLE-WAIVER: detaches a REVIEW on non-systemd hosts, not the session.
  setsid bash -c "node '$COMPANION' adversarial-review \"\$(cat '$PROMPT_FILE')\" > '$LOG_FILE' 2>&1; rc=\$?; echo \"Turn completed (rc=\$rc)\" >> '$LOG_FILE'; rm -f '$PROMPT_FILE'" \
    < /dev/null >/dev/null 2>&1 &
  DETACH="setsid:$!"
fi

PROMPT_SHA="$(printf '%s' "$PROMPT" | sha256sum | cut -d' ' -f1)"
if [[ -n "$SECTION" ]]; then
  printf '{"ts": %s, "kind": "%s", "todo": "%s", "section": %s, "jobId": "%s", "logFile": "%s", "prompt_sha256": "%s"}\n' \
    "$(date +%s)" "$KIND" "$TODO_PATH" "$SECTION" "$DETACH" "$LOG_FILE" "$PROMPT_SHA" >> "$MANIFEST"
else
  printf '{"ts": %s, "kind": "%s", "todo": "%s", "jobId": "%s", "logFile": "%s", "prompt_sha256": "%s"}\n' \
    "$(date +%s)" "$KIND" "$TODO_PATH" "$DETACH" "$LOG_FILE" "$PROMPT_SHA" >> "$MANIFEST"
fi

printf '{"kind": "%s", "jobId": "%s", "logFile": "%s", "manifest": "%s"}\n' \
  "$KIND" "$DETACH" "$LOG_FILE" "$MANIFEST"
