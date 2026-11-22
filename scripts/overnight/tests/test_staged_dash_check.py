"""The dash check refuses an added dash in any lane's commit and leaves old dashes alone."""
import io
import os
import shutil
import importlib.util
import subprocess

import pytest
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CHECK = REPO / "scripts/staged_dash_check.py"
GATE = REPO / ".githooks/lib/dash-gate.sh"
DASH = chr(0x2014)
_spec = importlib.util.spec_from_file_location("staged_dash_check", CHECK)
m = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(m)


def _clean_env(extra=None):
    """The caller's repository settings never reach the fixture: a GIT_INDEX_FILE
    or GIT_DIR from an outer commit would make these commands write the caller's
    index."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_") and k != "SKIP_DASH_CHECK"}
    # Nor do the caller's global or system settings: a global commit.gpgSign or
    # core.hooksPath would fail or redirect the fixture's own commits.
    env.update({"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"})
    env.update(extra or {})
    return env


def _sub(cmd, *, cwd=None, input=None, env=None, timeout=120, check=False, capture_output=True, text=False):
    """subprocess.run for every fixture command: the command runs in its own
    process group, and the whole group (a hook, the checker, a cat-file it
    started) is stopped on timeout and after the command returns, so nothing
    a test starts outlives it."""
    import signal
    proc = subprocess.Popen(cmd, cwd=cwd, env=_clean_env() if env is None else env, text=text,
                            stdin=subprocess.PIPE if input is not None else subprocess.DEVNULL,
                            stdout=subprocess.PIPE if capture_output else None,
                            stderr=subprocess.PIPE if capture_output else None, start_new_session=True)
    try:
        out, err = proc.communicate(input, timeout=timeout)
    except subprocess.TimeoutExpired:
        # SIGTERM first, so a checker ends the transport group it started in its own session (which this
        # killpg cannot reach); SIGKILL after a bounded wait.
        try:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=5)
        except (ProcessLookupError, subprocess.TimeoutExpired):  # gone already, or ignoring it: SIGKILL below
            pass
        raise
    finally:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:                         # the group has already exited: nothing to stop
            pass
        if proc.returncode is None:
            proc.communicate()
    r = subprocess.CompletedProcess(cmd, proc.returncode, out, err)
    if check:
        r.check_returncode()
    return r


def _git(root, *a):
    return _sub(["git", *a], cwd=root, check=True, capture_output=True, timeout=30,
                          env=_clean_env()).stdout.decode().strip()


def _repo(tmp_path):
    root = tmp_path / "r"
    root.mkdir(parents=True)
    _git(root, "init", "-q")
    _git(root, "config", "user.name", "t")       # no fixture git step may need the caller's identity
    _git(root, "config", "user.email", "t@t")
    _commit(root, "base", "--allow-empty")
    return root


def _dest(tmp_path, root, name, refs=None):
    """A bare repository at tmp_path/name holding `refs` ({ref: oid}) pushed from root; its path is the URL."""
    bare = tmp_path / name
    _git(tmp_path, "init", "-q", "--bare", str(bare))
    for ref, oid in (refs or {}).items():
        _git(root, "push", "-q", str(bare), f"{oid}:{ref}")
    return str(bare)


def _push_check(root, dest, stdin):
    return _sub([sys.executable, str(CHECK), "--push", dest], cwd=root, input=stdin, text=True, timeout=60)

def _commit(root, msg, *extra):
    _git(root, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", *extra, "-m", msg)  # a fresh repo: no hooks


def _run(root, *args, extra=None):
    return _sub([sys.executable, str(CHECK), *args], cwd=root, env=_clean_env(extra),
                          capture_output=True, text=True, timeout=60)


def _stage(root, files):
    for name, data in files.items():
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))
    _git(root, "add", "-A")


@pytest.mark.parametrize("dash", [chr(0x2013), chr(0x2014)])
def test_an_added_dash_is_refused_and_named(tmp_path, dash):
    root = _repo(tmp_path)
    _stage(root, {"a.md": "ok\nbad " + dash + " line\n"})
    r = _run(root)
    assert r.returncode == 1 and "a.md:2" in r.stderr, r


def test_exchanging_one_dash_for_the_other_passes(tmp_path):
    root = _repo(tmp_path)
    _stage(root, {"a.md": "x " + chr(0x2014) + " y\n"})
    _commit(root, "base")
    _stage(root, {"a.md": "x " + chr(0x2013) + " y\n"})
    assert _run(root).returncode == 0


def test_pre_commit_does_not_look_for_a_merge_it_already_ruled_out(monkeypatch):
    def no(*_a):
        raise AssertionError("merge_heads read on the --no-merge path")
    monkeypatch.setattr(m, "merge_heads", no)
    monkeypatch.setattr(m, "_git", lambda *a, **k: b"")
    assert m.staged(merging=False) == [("", [[]])]


def test_an_old_dash_on_an_untouched_line_and_a_generated_file_pass(tmp_path):
    root = _repo(tmp_path)
    _stage(root, {"old.md": "old " + DASH + " line\n"})
    _commit(root, "old")
    _stage(root, {"old.md": "old " + DASH + " line\nnew line\n", "todo/implementation-plan.md": "t " + DASH + "\n"})
    r = _run(root)
    assert r.returncode == 0, r


def test_no_display_setting_name_or_content_hides_an_added_dash(tmp_path):
    """Whatever once fooled the patch parser (no b/ prefix, a quoted non-ASCII
    name, added text that looks like a header, a form feed inside a line, a
    symlink replaced by a file) still reports at the right line."""
    root = _repo(tmp_path)
    (root / "link").symlink_to("target")
    _git(root, "add", "link")
    _commit(root, "link")
    (root / "link").unlink()
    _stage(root, {"link": "x " + DASH + "\n", "café.md": "++ heading\nbad " + DASH + "\n",
                  "sep.md": "a \fb " + DASH + "\n"})
    _git(root, "config", "diff.noprefix", "true")
    r = _run(root)
    assert r.returncode == 1, r
    for want in ("link:1", "café.md:2", "sep.md:1"):
        assert want in r.stderr, (want, r.stderr)


def test_git_settings_renames_and_odd_names_never_change_the_answer(tmp_path):
    root = _repo(tmp_path)
    _stage(root, {"legacy.md": "a\nold " + DASH + " line\nc\n", "moved.md": "keep " + DASH + " me\n",
                  "notes.md": "clean\n"})
    _commit(root, "legacy")
    _stage(root, {"legacy.md": "A\nold " + DASH + " line\nC\n"})
    _git(root, "mv", "moved.md", "renamed.md")
    _git(root, "config", "diff.interHunkContext", "100")
    env = {"GIT_LITERAL_PATHSPECS": "1"}
    assert _run(root, extra=env).returncode == 0
    _commit(root, "clean")
    _git(root, "mv", "renamed.md", "again.md")
    _stage(root, {"again.md": "keep " + DASH + " me\nnew " + DASH + "\n", "0:notes.md": "x\nbad " + DASH + "\n"})
    r = _run(root, extra=env)
    assert r.returncode == 1 and "again.md:2" in r.stderr and "0:notes.md:2" in r.stderr, r
    assert "again.md:1" not in r.stderr, r


def test_reshaping_a_file_never_changes_the_decision(tmp_path):
    """The class behind rounds 4 to 7: the decision is one dash count per file,
    so no reshaping of its lines can refuse an old dash or hide a new one."""
    lines = ["one " + DASH + " a", "two", "three " + DASH + " b", "four"]
    shapes = {
        "reordered": lambda ls: "\n".join(reversed(ls)) + "\n",
        "utf8 bom": lambda ls: "﻿" + "\n".join(ls) + "\n",
        "crlf": lambda ls: "\r\n".join(ls) + "\r\n",
        "moved within": lambda ls: "\n".join(ls[1:] + ls[:1]) + "\n",
    }
    for name, shape in shapes.items():
        root = _repo(tmp_path / name.replace(" ", "-"))
        _stage(root, {"f.md": "\n".join(lines) + "\n", "s.ps1": ("x " + DASH + "\r\n").encode("utf-8")})
        _commit(root, "base")
        _stage(root, {"f.md": shape(lines), "s.ps1": ("x " + DASH + "\r\n").encode("utf-16")})
        assert _run(root).returncode == 0, name
        _stage(root, {"f.md": shape(lines + ["five " + DASH])})
        r = _run(root)
        assert r.returncode == 1 and "f.md gains 1" in r.stderr, (name, r)
    root = _repo(tmp_path / "rename")
    body = "".join(f"line {i}\n" for i in range(30))
    _stage(root, {"a.md": body + "legacy " + DASH + "\n"})
    _commit(root, "base")
    _git(root, "mv", "a.md", "b.md")
    _stage(root, {"b.md": "edited\n" + body + "legacy " + DASH + "\n"})
    assert _run(root).returncode == 0


def test_a_line_moved_into_another_file_is_written_there_and_binary_is_not_text(tmp_path):
    root = _repo(tmp_path)
    para = "para " + DASH + " moved\n"
    _stage(root, {"a.md": "one\n" + para, "b.md": "two\n", "gone.md": "gone " + DASH + " line\n"})
    _commit(root, "base")
    (root / "gone.md").unlink()
    shutil.copy(REPO / "resources/icons/color/64/settings.png", root / "icon.png")
    _stage(root, {"a.md": "one\n", "b.md": "two\n" + para})
    r = _run(root)
    assert r.returncode == 1 and "b.md:2" in r.stderr, r
    assert "icon.png" not in r.stderr and "a.md" not in r.stderr and "gone.md" not in r.stderr, r


def test_the_fixture_never_writes_the_callers_index(tmp_path, monkeypatch):
    outer = tmp_path / "outer-index"
    outer.write_bytes(b"caller state")
    monkeypatch.setenv("GIT_INDEX_FILE", str(outer))
    root = _repo(tmp_path)
    _stage(root, {"x.md": "x\n"})
    assert _run(root).returncode == 0
    assert outer.read_bytes() == b"caller state"


def test_a_copy_is_named_and_the_untouched_original_is_not(tmp_path):
    root = _repo(tmp_path)
    legacy = "legacy " + DASH + " row\n"
    _stage(root, {"z.md": legacy + "tail\n", "COUNT.md": "# Count " + DASH + " heading\n1\n"})
    _commit(root, "base")
    _stage(root, {"a.md": legacy, "z.md": legacy + "tail edited\n", "COUNT.md": "# Count " + DASH + " heading\n2\n",
                  "notes.md": "# Count " + DASH + " heading\n"})
    r = _run(root)
    assert r.returncode == 1 and "a.md:1" in r.stderr and "notes.md:1" in r.stderr, r
    assert "z.md" not in r.stderr and "COUNT.md" not in r.stderr, r


def test_a_copy_inserted_above_its_original_names_both_copies(tmp_path):
    root = _repo(tmp_path)
    legacy = "legacy " + DASH + " row\n"
    _stage(root, {"f.md": "a\nb\n" + legacy})
    _commit(root, "base")
    _stage(root, {"f.md": legacy + "a\nb\n" + legacy})
    r = _run(root)
    assert r.returncode == 1 and "one of f.md:1, f.md:4" in r.stderr, r


def test_a_rename_out_of_a_generated_file_and_a_utf16_script_are_read(tmp_path):
    root = _repo(tmp_path)
    body = "".join(f"count line {i}\n" for i in range(40))
    _stage(root, {"COUNT.md": body, "old.ps1": ("Write-Output 'legacy " + DASH + "'\r\n").encode("utf-16")})
    _commit(root, "base")
    _git(root, "mv", "COUNT.md", "snapshot.md")
    _stage(root, {"snapshot.md": body + "added " + DASH + " sentence\n",
                  "old.ps1": ("Write-Output 'legacy " + DASH + "'\r\nWrite-Output 'new " + DASH + "'\r\n").encode("utf-16")})
    r = _run(root)
    assert r.returncode == 1 and "snapshot.md:41" in r.stderr and "old.ps1:2" in r.stderr, r
    assert "old.ps1:1" not in r.stderr, r


def test_no_old_blob_is_read_when_the_new_blob_holds_no_dash():
    asked = []

    class Reader:
        def count(self, oid):
            asked.append(oid)
            return 0 if oid == "b" * 40 else 1

    assert m.refused([[("a" * 40, "b" * 40, "clean.md")]], Reader()) == []
    assert asked == ["b" * 40], asked


def test_reads_that_fail_are_errors_not_clean(monkeypatch, capsys):
    odd = ("ok " + DASH + "\n").encode("utf-16") + b"\n"            # a UTF-16 file cut mid-character

    class Fake:
        """A stream that answers from fixed bytes."""

        def __init__(self, data):
            self.out = io.BytesIO(data)

        def write(self, data):
            pass

        def read(self, n):
            return self.out.read(n)

        def readline(self):
            return self.out.readline()

    reader = m.BlobReader()
    reader.stream = Fake(b"abc blob %d\n" % len(odd) + odd + b"\n")
    try:
        reader.lines("abc")
    except ValueError as exc:
        assert "UTF-16" in str(exc)
    else:
        raise AssertionError("a truncated UTF-16 file read as clean")

    reader.stream = Fake(b"abc blob 100\nclean")
    try:
        reader.lines("abc")
    except ValueError as exc:
        assert "short" in str(exc)
    else:
        raise AssertionError("a truncated blob was accepted")

    raw = b":100644 100644 " + b"1" * 40 + b" " + b"2" * 40 + b" M\0f.md\0"
    monkeypatch.setattr(m, "_git", lambda *a, **k: raw)

    class Exits:
        def count(self, oid):
            return 0

        def close(self):
            return 128

    monkeypatch.setattr(m, "BlobReader", Exits)
    monkeypatch.setattr(m, "merge_heads", lambda path="": [])
    assert m.main([]) == 2
    assert "cat-file exited 128" in capsys.readouterr().err


def test_the_range_mode_checks_every_outgoing_commit(tmp_path):
    """A rebase that resolves a conflict commits without pre-commit; pre-push
    checks each outgoing commit against its parent."""
    root = _repo(tmp_path)
    _stage(root, {"legacy.md": "old " + DASH + "\n"})
    _commit(root, "legacy")
    base = _git(root, "rev-parse", "HEAD")
    _stage(root, {"legacy.md": "old " + DASH + "\nmore\n"})
    _commit(root, "clean edit")
    assert _run(root, "--range", base, "HEAD").returncode == 0
    _stage(root, {"doc.md": "resolved " + DASH + " conflict\n"})
    _commit(root, "resolution")
    bad = _git(root, "rev-parse", "--short=12", "HEAD")
    r = _run(root, "--range", base, "HEAD")
    assert r.returncode == 1 and f"commit {bad}: doc.md gains 1" in r.stderr and "doc.md:1" in r.stderr, r
    assert "legacy.md" not in r.stderr, r


def test_the_hook_gates_print_their_guidance_under_errexit(tmp_path):
    for rc, expect in ((1, "SKIP_DASH_CHECK=1"), (2, "could not read")):
        fake = tmp_path / str(rc)
        (fake / "scripts").mkdir(parents=True)
        (fake / "scripts/staged_dash_check.py").write_text(f"import sys\nsys.exit({rc})\n", encoding="utf-8")
        for call in ("dash_gate", "dash_push_gate origin < /dev/null"):
            r = _sub(["bash", "-c", f'set -euo pipefail; REPO_ROOT="{fake}"; . "{GATE}"; {call} || exit 7'],
                               capture_output=True, text=True, timeout=30, env=_clean_env())
            assert r.returncode == 7 and expect in r.stdout, (call, r)


def test_merges_are_judged_on_what_the_merge_added(tmp_path):
    """Two clean parents whose automatic merge doubles a dash, and a conflict
    resolution that adds one, are refused; a clean merge passes."""
    root = _repo(tmp_path)
    body = [f"line {i}" for i in range(40)]
    moved_up = "\n".join(["legacy " + DASH] + body) + "\n"
    _stage(root, {"f.md": "\n".join(body + ["legacy " + DASH]) + "\n", "g.md": "g\n"})
    _commit(root, "base")
    base = _git(root, "rev-parse", "HEAD")
    _git(root, "checkout", "-q", "-b", "side")
    _stage(root, {"f.md": moved_up})
    _commit(root, "move up")
    _git(root, "checkout", "-q", "-")
    _stage(root, {"g.md": "g edited\n"})
    _commit(root, "clean")
    tip = _git(root, "rev-parse", "HEAD")
    # an automatic merge whose file ends with both copies
    _git(root, "-c", "user.name=t", "-c", "user.email=t@t", "merge", "-q", "--no-commit", "side")
    _stage(root, {"f.md": moved_up + "legacy " + DASH + "\n"})
    _commit(root, "doubled")
    merge = _git(root, "rev-parse", "--short=12", "HEAD")
    r = _run(root, "--range", base, "HEAD")
    assert r.returncode == 1 and f"commit {merge}: f.md gains 1" in r.stderr, r
    # a resolution that adds a dash in another file
    _git(root, "reset", "-q", "--hard", tip)
    _git(root, "-c", "user.name=t", "-c", "user.email=t@t", "merge", "-q", "--no-commit", "side")
    _stage(root, {"f.md": moved_up, "h.md": "resolved " + DASH + "\n"})
    _commit(root, "resolved")
    r = _run(root, "--range", base, "HEAD")
    assert r.returncode == 1 and "h.md gains 1" in r.stderr and "f.md" not in r.stderr, r
    # a clean merge
    _git(root, "reset", "-q", "--hard", tip)
    _git(root, "-c", "user.name=t", "-c", "user.email=t@t", "merge", "-q", "--no-edit", "side")
    assert _run(root, "--range", base, "HEAD").returncode == 0


def test_a_staged_merge_resolution_is_judged_against_every_parent(tmp_path):
    """Main removed a legacy dash; the other parent kept it. Keeping the other
    parent's text gains nothing over it and passes, as the push check would;
    adding a dash in the resolution is refused."""
    root = _repo(tmp_path)
    _stage(root, {"f.md": "a\nlegacy " + DASH + "\nb\n"})
    _commit(root, "base")
    _git(root, "checkout", "-q", "-b", "side")
    kept = "a\nlegacy " + DASH + " edited\nb\n"
    _stage(root, {"f.md": kept})
    _commit(root, "side keeps it")
    _git(root, "checkout", "-q", "-")
    _stage(root, {"f.md": "a\nb\n"})
    _commit(root, "main removes it")
    _sub(["git", "-c", "user.name=t", "-c", "user.email=t@t", "merge", "-q", "--no-commit", "side"],
                   cwd=root, capture_output=True, timeout=30, env=_clean_env())
    assert _git(root, "rev-parse", "MERGE_HEAD") == _git(root, "rev-parse", "side")
    _stage(root, {"f.md": kept})
    assert _run(root).returncode == 0
    _stage(root, {"f.md": kept, "g.md": "new " + DASH + "\n"})
    r = _run(root)
    assert r.returncode == 1 and "g.md gains 1" in r.stderr and "f.md" not in r.stderr, r
    # the resolution keeps the other parent's line and adds one: only the added line is named
    _stage(root, {"f.md": kept + "added " + DASH + "\n"})
    r = _run(root)
    assert r.returncode == 1 and "f.md gains 1" in r.stderr and "f.md:4 is a new" in r.stderr, r
    assert "f.md:2" not in r.stderr, r


def _hooked_repo(tmp_path):
    root = _repo(tmp_path)
    hooks = root / ".githooks"
    (hooks / "lib").mkdir(parents=True)
    (root / "scripts").mkdir()
    shutil.copy(REPO / ".githooks/commit-msg", hooks / "commit-msg")
    shutil.copy(GATE, hooks / "lib/dash-gate.sh")
    shutil.copy(CHECK, root / "scripts/staged_dash_check.py")
    shutil.copy(REPO / "scripts/bounded_run.py", root / "scripts/bounded_run.py")
    _git(root, "config", "core.hooksPath", ".githooks")
    return root


def _auto_merge(root):
    return _sub(["git", "-c", "user.name=t", "-c", "user.email=t@t", "merge", "--no-edit", "side"],
                          cwd=root, capture_output=True, text=True, timeout=60, env=_clean_env())


def test_an_automatic_merge_is_refused_before_head_moves(tmp_path):
    """git commits an automatic merge without pre-commit; commit-msg judges it."""
    root = _hooked_repo(tmp_path)
    body = [f"line {i}" for i in range(40)]
    _stage(root, {"f.md": "\n".join(body + ["legacy " + DASH]) + "\n"})
    _commit(root, "base")
    _git(root, "checkout", "-q", "-b", "side")
    _stage(root, {"f.md": "\n".join(["legacy " + DASH] + body) + "\n"})
    _commit(root, "side moves it to the top")
    _git(root, "checkout", "-q", "-")
    _stage(root, {"f.md": "\n".join(body[:20] + ["legacy " + DASH] + body[20:]) + "\n"})
    _commit(root, "main moves it to the middle")
    before = _git(root, "rev-parse", "HEAD")
    r = _auto_merge(root)
    assert r.returncode != 0 and "f.md gains 1" in r.stderr, r
    assert _git(root, "rev-parse", "HEAD") == before


def test_an_automatic_merge_that_keeps_the_other_parents_dash_passes(tmp_path):
    """Main deletes a legacy dash line, the other parent moves it: the merge
    keeps one dash, as many as the other parent has, and must go through."""
    root = _hooked_repo(tmp_path)
    body = [f"line {i}" for i in range(40)]
    _stage(root, {"f.md": "\n".join(body + ["legacy " + DASH]) + "\n"})
    _commit(root, "base")
    _git(root, "checkout", "-q", "-b", "side")
    _stage(root, {"f.md": "\n".join(["legacy " + DASH] + body) + "\n"})
    _commit(root, "side moves it to the top")
    _git(root, "checkout", "-q", "-")
    _stage(root, {"f.md": "\n".join(body) + "\n"})
    _commit(root, "main deletes it")
    r = _auto_merge(root)
    assert r.returncode == 0, r
    assert len(_git(root, "rev-list", "--parents", "-1", "HEAD").split()) == 3


def test_a_new_ref_is_judged_against_the_destination_remote(tmp_path):
    """A commit another remote holds is still outgoing to this one: the destination says what it has."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    mirror = _dest(tmp_path, root, "mirror.git", {"refs/heads/main": tip})
    origin = _dest(tmp_path, root, "origin.git", {"refs/heads/main": _git(root, "rev-parse", "HEAD~1")})
    _git(root, "update-ref", "refs/remotes/mirror/main", tip)
    assert _run(root, "--range", m.NULL_OID, tip).returncode == 0      # no destination named: tracking refs
    r = _run(root, "--range", m.NULL_OID, tip, origin)
    assert r.returncode == 1 and "doc.md gains 1" in r.stderr, r
    assert _run(root, "--range", m.NULL_OID, tip, mirror).returncode == 0
    r = _run(root, "--range", m.NULL_OID, tip, _dest(tmp_path, root, "empty.git"))
    assert r.returncode == 1 and "doc.md gains 1" in r.stderr, r


