"""The staged dash check refuses an added dash in any lane's commit and leaves old lines alone."""
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CHECK = REPO / "scripts/staged_dash_check.py"
DASH = chr(0x2014)


def _git(root, *a):
    subprocess.run(["git", *a], cwd=root, check=True, capture_output=True, timeout=30)


def _repo(tmp_path):
    root = tmp_path / "r"
    root.mkdir()
    _git(root, "init", "-q")
    _git(root, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--allow-empty", "-m", "base")
    return root


def _run(root):
    env = {k: v for k, v in os.environ.items() if k != "SKIP_DASH_CHECK"}
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
