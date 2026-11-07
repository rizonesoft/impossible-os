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
# Per-label verdicts, because last-artifact.json holds only the newest step and
# `receipts.py record-rollover` must read all three J1 legs. One file per label, so a
# wrapper can never read and rewrite another label's result. Marked running first, so
# an unfinished or killed step can never read as its previous green; if that mark
# cannot be written, the old verdict goes rather than stands.
path = os.path.join(".claude/state/artifact-verdicts",
                    "".join(c if c.isalnum() or c in "-_" else "_" for c in sys.argv[1]) + ".json")
try:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "w") as fh:
        json.dump({"state": "running", "exit": None, "epoch": int(time.time())}, fh)
    os.replace(tmp, path)
except Exception:
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass  # fail-direction: nothing to remove, and no record refuses at rollover
    except OSError:
        # A J1 leg must not run while its old green stands: a failure here could not
        # be recorded either, and that green would then pass `record-rollover`.
        sys.exit(3 if sys.argv[1].startswith("j1-") else 0)
PY
if [ $? -eq 3 ]; then
  echo "run-artifact: cannot clear the previous $LABEL verdict; refusing to run it" >&2
  exit 3
fi

"$@" > "$ART" 2>&1
RC=$?

ls -1t "$ART_DIR"/*.log 2>/dev/null | tail -n +41 | while IFS= read -r old; do
  rm -f "$old"
done

RUN_ARTIFACT_DIR="$SCRIPT_DIR" python3 - "$LABEL" "$RC" "$ART" <<'PY'
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
path = os.path.join(".claude/state/artifact-verdicts",
                    "".join(c if c.isalnum() or c in "-_" else "_" for c in label) + ".json")
try:
    sys.path.insert(0, os.environ["RUN_ARTIFACT_DIR"])
    from pathlib import Path
    import receipts, time
    record = {"state": "complete", "exit": rc, "epoch": int(time.time()),
              "build_input_key": receipts.build_input_key(Path(".").resolve()),
              "image_key": receipts.image_key(Path(".").resolve())}
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "w") as fh:
        json.dump(record, fh)
    os.replace(tmp, path)
except Exception:
    pass  # fail-direction: the record still reads `running`, which refuses at rollover
PY

# I1: seed the failure ledger on a nonzero result so recurring build/test/smoke
# failures are detected instead of re-derived (the ledger was never written into
# the flow before). Best-effort; absolute path so it works regardless of CWD.
if [ "$RC" -ne 0 ] && [ -f "$SCRIPT_DIR/failure-ledger.py" ]; then
  python3 "$SCRIPT_DIR/failure-ledger.py" record "$LABEL" < "$ART" >/dev/null 2>&1 || true
fi

exit "$RC"