def test_a_pushurl_is_judged_by_what_it_holds_not_by_the_fetch_side(tmp_path):
    """origin fetches from a repository holding the tip and pushes to an empty one: the push sends it all."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    fetch = _dest(tmp_path, root, "fetch.git", {"refs/heads/main": tip})
    push = _dest(tmp_path, root, "push.git")
    _git(root, "remote", "add", "origin", fetch)
    _git(root, "remote", "set-url", "--push", "origin", push)
    _git(root, "update-ref", "refs/remotes/origin/main", tip)
    stdin = f"refs/heads/main {tip} refs/heads/main {m.NULL_OID}\n"
    r = _push_check(root, push, stdin)
    assert r.returncode == 1 and "doc.md gains 1" in r.stderr, r
    assert _push_check(root, fetch, stdin).returncode == 0       # the same push to the fetch side sends nothing


def test_an_unreadable_range_endpoint_is_a_read_error(tmp_path):
    root = _repo(tmp_path)
    r = _run(root, "--range", "HEAD", "missing-section-review-tip")
    assert r.returncode == 2 and "names no object" in r.stderr, r
    r = _push_check(root, _dest(tmp_path, root, "d.git"), f"refs/heads/main {'1' * 40} refs/heads/main {m.NULL_OID}\n")
    assert r.returncode == 2 and "not in this repository" in r.stderr, r


def test_a_destination_commit_this_clone_lacks_is_named_in_the_refusal(tmp_path):
    root = _repo(tmp_path)
    other = _repo(tmp_path / "o")
    _stage(other, {"x.md": "elsewhere\n"})
    _commit(other, "elsewhere")
    dest = _dest(tmp_path, other, "d.git", {"refs/heads/other": _git(other, "rev-parse", "HEAD")})
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    r = _push_check(root, dest, f"refs/heads/main {tip} refs/heads/main {m.NULL_OID}\n")
    assert r.returncode == 1 and "1 commit(s) the destination holds are not in this clone" in r.stderr, r


def test_the_callers_rename_limit_never_changes_the_verdict(tmp_path):
    """With diff.renameLimit=1 git skips inexact renames; the checker sets its own limit."""
    root = _repo(tmp_path)
    body = "".join(f"line {i} " + DASH + "\n" for i in range(30))
    _stage(root, {f"a{i}.md": body for i in range(3)})
    _commit(root, "dashes")
    base = _git(root, "rev-parse", "HEAD")
    for i in range(3):
        _git(root, "mv", f"a{i}.md", f"b{i}.md")
        (root / f"b{i}.md").write_text(body + "edited\n")
    _git(root, "add", "-A")
    limit = {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "diff.renameLimit", "GIT_CONFIG_VALUE_0": "1"}
    assert _run(root, extra=limit).returncode == 0, _run(root, extra=limit)
    _commit(root, "renamed")
    assert _run(root, "--range", base, "HEAD", extra=limit).returncode == 0


def test_a_blob_is_counted_in_chunks_with_a_dash_across_each_boundary(tmp_path):
    """Text past the binary prefix is drained in chunks; a dash split across a chunk edge still counts."""
    root = _repo(tmp_path)
    edge = m.CHUNK
    data = bytearray(b"a" * (3 * edge + 10))
    for at in (m.BINARY_PREFIX - 1, edge - 1, 2 * edge - 2, 3 * edge):   # straddling the prefix and each chunk edge
        data[at:at + 3] = DASH.encode()
    _stage(root, {"big.md": bytes(data)})
    oid = _git(root, "rev-parse", ":big.md")
    utf16 = ("x" * 40000 + DASH + "y" * 40000 + DASH).encode("utf-16")
    _stage(root, {"w.md": utf16})
    oid16 = _git(root, "rev-parse", ":w.md")
    import contextlib
    with contextlib.chdir(root):
        reader = m.BlobReader()
        try:
            assert reader.count(oid) == 4 and reader.count(oid16) == 2
        finally:
            reader.close()

def test_one_push_reads_a_commit_two_refs_carry_once(tmp_path, monkeypatch):
    """A branch and a new tag on the same tip: each outgoing commit is judged once."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    base = _git(root, "rev-parse", "HEAD~1")
    stdin = f"refs/heads/main {tip} refs/heads/main {base}\nrefs/tags/v1 {tip} refs/tags/v1 {m.NULL_OID}\n"
    stdin += f"(delete) {m.NULL_OID} refs/heads/old {base}\n"
    dest = _dest(tmp_path, root, "d.git", {"refs/heads/main": base, "refs/heads/old": base})
    r = _push_check(root, dest, stdin)
    assert r.returncode == 1 and r.stderr.count("doc.md gains 1") == 1, r
    _production_git_in(root, monkeypatch)
    labels = [label for label, _changes in m.outgoing(m.push_updates(stdin), dest)[0]]
    # The new tag walks nothing below main's advertised remote oid: only the tip is judged.
    assert labels == [f"commit {tip[:12]}: "], labels


