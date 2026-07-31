#!/usr/bin/env python3
"""segment-sync.sh: pull operator repairs at a segment start, and never break the run.

Every assertion here is a clause of the script's contract, and each one exists
because the alternative is a failure nobody would be watching:

  - exit 0 ALWAYS, so a sync problem can never stop a segment from starting
  - SKIP on a dirty tree, so a crashed segment's WIP is never pulled over
  - --ff-only, so a diverged main is reported rather than silently merged by an
    unattended run at 03:00
  - one descriptive line, so "did this segment pick up my fix?" is a grep

Driven against REAL git repositories (a bare origin plus two clones) because
the whole point is git's fast-forward semantics; a mock would assume the answer.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

SYNC = pathlib.Path(__file__).resolve().parents[1] / "segment-sync.sh"


def _run(*args, cwd=None):
    return subprocess.run(list(args), cwd=cwd, capture_output=True,
                          text=True, timeout=120)


def _git(*args, cwd):
    r = _run("git", *args, cwd=cwd)
    assert r.returncode == 0, f"git {' '.join(args)} failed: {r.stderr}"
    return r.stdout.strip()


def _sync(project):
    r = _run("bash", str(SYNC), str(project))
    assert r.returncode == 0, f"segment-sync must ALWAYS exit 0, got {r.returncode}"
    return r.stdout.strip()


def _fixture(td):
    """A bare origin, an 'operator' clone, and the 'run' clone under test."""
    root = pathlib.Path(td)
    origin = root / "origin.git"
    _run("git", "init", "--bare", "-b", "main", str(origin))
    op = root / "operator"
    _git("clone", str(origin), str(op), cwd=root)
    _git("config", "user.email", "t@t", cwd=op)
    _git("config", "user.name", "t", cwd=op)
    (op / "seed.txt").write_text("seed\n")
    _git("add", "seed.txt", cwd=op)
    _git("commit", "-m", "seed", cwd=op)
    _git("push", "origin", "main", cwd=op)
    run = root / "run"
    _git("clone", str(origin), str(run), cwd=root)
    _git("config", "user.email", "t@t", cwd=run)
    _git("config", "user.name", "t", cwd=run)
    return origin, op, run


def test_picks_up_an_operator_repair():
    with tempfile.TemporaryDirectory() as td:
        _, op, run = _fixture(td)
        before = _git("rev-parse", "--short", "HEAD", cwd=run)
        (op / "hook.py").write_text("# repair\n")
        _git("add", "hook.py", cwd=op)
        _git("commit", "-m", "operator repair", cwd=op)
        _git("push", "origin", "main", cwd=op)
        out = _sync(run)
        after = _git("rev-parse", "--short", "HEAD", cwd=run)
        assert after != before, "the repair was not pulled"
        assert (run / "hook.py").exists(), "repair file missing after sync"
        assert "operator repairs picked up" in out, out


def test_already_current_is_reported_distinctly():
    with tempfile.TemporaryDirectory() as td:
        _, _, run = _fixture(td)
        out = _sync(run)
        assert "already current" in out, out


def test_dirty_tree_is_skipped_not_pulled_over():
    with tempfile.TemporaryDirectory() as td:
        _, op, run = _fixture(td)
        (op / "hook.py").write_text("# repair\n")
        _git("add", "hook.py", cwd=op)
        _git("commit", "-m", "operator repair", cwd=op)
        _git("push", "origin", "main", cwd=op)
        (run / "seed.txt").write_text("crashed segment WIP\n")   # dirty
        out = _sync(run)
        assert "SKIPPED, tree not clean" in out, out
        assert not (run / "hook.py").exists(), "pulled over in-flight WIP"
        assert (run / "seed.txt").read_text() == "crashed segment WIP\n"


def test_diverged_history_fails_loudly_and_never_merges():
    with tempfile.TemporaryDirectory() as td:
        _, op, run = _fixture(td)
        (op / "a.txt").write_text("theirs\n")
        _git("add", "a.txt", cwd=op)
        _git("commit", "-m", "theirs", cwd=op)
        _git("push", "origin", "main", cwd=op)
        (run / "b.txt").write_text("ours\n")                     # local divergence
        _git("add", "b.txt", cwd=run)
        _git("commit", "-m", "ours", cwd=run)
        out = _sync(run)
        assert "FAILED" in out, out
        log = _git("log", "--oneline", "-5", cwd=run)
        assert "Merge" not in log, f"unattended merge created:\n{log}"


def test_non_git_directory_is_survivable():
    with tempfile.TemporaryDirectory() as td:
        out = _sync(td)
        assert "not a git checkout" in out, out


if __name__ == "__main__":
    test_picks_up_an_operator_repair()
    test_already_current_is_reported_distinctly()
    test_dirty_tree_is_skipped_not_pulled_over()
    test_diverged_history_fails_loudly_and_never_merges()
    test_non_git_directory_is_survivable()
    print("PASS: segment-sync (pull at segment start, never break the run)")
