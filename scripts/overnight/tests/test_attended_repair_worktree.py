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


def _guard_in(root, command="git add -A"):
    env = dict(os.environ, CLAUDE_PROJECT_DIR=str(root))
    env.pop("OVERNIGHT_SEQUENCER_RUN", None)
    env.pop("ATTENDED_REPAIR_OVERRIDE", None)
    payload = '{"tool_name":"Bash","tool_input":{"command":"%s"}}' % command
    return subprocess.run([sys.executable, str(HOOK)], input=payload, env=env,
                          cwd=str(root), capture_output=True, text=True, timeout=30)


def test_live_needs_the_armed_marker_as_well_as_active_state():
    """2026-09-29: a deadline stop left `active: true` with the marker gone, and
    the guard blocked git verbs for hours with nothing running. Live now means
    active state AND the armed marker; the refusal direction must still hold."""
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        state = root / ".claude/state"
        state.mkdir(parents=True)
        (state / "sequencer-run.json").write_text(
            '{"active": true, "phase": "SECTIONS", "file": "todo/x.md", "section_idx": 3}')
        stale = _guard_in(root)
        assert stale.returncode == 0, ("stale active state without a marker must not block",
                                       stale.stderr)
        (state / "sequencer-armed").write_text("")
        live = _guard_in(root)
        assert live.returncode == 2 and "attended-repair BLOCK" in live.stderr, \
            ("a live run (active + marker) must still block git add -A", live.returncode, live.stderr)
        scoped = _guard_in(root, "git add todo/a.md")
        assert scoped.returncode == 0, ("explicit paths stay allowed while live", scoped.stderr)
        (state / "sequencer-run.json").write_text('{"active": false}')
        cleared = _guard_in(root)
        assert cleared.returncode == 0, ("cleared state with a marker must not block", cleared.stderr)


if __name__ == "__main__":
    test_resolves_primary_from_the_primary()
    test_resolves_primary_from_a_linked_worktree()
    test_guard_is_still_inert_with_no_active_run()
    test_live_needs_the_armed_marker_as_well_as_active_state()
    print("PASS: attended-repair guard resolves the primary worktree")