@pytest.mark.parametrize("space", ["\u00a0", "\u2028"])
def test_a_ref_name_holding_other_whitespace_is_one_field(tmp_path, space):
    """git accepts a U+00A0 or a U+2028 in a tag name; neither may split a field or a record."""
    root = _repo(tmp_path)
    tip = _git(root, "rev-parse", "HEAD")
    dest = _dest(tmp_path, root, "d.git", {"refs/heads/main": tip})
    tag = f"refs/tags/Release{space}1"
    assert _sub(["git", "check-ref-format", tag], cwd=root, timeout=30).returncode == 0
    stdin = f"refs/heads/main {tip} refs/heads/main {tip}\n{tag} {tip} {tag} {m.NULL_OID}\n"
    r = _push_check(root, dest, stdin)
    assert r.returncode == 0, r
    assert m.push_updates(stdin) == [(tip, tip), (m.NULL_OID, tip)]


def _production_git_in(root, monkeypatch):
    """Run the checker's own _git (and its timeout) in the fixture, isolated."""
    for k in [k for k in os.environ if k.startswith("GIT_")]:
        monkeypatch.delenv(k)
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", os.devnull)
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")
    monkeypatch.chdir(root)


def test_a_new_ref_to_a_second_remote_judges_what_that_remote_lacks(tmp_path, monkeypatch):
    """A repository that holds nothing yet: a commit origin already has is still judged on its first push there."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    origin = _dest(tmp_path, root, "origin.git", {"refs/heads/main": tip})
    second = _dest(tmp_path, root, "second.git")
    stdin = f"refs/heads/main {tip} refs/heads/main {m.NULL_OID}\n"
    r = _push_check(root, second, stdin)
    assert r.returncode == 1 and "doc.md gains 1" in r.stderr, r
    assert _push_check(root, origin, stdin).returncode == 0


def test_a_push_source_holding_spaces_is_one_field(tmp_path):
    root = _repo(tmp_path)
    tip = _git(root, "rev-parse", "HEAD")
    line = f"HEAD@{{0 seconds ago}} {tip} refs/tags/proof {m.NULL_OID}\n"
    assert m.push_updates(line) == [(m.NULL_OID, tip)]


def test_a_real_dry_run_push_with_a_spaced_source_reaches_every_check_intact(tmp_path):
    """git hands pre-push the source as typed. The fixture hook is the shipped
    record handling and dash gate; the shipped hook's loops all read it."""
    root = _repo(tmp_path)
    (root / ".githooks/lib").mkdir(parents=True)
    (root / "scripts").mkdir()
    for rel in (".githooks/lib/push-records.sh", ".githooks/lib/dash-gate.sh", "scripts/staged_dash_check.py",
                "scripts/bounded_run.py"):
        shutil.copy(REPO / rel, root / rel)
    hook = root / ".githooks/pre-push"
    hook.write_text('#!/usr/bin/env bash\nset -uo pipefail\nREPO_ROOT="$(git rev-parse --show-toplevel)"\n'
                    '. "$REPO_ROOT/.githooks/lib/push-records.sh"\n. "$REPO_ROOT/.githooks/lib/dash-gate.sh"\n'
                    'STDIN_DATA="$(cat)"\nPUSH_RECORDS="$(push_records <<< "$STDIN_DATA")"\n'
                    "while IFS=' ' read -r _l loid rref roid; do echo \"REC $loid $rref $roid\" >&2; done"
                    ' <<< "$PUSH_RECORDS"\n'
                    'dash_push_gate "${2:-}" <<< "$STDIN_DATA" || exit 1\n')
    hook.chmod(0o755)
    bare = tmp_path / "origin.git"
    _git(tmp_path, "init", "-q", "--bare", str(bare))
    _git(root, "remote", "add", "origin", str(bare))
    _git(root, "config", "core.hooksPath", ".githooks")
    tip = _git(root, "rev-parse", "HEAD")
    for source in ("HEAD@{0 seconds ago}", "HEAD@{0\tseconds ago}"):     # a space, and a tab git keeps as typed
        r = _sub(["git", "push", "--dry-run", "origin", f"{source}:refs/heads/main"], cwd=root, text=True)
        assert r.returncode == 0 and f"REC {tip} refs/heads/main {m.NULL_OID}" in r.stderr, r.stdout + r.stderr
    push = ["git", "push", "--dry-run", "origin", "HEAD@{0\tseconds ago}:refs/heads/main"]
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    r = _sub(push, cwd=root, text=True)
    assert r.returncode != 0 and "doc.md gains 1" in r.stderr, r.stdout + r.stderr
    shipped = (REPO / ".githooks/pre-push").read_text()
    assert shipped.count('done <<< "$PUSH_RECORDS"') == 6 and 'done <<< "$STDIN_DATA"' not in shipped
    assert shipped.count("while IFS=' ' read -r ") == 6 and "while read -r " not in shipped
    assert 'dash_push_gate "${2:-}" <<< "$STDIN_DATA"' in shipped


