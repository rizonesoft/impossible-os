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


def _commit(path, when):
    import subprocess
    env = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid", "GIT_COMMITTER_NAME": "t",
           "GIT_COMMITTER_EMAIL": "t@example.invalid", "GIT_COMMITTER_DATE": f"@{when} +0000",
           "PATH": __import__("os").environ["PATH"], "HOME": str(path)}
    subprocess.run(["git", "-C", str(path), "-c", "core.hooksPath=/dev/null", "commit", "-q", "--allow-empty",
                    "-m", "x"], check=True, timeout=30, env=env)


def test_a_post_commit_hook_acts_only_where_a_commit_just_landed(tmp_path, monkeypatch):
    """Live 2026-11-11: worktree commits ran the suite in the primary tree, under a live run's matrix."""
    here, other = _repo(tmp_path / "here"), _repo(tmp_path / "other")
    now = 1_800_000_000
    _commit(pathlib.Path(here), now - 13 * 60)      # the primary tree: its HEAD is the run's, minutes old
    _commit(pathlib.Path(other), now - 3)           # the worktree: the commit this call made
    monkeypatch.chdir(here)
    # The commit went to the other tree: that tree is the one tested, never this one.
    assert gate.commit_landed_root(f"git -C {other} commit -q -F m.txt -- a.c", now=now) == other
    # A shape whose target cannot be read is judged here, and here nothing landed.
    assert gate.commit_landed_root(f"cd {other} && git commit -m x", now=now) is None
    assert gate.commit_landed_root("git commit -m x", now=now) is None
    # The control: a commit that did land here is acted on here.
    _commit(pathlib.Path(here), now - 2)
    assert gate.commit_landed_root("git commit -m x", now=now) == here
    # A refused commit in the other tree leaves its old HEAD: nothing to test.
    assert gate.commit_landed_root(f"git -C {other} commit -m x", now=now + 3600) is None


def test_both_post_commit_hooks_ask_where_the_commit_landed():
    for name in ("post_commit_smoketest.py", "post_commit_smoketest_boot.py"):
        text = (HOOKS / name).read_text()
        assert "scg.commit_landed_root(cmd)" in text, name
        assert '"rev-parse", "--show-toplevel"' not in text, name
