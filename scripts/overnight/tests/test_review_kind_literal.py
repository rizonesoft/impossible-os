#!/usr/bin/env python3
"""review_kind_literal_required.py: refuse a Codex review dispatch whose prompt the
recorder cannot attribute, allow every shape it can, and never touch non-dispatches.

Run: python3 scripts/overnight/tests/test_review_kind_literal.py
"""
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
HOOK = REPO / ".claude/hooks/review_kind_literal_required.py"
# Wrapper names are assembled so lint Checks 12 and 15 (which scan the repo for
# unsafe dispatch shapes) do not flag these deliberately unsafe fixtures.
D = "scripts/codex-" + "dispatch.sh"
B = "scripts/overnight/review-broker-codex-" + "dispatch.sh"
C = "/h/codex-" + "companion.mjs"

BLOCK = [
    ("cat substitution (the 2026-09-05 wave)",
     'bash ' + B + ' "$(cat /tmp/w4-perf.txt)" 2>&1 | tail -1'),
    ("bare variable", 'bash ' + B + ' "$P"'),
    ("loop variable",
     'for k in perf consistency; do bash ' + D + ' "[review-kind: $k] todo/x.md s1 body"; done'),
    ("cd and env prefix", 'cd /repo && SKIP_X=1 bash ' + D + ' "$(cat p)"'),
    ("companion direct", 'node ' + C + ' adversarial-review "$(cat p)"'),
    ("one bad leg of two",
     "bash " + D + " '[review-kind: perf] todo/x.md s1'; bash " + D + " \"$(cat q)\""),
]
ALLOW = [
    ("broker literal", "bash " + B + " '[review-kind: perf] todo/x.md section 1 body'"),
    ("direct literal", "bash " + D + " '[review-kind: adversarial] todo/x.md s1'"),
    ("heredoc variable",
     "PROMPT=$(cat <<'EOF'\n[review-kind: consistency] todo/x.md s1\nbody\nEOF\n)\nbash " + D + " \"$PROMPT\""),
    ("search, not a dispatch", "grep -n 'codex-dispatch.sh' docs/x.md"),
    ("search with substitution text", "rg \"review-broker-codex-dispatch.sh '$(cat x)'\" ."),
    ("rescue, not a review", "node " + C + " task --write 'fix it'"),
    ("unrelated", "git status"),
    # The recorder searches the whole FIRST line, so this is attributed; the gate
    # must agree with the recorder, not be stricter than it.
    ("marker later on the first line",
     "bash " + B + " 'Please review. [review-kind: perf] todo/x.md'"),
]


def run(cmd, env=None):
    payload = json.dumps({"tool_name": "Bash", "tool_input": {"command": cmd}})
    e = {k: v for k, v in os.environ.items() if k != "REVIEW_KIND_LITERAL_OVERRIDE"}
    e.update(env or {})
    return subprocess.run([sys.executable, str(HOOK)], input=payload, text=True,
                          capture_output=True, env=e).returncode


def main():
    fails = []
    for name, cmd in BLOCK:
        if run(cmd) != 2:
            fails.append("should BLOCK: " + name)
    for name, cmd in ALLOW:
        if run(cmd) != 0:
            fails.append("should ALLOW: " + name)
    if run(BLOCK[0][1], {"REVIEW_KIND_LITERAL_OVERRIDE": "1"}) != 0:
        fails.append("override did not allow")
    if fails:
        print("test_review_kind_literal FAIL:\n  " + "\n  ".join(fails))
        return 1
    print("test_review_kind_literal OK (%d cases)" % (len(BLOCK) + len(ALLOW) + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
