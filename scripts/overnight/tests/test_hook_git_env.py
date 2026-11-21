#!/usr/bin/env python3
"""A test suite run from a git hook must not reach the real repository.

WHY (2026-11-09): pushing from a linked repair worktree, git exported an absolute
GIT_DIR to .githooks/pre-push. The site freshness fixture builds a throwaway repo with
`dict(os.environ, ...)` and `git init`, so it re-initialised the REAL repository as
bare and committed 28 fixture commits onto the worktree's HEAD under t@example.invalid.
.githooks/lib/isolate-git-env.sh drops those variables before a suite runs.

Real git, real worktree: the failure is a property of what git exports to a hook.
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
HELPER = REPO / ".githooks/lib/isolate-git-env.sh"

# What the site fixture does: a fresh repo in a temp dir, environment inherited.
FIXTURE = r'''
import os, subprocess, tempfile
root = tempfile.mkdtemp(prefix="fixture-")
env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
           GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
subprocess.run(["git", "init", "-q"], cwd=root, env=env, check=True)
subprocess.run(["git", "commit", "-q", "--allow-empty", "--no-verify", "-m", "fixture"],
               cwd=root, env=env, check=True)
'''


def _git(*args, cwd):
    return subprocess.run(["git", "-c", "user.name=p", "-c", "user.email=p@p", *args],
                          cwd=str(cwd), capture_output=True, text=True, timeout=60)


def _push_from_worktree(d: pathlib.Path, isolate: bool):
    _git("init", "-q", "--bare", "origin.git", cwd=d)
    main = d / "main"
    _git("init", "-q", str(main), cwd=d)
    _git("commit", "-q", "--allow-empty", "-m", "0", cwd=main)
    _git("remote", "add", "origin", str(d / "origin.git"), cwd=main)
    hooks = d / "hooks"
    hooks.mkdir()
    (d / "fixture.py").write_text(FIXTURE)
    source = f'. "{HELPER}"\nisolate_git_env\n' if isolate else ""
    hook = hooks / "pre-push"
    hook.write_text(f'#!/bin/bash\n{source}python3 "{d}/fixture.py"\nexit 1\n')
    hook.chmod(0o755)
    _git("config", "core.hooksPath", str(hooks), cwd=main)
    wt = d / "wt"
    _git("worktree", "add", "-q", "--detach", str(wt), cwd=main)
    head = _git("rev-parse", "HEAD", cwd=wt).stdout.strip()
    _git("push", "origin", "HEAD:refs/heads/main", cwd=wt)
    bare = _git("config", "--get", "core.bare", cwd=main).stdout.strip()
    return bare, head, _git("rev-parse", "HEAD", cwd=wt).stdout.strip()


def test_the_control_a_hook_without_isolation_damages_the_real_repo():
    with tempfile.TemporaryDirectory() as d:
        bare, before, after = _push_from_worktree(pathlib.Path(d), isolate=False)
        assert bare == "true" or after != before, \
            "the control: without isolation the fixture must reach the real repo"


def test_refusal_an_isolated_hook_leaves_the_real_repo_alone():
    with tempfile.TemporaryDirectory() as d:
        bare, before, after = _push_from_worktree(pathlib.Path(d), isolate=True)
        assert bare == "false", f"core.bare={bare}"
        assert after == before, "a fixture commit landed on the worktree HEAD"


def test_both_hooks_isolate_before_their_suites():
    push = (REPO / ".githooks/pre-push").read_text()
    at = push.index("isolate_git_env\n")
    for suite in ("scripts/test-tooling.sh", "scripts/overnight/tests/run-all.sh",
                  "identity-gate.sh", "scripts/test.sh"):
        assert push.index(suite, at) > at, suite
    # pre-commit runs the suite over the staged tree; the helper isolates it.
    commit = (REPO / ".githooks/pre-commit").read_text()
    assert "run_on_staged_tree scripts/overnight/tests/run-all.sh" in commit, "pre-commit runs run-all.sh directly"
    staged = (REPO / ".githooks/lib/staged-tree.sh").read_text()
    assert staged.index("isolate_git_env\n") < staged.index('bash "$tree/$script"'), "the staged tree runs unisolated"


def _toplevel_seen_from_a_subdirectory(d: pathlib.Path, anchor: bool) -> tuple[str, str]:
    """What `git rev-parse --show-toplevel` answers from sub/ inside a linked worktree's pre-commit hook."""
    main = d / "main"
    _git("init", "-q", str(main), cwd=d)
    _git("commit", "-q", "--allow-empty", "-m", "0", cwd=main)
    hooks = d / "hooks"
    hooks.mkdir()
    source = f'. "{HELPER}"\nanchor_git_env\n' if anchor else ""
    hook = hooks / "pre-commit"
    hook.write_text(f'#!/bin/bash\n{source}mkdir -p sub\ngit -C sub rev-parse --show-toplevel > "{d}/seen"\n')
    hook.chmod(0o755)
    _git("config", "core.hooksPath", str(hooks), cwd=main)
    wt = d / "wt"
    _git("worktree", "add", "-q", "--detach", str(wt), cwd=main)
    _git("commit", "-q", "--allow-empty", "-m", "1", cwd=wt)
    return (d / "seen").read_text().strip(), str(wt.resolve())


