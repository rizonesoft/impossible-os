#!/usr/bin/env python3
"""The control-plane commit gate judges the commit, not the working tree.

WHY (2026-11-13): .githooks/pre-commit ran the runner suite in the primary tree,
so unstaged tests for the next push refused a commit that was correct on its own,
and an unstaged repair would have let a broken commit land. The gate now runs the
suite through .githooks/lib/staged-tree.sh over an export of the commit's index.

Real git, real commits: a fixture repository whose pre-commit hook sources the
real helper and runs a suite that passes only when marker.txt says "fixed".
"""
from __future__ import annotations

import os
import pathlib
import signal
import subprocess
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
HELPER = REPO / ".githooks/lib/staged-tree.sh"

HOOK = f"""#!/usr/bin/env bash
export FIXTURE_HOOK_PID=$$
. "{HELPER}"
run_on_staged_tree suite.sh
"""
# The suite reads the tree it runs in, and asks git about history the way the
# runner's git-oracle tests do.
SUITE = """set -e
if [ -f hang ]; then
  [ -f stubborn ] && trap '' TERM
  setsid sleep 60 &
  echo "$$ $FIXTURE_HOOK_PID $!" > "$(cat hang)"
  sleep 60
fi
grep -qx fixed marker.txt
grep -qx ok dep.txt
git log -1 --format=%s >/dev/null
"""


def _git(root, *args):
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args],
                          cwd=str(root), capture_output=True, text=True, timeout=60, env=env)


def _fixture(d: pathlib.Path) -> pathlib.Path:
    root = d / "repo"
    root.mkdir()
    _git(root, "init", "-q")
    (root / "suite.sh").write_text(SUITE)
    (root / "marker.txt").write_text("fixed\n")
    (root / "dep.txt").write_text("ok\n")
    assert _git(root, "add", ".").returncode == 0
    assert _git(root, "commit", "-q", "-m", "seed").returncode == 0
    hooks = d / "hooks"
    hooks.mkdir()
    (hooks / "pre-commit").write_text(HOOK)
    (hooks / "pre-commit").chmod(0o755)
    assert _git(root, "config", "core.hooksPath", str(hooks)).returncode == 0
    return root


def _commit(root, staged: str, on_disk: str, *, paths: bool):
    """Stage `staged` as marker.txt, leave `on_disk` in the working tree, commit."""
    (root / "marker.txt").write_text(staged + "\n")
    _git(root, "add", "marker.txt")
    (root / "marker.txt").write_text(on_disk + "\n")
    args = ["commit", "-q", "--allow-empty", "-m", f"{staged} staged"]
    if paths:  # a path-limited commit: git stages into a temporary index
        (root / "marker.txt").write_text(staged + "\n")
        _git(root, "add", "marker.txt")
        args += ["--", "marker.txt"]
    return _git(root, *args)


def test_a_broken_commit_with_a_fixed_working_tree_is_refused():
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(pathlib.Path(d))
        r = _commit(root, "broken", "fixed", paths=False)
        assert r.returncode != 0, r.stdout + r.stderr
        assert _git(root, "log", "-1", "--format=%s").stdout.strip() == "seed"


def test_a_correct_commit_with_a_broken_working_tree_lands():
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(pathlib.Path(d))
        r = _commit(root, "fixed", "broken", paths=False)
        assert r.returncode == 0, r.stdout + r.stderr
        assert _git(root, "log", "-1", "--format=%s").stdout.strip() == "fixed staged"


def test_a_path_limited_commit_is_judged_by_its_own_index():
    """git commit -- marker.txt builds a temporary index from HEAD plus the named
    path; the ordinary index, holding a staged change the commit leaves out, must
    not decide it, in either direction."""
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(pathlib.Path(d))
        (root / "dep.txt").write_text("broken\n")       # staged, left out of the commit
        _git(root, "add", "dep.txt")
        (root / "marker.txt").write_text("fixed\n")
        r = _git(root, "commit", "-q", "--allow-empty", "-m", "path", "--", "marker.txt")
        assert r.returncode == 0, r.stdout + r.stderr
        assert _git(root, "show", "HEAD:dep.txt").stdout == "ok\n"
        assert _git(root, "diff", "--cached", "--name-only").stdout.split() == ["dep.txt"]

        _git(root, "reset", "-q", "dep.txt")
        (root / "dep.txt").write_text("ok\n")
        (root / "marker.txt").write_text("fixed\n")
        _git(root, "add", "marker.txt")                 # the ordinary index would pass
        (root / "marker.txt").write_text("broken\n")   # the path commit takes this
        r = _git(root, "commit", "-q", "-m", "path broken", "--", "marker.txt")
        assert r.returncode != 0, r.stdout + r.stderr
        assert _git(root, "log", "-1", "--format=%s").stdout.strip() == "path"


