#!/usr/bin/env python3
"""section_commit_gate reads a multi-line script per command, not as one commit.

v22, live 2026-11-07: a call holding `git commit -q -F f` and a later `grep -n` was
refused as `git commit --no-verify (or -n)`, because shlex treats an unquoted
newline as whitespace.
"""
import pathlib
import sys

HOOKS = pathlib.Path(__file__).resolve().parents[3] / ".claude/hooks"
sys.path.insert(0, str(HOOKS))
import section_commit_gate as gate  # noqa: E402

gate._ensure_crc()

CLEAN = (
    "git commit -q -F f\ngrep -n x y",
    "git commit -q -F f;grep -n x y",
    "git commit -q -F f && grep -n x y",
    "git commit -F - <<'E'\nsubject\n\n-n\nE\ngrep -n x y",
    "git commit -m 'one\n-n\ntwo' -- a.md",
    "git commit -mn -- a.md",
    "git commit -qa -m x",
    "cat > f <<'E'\nit's -n\nE\ngit commit -F f",
)
REFUSED = (
    "git commit -n -F f",
    "git commit --no-verify -F f",
    "git commit -qn -F f",
    "git commit -an -m x",
    "git commit -nm x",
    "git commit \\\n  -n -F f",
    "ls\ngit commit -n -m x",
    "git commit -q -F f\ngit commit -n -m x",
    "cat > f <<'E'\nit's\nE\ngit commit -n -F f",
    "bash -c 'git commit -n -m x'",
    "git commit -m 'a\nb' -n",
    # Review, 2026-11-07, rounds 1 and 2: inputs an earlier version hid by dropping text.
    "echo '<<EOF' ;\ngit commit -n -m x",
    "echo $((1<<x))\ntrue ; git commit -n -m x\nx\n",
    "echo foo\\ # hi ; git commit -n -m x",
    "cat <<A <<B\nbody\nA\nbody\nB\ngit commit -n -m x",
    "# don't\ngit commit -n -m x",
    # Round 3: a separator inside a substitution does not end the commit.
    "git commit -m x$(printf a;printf b) -n",
    "git commit -m x$(printf a\nprintf b) -n",
    "git commit -m x`printf a;printf b` -n",
    "git commit -m x${y:-;} -n",
    # Round 5: an inline comment can hold a quote, so a `#` keeps HEAD's reading.
    "git commit -q # '\ngit commit -n -m x",
    # Round 4: an inner quote is not the outer quote's end.
    'git commit -m "x$(printf "a;b")" -n',
    'git commit -m "x$(printf "a\nb")" -n',
)


def test_other_commands_lend_no_flag():
    for cmd in CLEAN:
        assert gate._bash_is_git_commit(cmd) == (True, False), cmd


def test_refusal_a_real_no_verify_is_still_seen():
    for cmd in REFUSED:
        assert gate._bash_is_git_commit(cmd) == (True, True), cmd


def test_no_commit_is_no_commit():
    assert gate._bash_is_git_commit("grep -n x y\nls -n") == (False, False)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")


def _repo(path):
    import subprocess
    subprocess.run(["git", "init", "-q", str(path)], check=True, timeout=30)
    return str(path.resolve())


def test_a_bare_git_C_commit_is_judged_on_the_tree_it_commits_to(tmp_path):
    """Live 2026-11-11: a repair-worktree commit was refused on the primary tree's index."""
    other = _repo(tmp_path / "other")
    for cmd in (f"git -C {other} commit -q -F msg.txt",
                f"git -C {other} commit -m 'plain words'",
                f"git -C '{other}' commit"):
        assert str(gate._commit_target_root(cmd)) == other, cmd


def test_any_other_shape_keeps_the_gates_own_directory(tmp_path):
    other = _repo(tmp_path / "other")
    for cmd in (f"cd {other} && git commit -m x",
                f"git -C {other} status; git commit -m x",
                f"git -C {other} status\ngit commit -m x",
                f"git -C {other} commit -m x && git commit -m y",
                f"git -C {other} commit -m \"$(cat f)\"",
                f"git -C {other} log commit",
                f"git -c a=b -C {other} commit -m x",
                f"env X=1 git -C {other} commit -m x",
                f"git -C {tmp_path / 'missing'} commit -m x",
                f"git -C {other} commit -m 'unterminated",
                "git commit -m x"):
        assert gate._commit_target_root(cmd) is None, cmd


SESSION = "0a1b2c3d-test-session"


