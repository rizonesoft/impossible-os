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


def _git(root, *a):
    return subprocess.run(["git", *a], cwd=root, check=True, capture_output=True, timeout=30,
                          env=_clean_env()).stdout.decode().strip()


def _repo(tmp_path):
    root = tmp_path / "r"
    root.mkdir(parents=True)
    _git(root, "init", "-q")
    _git(root, "config", "user.name", "t")       # no fixture git step may need the caller's identity
    _git(root, "config", "user.email", "t@t")
    _commit(root, "base", "--allow-empty")
    return root


def _commit(root, msg, *extra):
    _git(root, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", *extra, "-m", msg)  # a fresh repo: no hooks


def _run(root, *args, extra=None):
    return subprocess.run([sys.executable, str(CHECK), *args], cwd=root, env=_clean_env(extra),
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
    try:
        m.as_text(("ok " + DASH + "\n").encode("utf-16") + b"\n")
    except ValueError as exc:
        assert "UTF-16" in str(exc)
    else:
        raise AssertionError("a truncated UTF-16 file read as clean")

    class Short:
        stdin = io.BytesIO()
        stdout = io.BytesIO(b"abc blob 100\nclean")

    reader = m.BlobReader.__new__(m.BlobReader)
    reader.proc, reader.counts = Short(), {}
    try:
        reader._read("abc")
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
            r = subprocess.run(["bash", "-c", f'set -euo pipefail; REPO_ROOT="{fake}"; . "{GATE}"; {call} || exit 7'],
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
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "merge", "-q", "--no-commit", "side"],
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
    _git(root, "config", "core.hooksPath", ".githooks")
    return root


def _auto_merge(root):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "merge", "--no-edit", "side"],
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
    """A commit another remote already tracks is still outgoing to this one."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    _git(root, "remote", "add", "mirror", str(tmp_path / "mirror.git"))
    _git(root, "remote", "add", "origin", str(tmp_path / "origin.git"))
    _git(root, "update-ref", "refs/remotes/mirror/main", tip)
    _git(root, "update-ref", "refs/remotes/origin/main", "HEAD~1")
    assert _run(root, "--range", m.NULL_OID, tip).returncode == 0      # no remote named: all remotes
    r = _run(root, "--range", m.NULL_OID, tip, "origin")
    assert r.returncode == 1 and "doc.md gains 1" in r.stderr, r
    assert _run(root, "--range", m.NULL_OID, tip, "mirror").returncode == 0
    assert _run(root, "--range", m.NULL_OID, tip, "https://example.org/r.git").returncode == 0


def test_a_merge_the_user_commits_is_judged_once_by_commit_msg(tmp_path):
    """pre-commit leaves a merge to commit-msg, which refuses a dash the
    resolution adds; outside a merge pre-commit's gate still judges."""
    root = _hooked_repo(tmp_path)
    gate = f'set -euo pipefail; REPO_ROOT="{root}"; . "{GATE}"; dash_commit_gate'
    _stage(root, {"f.md": "a\n"})
    _commit(root, "base")
    _git(root, "checkout", "-q", "-b", "side")
    _stage(root, {"f.md": "side\n"})
    _commit(root, "side")
    _git(root, "checkout", "-q", "-")
    _stage(root, {"f.md": "main\n"})
    _commit(root, "main")
    subprocess.run(["git", "merge", "-q", "side"], cwd=root, capture_output=True, timeout=30, env=_clean_env())
    assert _git(root, "rev-parse", "MERGE_HEAD") == _git(root, "rev-parse", "side")
    _stage(root, {"f.md": "resolved " + DASH + "\n"})
    held = subprocess.run(["bash", "-c", gate], cwd=root, capture_output=True, text=True, timeout=60,
                          env=_clean_env())
    assert held.returncode == 0 and "[dash-check]" not in held.stderr, held
    r = subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--no-edit"],
                       cwd=root, capture_output=True, text=True, timeout=60, env=_clean_env())
    assert r.returncode != 0 and "f.md gains 1" in r.stderr, r
    _git(root, "merge", "--abort")
    _stage(root, {"g.md": "new " + DASH + "\n"})
    plain = subprocess.run(["bash", "-c", gate], cwd=root, capture_output=True, text=True, timeout=60,
                           env=_clean_env())
    assert plain.returncode == 1 and "g.md gains 1" in plain.stderr, plain


def test_a_change_of_line_endings_names_only_the_added_line(tmp_path):
    root = _repo(tmp_path)
    _stage(root, {"f.md": "legacy " + DASH + "\nplain\n"})
    _commit(root, "base")
    (root / "f.md").write_bytes(("legacy " + DASH + "\r\nadded " + DASH + "\r\nplain\r\n").encode())
    _git(root, "add", "f.md")
    r = _run(root)
    assert r.returncode == 1 and "f.md gains 1" in r.stderr and "f.md:2 is a new" in r.stderr, r
    assert "f.md:1" not in r.stderr, r


