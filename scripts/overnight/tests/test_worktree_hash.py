#!/usr/bin/env python3
"""Contract tests for worktree_hash.py -- the working-tree content-binding
primitive that section-manifest / evidence-bundle / impact-cone now share.

The bug this guards (2026-07-11): evidence was bound to git-index blobs, so a
mid-implementation session's unstaged/untracked changes reused stale evidence.
These pin that hashing follows WORKING-TREE bytes, untracked included.
"""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from worktree_hash import content_hashes, worktree_key, changed_paths  # noqa: E402


def _repo(d):
    root = pathlib.Path(d)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    return root


def _commit(root, *paths):
    subprocess.run(["git", "-C", str(root), "add", *paths], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t", "-c",
                    "user.name=t", "commit", "-qm", "c"], check=True)


def test_content_hash_follows_worktree_not_index():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        (root / "a.c").write_text("int a;\n")
        _commit(root, "a.c")
        h0 = content_hashes(root, ["a.c"])["a.c"]
        assert h0.startswith("sha256:"), h0
        # unstaged edit must change the hash even though the INDEX blob is the same
        (root / "a.c").write_text("int a; int b;\n")
        h1 = content_hashes(root, ["a.c"])["a.c"]
        assert h1 != h0, "unstaged edit did not change the content hash"


def test_untracked_file_is_hashed_and_absent_reported():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        (root / "new.c").write_text("int n;\n")   # untracked
        h = content_hashes(root, ["new.c", "missing.c"])
        assert h["new.c"].startswith("sha256:"), h
        assert h["missing.c"] == "absent", h


def test_worktree_key_stable_and_sensitive():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        (root / "a.c").write_text("x\n")
        k_a = worktree_key(root, ["a.c"])
        assert k_a == worktree_key(root, ["a.c"]), "key not stable for same bytes"
        (root / "a.c").write_text("y\n")
        assert worktree_key(root, ["a.c"]) != k_a, "key not sensitive to a byte change"


def test_changed_paths_includes_untracked_and_unstaged():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        (root / "tracked.c").write_text("1\n")
        _commit(root, "tracked.c")
        (root / "tracked.c").write_text("2\n")     # unstaged edit
        (root / "untracked.c").write_text("3\n")   # untracked
        changed = changed_paths(root)
        assert "tracked.c" in changed, changed
        assert "untracked.c" in changed, changed


if __name__ == "__main__":
    test_content_hash_follows_worktree_not_index()
    test_untracked_file_is_hashed_and_absent_reported()
    test_worktree_key_stable_and_sensitive()
    test_changed_paths_includes_untracked_and_unstaged()
    print("PASS: worktree-hash")
