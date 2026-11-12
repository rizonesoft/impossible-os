#!/usr/bin/env python3
"""A `git commit` that skipped pre-commit is refused by git itself.

`--no-verify` skips .githooks/pre-commit, the lint and section-commit gate. The harness
layer reads the Bash command to spot it, and four review rounds (2026-11-12) found a new
spelling each time ($'\\x2dn', a heredoc, a NUL, a nested substitution). prepare-commit-msg
still runs under --no-verify and shares pre-commit's git process, so it refuses a commit
whose process pre-commit never recorded, however -n was written.

These run real git in a scratch repo with the real prepare-commit-msg and a pre-commit
stub that records itself through the same library the real one uses.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
STUB_PRE_COMMIT = """#!/usr/bin/env bash
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$REPO_ROOT/.githooks/lib/pre-commit-mark.sh"
mark_pre_commit_ran
"""
STUB_POST_COMMIT_AMEND = """#!/usr/bin/env bash
# As .githooks/post-commit does for COUNT.md, once.
[ -n "${SKIP_COUNT:-}" ] && exit 0
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/pre-commit-mark.sh"
echo counted > COUNT.md && git add COUNT.md
allow_hook_amend
SKIP_COUNT=1 git commit -q --amend --only --no-edit --no-verify -- COUNT.md
"""


def _repo(root: pathlib.Path, post_commit: bool = False) -> pathlib.Path:
    hooks = root / "repo" / ".githooks"
    (hooks / "lib").mkdir(parents=True)
    shutil.copy(REPO / ".githooks/prepare-commit-msg", hooks / "prepare-commit-msg")
    shutil.copy(REPO / ".githooks/lib/pre-commit-mark.sh", hooks / "lib/pre-commit-mark.sh")
    (hooks / "pre-commit").write_text(STUB_PRE_COMMIT)
    if post_commit:
        (hooks / "post-commit").write_text(STUB_POST_COMMIT_AMEND)
    for h in hooks.iterdir():
        h.chmod(0o755)
    repo = root / "repo"
    for args in (["init", "-q", "-b", "main"], ["config", "core.hooksPath", ".githooks"],
                 ["config", "user.name", "t"], ["config", "user.email", "t@example.invalid"],
                 ["config", "alias.ci", "commit"]):
        _git(repo, *args)
    return repo


# Set by the ps-path tests: the hook reads argv from `ps` when this has no cmdline files.
EXTRA_ENV: dict[str, str] = {}


def _git(repo: pathlib.Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")} | EXTRA_ENV
    return subprocess.run(["git", *args], cwd=str(repo), capture_output=True, text=True,
                          timeout=60, check=check, env=env)


def _commit_file(repo: pathlib.Path, name: str, *flags: str) -> subprocess.CompletedProcess:
    (repo / name).write_text(name + "\n")
    _git(repo, "add", name)
    return _git(repo, "commit", "-q", *flags, "-m", name, check=False)


def test_refusal_every_spelling_of_no_verify_is_refused():
    with tempfile.TemporaryDirectory() as d:
        repo = _repo(pathlib.Path(d))
        assert _commit_file(repo, "a").returncode == 0
        for i, flags in enumerate((["--no-verify"], ["-n"], ["-qn"])):
            done = _commit_file(repo, f"nv{i}", *flags)
            assert done.returncode != 0 and "pre-commit did not run" in done.stderr, (flags, done.stderr)
        done = _git(repo, "ci", "-q", "-n", "-m", "alias", check=False)
        assert done.returncode != 0, "an alias for commit is still a commit"
        done = _git(repo, "-c", "x.y=z", "commit", "-q", "--amend", "--no-verify", "-m", "amend", check=False)
        assert done.returncode != 0, "an amend is refused too"
        done = _git(repo, "-c", "alias.commit=status", "commit", "-q", "-n", "-m", "shadow", check=False)
        assert done.returncode != 0, "an alias cannot shadow the commit built-in"
        exec_path = _git(repo, "--exec-path").stdout.strip()
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        (repo / "dashed").write_text("dashed\n")
        _git(repo, "add", "dashed")
        done = subprocess.run([os.path.join(exec_path, "git-commit"), "-q", "-n", "-m", "dashed"],
                              cwd=str(repo), capture_output=True, text=True, timeout=60, env=env)
        assert done.returncode != 0, "git-commit run directly is a commit"


def test_refusal_a_stale_record_from_another_commit_does_not_count():
    with tempfile.TemporaryDirectory() as d:
        repo = _repo(pathlib.Path(d))
        (repo / ".git/pre-commit.ran").write_text("1\n")
        done = _commit_file(repo, "a", "--no-verify")
        assert done.returncode != 0, done.stderr


def test_ordinary_commits_pass():
    with tempfile.TemporaryDirectory() as d:
        repo = _repo(pathlib.Path(d))
        assert _commit_file(repo, "a").returncode == 0
        assert _git(repo, "commit", "-q", "--amend", "-m", "a2", check=False).returncode == 0
        assert _git(repo, "commit", "-q", "--allow-empty", "-m", "empty", check=False).returncode == 0
        _git(repo, "checkout", "-q", "-b", "side")
        _commit_file(repo, "b")
        _commit_file(repo, "c")
        _git(repo, "checkout", "-q", "main")
        _commit_file(repo, "d")
        assert _git(repo, "cherry-pick", "side~1", check=False).returncode == 0, "cherry-pick runs no pre-commit"
        assert _git(repo, "revert", "--no-edit", "HEAD", check=False).returncode == 0, "nor does revert"
        assert _git(repo, "merge", "-q", "--no-ff", "-m", "m", "side", check=False).returncode == 0
        _git(repo, "checkout", "-q", "side")
        assert _git(repo, "rebase", "-q", "main", check=False).returncode == 0


def test_rebase_commits_through_its_sequencer():
    with tempfile.TemporaryDirectory() as d:
        repo = _repo(pathlib.Path(d))
        _commit_file(repo, "a")
        _commit_file(repo, "b")
        _commit_file(repo, "c")
        squash = {"GIT_SEQUENCE_EDITOR": "sed -i '2s/^pick/squash/'", "GIT_EDITOR": "true"}
        done = _git_env(repo, squash, "rebase", "-q", "-i", "HEAD~2")
        assert done.returncode == 0, ("interactive squash", done.stderr)
        # A conflict, resolved and continued.
        _git(repo, "checkout", "-q", "-b", "side", "HEAD~1")
        (repo / "a").write_text("side\n")
        _git(repo, "commit", "-q", "-am", "side")
        _git(repo, "checkout", "-q", "main")
        (repo / "a").write_text("main\n")
        _git(repo, "commit", "-q", "-am", "main")
        assert _git(repo, "rebase", "-q", "side", check=False).returncode != 0, "the conflict stops it"
        (repo / "a").write_text("both\n")
        _git(repo, "add", "a")
        done = _git(repo, "commit", "-q", "-n", "-m", "by hand", check=False)
        assert done.returncode != 0, "a hand -n commit during a paused rebase is still refused"
        done = _git_env(repo, {"GIT_EDITOR": "true"}, "rebase", "--continue")
        assert done.returncode == 0, ("rebase --continue", done.stderr)
        # A todo line the user wrote is not the sequencer, whatever its parent.
        before = _git(repo, "rev-parse", "HEAD").stdout
        done = _git(repo, "rebase", "-q", "HEAD~1", "--exec",
                    "exec git commit -q --allow-empty -n -m bypass", check=False)
        assert done.returncode != 0, "an exec line's commit -n is refused"
        _git(repo, "rebase", "--abort", check=False)
        assert _git(repo, "rev-parse", "HEAD").stdout == before


def _git_env(repo: pathlib.Path, extra: dict, *args: str) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")} | EXTRA_ENV
    env.update(extra)
    return subprocess.run(["git", *args], cwd=str(repo), capture_output=True, text=True,
                          timeout=60, env=env)


def test_without_proc_the_hook_reads_ps():
    EXTRA_ENV["PREPARE_COMMIT_MSG_PROC_ROOT"] = "/nonexistent-proc"
    try:
        test_refusal_every_spelling_of_no_verify_is_refused()
        with tempfile.TemporaryDirectory() as d:  # ps cannot keep a spaced value whole
            repo = _repo(pathlib.Path(d))
            (repo / "a").write_text("a\n")
            _git(repo, "add", "a")
            done = _git(repo, "-c", "x.y=a rebase", "commit", "-q", "-n", "-m", "a", check=False)
            assert done.returncode != 0, "a spaced global option cannot pose as rebase"
            _git(repo, "commit", "-q", "-m", "a")
            _commit_file(repo, "b")
            _git(repo, "checkout", "-q", "-b", "side", "HEAD~1")
            _commit_file(repo, "c")
            _git(repo, "checkout", "-q", "main")
            (pathlib.Path(d) / "my repo").symlink_to(repo)
            done = _git(repo, "-C", str(pathlib.Path(d) / "my repo"), "cherry-pick", "side", check=False)
            assert done.returncode == 0, ("cherry-pick after a spaced -C path", done.stderr)
            done = _git(repo, "-c", "core.editor=true", "rebase", "-q", "side", check=False)
            assert done.returncode == 0, ("rebase after a global option", done.stderr)
            # The same spaced option during a paused rebase, where rebase state exists.
            _git(repo, "checkout", "-q", "-b", "c2")
            (repo / "c").write_text("other\n")
            _git(repo, "commit", "-q", "-am", "c2")
            _git(repo, "checkout", "-q", "main")
            (repo / "c").write_text("mine\n")
            _git(repo, "commit", "-q", "-am", "mine")
            _git(repo, "checkout", "-q", "c2")
            assert _git(repo, "rebase", "-q", "main", check=False).returncode != 0, "a conflict pauses it"
            (repo / "c").write_text("both\n")
            _git(repo, "add", "c")
            done = _git(repo, "-c", "x.y=a rebase", "commit", "-q", "-n", "-m", "manual", check=False)
            assert done.returncode != 0, "paused-rebase state does not vouch for a hand commit"
            _git(repo, "rebase", "--abort", check=False)
        test_ordinary_commits_pass()
        test_rebase_commits_through_its_sequencer()
    finally:
        EXTRA_ENV.clear()


def test_git_named_git_exe_is_still_git():
    """Git for Windows runs as git.exe: its history operations pass, its -n is refused."""
    with tempfile.TemporaryDirectory() as d:
        repo = _repo(pathlib.Path(d))
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        (bindir / "git.exe").symlink_to(shutil.which("git"))
        exe = str(bindir / "git.exe")
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")} | EXTRA_ENV

        def run(*args):
            return subprocess.run([exe, *args], cwd=str(repo), capture_output=True, text=True,
                                  timeout=60, env=env)
        _commit_file(repo, "a")
        _git(repo, "checkout", "-q", "-b", "side")
        _commit_file(repo, "b")
        _git(repo, "checkout", "-q", "main")
        _commit_file(repo, "c")
        done = run("cherry-pick", "side")
        assert done.returncode == 0, ("git.exe cherry-pick", done.stderr)
        (repo / "d").write_text("d\n")
        _git(repo, "add", "d")
        done = run("commit", "-q", "-n", "-m", "d")
        assert done.returncode != 0 and "pre-commit did not run" in done.stderr, done.stderr


def test_the_post_commit_amend_is_the_one_internal_no_verify_commit():
    with tempfile.TemporaryDirectory() as d:
        repo = _repo(pathlib.Path(d), post_commit=True)
        assert _commit_file(repo, "a").returncode == 0
        shown = _git(repo, "show", "--name-only", "--format=", "HEAD").stdout.split()
        assert "COUNT.md" in shown, "the post-commit amend landed"
        assert not (repo / ".git/hook-amend.by").exists()


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
