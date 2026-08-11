#!/usr/bin/env bash
# ============================================================================
# noship-update.sh -- maintain the no-ship streak, OUTCOME-AWARE.
#
#   usage: noship-update.sh <project_dir> <start_head> [outcome_json]
#
# Called by overnight-launch.sh at segment end; feeds deadline-check.sh's abort
# (stop after NOSHIP_LIMIT consecutive no-ship segments). Extracted from the
# launcher 2026-08-11 so the rules are testable, and made outcome-aware the
# same day, because the raw HEAD test measured the wrong thing in BOTH
# directions:
#
#   * A VERIFIED-ROLLOVER segment is short and commit-free BY DESIGN --
#     run-outcome.py already classifies it `checkpoint` and leaves ITS streak
#     untouched for exactly that reason -- yet this counter incremented on it.
#     Observed live 2026-08-11: ship at 12:04, rollover 12:08, then two
#     rotation segments counted 1 and 2 and the breaker stopped an
#     otherwise-healthy run. Two counters, one informed and one blind, and the
#     blind one pulled the trigger.
#   * A segment that finds its section BLOCKED on an unmet dependency, defers
#     it properly and ends has done exactly what doctrine asks -- and counted
#     against the limit whenever the deferral produced no commit.
#
# THE RULES (order matters):
#   1. HEAD moved this segment        -> RESET. Any commit is evidence of life.
#   2. outcome `checkpoint`/`snoozed` -> UNTOUCHED. Not reset -- run-outcome.py
#      states the reason: a genuinely dead loop interleaved with rollovers must
#      still trip on its own segments. Exempting is not excusing.
#   3. anything else                  -> INCREMENT. Unreadable/absent outcome
#      counts too: fail toward stopping, never toward a silent infinite loop.
#
# KNOWN RESIDUAL, deliberate: rule 1 resets on ANY commit, so a segment that
# lands only bookkeeping (a run-log line, a gotcha note) reads as alive. Keying
# reset on a genuine section-ship would re-open the blocked-deferral hole (a
# deferral commit is progress but not a ship). Filed in the capture file rather
# than half-fixed here.
#
# The pure rollover loop this exemption would otherwise hide is closed at
# SOURCE by run_phase_guard.py's idle-rollover refusal (a rollover of an
# unmoved HEAD is refused and redirected to work); a segment that idles anyway
# ends unverified, classifies `unproductive`, and rule 3 counts it.
# ============================================================================
set -uo pipefail

PROJECT_DIR="${1:?usage: noship-update.sh <project_dir> <start_head> [outcome_json]}"
START_HEAD="${2:-}"
OUTCOME_JSON="${3:-}"

NOSHIP_FILE="$PROJECT_DIR/.claude/overnight/noship-streak"

END_HEAD="$(git -C "$PROJECT_DIR" rev-parse HEAD 2>/dev/null || echo "")"
if [ -n "$END_HEAD" ] && [ "$END_HEAD" != "$START_HEAD" ]; then
    rm -f "$NOSHIP_FILE" 2>/dev/null || true
    echo "no-ship streak: reset (HEAD moved this segment)"
    exit 0
fi

# The outcome KIND, parsed defensively: malformed or absent JSON yields "" and
# falls through to increment -- an unreadable verdict must never read as "this
# segment was fine".
OUTCOME_KIND="$(printf '%s' "$OUTCOME_JSON" | python3 -c '
import json, sys
try:
    print(json.load(sys.stdin).get("outcome", ""))
except Exception:
    print("")' 2>/dev/null || echo "")"

case "$OUTCOME_KIND" in
    checkpoint|snoozed)
        PREV="$(awk 'NR==1{print $1+0}' "$NOSHIP_FILE" 2>/dev/null || echo 0)"
        echo "no-ship streak: untouched at ${PREV:-0} (outcome=$OUTCOME_KIND -- commit-free BY DESIGN, not evidence of a dead run)"
        exit 0
        ;;
esac

PREV="$(awk 'NR==1{print $1+0}' "$NOSHIP_FILE" 2>/dev/null || echo 0)"
NEXT=$(( ${PREV:-0} + 1 ))
echo "$NEXT" > "$NOSHIP_FILE" 2>/dev/null || true
echo "no-ship streak: $NEXT (HEAD unmoved this segment, outcome=${OUTCOME_KIND:-unreadable})"
exit 0
