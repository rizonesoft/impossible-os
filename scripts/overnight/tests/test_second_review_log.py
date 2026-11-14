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


FOUND = ("--finding", "fixed: the retry count is read before the lock",
         "--finding", "rejected: the buffer can overflow | it is static-asserted at 64 bytes, vmm.c:212",
         "--finding", "filed: no test for the error path | the memory manager roadmap, error-path item")


def test_each_finding_keeps_its_verdict_and_the_totals_come_from_them(tmp_path):
    root = _tree(tmp_path)
    r = _run(root, "record", "--todo", TODO, "--section", "7", *FOUND)
    assert r.returncode == 0, r.stderr
    assert f"{TODO} section 7: raised 3, real 2, rejected 1" in r.stdout
    assert "1 of 10 sections, raised 3, real 2, rejected 1 (9 more before deciding)" in r.stdout
    listing = _run(root, "stats", "--findings").stdout
    assert "  fixed: the retry count is read before the lock" in listing
    assert "  rejected: the buffer can overflow | it is static-asserted at 64 bytes, vmm.c:212" in listing
    assert "  filed: no test for the error path | the memory manager roadmap, error-path item" in listing
    # Recorded again, the section counts once, as its latest line. No finding at all is a record too.
    _run(root, "record", "--todo", TODO, "--section", "7", "--finding", "fixed: one thing")
    for n in range(8, 17):
        assert _run(root, "record", "--todo", TODO, "--section", str(n)).returncode == 0
    r = _run(root, "stats")
    assert r.returncode == 0 and "10 of 10 sections, raised 1, real 1, rejected 0 (decide: keep, narrow or drop)" in r.stdout


def test_refusal_a_finding_without_a_verdict_or_its_evidence_and_a_todo_that_does_not_exist(tmp_path):
    root = _tree(tmp_path)
    for finding, needle in (("the lock is wrong", "starts with fixed, rejected or filed"),
                            ("ignored: the lock is wrong", "starts with fixed, rejected or filed"),
                            ("fixed:", "says what was found"),
                            ("rejected: the lock is wrong", "the code evidence"),
                            ("rejected: the lock is wrong | ", "the code evidence"),
                            ("filed: no test", "the tracked item")):
        r = _run(root, "record", "--todo", TODO, "--section", "7", "--finding", finding)
        assert r.returncode == 2 and needle in r.stderr, (finding, r.stderr)
    r = _run(root, "record", "--todo", "todo/none.md", "--section", "7")
    assert r.returncode == 2 and "no such TODO" in r.stderr
    assert not (root / ".claude").exists()


def test_refusal_a_damaged_log_is_reported_not_skipped(tmp_path):
    root = _tree(tmp_path)
    log = root / ".claude/overnight/second-review-log.jsonl"
    log.parent.mkdir(parents=True)
    good = '{"todo": "x", "section": 7, "findings": [{"verdict": "fixed", "finding": "f", "note": ""}]}\n'
    for bad in ("{not json\n", '{"todo": "x", "section": 7, "raised": 1}\n',
                '{"todo": "x", "section": 7, "findings": [{"verdict": "maybe", "finding": "f"}]}\n'):
        log.write_text(good + bad)
        r = _run(root, "stats")
        assert r.returncode == 2 and "cannot be read" in r.stderr, bad
    log.write_text(good)
    assert _run(root, "stats").returncode == 0


def test_the_sections_marked_unavailable_are_listed_for_a_later_read(tmp_path):
    root = _tree(tmp_path)
    assert _run(root, "unavailable").returncode == 0
    (root / TODO).write_text("# X\n\nsecond: unavailable in prose is not a stamp\n\n## 7. Seven\n\n"
                             "> **Quality reviewed:** 2026-11-14 | Codex 4x | second: unavailable\n\n## 8. Eight\n\n"
                             "> **Quality reviewed:** 2026-11-14 | Codex 4x, Gemini 2x\n")
    r = _run(root, "unavailable")
    assert r.returncode == 1 and r.stdout.strip() == f"{TODO} section 7"
    # A stamp quoted in a fence, or under a heading with no number, is not a section's stamp.
    (root / TODO).write_text("# X\n\n## 7. Seven\n\n```\n> **Quality reviewed:** x | second: unavailable\n```\n\n"
                             "## History\n\n> **Quality reviewed:** x | second: unavailable\n")
    assert _run(root, "unavailable").returncode == 0
