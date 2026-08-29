#!/usr/bin/env python3
"""skill_step_observer step 19 (the section commit) records only a commit into
THIS project (v17 close-out, 2026-08-29).

Observed live: a multi-line Bash call set up a throwaway repo under /tmp and
committed into it; the step-19 rule (`\\bgit\\s+commit\\b` over the whole
text) matched, the observer recorded the call's FIRST line as the section's
terminal-commit evidence, and the next tool call was blocked for skipping
steps that had simply not happened yet. skill_step_block already refuses to
GATE on such a commit; the observer must not RECORD it either.
"""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
HOOKS = HERE.parent.parent.parent / ".claude/hooks"
sys.path.insert(0, str(HOOKS))
import skill_step_observer as sso  # noqa: E402


def test_scratch_repo_commit_is_not_step_19():
    with tempfile.TemporaryDirectory() as d:
        subprocess.run(["git", "init", "-q", d], check=True)
        for cmd in (f"cd {d} && rm -rf x && mkdir x\ncd {d} && git commit -q --allow-empty -m x",
                    f"git -C {d} commit -q --allow-empty -m x"):
            assert not sso._step19_commit_is_ours({"command": cmd}), cmd


def test_no_commit_at_all_is_not_step_19():
    assert not sso._step19_commit_is_ours(
        {"command": "cd /tmp && rm -rf s59seed && mkdir s59seed"})


def test_project_commit_is_still_step_19():
    """Refusal direction: the real section commit must still be observed."""
    assert sso._step19_commit_is_ours(
        {"command": "git commit -m 'boot: x' -- src/a.c"})
    assert sso._step19_commit_is_ours(
        {"command": "SKIP_REVIEW_HOOK=1 git commit -m 'todo: stamp' -- todo/x.md"})


if __name__ == "__main__":
    test_scratch_repo_commit_is_not_step_19()
    test_no_commit_at_all_is_not_step_19()
    test_project_commit_is_still_step_19()
    print("PASS: step-19 scope")