def test_a_binary_blob_is_counted_from_its_prefix_and_the_stream_stays_in_step(tmp_path, monkeypatch):
    root = _repo(tmp_path)
    big = b"\0" + DASH.encode() * 200000
    _stage(root, {"bin.dat": big, "a.md": "x " + DASH + "\n"})
    _commit(root, "two")
    oids = [_git(root, "rev-parse", f"HEAD:{name}") for name in ("bin.dat", "a.md")]
    _production_git_in(root, monkeypatch)
    reader = m.BlobReader()
    try:
        assert reader.count(oids[0]) == 0 and reader.count(oids[1]) == 1
        assert reader.lines(oids[0]) == [] and reader.lines(oids[1]) == [(1, m.line_key(("x " + DASH).encode()))]
    finally:
        assert reader.close() == 0


def test_commit_msg_passes_the_merge_head_it_found(monkeypatch, tmp_path):
    calls = []
    head = tmp_path / "MERGE_HEAD"
    head.write_text("a" * 40 + "\n")
    monkeypatch.setattr(m, "_git", lambda *a, **k: calls.append(a) or b"")
    assert m.staged(merge_head=str(head)) == [("", [[]])]          # nothing against HEAD: no other parent read
    assert not any(a[:1] == ("rev-parse",) for a in calls), calls


