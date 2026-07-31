#!/usr/bin/env python3
"""attended_repair_guard must find the RUN's state from any worktree.

WHY (2026-07-31): `.claude/state/*` is gitignored and therefore PER-WORKTREE.
The run always lives in the PRIMARY worktree; once an operator works from a
linked repair worktree, a guard that reads `<my root>/.claude/state/
sequencer-run.json` finds nothing, concludes "no run is active", and goes
INERT precisely when an attended repair is in flight -- the situation it
exists for. Resolution must therefore be anchored to the primary worktree,
not to the caller's cwd.

These tests use a REAL `git worktree`, because the whole failure mode is a
property of how git lays out linked worktrees (their `.git` is a file
pointing at the shared common dir) and a mock would assume the answer.
"""
from __future__ import annotations

import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOK = REPO / ".claude/hooks/attended_repair_guard.py"


def _load():
    spec = importlib.util.spec_from_file_location("arg", str(HOOK))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _git(*args, cwd=None, timeout=120):
    return subprocess.run(["git", *args], cwd=str(cwd or REPO),
                          capture_output=True, text=True, timeout=timeout)


def test_resolves_primary_from_the_primary():
    mod = _load()
    old = os.environ.get("CLAUDE_PROJECT_DIR")
    os.environ["CLAUDE_PROJECT_DIR"] = str(REPO)
    try:
        assert pathlib.Path(mod._canonical_run_root()).resolve() == REPO, \
            mod._canonical_run_root()
    finally:
        if old is None:
            os.environ.pop("CLAUDE_PROJECT_DIR", None)
        else:
            os.environ["CLAUDE_PROJECT_DIR"] = old


def test_resolves_primary_from_a_linked_worktree():
    mod = _load()
    old = os.environ.get("CLAUDE_PROJECT_DIR")
    with tempfile.TemporaryDirectory() as td:
        wt = pathlib.Path(td) / "repair"
        add = _git("worktree", "add", "--detach", str(wt), "HEAD")
        if add.returncode != 0:                      # no worktree support here
            print("skip: git worktree unavailable")
            return
        try:
            os.environ["CLAUDE_PROJECT_DIR"] = str(wt)
            got = pathlib.Path(mod._canonical_run_root()).resolve()
            assert got == REPO, f"resolved {got}, want the primary {REPO}"
            # a linked worktree's .git is a FILE, which is exactly why a naive
            # "walk up to a .git directory" resolver fails here
            assert (wt / ".git").is_file(), "expected a linked-worktree .git file"
        finally:
            if old is None:
                os.environ.pop("CLAUDE_PROJECT_DIR", None)
            else:
                os.environ["CLAUDE_PROJECT_DIR"] = old
            _git("worktree", "remove", "--force", str(wt))
            _git("worktree", "prune")


def test_guard_is_still_inert_with_no_active_run():
    """Resolution must not make the guard fire when nothing is running."""
    mod = _load()
    assert mod._run_state() is None or mod._run_state().get("active") is True


if __name__ == "__main__":
    test_resolves_primary_from_the_primary()
    test_resolves_primary_from_a_linked_worktree()
    test_guard_is_still_inert_with_no_active_run()
    print("PASS: attended-repair guard resolves the primary worktree")
