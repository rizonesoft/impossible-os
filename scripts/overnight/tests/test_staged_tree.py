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
. "{HELPER}"
run_on_staged_tree suite.sh
"""
# The suite reads the tree it runs in, and asks git about history the way the
# runner's git-oracle tests do.
SUITE = """set -e
if [ -f hang ]; then echo $$ > "$(cat hang)"; sleep 60; fi
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


def test_an_interrupted_commit_leaves_no_tree_and_no_suite():
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d) / "tmp"
        tmp.mkdir()
        root = _fixture(pathlib.Path(d))
        pidfile = pathlib.Path(d) / "suite.pid"
        (root / "hang").write_text(str(pidfile))
        _git(root, "add", "hang")
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        env["TMPDIR"] = str(tmp)
        p = subprocess.Popen(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                              "commit", "-q", "-m", "hang"], cwd=str(root), env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            deadline = time.monotonic() + 30
            while not pidfile.exists() or not pidfile.read_text().strip():
                assert time.monotonic() < deadline and p.poll() is None, "the suite never started"
                time.sleep(0.1)
            suite = int(pidfile.read_text())
            os.killpg(p.pid, signal.SIGTERM)
            p.wait(timeout=30)
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait(timeout=30)
        deadline = time.monotonic() + 10
        while _alive(suite) and time.monotonic() < deadline:
            time.sleep(0.1)
        assert not _alive(suite), "the suite outlived the interrupted commit"
        # git does not wait for its hook on SIGTERM, so the hook may still be removing it.
        while list(tmp.iterdir()) and time.monotonic() < deadline + 10:
            time.sleep(0.1)
        assert list(tmp.iterdir()) == [], list(tmp.iterdir())


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
    test_the_scratch_tree_is_removed()
    print("PASS: staged-tree gate")