def test_an_ordinary_commit_is_refused_by_the_shipped_pre_commit_hook(tmp_path):
    root = _repo(tmp_path)
    (root / "scripts").mkdir()
    shutil.copytree(REPO / ".githooks", root / ".githooks")
    shutil.copy(CHECK, root / "scripts/staged_dash_check.py")
    shutil.copy(REPO / "scripts/bounded_run.py", root / "scripts/bounded_run.py")
    (root / "scripts/lint.sh").write_text("exit 0\n")       # the lint has its own suite; this test is the dash step
    _git(root, "config", "core.hooksPath", ".githooks")
    before = _git(root, "rev-parse", "HEAD")
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    r = _sub(["git", "commit", "-q", "-m", "dash"], cwd=root, text=True)
    assert r.returncode != 0 and "doc.md gains 1" in r.stderr, r.stdout + r.stderr
    assert _git(root, "rev-parse", "HEAD") == before


def test_a_deleted_refs_remote_oid_bounds_every_walk(tmp_path, monkeypatch):
    """Renaming a tag sends nothing: the deletion names the commit the destination holds."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "old " + DASH + "\n"})
    _commit(root, "historic dash")
    tip = _git(root, "rev-parse", "HEAD")
    dest = _dest(tmp_path, root, "d.git", {"refs/tags/old": tip})
    stdin = f"(delete) {m.NULL_OID} refs/tags/old {tip}\nrefs/tags/new {tip} refs/tags/new {m.NULL_OID}\n"
    r = _push_check(root, dest, stdin)
    assert r.returncode == 0, r
    assert m.push_updates(stdin) == [(tip, m.NULL_OID), (m.NULL_OID, tip)]


def test_an_existing_ref_walks_nothing_another_ref_already_holds(tmp_path, monkeypatch):
    """Two refs fast-forward to one tip from unequal old tips: only the commits past the newer one are sent."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "old " + DASH + "\n"})
    _commit(root, "historic dash")
    newer = _git(root, "rev-parse", "HEAD")
    older = _git(root, "rev-parse", "HEAD~1")
    _stage(root, {"clean.md": "plain\n"})
    _commit(root, "clean")
    tip = _git(root, "rev-parse", "HEAD")
    dest = _dest(tmp_path, root, "d.git", {"refs/heads/main": newer, "refs/heads/side": older})
    stdin = f"refs/heads/main {tip} refs/heads/main {newer}\nrefs/heads/side {tip} refs/heads/side {older}\n"
    _production_git_in(root, monkeypatch)
    labels = [label for label, _changes in m.outgoing(m.push_updates(stdin), dest)[0]]
    assert labels == [f"commit {tip[:12]}: "], labels


def test_a_refusal_reads_only_the_parents_that_hold_a_dash(tmp_path):
    reads = []

    class Reader:
        def count(self, oid):
            return {"new": 1, "bin": 0, "dash": 0}[oid]

        def lines(self, oid):
            reads.append(oid)
            return m.dash_lines(("x " + DASH + "\n").encode())

    found = m.refused([[("bin", "new", "f.md")], [("bin", "new", "f.md")]], Reader())
    assert [f[0] for f in found] == ["f.md"] and reads == ["new"], reads


def test_a_merge_parent_holding_as_many_decides_without_reading_the_rest():
    counted = []

    class Reader:
        def count(self, oid):
            counted.append(oid)
            return {"new": 1, "p1": 1, "p2": 0}[oid]

        def lines(self, oid):
            raise AssertionError("a clean file is never read")

    assert m.refused([[("p1", "new", "f.md")], [("p2", "new", "f.md")]], Reader()) == []
    assert counted == ["new", "p1"], counted


def test_the_shipped_pre_push_refuses_when_its_record_helper_cannot_load(tmp_path):
    """Without errexit, a missing helper used to leave every policy loop an empty record list."""
    root = _repo(tmp_path)
    (root / ".githooks/lib").mkdir(parents=True)
    shutil.copy(REPO / ".githooks/pre-push", root / ".githooks/pre-push")
    shutil.copy(REPO / ".githooks/lib/isolate-git-env.sh", root / ".githooks/lib/isolate-git-env.sh")
    tip = _git(root, "rev-parse", "HEAD")
    r = _sub(["bash", str(root / ".githooks/pre-push"), "origin", "x"], cwd=root, text=True, timeout=60,
             input=f"refs/heads/main {tip} refs/heads/main {m.NULL_OID}\n")
    assert r.returncode != 0 and "push-records.sh could not be loaded" in r.stderr, r


def test_the_dash_gate_runs_before_any_slow_pre_commit_check():
    """A refusal the staged index can give in milliseconds never waits behind the lint or the suites."""
    hook = (REPO / ".githooks/pre-commit").read_text()
    gate = hook.index("dash_commit_gate || exit 1")
    assert hook.index("git diff --cached --quiet --exit-code") < gate < hook.index('echo "Pre-commit: running scripts/lint.sh')


def test_a_range_between_two_commits_is_that_range_whatever_a_remote_tracks(tmp_path):
    root = _repo(tmp_path)
    base = _git(root, "rev-parse", "HEAD")
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    _git(root, "update-ref", "refs/remotes/origin/main", "HEAD")      # already published
    r = _run(root, "--range", base, "HEAD")
    assert r.returncode == 1 and "doc.md gains 1" in r.stderr, r
    assert _run(root, "--range", m.NULL_OID, "HEAD").returncode == 0  # from nothing: tracked commits stand in


def test_a_local_replacement_never_stands_in_for_the_object_pushed(tmp_path):
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    dashed = _git(root, "rev-parse", "HEAD:doc.md")
    clean = _sub(["git", "hash-object", "-w", "--stdin"], cwd=root, input=b"clean\n").stdout.decode().strip()
    _git(root, "replace", dashed, clean)
    r = _run(root, "--range", _git(root, "rev-parse", "HEAD~1"), "HEAD")
    assert r.returncode == 1 and "doc.md gains 1" in r.stderr, r


def test_a_failed_destination_lookup_never_prints_its_url(tmp_path):
    root = _repo(tmp_path)
    tip = _git(root, "rev-parse", "HEAD")
    url = "https://user:SECRET-TOKEN@127.0.0.1:9/x.git"                 # port 9: refused at once
    r = _push_check(root, url, f"refs/heads/main {tip} refs/heads/main {m.NULL_OID}\n")
    assert r.returncode == 2 and "SECRET-TOKEN" not in r.stdout + r.stderr, r
    assert "could not be listed" in r.stderr, r


def test_a_stalled_transport_is_stopped_with_its_whole_group(tmp_path, monkeypatch):
    """A transport helper is git's child; on timeout the group goes, and the message names no URL."""
    root = _repo(tmp_path)
    pidfile = tmp_path / "helper.pid"
    helper = tmp_path / "helper.sh"
    helper.write_text(f"#!/bin/sh\necho $$ > '{pidfile}'\nexec sleep 30\n")
    helper.chmod(0o755)
    _production_git_in(root, monkeypatch)
    monkeypatch.setenv("GIT_CONFIG_COUNT", "1")
    monkeypatch.setenv("GIT_CONFIG_KEY_0", "protocol.ext.allow")
    monkeypatch.setenv("GIT_CONFIG_VALUE_0", "always")
    monkeypatch.setattr(m, "TIMEOUT", 2)
    with pytest.raises(ValueError) as err:
        m.destination_oids(f"ext::{helper} SECRET-TOKEN")
    assert "SECRET-TOKEN" not in str(err.value) and "within 2 s" in str(err.value)
    assert "cannot prompt on the terminal" in str(err.value), err.value
    _assert_helper_ends(int(pidfile.read_text()))


def test_the_dash_check_asks_the_destination_only_after_the_local_refusals():
    """The branch policy and the review debt refuse without the network; the dash check asks the destination."""
    hook = (REPO / ".githooks/pre-push").read_text()
    assert hook.index("OWED_TOOL=") < hook.index('dash_push_gate "${2:-}"')


def test_one_refused_change_at_many_paths_is_diagnosed_once():
    texts = []

    class Reader:
        def count(self, oid):
            return {"new": 1, "old": 0}[oid]

        def lines(self, oid):
            texts.append(oid)
            return m.dash_lines(("x " + DASH + "\n").encode())

    found = m.refused([[("old", "new", f"f{i}.md") for i in range(5)]], Reader(), {})
    assert len(found) == 5 and texts == ["new"], texts


def test_a_second_url_rewrite_is_refused_never_followed(tmp_path):
    """pre-push hands over the rewritten URL; a rule that rewrites it again would list another repository."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    lacks = _dest(tmp_path, root, "a.git")
    holds = _dest(tmp_path, root, "b.git", {"refs/heads/main": tip})
    _git(root, "config", f"url.{holds}.insteadOf", lacks)
    r = _push_check(root, lacks, f"refs/heads/main {tip} refs/heads/main {m.NULL_OID}\n")
    assert r.returncode == 2 and "rewrites the push URL again" in r.stderr, r


def _helper_state(pid):
    """The helper's /proc state letter, or "" once it is gone."""
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except (FileNotFoundError, ProcessLookupError):        # gone before the open, or during the read (ESRCH)
        return ""


