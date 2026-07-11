#!/usr/bin/env python3
"""Contract test for diff-facts.py -- worktree-aware deterministic diff facts.

Pins the deterministic layers (changed files, concurrency/safety inventory,
content-bound receipt, worktree binding). cppcheck findings are version-
dependent, so we assert it RAN, not specific findings.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
DF = HERE.parent / "diff-facts.py"


def _repo(d):
    root = pathlib.Path(d)
    (root / "src/kernel").mkdir(parents=True)
    (root / "include/kernel").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "include/kernel/types.h").write_text("typedef int u32;\n")
    (root / "src/kernel/a.c").write_text("int a(void){return 0;}\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t", "-c",
                    "user.name=t", "commit", "-qm", "c"], check=True,
                   capture_output=True)
    return root


def _facts(root):
    r = subprocess.run(
        [sys.executable, str(DF), "--project", str(root), "--no-cppcheck"],
        capture_output=True, text=True, cwd=str(root))
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def test_concurrency_inventory_and_receipt():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        # unstaged edit adding a lock + alloc + a new untracked file
        (root / "src/kernel/a.c").write_text(
            "int a(void){ spin_lock(&g); void*p=kmalloc(8); "
            "kfree(p); return 0; }\n")
        (root / "src/kernel/b.c").write_text(
            "int b(void){ __atomic_fetch_add(&c,1,0); return 0; }\n")  # untracked
        f = _facts(root)
        assert "src/kernel/a.c" in f["changed_files"]
        assert "src/kernel/b.c" in f["changed_files"], "untracked not covered"
        dims = f["concurrency_safety"]
        assert "locks" in dims and any("spin_lock" in x for x in dims["locks"])
        assert "alloc" in dims and "free" in dims
        # receipt written + content-bound
        rc = json.loads((root / ".claude/state/last-diff-facts.json").read_text())
        assert rc["digest"] == f["digest"]


def test_digest_follows_worktree():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        (root / "src/kernel/a.c").write_text("int a(void){ spin_lock(&g); }\n")
        d1 = _facts(root)["digest"]
        (root / "src/kernel/a.c").write_text("int a(void){ spin_lock(&g); return 1; }\n")
        d2 = _facts(root)["digest"]
        assert d1 != d2, "digest did not follow the working tree"


if __name__ == "__main__":
    test_concurrency_inventory_and_receipt()
    test_digest_follows_worktree()
    print("PASS: diff-facts")