def _git(path, *args, session=SESSION):
    import os
    import subprocess
    env = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid", "GIT_COMMITTER_NAME": "t",
           "GIT_COMMITTER_EMAIL": "t@example.invalid", "PATH": os.environ["PATH"], "HOME": str(path),
           "SKIP_COUNT": "1"}
    if session:
        env["CLAUDE_CODE_SESSION_ID"] = session
    return subprocess.run(["git", "-C", str(path), *args], capture_output=True, text=True, timeout=60, env=env)


def _trees(tmp_path):
    """A primary tree and a linked worktree, both running the repository's real post-commit hook."""
    here = pathlib.Path(_repo(tmp_path / "here"))
    hooks = tmp_path / "hooks"
    hooks.mkdir()
    (hooks / "post-commit").write_text((HOOKS.parents[1] / ".githooks/post-commit").read_text())
    (hooks / "post-commit").chmod(0o755)
    (hooks / "pre-commit").write_text("#!/bin/sh\n[ ! -e refuse-commits ]\n")
    (hooks / "pre-commit").chmod(0o755)
    _git(here, "config", "core.hooksPath", str(hooks))
    assert _git(here, "commit", "-q", "--allow-empty", "-m", "0").returncode == 0
    other = tmp_path / "other"
    assert _git(here, "worktree", "add", "-q", "--detach", str(other)).returncode == 0
    return here, other.resolve()


def test_a_post_commit_hook_acts_only_where_this_sessions_commit_landed(tmp_path, monkeypatch):
    """Live 2026-11-11: worktree commits ran the suite in the primary tree, under a live run's matrix."""
    here, other = _trees(tmp_path)
    monkeypatch.chdir(here)                       # a harness hook always runs in the primary tree
    assert gate.commit_landed_root(SESSION, "smoketest") == str(here)      # the fixture's first commit
    assert gate.commit_landed_root(SESSION, "smoketest") is None           # acted on once
    # The commit goes to the worktree, with a message no shell-text parser reads: that tree is tested.
    assert _git(other, "commit", "-q", "--allow-empty", "-m", "fix #123 $(x); y").returncode == 0
    assert gate.commit_landed_root(SESSION, "smoketest") == str(other)
    assert gate.commit_landed_root(SESSION, "smoketest-boot") == str(other), "each consumer has its own mark"
    assert gate.commit_landed_root(SESSION, "smoketest") is None
    # A refused commit lands nothing, however soon after the last one.
    (other / "refuse-commits").write_text("")
    assert _git(other, "commit", "-q", "--allow-empty", "-m", "refused").returncode != 0
    assert gate.commit_landed_root(SESSION, "smoketest") is None
    (other / "refuse-commits").unlink()
    # Another session's commit, and one made outside any session, are not this session's.
    assert _git(here, "commit", "-q", "--allow-empty", "-m", "the run", session="ffffffff-other").returncode == 0
    assert _git(here, "commit", "-q", "--allow-empty", "-m", "a person", session=None).returncode == 0
    assert gate.commit_landed_root(SESSION, "smoketest") is None
    assert gate.commit_landed_root("ffffffff-other", "smoketest") == str(here)
    # A commit followed by a long tail in the same call is still seen: nothing depends on its age.
    assert _git(other, "commit", "-q", "--allow-empty", "-m", "then a long build").returncode == 0
    import os
    marker = pathlib.Path(_git(here, "rev-parse", "--path-format=absolute", "--git-common-dir").stdout.strip()) \
        / gate.COMMIT_LANDED_DIR / SESSION
    os.utime(marker, (1, 1))
    assert gate.commit_landed_root(SESSION, "smoketest") == str(other)


def test_refusal_a_bad_session_id_or_marker_starts_nothing(tmp_path, monkeypatch):
    here, _other = _trees(tmp_path)
    monkeypatch.chdir(here)
    for bad in ("", "short", "../../etc/passwd", "a" * 65, None, 7):
        assert gate.commit_landed_root(bad, "smoketest") is None, bad
    assert gate.commit_landed_root(SESSION, "../x") is None
    marker = pathlib.Path(_git(here, "rev-parse", "--path-format=absolute", "--git-common-dir").stdout.strip()) \
        / gate.COMMIT_LANDED_DIR / SESSION
    for text in ("", "only-one-line\n", "/no/such/tree\nmark\n", "\nmark\n"):
        marker.write_text(text)
        assert gate.commit_landed_root(SESSION, "smoketest") is None, text


def test_both_post_commit_hooks_ask_where_the_commit_landed():
    for name, consumer in (("post_commit_smoketest.py", "smoketest"), ("post_commit_smoketest_boot.py", "smoketest-boot")):
        text = (HOOKS / name).read_text()
        assert f'scg.commit_landed_root(d.get("session_id") or "", "{consumer}")' in text, name
        assert '"rev-parse", "--show-toplevel"' not in text, name
