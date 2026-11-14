#!/usr/bin/env python3
"""scripts/overnight/second-review-log.py: the second reviewer's record, and the sections it still owes."""
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPO / "scripts" / "overnight" / "second-review-log.py"
TODO = "todo/00-x/TODO-01-x.md"


def _run(root, *args):
    return subprocess.run([sys.executable, str(SCRIPT), "--root", str(root), *args], capture_output=True, text=True,
                          timeout=60)


def _tree(tmp_path, body="# X\n\n## 7. Seven\n\n## 8. Eight\n"):
    (tmp_path / TODO).parent.mkdir(parents=True)
    (tmp_path / TODO).write_text(body)
    return tmp_path


def test_a_section_is_recorded_once_and_the_total_says_when_to_decide(tmp_path):
    root = _tree(tmp_path)
    r = _run(root, "record", "--todo", TODO, "--section", "7", "--raised", "3", "--real", "2", "--rejected", "1")
    assert r.returncode == 0, r.stderr
    assert f"{TODO} section 7: raised 3, real 2, rejected 1" in r.stdout
    assert "1 of 10 sections, raised 3, real 2, rejected 1 (9 more before deciding)" in r.stdout
    # Recorded again, the section counts once, as its latest line.
    _run(root, "record", "--todo", TODO, "--section", "7", "--raised", "1", "--real", "1", "--rejected", "0")
    for n in range(8, 17):
        assert _run(root, "record", "--todo", TODO, "--section", str(n), "--raised", "0", "--real", "0",
                    "--rejected", "0").returncode == 0
    r = _run(root, "stats")
    assert r.returncode == 0 and "10 of 10 sections, raised 1, real 1, rejected 0 (decide: keep, narrow or drop)" in r.stdout


def test_refusal_counts_that_do_not_add_up_and_a_todo_that_does_not_exist(tmp_path):
    root = _tree(tmp_path)
    for args in (("--raised", "3", "--real", "1", "--rejected", "1"), ("--raised", "0", "--real", "-1", "--rejected", "1")):
        r = _run(root, "record", "--todo", TODO, "--section", "7", *args)
        assert r.returncode == 2 and "must equal raised" in r.stderr
    r = _run(root, "record", "--todo", "todo/none.md", "--section", "7", "--raised", "0", "--real", "0", "--rejected", "0")
    assert r.returncode == 2 and "no such TODO" in r.stderr
    assert not (root / ".claude").exists()


def test_refusal_a_damaged_log_is_reported_not_skipped(tmp_path):
    root = _tree(tmp_path)
    log = root / ".claude/overnight/second-review-log.jsonl"
    log.parent.mkdir(parents=True)
    log.write_text('{"todo": "x", "section": 7, "raised": 1, "real": 1, "rejected": 0}\n{not json\n')
    r = _run(root, "stats")
    assert r.returncode == 2 and "cannot be read" in r.stderr


def test_the_sections_marked_unavailable_are_listed_for_a_later_read(tmp_path):
    root = _tree(tmp_path)
    assert _run(root, "unavailable").returncode == 0
    (root / TODO).write_text("# X\n\nsecond: unavailable in prose is not a stamp\n\n## 7. Seven\n\n"
                             "> **Quality reviewed:** 2026-11-14 | Codex 4x | second: unavailable\n\n## 8. Eight\n\n"
                             "> **Quality reviewed:** 2026-11-14 | Codex 4x, Gemini 2x\n")
    r = _run(root, "unavailable")
    assert r.returncode == 1 and r.stdout.strip() == f"{TODO} section 7"