def test_one_push_reads_a_commit_two_refs_carry_once(tmp_path, monkeypatch):
    """A branch and a new tag on the same tip: each outgoing commit is judged once."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    base = _git(root, "rev-parse", "HEAD~1")
    stdin = f"refs/heads/main {tip} refs/heads/main {base}\nrefs/tags/v1 {tip} refs/tags/v1 {m.NULL_OID}\n"
    stdin += f"(delete) {m.NULL_OID} refs/heads/old {base}\n"
    r = subprocess.run([sys.executable, str(CHECK), "--push", "origin"], cwd=root, input=stdin,
                       env=_clean_env(), capture_output=True, text=True, timeout=60)
    assert r.returncode == 1 and r.stderr.count("doc.md gains 1") == 1, r
    _production_git_in(root, monkeypatch)
    labels = [label for label, _changes in m.outgoing(m.push_updates(stdin), "origin")]
    # The new tag walks nothing below main's advertised remote oid: only the tip is judged.
    assert labels == [f"commit {tip[:12]}: "], labels


@pytest.mark.parametrize("space", ["\u00a0", "\u2028"])
def test_a_ref_name_holding_other_whitespace_is_one_field(tmp_path, space):
    """git accepts a U+00A0 or a U+2028 in a tag name; neither may split a field or a record."""
    root = _repo(tmp_path)
    tip = _git(root, "rev-parse", "HEAD")
    _git(root, "update-ref", "refs/remotes/origin/main", tip)
    tag = f"refs/tags/Release{space}1"
    assert subprocess.run(["git", "check-ref-format", tag], cwd=root, timeout=30).returncode == 0
    stdin = f"refs/heads/main {tip} refs/heads/main {tip}\n{tag} {tip} {tag} {m.NULL_OID}\n"
    r = subprocess.run([sys.executable, str(CHECK), "--push", "origin"], cwd=root, input=stdin,
                       env=_clean_env(), capture_output=True, text=True, timeout=60)
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
    """A configured remote with no tracking refs holds nothing: a commit origin
    already has is still judged on its first push there."""
    root = _repo(tmp_path)
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    tip = _git(root, "rev-parse", "HEAD")
    _git(root, "update-ref", "refs/remotes/origin/main", tip)
    _git(root, "remote", "add", "second", str(tmp_path / "second.git"))
    stdin = f"refs/heads/main {tip} refs/heads/main {m.NULL_OID}\n"
    r = subprocess.run([sys.executable, str(CHECK), "--push", "second"], cwd=root, input=stdin,
                       env=_clean_env(), capture_output=True, text=True, timeout=60)
    assert r.returncode == 1 and "doc.md gains 1" in r.stderr, r
    origin = subprocess.run([sys.executable, str(CHECK), "--push", "origin"], cwd=root, input=stdin,
                            env=_clean_env(), capture_output=True, text=True, timeout=60)
    assert origin.returncode == 0, origin


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
    for rel in (".githooks/lib/push-records.sh", ".githooks/lib/dash-gate.sh", "scripts/staged_dash_check.py"):
        shutil.copy(REPO / rel, root / rel)
    hook = root / ".githooks/pre-push"
    hook.write_text('#!/usr/bin/env bash\nset -uo pipefail\nREPO_ROOT="$(git rev-parse --show-toplevel)"\n'
                    '. "$REPO_ROOT/.githooks/lib/push-records.sh"\n. "$REPO_ROOT/.githooks/lib/dash-gate.sh"\n'
                    'STDIN_DATA="$(cat)"\nPUSH_RECORDS="$(push_records <<< "$STDIN_DATA")"\n'
                    'while read -r _l loid rref roid; do echo "REC $loid $rref $roid" >&2; done <<< "$PUSH_RECORDS"\n'
                    'dash_push_gate "${1:-}" <<< "$STDIN_DATA" || exit 1\n')
    hook.chmod(0o755)
    bare = tmp_path / "origin.git"
    _git(tmp_path, "init", "-q", "--bare", str(bare))
    _git(root, "remote", "add", "origin", str(bare))
    _git(root, "config", "core.hooksPath", ".githooks")
    push = ["git", "push", "--dry-run", "origin", "HEAD@{0 seconds ago}:refs/heads/main"]
    tip = _git(root, "rev-parse", "HEAD")
    r = subprocess.run(push, cwd=root, capture_output=True, text=True, timeout=120, env=_clean_env())
    assert r.returncode == 0 and f"REC {tip} refs/heads/main {m.NULL_OID}" in r.stderr, r.stdout + r.stderr
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    _commit(root, "dash")
    r = subprocess.run(push, cwd=root, capture_output=True, text=True, timeout=120, env=_clean_env())
    assert r.returncode != 0 and "doc.md gains 1" in r.stderr, r.stdout + r.stderr
    shipped = (REPO / ".githooks/pre-push").read_text()
    assert shipped.count('done <<< "$PUSH_RECORDS"') == 6 and 'done <<< "$STDIN_DATA"' not in shipped
    assert 'dash_push_gate "${1:-}" <<< "$STDIN_DATA"' in shipped


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
        assert reader.text(oids[1]).count(DASH.encode()) == 1
    finally:
        assert reader.close() == 0


def test_commit_msg_passes_the_merge_head_it_found(monkeypatch, tmp_path):
    calls = []
    head = tmp_path / "MERGE_HEAD"
    head.write_text("a" * 40 + "\n")
    monkeypatch.setattr(m, "_git", lambda *a, **k: calls.append(a) or b"")
    assert m.staged(merge_head=str(head)) == [("", [[], []])]
    assert not any(a[:1] == ("rev-parse",) for a in calls), calls


def test_an_ordinary_commit_is_refused_by_the_shipped_pre_commit_hook(tmp_path):
    root = _repo(tmp_path)
    (root / "scripts").mkdir()
    shutil.copytree(REPO / ".githooks", root / ".githooks")
    shutil.copy(CHECK, root / "scripts/staged_dash_check.py")
    (root / "scripts/lint.sh").write_text("exit 0\n")       # the lint has its own suite; this test is the dash step
    _git(root, "config", "core.hooksPath", ".githooks")
    before = _git(root, "rev-parse", "HEAD")
    _stage(root, {"doc.md": "new " + DASH + "\n"})
    r = subprocess.run(["git", "commit", "-q", "-m", "dash"], cwd=root, capture_output=True, text=True, timeout=120,
                       env=_clean_env())
    assert r.returncode != 0 and "doc.md gains 1" in r.stderr, r.stdout + r.stderr
    assert _git(root, "rev-parse", "HEAD") == before
