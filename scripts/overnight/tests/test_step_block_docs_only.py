#!/usr/bin/env python3
"""A commit whose tree touches only todo/ and docs/ passes the skill-step gate;
anything else is still refused.

v19 capture (2026-09-03, and ~8 times in the 2026-09-27/28 attended session):
the gate refused every TODO-only deferral, shape repair and close-out sweep made
under implement-todo-section / complete-todo-file, and the honest answer was
always the blanket SKIP_SKILL_STEP_BLOCK. The exemption is decided on the whole
working tree (staged, unstaged AND untracked), because one command can only
commit what exists in the tree.

REFUSAL-DIRECTION controls: a modified source file, an untracked source file,
and a rename whose ORIGINAL path is source all keep the gate in force.
"""
from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOK = REPO / ".claude/hooks/skill_step_block.py"


def _load():
    if str(HOOK.parent) not in sys.path:
        sys.path.insert(0, str(HOOK.parent))
    spec = importlib.util.spec_from_file_location("skill_step_block", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _repo():
    root = pathlib.Path(tempfile.mkdtemp(prefix="ssb-docs-"))
    def git(*a):
        subprocess.run(["git", "-C", str(root), *a], check=True, capture_output=True)
    git("init", "-q")
    git("config", "user.email", "t@t"); git("config", "user.name", "t")
    for rel in ("todo/a.md", "docs/b.md", "src/k.c"):
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text("x\n")
    git("add", "-A"); git("commit", "-q", "-m", "base")
    return root, git


def main() -> int:
    m = _load()
    failures = []

    def expect(label, root, want):
        got = m._tree_is_docs_only(str(root))
        if got != want:
            failures.append(f"{label}: expected {want}, got {got}")

    root, git = _repo()
    expect("clean tree is not an exemption", root, False)
    (root / "todo/a.md").write_text("y\n")
    (root / "docs/new.md").write_text("n\n")
    expect("todo edit + new docs page -> exempt", root, True)
    git("add", "todo/a.md")
    expect("staged todo edit -> exempt", root, True)

    (root / "src/k.c").write_text("changed\n")
    expect("REFUSAL: a modified source file keeps the gate", root, False)
    subprocess.run(["git", "-C", str(root), "checkout", "--", "src/k.c"], check=True)
    expect("back to docs-only after reverting the source", root, True)

    (root / "src/new.c").write_text("int x;\n")
    expect("REFUSAL: an untracked source file keeps the gate", root, False)
    (root / "src/new.c").unlink()

    git("mv", "src/k.c", "todo/k.c")
    expect("REFUSAL: a rename whose original path is source keeps the gate", root, False)

    if failures:
        print("test_step_block_docs_only FAIL:\n  " + "\n  ".join(failures))
        return 1
    print("test_step_block_docs_only OK (7 cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