def _hang(d: pathlib.Path, *, stubborn=False, **extra):
    """Start a commit whose suite hangs; returns (process, tmp, suite, hook, grandchild).

    The suite reports itself, the hook that started it, and a grandchild in a
    session of its own, which a signal to the suite's process group misses."""
    tmp = d / "tmp"
    tmp.mkdir()
    root = _fixture(d)
    pidfile = d / "suite.pid"
    (root / "hang").write_text(str(pidfile))
    _git(root, "add", "hang")
    if stubborn:
        (root / "stubborn").write_text("")
        _git(root, "add", "stubborn")
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(TMPDIR=str(tmp), **extra)
    p = subprocess.Popen(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                          "commit", "-q", "-m", "hang"], cwd=str(root), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    deadline = time.monotonic() + 30
    while not pidfile.exists() or len(pidfile.read_text().split()) < 3:
        if time.monotonic() > deadline or p.poll() is not None:
            _reap(p)
            raise AssertionError("the suite never started")
        time.sleep(0.1)
    suite, hook, grandchild = map(int, pidfile.read_text().split())
    return p, tmp, (suite, hook, grandchild)


def _reap(p):
    """Wait for the commit; KILL its group only if it is still there after 30 s
    (a KILL at once would beat the hook's own trap, which is what is tested)."""
    try:
        p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait(timeout=30)


def _all_gone(tmp, pids, within=30):
    deadline = time.monotonic() + within
    while (any(map(_alive, pids)) or list(tmp.iterdir())) and time.monotonic() < deadline:
        time.sleep(0.1)
    alive = [pid for pid in pids if _alive(pid)]
    for pid in alive:  # never leave a fixture process behind, even on failure
        os.kill(pid, signal.SIGKILL)
    assert not alive, f"outlived the commit: {alive}"
    assert list(tmp.iterdir()) == [], list(tmp.iterdir())


def test_an_interrupted_commit_leaves_no_tree_and_no_suite():
    """Ctrl-C at a terminal: the whole foreground group gets the signal."""
    with tempfile.TemporaryDirectory() as d:
        p, tmp, (suite, _, grandchild) = _hang(pathlib.Path(d))
        try:
            os.killpg(p.pid, signal.SIGTERM)
        finally:
            _reap(p)
        _all_gone(tmp, (suite, grandchild))


def test_a_signal_to_the_hook_alone_stops_the_suite():
    with tempfile.TemporaryDirectory() as d:
        p, tmp, (suite, hook, grandchild) = _hang(pathlib.Path(d))
        try:
            os.kill(hook, signal.SIGTERM)
            _all_gone(tmp, (suite, grandchild))
        finally:
            _reap(p)


def test_a_suite_past_its_deadline_is_stopped_even_if_it_ignores_term():
    with tempfile.TemporaryDirectory() as d:
        p, tmp, (suite, _, grandchild) = _hang(pathlib.Path(d), stubborn=True,
                                               STAGED_TREE_TIMEOUT="3", STAGED_TREE_GRACE="1")
        try:
            assert p.wait(timeout=60) != 0, "a suite past its deadline let the commit land"
        finally:
            _reap(p)
        _all_gone(tmp, (suite, grandchild))


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def test_the_scratch_tree_is_removed():
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d) / "tmp"
        tmp.mkdir()
        root = _fixture(pathlib.Path(d))
        os.environ["TMPDIR"], saved = str(tmp), os.environ.get("TMPDIR")
        try:
            _commit(root, "fixed", "fixed", paths=False)
            _commit(root, "broken", "fixed", paths=False)
        finally:
            if saved is None:
                os.environ.pop("TMPDIR")
            else:
                os.environ["TMPDIR"] = saved
        assert list(tmp.iterdir()) == []


if __name__ == "__main__":
    test_a_broken_commit_with_a_fixed_working_tree_is_refused()
    test_a_correct_commit_with_a_broken_working_tree_lands()
    test_a_path_limited_commit_is_judged_by_its_own_index()
    test_an_interrupted_commit_leaves_no_tree_and_no_suite()
    test_a_signal_to_the_hook_alone_stops_the_suite()
    test_a_suite_past_its_deadline_is_stopped_even_if_it_ignores_term()
    test_the_scratch_tree_is_removed()
    print("PASS: staged-tree gate")