def _assert_helper_ends(pid):
    import time
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:                                   # gone, or dead and awaiting its reaper
        if _helper_state(pid) in ("", "Z", "X"):
            return
        time.sleep(0.05)
    raise AssertionError(f"transport helper {pid} still running")


@pytest.mark.parametrize("sig", ["SIGTERM", "SIGINT"])
def test_a_cancelled_lookup_ends_the_transport_group_too(tmp_path, sig):
    """Ctrl-C or SIGTERM to the checker while ls-remote waits on a stalled helper ends the helper as well."""
    import signal
    import time
    root = _repo(tmp_path)
    pidfile = tmp_path / "helper.pid"
    helper = tmp_path / "helper.sh"
    helper.write_text(f"#!/bin/sh\necho $$ > '{pidfile}'\nexec sleep 30\n")
    helper.chmod(0o755)
    # As at a terminal: a caller started in the background inherits SIGINT ignored, and Python then never
    # raises KeyboardInterrupt, so the child restores the default first.
    code = ("import signal\nsignal.signal(signal.SIGINT, signal.default_int_handler)\n"
            f"import importlib.util,sys\nspec=importlib.util.spec_from_file_location('c', {str(CHECK)!r})\n"
            "c=importlib.util.module_from_spec(spec); spec.loader.exec_module(c)\n"
            f"c.destination_oids('ext::{helper} x')\n")
    env = _clean_env({"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "protocol.ext.allow",
                      "GIT_CONFIG_VALUE_0": "always"})
    proc = subprocess.Popen([sys.executable, "-c", code], cwd=root, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        deadline = time.monotonic() + 20
        while not (pidfile.exists() and pidfile.read_text().strip()) and time.monotonic() < deadline:
            time.sleep(0.05)
        pid = int(pidfile.read_text())
        os.kill(proc.pid, getattr(signal, sig))
        proc.wait(timeout=10)
        assert proc.returncode != 0                                      # the cancellation still ends the check
        _assert_helper_ends(pid)
    finally:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:                                       # the group has already exited
            pass
        proc.wait()


def test_a_range_between_two_commits_ignores_what_a_url_holds(tmp_path):
    """The optional URL bounds only a range from nothing; two named commits are exactly that range."""
    root = _repo(tmp_path)
    base = _git(root, "rev-parse", "HEAD")
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    holds = _dest(tmp_path, root, "holds.git", {"refs/heads/main": tip})
    r = _run(root, "--range", base, tip, holds)
    assert r.returncode == 1 and "doc.md gains 1" in r.stderr, r
    assert _run(root, "--range", m.NULL_OID, tip, holds).returncode == 0


def test_a_local_graft_never_changes_the_parents_judged(tmp_path):
    """A graft that makes a commit a root would have it judged against the empty tree."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "old " + DASH + "\n"})
    _commit(root, "the dash, already there")
    parent = _git(root, "rev-parse", "HEAD")
    _stage(root, {"other.md": "plain\n"})
    _commit(root, "plain change")
    tip = _git(root, "rev-parse", "HEAD")
    (root / ".git/info").mkdir(exist_ok=True)
    (root / ".git/info/grafts").write_text(tip + "\n")                  # the tip as a root
    r = _run(root, "--range", parent, tip)
    assert r.returncode == 0, r


def test_the_empty_index_shortcut_reads_the_stored_head(tmp_path):
    """A replacement making HEAD look like the index must not let a staged dash skip the commit-time check."""
    root = _repo(tmp_path)
    (root / "scripts").mkdir()
    shutil.copytree(REPO / ".githooks", root / ".githooks")
    shutil.copy(CHECK, root / "scripts/staged_dash_check.py")
    shutil.copy(REPO / "scripts/bounded_run.py", root / "scripts/bounded_run.py")
    (root / "scripts/lint.sh").write_text("exit 0\n")
    _git(root, "config", "core.hooksPath", ".githooks")
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    tree = _git(root, "write-tree")
    head = _git(root, "rev-parse", "HEAD")
    fake = _sub(["git", "commit-tree", tree, "-m", "lookalike"], cwd=root,
                env=_clean_env({"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
                                "GIT_COMMITTER_EMAIL": "t@t"})).stdout.decode().strip()
    _git(root, "replace", head, fake)
    r = _sub(["git", "commit", "-q", "--allow-empty", "-m", "dash"], cwd=root, text=True)
    assert r.returncode != 0 and "doc.md gains 1" in r.stderr, r.stdout + r.stderr


def _stand_in(tmp_path):
    """(shell commands, pid file) for a stand-in that records its own process id, then sleeps about a minute:
    each test watches and reaps exactly that process, and one a test fails to end outlives it only briefly."""
    pidfile = tmp_path / f"stand-in-{len(list(tmp_path.glob('stand-in-*')))}.pid"
    # Recorded whole or not at all (written aside, then renamed), so a stand-in ended mid-write leaves no file.
    return f'echo $$ > "{pidfile}.part" && mv "{pidfile}.part" "{pidfile}"; exec sleep 61', pidfile


def _stand_in_ends(pidfile, seconds=5):
    """True once the recorded stand-in is gone (or never started)."""
    import time
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if not pidfile.exists() or _helper_state(int(pidfile.read_text())) in ("", "Z", "X"):
            return True
        time.sleep(0.05)
    return False


def _reap_stand_in(pidfile):
    """End the recorded stand-in, whatever the code under test did."""
    import signal
    try:
        os.kill(int(pidfile.read_text()), signal.SIGKILL)
    except (FileNotFoundError, ValueError, ProcessLookupError):   # never started, or already gone
        pass


def test_a_stalled_url_check_never_prints_the_url(tmp_path, monkeypatch, capsys):
    """The first command that takes the URL is bounded the same way: its timeout names no URL."""
    root = _repo(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("git")
    stand_in, pidfile = _stand_in(tmp_path)
    (bindir / "git").write_text(f'#!/bin/sh\ncase " $* " in *" --get-url "*) {stand_in} ;; esac\nexec "{real}" "$@"\n')
    (bindir / "git").chmod(0o755)
    _production_git_in(root, monkeypatch)
    monkeypatch.setenv("PATH", f"{bindir}:{os.environ['PATH']}")
    monkeypatch.setattr(m, "TIMEOUT", 1)
    try:
        with pytest.raises(ValueError) as err:
            m.destination_oids("https://user:SECRET-TOKEN@example.invalid/x.git")
        out = capsys.readouterr()
        assert "SECRET-TOKEN" not in str(err.value) + out.out + out.err and "within 1 s" in str(err.value)
        assert pidfile.exists() and _stand_in_ends(pidfile)
    finally:
        _reap_stand_in(pidfile)


def test_a_local_destination_ending_in_lf_is_not_a_rewrite(tmp_path):
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    dest = _dest(tmp_path, root, "d.git\n")
    r = _push_check(root, dest, f"refs/heads/main {tip} refs/heads/main {m.NULL_OID}\n")
    assert r.returncode == 1 and "doc.md gains 1" in r.stderr, r


def test_a_cancellation_while_the_group_starts_still_ends_it(tmp_path):
    """SIGTERM landing before the group is registered is held, then ends the group and the checker."""
    stand_in, pidfile = _stand_in(tmp_path)
    code = ("import os, signal, subprocess, importlib.util\n"
            f"spec = importlib.util.spec_from_file_location('c', {str(CHECK)!r})\n"
            "c = importlib.util.module_from_spec(spec); spec.loader.exec_module(c)\n"
            "real = subprocess.Popen\n"
            "def popen(*a, **k):\n"
            "    os.kill(os.getpid(), signal.SIGTERM)\n"
            "    return real(*a, **k)\n"
            "c.subprocess.Popen = popen\n"
            f"c._bounded(['sh', '-c', {stand_in!r}], 'a stand-in')\n")
    try:
        proc = _sub([sys.executable, "-c", code], cwd=tmp_path, env=_clean_env(), timeout=30)
        assert proc.returncode == -15, proc                              # the cancellation reaches the caller
        assert _stand_in_ends(pidfile)
    finally:
        _reap_stand_in(pidfile)


def test_a_fixture_timeout_lets_the_checker_end_its_transport(tmp_path):
    """The fixture's own timeout sends SIGTERM first, so the checker ends the group it started."""
    root = _repo(tmp_path)
    helper = tmp_path / "helper.sh"
    stand_in, pidfile = _stand_in(tmp_path)
    helper.write_text(f"#!/bin/sh\n{stand_in}\n")
    helper.chmod(0o755)
    code = (f"import importlib.util\nspec = importlib.util.spec_from_file_location('c', {str(CHECK)!r})\n"
            "c = importlib.util.module_from_spec(spec); spec.loader.exec_module(c)\n"
            f"c.destination_oids('ext::{helper} x')\n")
    env = _clean_env({"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "protocol.ext.allow",
                      "GIT_CONFIG_VALUE_0": "always"})
    try:
        with pytest.raises(subprocess.TimeoutExpired):
            _sub([sys.executable, "-c", code], cwd=root, env=env, timeout=3)
        assert pidfile.exists() and _stand_in_ends(pidfile)
    finally:
        _reap_stand_in(pidfile)


def test_a_shallow_boundary_is_judged_against_its_stored_parent(tmp_path):
    """rev-list shows a shallow boundary as a root; judged as one, its inherited dashes would read as new."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "old " + DASH + "\n"})
    _commit(root, "the dash, already there")
    parent = _git(root, "rev-parse", "HEAD")
    _stage(root, {"other.md": "plain\n"})
    _commit(root, "plain change")
    tip = _git(root, "rev-parse", "HEAD")
    (root / ".git/shallow").write_text(tip + "\n")
    assert _run(root, "--range", parent, tip).returncode == 0
    r = _run(root, "--range", m.NULL_OID, tip)                          # the history before it would go unjudged
    assert r.returncode == 2 and "shallow" in r.stderr, r


def test_a_series_of_refused_edits_reads_each_version_once():
    texts = []

    class Reader:
        def count(self, oid):
            return {"v0": 0, "v1": 1, "v2": 2}[oid]

        def lines(self, oid):
            texts.append(oid)
            return m.dash_lines("".join(f"line {i} {DASH}\n" for i in range(self.count(oid))).encode())

    lines = {}
    assert m.refused([[("v0", "v1", "f.md")]], Reader(), lines)
    assert m.refused([[("v1", "v2", "f.md")]], Reader(), lines)
    assert sorted(texts) == ["v1", "v2"], texts                         # v1 read once, as child then parent


def test_a_cancellation_as_the_handlers_are_put_back_still_reaches_the_caller(tmp_path):
    """SIGTERM landing while _bounded restores the caller's handlers is delivered, never recorded and lost."""
    code = ("import os, subprocess, importlib.util, signal as sg\n"
            f"spec = importlib.util.spec_from_file_location('c', {str(CHECK)!r})\n"
            "c = importlib.util.module_from_spec(spec); spec.loader.exec_module(c)\n"
            "state = {'ran': False, 'sent': False}\n"
            "real_popen, real_signal = subprocess.Popen, sg.signal\n"
            "def popen(*a, **k):\n"
            "    p = real_popen(*a, **k); state['ran'] = True; return p\n"
            "def put(sig, handler):\n"
            "    if state['ran'] and not state['sent']:\n"
            "        state['sent'] = True; os.kill(os.getpid(), sg.SIGTERM)\n"
            "    return real_signal(sig, handler)\n"
            "c.subprocess.Popen = popen\n"
            "sg.signal = put\n"
            "c._bounded(['true'], 'a stand-in')\n"
            "print('returned')\n")
    proc = _sub([sys.executable, "-c", code], cwd=tmp_path, env=_clean_env(), timeout=30, text=True)
    assert proc.returncode == -15 and "returned" not in proc.stdout, proc


def test_a_decoding_error_ends_the_blob_reader_at_once(tmp_path, monkeypatch):
    """A UTF-16 blob invalid near its start with a pipeful left: the error is reported, not held to the deadline."""
    import time
    root = _repo(tmp_path)
    base = _git(root, "rev-parse", "HEAD")
    bad = b"\xff\xfe" + b"\x00\xd8" + b"a\x00" * (1 << 19)          # an unpaired surrogate, then 1 MiB
    _stage(root, {"w.txt": bad})
    _commit(root, "utf-16")
    tip = _git(root, "rev-parse", "HEAD")
    _production_git_in(root, monkeypatch)
    monkeypatch.setattr(m, "TIMEOUT", 20)
    began = time.monotonic()
    with pytest.raises(ValueError, match="could not be decoded"):
        m.judge(m.outgoing([(base, tip)])[0])
    assert time.monotonic() - began < 10


def test_a_shallow_parent_the_destination_reaches_is_a_comparison(tmp_path):
    """The boundary's parent is not advertised, but a commit the destination has descends from it."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "old " + DASH + "\n"})
    _commit(root, "the dash, already there")
    fork = _git(root, "rev-parse", "HEAD")
    _commit(root, "on main", "--allow-empty")
    bound = _git(root, "rev-parse", "HEAD")
    _git(root, "checkout", "-q", "-b", "side", fork)
    _stage(root, {"other.md": "plain\n"})
    _commit(root, "plain change")
    tip = _git(root, "rev-parse", "HEAD")
    (root / ".git/shallow").write_text(tip + "\n")
    assert _run(root, "--range", bound, tip).returncode == 0


def test_a_depth_one_clone_whose_advertised_parent_it_lacks_is_told_to_deepen(tmp_path):
    src = _repo(tmp_path)
    _stage(src, {"doc.md": "old " + DASH + "\n"})
    _commit(src, "parent")
    parent = _git(src, "rev-parse", "HEAD")
    _stage(src, {"doc.md": "old " + DASH + "\nmore\n"})
    _commit(src, "tip")
    clone = tmp_path / "clone"
    _git(tmp_path, "clone", "-q", "--depth", "1", f"file://{src}", str(clone))
    _stage(clone, {"n.md": "plain\n"})
    _commit(clone, "new")
    new = _git(clone, "rev-parse", "HEAD")
    dest = _dest(tmp_path, src, "d.git", {"refs/heads/old": parent})
    r = _push_check(clone, dest, f"refs/heads/main {new} refs/heads/main {m.NULL_OID}\n")
    assert r.returncode == 2 and "shallow" in r.stderr and "deepen" in r.stderr, r


def test_dash_lines_names_exactly_the_lines_a_full_split_would():
    samples = [b"", b"x" + DASH.encode(), DASH.encode() * 3 + b"\n" + b"plain\r\n" + b"a" + DASH.encode() + b"\r\n",
               b"\n\n" + "\u2013".encode() + b"\nend" + DASH.encode(), b"no dash at all\n" * 4]
    for text in samples:
        naive = [(n, m.line_key(ln.removesuffix(b"\r"))) for n, ln in enumerate(text.split(b"\n"), 1)
                 if any(d in ln for d in m.DASHES)]
        assert m.dash_lines(text) == naive, text


def test_naming_the_lines_of_a_long_dash_dense_line_is_linear():
    import time
    line = (b"x" + DASH.encode()) * 400_000                             # about 1.6 MB on one line
    began = time.monotonic()
    found = m.dash_lines(b"head\n" + line + b"\r\ntail " + DASH.encode())
    assert time.monotonic() - began < 1.0
    assert [n for n, _ln in found] == [2, 3] and found[0][1] == m.line_key(line)


def test_a_merge_with_nothing_changed_against_head_reads_no_other_parent(monkeypatch):
    calls = []
    monkeypatch.setattr(m, "_git", lambda *a, **k: calls.append(a) or b"")
    monkeypatch.setattr(m, "merge_heads", lambda path="": ["a" * 40, "b" * 40])
    assert m.staged(merging=True) == [("", [[]])]
    assert len(calls) == 1, calls


def test_an_external_diff_that_calls_a_dash_equal_cannot_skip_pre_commit(tmp_path):
    root = _repo(tmp_path)
    (root / "scripts").mkdir()
    shutil.copytree(REPO / ".githooks", root / ".githooks")
    shutil.copy(CHECK, root / "scripts/staged_dash_check.py")
    shutil.copy(REPO / "scripts/bounded_run.py", root / "scripts/bounded_run.py")
    (root / "scripts/lint.sh").write_text("exit 0\n")
    (root / "same.sh").write_text("#!/bin/sh\nexit 0\n")
    (root / "same.sh").chmod(0o755)
    _git(root, "config", "core.hooksPath", ".githooks")
    _git(root, "config", "diff.external", str(root / "same.sh"))
    _git(root, "config", "diff.trustExitCode", "true")                  # its exit status decides "no change"
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    r = _sub(["git", "commit", "-q", "-m", "dash"], cwd=root, text=True)
    assert r.returncode != 0 and "doc.md gains 1" in r.stderr, r.stdout + r.stderr


def test_a_merge_whose_parents_share_no_changed_path_reads_no_further_parent(monkeypatch):
    def raw(path):
        return b":100644 100644 " + b"1" * 40 + b" " + b"2" * 40 + b" M\0" + path + b"\0"
    answers = {(): raw(b"a.md"), ("a" * 40,): raw(b"b.md")}
    calls = []

    def git(*args, **_kw):
        calls.append(args)
        return answers[args[8:]]                            # after diff --cached and its seven options
    monkeypatch.setattr(m, "_git", git)
    monkeypatch.setattr(m, "merge_heads", lambda path="": ["a" * 40, "b" * 40])
    assert m.staged(merging=True) == [("", [[]])]
    assert len(calls) == 2, calls                           # HEAD and the first parent; never the second


def test_a_failed_destination_lookup_says_it_cannot_prompt(tmp_path):
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "clean\n"})
    _commit(root, "clean")
    tip = _git(root, "rev-parse", "HEAD")
    r = _push_check(root, str(tmp_path / "missing.git"), f"refs/heads/main {tip} refs/heads/main {m.NULL_OID}\n")
    assert r.returncode == 2 and "cannot prompt on the terminal" in r.stderr, r


def test_the_staged_check_reads_the_whole_index_from_a_subdirectory(tmp_path):
    root = _repo(tmp_path)
    _git(root, "config", "diff.relative", "true")
    _stage(root, {"docs/ok.md": "fine\n", "top.md": "bad " + DASH + " line\n"})
    r = _sub([sys.executable, str(CHECK)], cwd=root / "docs", env=_clean_env(None), capture_output=True, text=True,
             timeout=60)
    assert r.returncode == 1 and "top.md:1" in r.stderr, r


def test_a_merge_with_no_change_against_head_reads_no_merge_head(monkeypatch):
    monkeypatch.setattr(m, "_git", lambda *a, **k: b"")

    def never(path=""):
        raise AssertionError("MERGE_HEAD was read")
    monkeypatch.setattr(m, "merge_heads", never)
    assert m.staged(merging=True) == [("", [[]])]


def test_a_push_of_tips_the_destination_holds_walks_nothing(tmp_path, monkeypatch):
    root = _repo(tmp_path)
    tip = _git(root, "rev-parse", "HEAD")
    dest = _dest(tmp_path, root, "dest.git", {"refs/heads/main": tip})
    monkeypatch.chdir(root)
    for k in [k for k in os.environ if k.startswith("GIT_")]:
        monkeypatch.delenv(k)
    calls = []
    real = m._git
    monkeypatch.setattr(m, "_git", lambda *a, **k: calls.append(a) or real(*a, **k))
    assert m.outgoing([(m.NULL_OID, tip)], dest) == ([], 0)
    assert not any(a and a[0] == "rev-list" for a in calls), calls


def test_an_ordinary_commit_builds_no_merge_path_set(monkeypatch):
    class Changes(list):
        def __iter__(self):
            raise AssertionError("the changed paths were walked for a merge")
    changes = Changes([("a", "b", "p")])
    monkeypatch.setattr(m, "_git", lambda *a, **k: b"")
    monkeypatch.setattr(m, "changes_from_raw", lambda raw: changes)
    result = m.staged(merging=False)
    assert result[0][1][0] is changes


def test_a_failed_read_of_the_ref_updates_refuses_the_push(tmp_path):
    root = _repo(tmp_path)
    shutil.copytree(REPO / ".githooks", root / ".githooks")
    (root / "scripts").mkdir()
    for rel in ("scripts/staged_dash_check.py", "scripts/bounded_run.py"):
        shutil.copy(REPO / rel, root / rel)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    tip = _git(root, "rev-parse", "HEAD")
    # git's records arrive through cat: one whole record, then a read error.
    (bindir / "cat").write_text(f'#!/bin/sh\nif [ $# -eq 0 ]; then echo "refs/heads/main {tip} refs/heads/main '
                                f'{"0" * 40}"; exit 1; fi\nexec "{shutil.which("cat")}" "$@"\n')
    (bindir / "cat").chmod(0o755)
    env = _clean_env({"PATH": f"{bindir}:{os.environ['PATH']}", "SKIP_PUSH_IDENTITY_CHECK": "1",
                      "SKIP_HOST_GATES": "1", "SKIP_TOOLING_SUITE": "1", "SKIP_RUNNER_SUITE": "1",
                      "SKIP_CI_PARITY": "1", "SKIP_CI_PARITY_REASON": "fixture", "SKIP_STATIC_ANALYSIS": "1"})
    r = _sub(["bash", str(root / ".githooks/pre-push"), "origin", str(tmp_path / "nowhere.git")], cwd=root, env=env,
             capture_output=True, text=True, timeout=120)
    assert r.returncode == 1 and "could not be read from git" in r.stderr, r


def test_a_refused_blobs_lines_are_read_as_a_stream(tmp_path, monkeypatch):
    """Naming the lines reads the blob in chunks like the count: never one read of the whole file, and lines
    that cross a chunk edge come out whole, in UTF-8 and BOM-marked UTF-16 alike."""
    root = _repo(tmp_path)
    filler = "x" * (m.CHUNK - 5) + "\n"
    text = "a\n" + filler + "edge " + DASH + " here\r\n" + filler * 3 + "tail " + DASH
    _stage(root, {"u8.md": text.encode(), "u16.md": text.encode("utf-16")})
    _commit(root, "big")
    want = m.dash_lines(text.encode())
    _production_git_in(root, monkeypatch)
    reader = m.BlobReader()
    largest = []
    real_read = reader._io().read

    def read(n):
        largest.append(n)
        return real_read(n)
    reader.stream.read = read
    try:
        for name in ("u8.md", "u16.md"):
            assert reader.lines(_git(root, "rev-parse", f"HEAD:{name}")) == want, name
    finally:
        reader.stream.read = real_read
        assert reader.close() == 0
    assert len(want) == 2 and 0 < max(largest) <= max(m.CHUNK, m.BINARY_PREFIX), max(largest)


def test_dash_line_keys_fed_in_any_pieces_name_what_dash_lines_names():
    import random
    rng = random.Random(7)
    d = DASH.encode()
    text = b"a\r\n" + b"x" * 50 + d + b"\r\r\n" + b"plain\n" + d[:2] + b"|" + d[2:] + b"\n" + b"b" + d + b"\r"
    want = m.dash_lines(text)
    for _ in range(200):
        cuts = sorted(rng.sample(range(1, len(text)), rng.randint(1, 12)))
        keys = m.DashLineKeys()
        for a, b in zip([0, *cuts], [*cuts, len(text)]):
            keys.feed(text[a:b])
        assert keys.end() == want, cuts


def test_a_long_dash_free_line_is_never_held_while_naming_lines():
    import tracemalloc
    piece = b"y" * (1 << 16)
    keys = m.DashLineKeys()
    tracemalloc.start()
    try:
        for _ in range(128):                               # an 8 MiB line with no dash
            keys.feed(piece)
        keys.feed(b"\nz " + DASH.encode())
        _now, peak = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()
    assert keys.end() == [(2, m.line_key(("z " + DASH).encode()))] and peak < (1 << 18), peak


def test_a_change_that_names_no_blob_starts_no_reader(monkeypatch):
    def started(*_a, **_k):
        raise AssertionError("a blob reader was started")
    monkeypatch.setattr(m.bounded_run, "Stream", started)
    assert m.judge([("", [[("a" * 40, m.NULL_OID, "gone.md")]])]) == []


def test_a_stalled_merge_check_refuses_the_commit_instead_of_holding_it(tmp_path):
    """git's path lookup stalls (a git on PATH that never answers): commit-msg and pre-commit's gate refuse
    within the probe bound, never wait on it."""
    import os
    import time
    root = _hooked_repo(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "git").write_text("#!/bin/bash\nsleep 30\n")
    (bindir / "git").chmod(0o755)
    env = {**_clean_env(), "PATH": f"{bindir}:{os.environ['PATH']}", "DASH_GATE_PROBE_SECS": "1"}
    msg = tmp_path / "msg"
    msg.write_text("m\n")
    for cmd in (["bash", str(root / ".githooks/commit-msg"), str(msg)],
                ["bash", "-c", f'REPO_ROOT={str(root)!r}; . "$REPO_ROOT/.githooks/lib/dash-gate.sh"; dash_commit_gate']):
        began = time.monotonic()
        r = _sub(cmd, cwd=root, capture_output=True, text=True, timeout=20, env=env)
        assert r.returncode == 1 and "could not be read in time" in r.stdout + r.stderr, r
        assert time.monotonic() - began < 5, time.monotonic() - began
