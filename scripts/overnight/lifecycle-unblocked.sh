#!/usr/bin/env bash
# Self-healing heal probe (overnight sequencer lifecycle).
#
# Called by overnight-launch.sh when the triage oracle reports BLOCKED: the queue
# has only recoverable (`Deferred: awaiting-*`) deferrals left and nothing is
# implementable right now. This decides whether a watchdog tick should spin up a
# real Claude session (a blocker has cleared, so there is work to retry) or
# cheap-exit and STAY ARMED (still blocked -- no Claude session, no disarm).
#
# Exit 0 = at least one recoverable blocker has cleared -> RUN.
# Exit 1 = still blocked, nothing to retry productively -> stay armed, skip.
#
# Per-token heal test:
#   awaiting-answer -> retry ONCE per content-hash change of todo/answers.md
#                       (hash memo .claude/overnight/answers-probe-hash; a
#                       missing answers.md means no retry -- nothing changed).
#   any other awaiting-* token (awaiting-hardware, awaiting-infra, ...) ->
#                       conservative retry-worthy. These are treated as
#                       transient (deploy/push-class infra, or a human
#                       decision made out-of-band) rather than requiring a
#                       specific probe per token; the watchdog's own cadence
#                       (~10-30 min, see overnight-arm.sh) is the backoff, so
#                       this never tight-loops even though it retries every
#                       tick until the blocker actually clears.
#
# impossible-os has no ChromeMCP-bridge lane in the sequencer path (ChromeMCP
# here is gh-pages-only and handled by the launcher separately), so there is
# no chromemcp branch -- unlike the generic runner-kit template.
set -euo pipefail

PROJECT_DIR="${1:-$PWD}"
ORACLE="$PROJECT_DIR/.claude/hooks/sequencer_triage.py"
ANSWERS_FILE="$PROJECT_DIR/todo/answers.md"

[ -f "$ORACLE" ] || { echo "unblocked: no oracle -> run (fail-open)"; exit 0; }

BLOCKERS_JSON="$(cd "$PROJECT_DIR" && python3 "$ORACLE" --blockers 2>/dev/null || true)"
[ -n "$BLOCKERS_JSON" ] || { echo "unblocked: no oracle output -> run (fail-open)"; exit 0; }

# Extract the distinct awaiting-* tokens from the oracle's JSON
# ({"blockers": [{"file","section","awaiting"}, ...]}) -- no jq dependency.
TOKENS="$(printf '%s' "$BLOCKERS_JSON" | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)
toks = sorted({b.get("awaiting", "") for b in data.get("blockers", []) if b.get("awaiting")})
print(" ".join(toks))
' 2>/dev/null || true)"

[ -n "$TOKENS" ] || { echo "unblocked: no recoverable blockers reported -> run"; exit 0; }

for tok in $TOKENS; do
  case "$tok" in
    awaiting-answer)
      # Retry once per answers.md CHANGE -- a permanent `[ -s answers.md ]`
      # check would spin up a full Claude session on every blocked tick even
      # when nothing new was answered. The probe hashes the file and only
      # reports unblocked when the hash differs from the last attempt that
      # still ended blocked.
      if [ -s "$ANSWERS_FILE" ]; then
        ANSWERS_HASH="$(sha256sum "$ANSWERS_FILE" 2>/dev/null | cut -d' ' -f1)"
        HASH_FILE="$PROJECT_DIR/.claude/overnight/answers-probe-hash"
        LAST_HASH="$(cat "$HASH_FILE" 2>/dev/null || true)"
        if [ "$ANSWERS_HASH" != "$LAST_HASH" ]; then
          mkdir -p "$(dirname "$HASH_FILE")"
          printf '%s' "$ANSWERS_HASH" > "$HASH_FILE"
          echo "unblocked: todo/answers.md changed since last attempt -> retry awaiting-answer slices"
          exit 0
        fi
      fi
      ;;
    awaiting-*)
      # Conservative retry-worthy: not answer-gated, so treat as transient
      # (infra/hardware/environment) and let the watchdog cadence be the
      # backoff rather than trying to build a bespoke probe per token.
      echo "unblocked: $tok is conservatively retry-worthy -> retry (watchdog cadence backoff)"
      exit 0
      ;;
  esac
done

echo "still blocked on: $(echo "$TOKENS" | tr '\n' ' ') -- no blocker cleared; staying armed"
exit 1
