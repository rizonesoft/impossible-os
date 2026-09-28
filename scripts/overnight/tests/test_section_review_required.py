#!/usr/bin/env python3
"""section_review_required.py (post-ship review gate), in a fixture repo whose HEAD
flips an Implementation Order row to [x] with no Verified stamp.

Refusal directions: with no review running, a review started BEFORE the ship
commit, or a review from another session, Bash and non-review skills stay gated.
Allow direction (2026-09-28): once review-todo-section is running for this
session and began after HEAD, the review's own Bash passes (report reads,
mutation controls, waits), as Edit and Write already did.

Run: python3 scripts/overnight/tests/test_section_review_required.py
"""
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
HOOK = REPO / ".claude/hooks/section_review_required.py"
SIGN = "§"   # escaped: the section sign is data here, not a code reference

WAIT = "timeout 540 bash -c 'until grep -q \"^rc=\" /tmp/ship-push.log; do sleep 5; done'"
MUTATE = "cp src/a.c /tmp/a.c.bak && sed -i 's/1/2/' src/a.c"
READ_LOOP = "for f in a b; do sed -n '/^# /,$p' /tmp/$f.out; done"


def git(root, *a):
    subprocess.run(["git", "-C", str(root), *a], check=True, capture_output=True,
                   env={**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                        "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"})


def fixture(d):
    root = Path(d)
    git(root, "init", "-q")
    t = root / "todo/01-x/TODO-01-x.md"
    t.parent.mkdir(parents=True)
    row = "| P | 3 | " + SIGN + "3 Thing | -- | {} |\n"
    t.write_text("## Implementation Order\n\n| T | Order | Deliverable | Depends On | Status |\n"
                 "| --- | --- | --- | --- | --- |\n" + row.format("[ ]"))
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "todo: add the thing")
    t.write_text(t.read_text().replace("[ ] |", "[x] |"))
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "kernel: thing -- section 3")
    (root / ".claude/state").mkdir(parents=True)
    (root / ".claude/state/session.json").write_text(json.dumps({"session_id": "s1"}))
    return root


def review_state(root, started_ns, sid="s1"):
    (root / ".claude/state/skill-progress.json").write_text(json.dumps(
        {"review-todo-section": {"started_ts": started_ns, "session_id": sid}}))


def run(root, tool, tool_input):
    payload = json.dumps({"tool_name": tool, "tool_input": tool_input})
    env = {**os.environ, "OVERNIGHT_SEQUENCER_RUN": "1"}   # this IS the run: always gated
    env.pop("SKIP_REVIEW_HOOK", None)
    return subprocess.run([sys.executable, str(HOOK)], input=payload, text=True,
                          capture_output=True, cwd=root, env=env).returncode


def main():
    fails = []

    def expect(name, got, want):
        if got != want:
            fails.append(f"{name}: rc {got}, want {want}")

    cases = ((WAIT, "push wait"), (MUTATE, "mutation control"), (READ_LOOP, "report read loop"))
    with tempfile.TemporaryDirectory() as d:
        root = fixture(d)
        for cmd, label in cases:
            expect(f"no review running: {label} is gated", run(root, "Bash", {"command": cmd}), 2)
        expect("git stays allowed without a review", run(root, "Bash", {"command": "git status"}), 0)

        review_state(root, time.time_ns() + 5_000_000_000)          # started after HEAD
        for cmd, label in cases:
            expect(f"review running: {label} passes", run(root, "Bash", {"command": cmd}), 0)
        expect("review running: a non-review skill is still gated",
               run(root, "Skill", {"skill": "implement-todo-section"}), 2)

        review_state(root, 1_000_000_000)                            # started long before HEAD
        expect("review started BEFORE the ship commit: gated", run(root, "Bash", {"command": WAIT}), 2)

        review_state(root, time.time_ns() + 5_000_000_000, sid="other")
        expect("another session's review: gated", run(root, "Bash", {"command": WAIT}), 2)

    if fails:
        print("test_section_review_required FAIL:\n  " + "\n  ".join(fails))
        return 1
    print("test_section_review_required OK (11 cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
