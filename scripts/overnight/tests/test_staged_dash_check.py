"""The staged dash check refuses an added dash in any lane's commit and leaves old lines alone."""
import io
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CHECK = REPO / "scripts/staged_dash_check.py"
DASH = chr(0x2014)


def _clean_env(extra=None):
    """The caller's repository settings never reach the fixture: a GIT_INDEX_FILE
    or GIT_DIR from an outer commit would make these commands write the caller's
    index."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_") and k != "SKIP_DASH_CHECK"}
    env.update(extra or {})
    return env


def _git(root, *a):
    subprocess.run(["git", *a], cwd=root, check=True, capture_output=True, timeout=30, env=_clean_env())


def _repo(tmp_path):
    root = tmp_path / "r"
    root.mkdir()
    _git(root, "init", "-q")
    _git(root, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--allow-empty", "-m", "base")
    return root


def _run(root, extra=None):
    env = _clean_env(extra)
    return subprocess.run([sys.executable, str(CHECK)], cwd=root, env=env, capture_output=True, text=True,
                          timeout=60)


def test_an_added_dash_is_refused_and_named(tmp_path):
    root = _repo(tmp_path)
    (root / "a.md").write_text("ok\nbad " + DASH + " line\n", encoding="utf-8")
    _git(root, "add", "a.md")
    r = _run(root)
    assert r.returncode == 1 and "a.md:2" in r.stderr, r


def test_an_old_dash_on_an_untouched_line_and_a_generated_file_pass(tmp_path):
    root = _repo(tmp_path)
    (root / "old.md").write_text("old " + DASH + " line\n", encoding="utf-8")
    _git(root, "add", "old.md")
    _git(root, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--no-verify", "-m", "old")
    (root / "old.md").write_text("old " + DASH + " line\nnew line\n", encoding="utf-8")
    (root / "todo").mkdir()
    (root / "todo/implementation-plan.md").write_text("title " + DASH + " copied\n", encoding="utf-8")
    _git(root, "add", "old.md", "todo/implementation-plan.md")
    r = _run(root)
    assert r.returncode == 0, r


def test_no_display_setting_name_or_content_hides_an_added_dash(tmp_path):
    """The class: whatever once fooled the patch parser (no b/ prefix, a quoted
    non-ASCII name, added text that looks like a header, a line separator inside
    a line, a symlink replaced by a file) still reports at the right line."""
    root = _repo(tmp_path)
    (root / "link").symlink_to("target")
    _git(root, "add", "link")
    _git(root, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--no-verify", "-m", "link")
    (root / "link").unlink()
    (root / "link").write_text("x " + DASH + "\n", encoding="utf-8")
    (root / "café.md").write_text("++ heading\nbad " + DASH + "\n", encoding="utf-8")
    (root / "sep.md").write_bytes(("a \fb " + DASH + "\n").encode())
    _git(root, "add", "-A")
    _git(root, "config", "diff.noprefix", "true")
    r = _run(root)
    assert r.returncode == 1, r
    for want in ("link:1", "café.md:2", "sep.md:1"):
        assert want in r.stderr, (want, r.stderr)


def _commit(root, msg):
    _git(root, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--no-verify", "-m", msg)


def test_git_settings_renames_and_odd_names_never_change_what_counts_as_added(tmp_path):
    """The class, second round: a rename, a widened hunk, a literal-pathspec
    environment and a name git reads as an index stage neither refuse an
    untouched dash nor hide an added one."""
    root = _repo(tmp_path)
    (root / "legacy.md").write_text("a\nold " + DASH + " line\nc\n", encoding="utf-8")
    (root / "moved.md").write_text("keep " + DASH + " me\n", encoding="utf-8")
    (root / "notes.md").write_text("clean\n", encoding="utf-8")
    _git(root, "add", "-A")
    _commit(root, "legacy")
    # clean: edit both sides of an untouched dash with a wide inter-hunk context, rename a file
    (root / "legacy.md").write_text("A\nold " + DASH + " line\nC\n", encoding="utf-8")
    _git(root, "mv", "moved.md", "renamed.md")
    _git(root, "config", "diff.interHunkContext", "100")
    _git(root, "add", "-A")
    env_extra = {"GIT_LITERAL_PATHSPECS": "1"}
    r = _run(root, env_extra)
    assert r.returncode == 0, r
    # refused: a dash added in a renamed file and in a file whose name looks like a stage
    _commit(root, "clean")
    _git(root, "mv", "renamed.md", "again.md")
    (root / "again.md").write_text("keep " + DASH + " me\nnew " + DASH + "\n", encoding="utf-8")
    (root / "0:notes.md").write_text("x\nbad " + DASH + "\n", encoding="utf-8")
    _git(root, "add", "-A")
    r = _run(root, env_extra)
    assert r.returncode == 1 and "again.md:2" in r.stderr and "0:notes.md:2" in r.stderr, r
    assert "again.md:1" not in r.stderr, r


def test_moved_lines_and_binary_blobs_are_not_additions(tmp_path):
    """A dash line moved to another file or out of a deleted file is not new; a
    second copy is; a PNG whose compressed bytes match the dash is not text."""
    import shutil
    root = _repo(tmp_path)
    para = "para " + DASH + " moved\n"
    (root / "a.md").write_text("one\n" + para, encoding="utf-8")
    (root / "b.md").write_text("two\n", encoding="utf-8")
    (root / "gone.md").write_text("gone " + DASH + " line\n", encoding="utf-8")
    _git(root, "add", "-A")
    _commit(root, "base")
    (root / "a.md").write_text("one\n", encoding="utf-8")
    (root / "b.md").write_text("two\n" + para, encoding="utf-8")
    (root / "gone.md").unlink()
    (root / "c.md").write_text("other\ngone " + DASH + " line\n", encoding="utf-8")
    shutil.copy(REPO / "resources/icons/color/64/settings.png", root / "icon.png")
    _git(root, "add", "-A")
    r = _run(root)
    assert r.returncode == 0, r
    (root / "b.md").write_text("two\n" + para + para, encoding="utf-8")
    _git(root, "add", "-A")
    r = _run(root)
    assert r.returncode == 1 and "b.md:3" in r.stderr and "icon.png" not in r.stderr, r


def test_the_fixture_never_writes_the_callers_index(tmp_path, monkeypatch):
    outer = tmp_path / "outer-index"
    outer.write_bytes(b"caller state")
    monkeypatch.setenv("GIT_INDEX_FILE", str(outer))
    root = _repo(tmp_path)
    (root / "x.md").write_text("x\n", encoding="utf-8")
    _git(root, "add", "x.md")
    assert _run(root).returncode == 0
    assert outer.read_bytes() == b"caller state"


def test_matching_is_per_file_first_and_generated_files_lend_nothing(tmp_path):
    root = _repo(tmp_path)
    legacy = "legacy " + DASH + " row\n"
    (root / "z.md").write_text(legacy + "tail\n", encoding="utf-8")
    (root / "COUNT.md").write_text("# Count " + DASH + " heading\n1\n", encoding="utf-8")
    _git(root, "add", "-A")
    _commit(root, "base")
    # a copy of z.md's untouched line: the copy is named, never the original
    (root / "a.md").write_text(legacy, encoding="utf-8")
    (root / "z.md").write_text(legacy + "tail edited\n", encoding="utf-8")
    # a retained generated heading copied elsewhere is new
    (root / "COUNT.md").write_text("# Count " + DASH + " heading\n2\n", encoding="utf-8")
    (root / "notes.md").write_text("# Count " + DASH + " heading\n", encoding="utf-8")
    _git(root, "add", "-A")
    r = _run(root)
    assert r.returncode == 1 and "a.md:1" in r.stderr and "notes.md:1" in r.stderr, r
    assert "z.md" not in r.stderr and "COUNT.md" not in r.stderr, r


def test_a_short_blob_read_is_an_error_not_clean():
    sys.path.insert(0, str(CHECK.parent))
    import staged_dash_check as m

    class Short:
        def __init__(self):
            self.stdin = io.BytesIO()
            self.stdout = io.BytesIO(b"abc blob 100\nclean")

    reader = m.BlobReader.__new__(m.BlobReader)
    reader.proc, reader.cache = Short(), {}
    try:
        reader._read("abc")
    except ValueError as exc:
        assert "short" in str(exc)
    else:
        raise AssertionError("a truncated blob was accepted")
