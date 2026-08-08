#!/usr/bin/env bash
# run-artifact.sh -- run a command, keep the transcript on disk, return an
# envelope. The main session ingests verdict + errors + evidence pointers,
# never thousands of raw output lines.
#
#   bash scripts/overnight/run-artifact.sh <label> -- <command...>
#
# Envelope (stdout, JSON): {label, exit, artifact, sha256, lines, errors[],
# tail[]}. `errors` carries every line matching the error/fail pattern
# (bounded 40); `tail` the last 8 lines. The full transcript lives at
# `artifact` -- slice-read it only when a specific error needs surrounding
# context. Artifacts rotate (newest 40 kept).
set -u

LABEL="${1:?usage: run-artifact.sh <label> -- <command...>}"
shift
[ "${1:-}" = "--" ] && shift
[ $# -gt 0 ] || { echo "usage: run-artifact.sh <label> -- <command...>" >&2; exit 2; }

ART_DIR=".claude/overnight/artifacts"
mkdir -p "$ART_DIR"
ART="$ART_DIR/$(date +%Y%m%d-%H%M%S)-${LABEL//[^a-zA-Z0-9_-]/_}.log"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# IN-FLIGHT MARKER, written BEFORE the command runs. A backgrounded launch
# (`( run-artifact.sh ... ) &`, which the ship sequence requires for anything
# over the 10-minute tool wall) returns to the harness the moment the SUBSHELL
# forks, so the tool result reads `completed (exit code 0)` while the suite is
# still running -- observed 2026-08-08, where the same run's envelope later said
# `FAIL 1/1293`, and the exit code was the one the notification surfaced.
#
# Nothing in this repo can change what the harness reports, so the fix is to
# make the SINK able to say "not finished": `state` is `running` until the
# command returns, and `exit` is null in that window. A reader that treats
# non-zero as failure therefore fails CLOSED on an unfinished run instead of
# reading a stale green from the previous one.
python3 - "$LABEL" "$ART" <<'PY'
import json, os, sys, time
try:
    os.makedirs(".claude/state", exist_ok=True)
    with open(".claude/state/last-artifact.json", "w") as fh:
        fh.write(json.dumps({
            "label": sys.argv[1], "artifact": sys.argv[2],
            "state": "running", "exit": None,
            "started_ns": time.time_ns(), "pid": os.getppid(),
        }))
except Exception:
    pass
PY

"$@" > "$ART" 2>&1
RC=$?

ls -1t "$ART_DIR"/*.log 2>/dev/null | tail -n +41 | while IFS= read -r old; do
  rm -f "$old"
done

python3 - "$LABEL" "$RC" "$ART" <<'PY'
import hashlib, json, os, re, sys
label, rc, art = sys.argv[1], int(sys.argv[2]), sys.argv[3]
data = open(art, "rb").read()
text = data.decode("utf-8", "replace")
ansi = re.compile(r"\x1b\[[0-9;]*m")
lines = [ansi.sub("", ln) for ln in text.splitlines()]
err_re = re.compile(r"(?i)\b(error|fail(ed|ure)?|fatal|panic|assert)\b")
errors = [ln.strip()[:300] for ln in lines if err_re.search(ln)][:40]
env = {
    "label": label, "exit": rc, "artifact": art,
    # Pairs with the `running` record written before the command: a reader can
    # tell "finished, and this is the verdict" from "still going, the exit code
    # you were handed belongs to the launcher, not the work".
    "state": "complete",
    "sha256": hashlib.sha256(data).hexdigest(),
    "lines": len(lines), "errors": errors,
    "tail": [ln[:300] for ln in lines[-8:]],
}
print(json.dumps(env, indent=1))
# I1: authoritative, un-maskable envelope sink. A caller that pipes this script
# into `| tail`/`| head` loses the real exit from the Bash tool's view; a
# receipt / verification step reads THIS file's `exit` field instead of trusting
# the pipeline's last-stage status.
try:
    os.makedirs(".claude/state", exist_ok=True)
    with open(".claude/state/last-artifact.json", "w") as fh:
        fh.write(json.dumps(env))
except Exception:
    pass
PY

# I1: seed the failure ledger on a nonzero result so recurring build/test/smoke
# failures are detected instead of re-derived (the ledger was never written into
# the flow before). Best-effort; absolute path so it works regardless of CWD.
if [ "$RC" -ne 0 ] && [ -f "$SCRIPT_DIR/failure-ledger.py" ]; then
  python3 "$SCRIPT_DIR/failure-ledger.py" record "$LABEL" < "$ART" >/dev/null 2>&1 || true
fi

exit "$RC"
