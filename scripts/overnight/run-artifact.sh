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

"$@" > "$ART" 2>&1
RC=$?

ls -1t "$ART_DIR"/*.log 2>/dev/null | tail -n +41 | while IFS= read -r old; do
  rm -f "$old"
done

python3 - "$LABEL" "$RC" "$ART" <<'PY'
import hashlib, json, re, sys
label, rc, art = sys.argv[1], int(sys.argv[2]), sys.argv[3]
data = open(art, "rb").read()
text = data.decode("utf-8", "replace")
ansi = re.compile(r"\x1b\[[0-9;]*m")
lines = [ansi.sub("", ln) for ln in text.splitlines()]
err_re = re.compile(r"(?i)\b(error|fail(ed|ure)?|fatal|panic|assert)\b")
errors = [ln.strip()[:300] for ln in lines if err_re.search(ln)][:40]
print(json.dumps({
    "label": label, "exit": rc, "artifact": art,
    "sha256": hashlib.sha256(data).hexdigest(),
    "lines": len(lines), "errors": errors,
    "tail": [ln[:300] for ln in lines[-8:]],
}, indent=1))
PY
exit "$RC"
