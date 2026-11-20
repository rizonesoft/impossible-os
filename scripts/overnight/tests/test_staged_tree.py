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

# The production hooks run under these options; cleanup must complete under them.
HOOK = f"""#!/usr/bin/env bash
set -euo pipefail
export FIXTURE_HOOK_PID=$$
# git puts its exec path first for a hook; a test's git shim goes ahead of it.
if [ -n "${{FIXTURE_SHIM:-}}" ]; then PATH="$FIXTURE_SHIM:$PATH"; fi
. "{HELPER}"
run_on_staged_tree suite.sh
"""
# The suite reads the tree it runs in, and asks git about history the way the
# runner's git-oracle tests do.
SUITE = f"""set -e
if [ -f nested ]; then
  # A nested supervisor, as this helper's own tests start one inside the gate.
  # Its own TMPDIR: KILLed after the outer grace, it may leave its tree there.
  TMPDIR="$(cat nested)" STAGED_TREE_GRACE=5 bash -c ". '{HELPER}'; run_on_staged_tree inner.sh"
fi
if [ -f hang ]; then
  if [ -f stubborn ]; then trap '' TERM; fi
  if [ -f stubborn-child ]; then setsid bash -c "trap '' TERM; sleep 60" & else setsid sleep 60 & fi
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


def _hang(d: pathlib.Path, *, stubborn=False, stubborn_child=False, **extra):
    """Start a commit whose suite hangs; returns (process, tmp, suite, hook, grandchild).

    The suite reports itself, the hook that started it, and a grandchild in a
    session of its own, which a signal to the suite's process group misses."""
    tmp = d / "tmp"
    tmp.mkdir()
    root = _fixture(d)
    pidfile = d / "suite.pid"
    (root / "hang").write_text(str(pidfile))
    _git(root, "add", "hang")
    for name, on in (("stubborn", stubborn), ("stubborn-child", stubborn_child)):
        if on:
            (root / name).write_text("")
            _git(root, "add", name)
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


def test_a_detached_child_that_ignores_term_is_stopped_under_set_e():
    """The leader dies on TERM, so its group is gone when the tagged child is
    reached: a cleanup aborted by `set -e` at that point left the child alive."""
    with tempfile.TemporaryDirectory() as d:
        p, tmp, (suite, hook, grandchild) = _hang(pathlib.Path(d), stubborn_child=True, STAGED_TREE_GRACE="1")
        try:
            os.kill(hook, signal.SIGTERM)
            _all_gone(tmp, (suite, grandchild))
        finally:
            _reap(p)


# A git that stalls on checkout-index: the export is the stage being tested.
STALL = """#!/usr/bin/env bash
if [ "${1:-}" = checkout-index ]; then echo $$ > "$STALL_PIDFILE"; sleep 60; fi
exec "$REAL_GIT" "$@"
"""


def _stalled_export(d: pathlib.Path, **extra):
    tmp = d / "tmp"
    tmp.mkdir()
    root = _fixture(d)
    shim = d / "shim"
    shim.mkdir()
    (shim / "git").write_text(STALL)
    (shim / "git").chmod(0o755)
    pidfile = d / "export.pid"
    real = subprocess.run(["bash", "-c", "command -v git"], capture_output=True, text=True).stdout.strip()
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(TMPDIR=str(tmp), REAL_GIT=real, STALL_PIDFILE=str(pidfile),
               FIXTURE_SHIM=str(shim), **extra)
    p = subprocess.Popen([real, "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                          "commit", "-q", "--allow-empty", "-m", "stall"], cwd=str(root), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    deadline = time.monotonic() + 30
    while not pidfile.exists() or not pidfile.read_text().strip():
        if time.monotonic() > deadline or p.poll() is not None:
            _reap(p)
            raise AssertionError("the export never started")
        time.sleep(0.1)
    return p, tmp, int(pidfile.read_text())


def test_a_stalled_export_is_bounded_by_the_deadline():
    with tempfile.TemporaryDirectory() as d:
        p, tmp, exporter = _stalled_export(pathlib.Path(d), STAGED_TREE_TIMEOUT="3", STAGED_TREE_GRACE="1")
        try:
            # Deadline 3 s plus grace 1 s; the stalled export alone sleeps 60 s.
            try:
                rc = p.wait(timeout=20)
            except subprocess.TimeoutExpired:
                raise AssertionError("a stalled export outlived the deadline") from None
            assert rc != 0, "a stalled export let the commit land"
        finally:
            _reap(p)
        _all_gone(tmp, (exporter,))


# The nested stage: a child that ignores TERM, in a session of its own.
INNER = """setsid bash -c "trap '' TERM; sleep 60" &
echo "$$ $! $FIXTURE_HOOK_PID" > "$INNER_PIDFILE"
sleep 60
"""


def test_a_nested_stage_is_stopped_with_the_outer_one():
    """The outer grace (1 s) is shorter than the inner one (5 s): the outer
    cleanup reaches the inner supervisor first, so it must see the inner stage's
    processes itself."""
    with tempfile.TemporaryDirectory() as d:
        d = pathlib.Path(d)
        tmp = d / "tmp"
        tmp.mkdir()
        root = _fixture(d)
        inner_tmp = d / "inner-tmp"
        inner_tmp.mkdir()
        for name, text in (("nested", str(inner_tmp)), ("inner.sh", INNER)):
            (root / name).write_text(text)
            _git(root, "add", name)
        pidfile = d / "inner.pid"
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        env.update(TMPDIR=str(tmp), INNER_PIDFILE=str(pidfile), STAGED_TREE_GRACE="1")
        p = subprocess.Popen(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                              "commit", "-q", "-m", "nested"], cwd=str(root), env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            deadline = time.monotonic() + 60
            while not pidfile.exists() or len(pidfile.read_text().split()) < 3:
                assert time.monotonic() < deadline and p.poll() is None, "the nested stage never started"
                time.sleep(0.1)
            inner, child, hook = map(int, pidfile.read_text().split())
            os.kill(hook, signal.SIGTERM)
            _all_gone(tmp, (inner, child))
        finally:
            _reap(p)


def test_the_helper_is_in_the_control_plane():
    """A change to the helper alone must run the suite that tests it."""
    repo = pathlib.Path(__file__).resolve().parents[3]
    for rel in (".githooks/lib/staged-tree.sh", ".githooks/lib/isolate-git-env.sh",
                ".githooks/pre-commit", ".githooks/pre-push"):        # the wiring that runs it, too
        assert (repo / rel).is_file(), rel
        r = subprocess.run(["bash", str(repo / "scripts/overnight/control-plane-match.sh")], input=rel + "\n",
                           capture_output=True, text=True, timeout=30, cwd=str(repo))
        assert r.returncode == 0, f"{rel} is not in the control-plane manifest"


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
    test_a_detached_child_that_ignores_term_is_stopped_under_set_e()
    test_a_stalled_export_is_bounded_by_the_deadline()
    test_a_nested_stage_is_stopped_with_the_outer_one()
    test_the_helper_is_in_the_control_plane()
    test_the_scratch_tree_is_removed()
    print("PASS: staged-tree gate")
