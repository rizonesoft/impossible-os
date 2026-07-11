#!/usr/bin/env python3
"""Contract test for section-checkpoint.py -- durable state across resume.

Pins that a checkpoint captures cursor + head + worktree digest, and that `show`
marks it stale the moment HEAD or the working tree moves (so a resume never
trusts invalidated facts)."""
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
CP = HERE.parent / "section-checkpoint.py"


def _repo(d):
    root = pathlib.Path(d)
    (root / ".claude/state").mkdir(parents=True)
    (root / "src").mkdir()
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "src/a.c").write_text("int a;\n")
    (root / ".claude/state/sequencer-run.json").write_text(
        json.dumps({"active": True, "phase": "SECTIONS",
                    "file": "todo/T.md", "section_idx": 2, "pass_no": 1}))
    subprocess.run(["git", "-C", str(root), "add", "src/a.c"], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t", "-c",
                    "user.name=t", "commit", "-qm", "c"], check=True,
                   capture_output=True)
    return root


def _run(root, verb):
    r = subprocess.run([sys.executable, str(CP), verb, "--project", str(root)],
                       capture_output=True, text=True, cwd=str(root))
    assert r.returncode == 0, r.stderr
    return r.stdout


def test_write_then_show_current_then_stale():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        # no checkpoint yet
        assert json.loads(_run(root, "show"))["checkpoint"] is None
        # write captures cursor + head
        _run(root, "write")
        shown = json.loads(_run(root, "show"))
        assert shown["stale"] is False
        cp = shown["checkpoint"]
        assert cp["cursor"]["file"] == "todo/T.md"
        assert cp["cursor"]["section_idx"] == 2
        assert cp["head"]
        # a working-tree edit invalidates the checkpoint
        (root / "src/a.c").write_text("int a; int b;\n")
        assert json.loads(_run(root, "show"))["stale"] is True


if __name__ == "__main__":
    test_write_then_show_current_then_stale()
    print("PASS: section-checkpoint")
