#!/usr/bin/env bash
# token-probe.sh <env-file> -- does the long-lived Claude token actually authenticate?
#
# Called by arm-sequencer.sh inside the pre-arm health gate. Observed 2026-09-28:
# arming checked only that the token file existed and carried a
# CLAUDE_CODE_OAUTH_TOKEN= line; the token had been revoked (its expires_at was a
# year out, so no date check would have caught it), both launches died in 5 s
# with "401 OAuth access token is invalid", and the circuit breaker disarmed the
# run two minutes after the operator walked away. One tiny authenticated call at
# arm time turns that into a refusal while the operator is still watching.
#
# Exit codes:
#   0  authenticates, OR nothing to probe (no file / no token line: the run then
#      falls back to the shared credentials file, as arm-sequencer already warns),
#      OR inconclusive (network, timeout, CLI missing) -- never refuse on a guess
#   3  the token was REJECTED (401 / invalid / failed to authenticate): refuse to arm
#
# The token value is never printed: output names it by fingerprint (sha256, first
# 12 hex) and redacts it from anything the CLI echoes. CLAUDE_BIN overrides the
# binary for tests.
set -u

ENV_FILE="${1:?usage: token-probe.sh <env-file>}"
CLAUDE_BIN="${CLAUDE_BIN:-claude}"

if [ ! -f "$ENV_FILE" ]; then
  echo "token-probe: no token file at $ENV_FILE -- skipped (the run will use the shared credentials file)"
  exit 0
fi

TOKEN="$(sed -n 's/^[[:space:]]*\(export[[:space:]]\{1,\}\)\{0,1\}CLAUDE_CODE_OAUTH_TOKEN=//p' "$ENV_FILE" | head -1)"
TOKEN="${TOKEN%\"}"; TOKEN="${TOKEN#\"}"; TOKEN="${TOKEN%\'}"; TOKEN="${TOKEN#\'}"
if [ -z "$TOKEN" ]; then
  echo "token-probe: $ENV_FILE has no CLAUDE_CODE_OAUTH_TOKEN= line -- skipped"
  exit 0
fi
FP="$(printf '%s' "$TOKEN" | sha256sum | cut -c1-12)"

if ! command -v "$CLAUDE_BIN" >/dev/null 2>&1; then
  echo "token-probe: WARN -- '$CLAUDE_BIN' not found; token $FP not verified"
  exit 0
fi

# Run outside any repo so no project instructions load; one turn, cheapest model.
OUT="$(cd /tmp && env -u ANTHROPIC_API_KEY CLAUDE_CODE_OAUTH_TOKEN="$TOKEN" \
        timeout 120 "$CLAUDE_BIN" -p --model haiku --max-turns 1 "Reply with the single word ok." 2>&1)"
RC=$?
# Redact the token from anything echoed back, then keep one line for the message.
FIRST="$(printf '%s\n' "$OUT" | awk -v t="$TOKEN" '{ while ((i = index($0, t)) > 0) $0 = substr($0, 1, i - 1) "[redacted]" substr($0, i + length(t)); print }' \
         | grep -v '^[[:space:]]*$' | head -1 | cut -c1-200)"

if [ "$RC" -eq 0 ]; then
  echo "token-probe: ok -- token $FP authenticates"
  exit 0
fi
if printf '%s' "$OUT" | grep -qiE '401|OAuth access token is invalid|Failed to authenticate|authentication_error|invalid x-api-key|token (has )?expired|revoked'; then
  echo "token-probe: REFUSED -- token $FP does not authenticate: ${FIRST}"
  echo "  Re-mint it (claude setup-token), write CLAUDE_CODE_OAUTH_TOKEN=<value> to $ENV_FILE (chmod 600), then re-arm."
  exit 3
fi
echo "token-probe: WARN -- inconclusive (rc $RC) for token $FP: ${FIRST}"
exit 0
