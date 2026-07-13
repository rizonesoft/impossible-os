#!/usr/bin/env python3
# block-via: warning-only (PreToolUse reminder; never blocks -- a false positive
# must never wedge a live run, and legit `| python3`/`| jq` envelope parsing
# stays allowed)
"""PreToolUse (Bash): warn when run-artifact.sh is piped into an exit-MASKING
consumer.

`run-artifact.sh` already `exit "$RC"`s with the child's real status and emits a
BOUNDED JSON envelope (verdict + error lines + tail). But callers routinely pipe
it -- `run-artifact.sh build -- bash scripts/build.sh 2>&1 | tail -20` -- and the
Bash-tool-visible exit code is then the LAST pipeline stage's (tail = 0), NOT the
build's. Measured (I1, run-20260713-120304.log:185): a real compile failure at
exit 1 surfaced as a green tool result and never raised a Bash error. A broken
build passing silently is the highest-risk failure this hook prevents.

Fires when the command contains `run-artifact.sh` AND pipes it into a
truncation/filter stage (tail/head/grep/sed/awk/cut/wc/less/more/column/sort/
uniq) AND does NOT guard the pipeline (`set -o pipefail` or `${PIPESTATUS[0]}`).
It does NOT fire on `| python3`/`| jq` (legit envelope parsing -- those read the
`exit` field) or on a guarded pipeline. Warning-only + fail-open by design; the
authoritative fix is the un-maskable `.claude/state/last-artifact.json` sink the
wrapper now writes -- this hook just steers callers to the standalone shape.
"""
from __future__ import annotations

import json
import re
import sys

# Truncation / filter stages that DISCARD the envelope and mask the exit code.
# python3/jq are deliberately excluded: piping into them is legitimate envelope
# parsing (they read `.exit`), not exit-masking.
_MASK_RE = re.compile(
    r"\|\s*(?:sudo\s+)?(?:tail|head|grep|egrep|fgrep|sed|awk|cut|wc|less|more|"
    r"column|sort|uniq|fold|tr|xargs)\b")

_MSG = (
    "[artifact-pipe -- not a block] `run-artifact.sh` is piped into a "
    "truncation/filter stage, so the Bash-tool exit code becomes that stage's "
    "(e.g. `tail` = 0), MASKING a real build/test failure. Run it STANDALONE -- "
    "its JSON envelope is already bounded (verdict + error lines + tail) and it "
    "exits with the child's real status. If you must pipe, guard it "
    "(`set -o pipefail` or check `${PIPESTATUS[0]}`), or read the authoritative "
    "`.claude/state/last-artifact.json` `exit` field. A masked nonzero is how a "
    "broken build passes silently (I1)."
)


def _should_warn(cmd: str) -> bool:
    if "run-artifact.sh" not in cmd:
        return False
    if not _MASK_RE.search(cmd):
        return False
    # A guarded pipeline propagates the real status -- do not nag it.
    if "pipefail" in cmd or "PIPESTATUS" in cmd:
        return False
    return True


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    cmd = (d.get("tool_input") or {}).get("command") or ""
    if not _should_warn(cmd):
        return 0
    print(json.dumps({"systemMessage": _MSG}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