def test_the_control_an_unanchored_hook_takes_a_subdirectory_for_the_work_tree():
    with tempfile.TemporaryDirectory() as d:
        seen, wt = _toplevel_seen_from_a_subdirectory(pathlib.Path(d).resolve(), anchor=False)
        assert seen == wt + "/sub", (seen, wt)


def test_an_anchored_hook_keeps_the_work_tree_root_from_a_subdirectory():
    with tempfile.TemporaryDirectory() as d:
        seen, wt = _toplevel_seen_from_a_subdirectory(pathlib.Path(d).resolve(), anchor=True)
        assert seen == wt, (seen, wt)


def _lint_view_from_a_worktree_commit(d: pathlib.Path, anchor: bool) -> dict[str, str]:
    """What a pre-commit lint sees from a linked worktree, for a path-limited commit.

    WHY (2026-11-21): Checks 40 and 41 build a snapshot of the staged tree in a temp
    directory and run git there. The 2026-11-11 anchor pinned GIT_WORK_TREE but kept
    GIT_DIR, and an exported GIT_DIR overrides discovery, so `git -C <snapshot>` read
    the real repository: the TODO graph build refused every worktree commit as "the
    corpus git history moved". The lint needs the commit's index and nothing else.
    """
    main = d / "main"
    _git("init", "-q", str(main), cwd=d)
    (main / "a").write_text("0\n")
    _git("add", "a", cwd=main)
    _git("commit", "-q", "-m", "0", cwd=main)
    snap = d / "snapshot"
    snap.mkdir()
    hooks = d / "hooks"
    hooks.mkdir()
    source = f'. "{HELPER}"\nanchor_git_env\n' if anchor else ""
    hook = hooks / "pre-commit"
    hook.write_text(
        f'#!/bin/bash\n{source}'
        f'git -C "{snap}" rev-parse --git-dir > "{d}/snapshot-git" 2>/dev/null || echo none > "{d}/snapshot-git"\n'
        f'mkdir -p sub\ngit -C sub rev-parse --show-toplevel > "{d}/toplevel"\n'
        f'(cd sub && git diff --cached --name-only) > "{d}/staged"\n')
    hook.chmod(0o755)
    _git("config", "core.hooksPath", str(hooks), cwd=main)
    wt = d / "wt"
    _git("worktree", "add", "-q", "--detach", str(wt), cwd=main)
    (wt / "a").write_text("1\n")
    (wt / "b").write_text("1\n")
    _git("add", "b", cwd=wt)
    _git("commit", "-q", "-m", "1", "--", "a", cwd=wt)  # path-limited: git stages into a temporary index
    return {k: (d / k).read_text().strip() for k in ("snapshot-git", "toplevel", "staged")} | {"wt": str(wt.resolve())}


def test_the_control_a_kept_git_dir_reaches_the_real_repo_from_a_snapshot():
    with tempfile.TemporaryDirectory() as d:
        seen = _lint_view_from_a_worktree_commit(pathlib.Path(d).resolve(), anchor=False)
        assert seen["snapshot-git"] != "none", seen


def test_an_anchored_lint_reads_the_commit_index_and_leaves_snapshots_alone():
    with tempfile.TemporaryDirectory() as d:
        seen = _lint_view_from_a_worktree_commit(pathlib.Path(d).resolve(), anchor=True)
        assert seen["snapshot-git"] == "none", f"a snapshot reached a repository: {seen}"
        assert seen["toplevel"] == seen["wt"], seen
        assert seen["staged"] == "a", f"the lint lost the path-limited commit's index: {seen}"


def test_pre_commit_anchors_before_the_lint():
    commit = (REPO / ".githooks/pre-commit").read_text()
    assert commit.index("\nanchor_git_env\n") < commit.index('bash "$REPO_ROOT/scripts/lint.sh"')


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
