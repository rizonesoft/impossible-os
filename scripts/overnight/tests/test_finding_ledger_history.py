#!/usr/bin/env python3
"""The ledger keeps section and round, and trimming archives rather than discards."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

LEDGER = pathlib.Path(__file__).resolve().parent.parent / "finding-ledger.py"


def test_fields_and_archive():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        subprocess.run(["git", "init", "-q", d], check=True)
        state = root / ".claude/state"
        os.makedirs(state)
        (state / "finding-triage.jsonl").write_text(
            "".join(json.dumps({"id": str(i), "decision": "fix"}) + "\n" for i in range(500)))
        r = subprocess.run([sys.executable, str(LEDGER), "record", "adversarial", "high", "a.c:1",
                            "--decision", "fix", "--todo", "t.md", "--evidence", "e",
                            "--section", "3", "--round", "2", "a title"],
                           cwd=d, capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        live = [json.loads(x) for x in (state / "finding-triage.jsonl").read_text().splitlines()]
        assert len(live) == 500
        assert live[-1]["section"] == "3" and live[-1]["round"] == "2"
        assert live[-1]["title"] == "a title", "flags must not leak into the title"
        arch = [json.loads(x) for x in (state / "finding-triage-archive.jsonl").read_text().splitlines()]
        assert [e["id"] for e in arch] == ["0"], arch


def test_refusal_record_still_needs_todo_and_evidence():
    with tempfile.TemporaryDirectory() as d:
        subprocess.run(["git", "init", "-q", d], check=True)
        r = subprocess.run([sys.executable, str(LEDGER), "record", "adversarial", "high", "a.c:1",
                            "--decision", "fix", "--section", "3", "a title"],
                           cwd=d, capture_output=True, text=True)
        assert r.returncode == 2, r


if __name__ == "__main__":
    test_fields_and_archive()
    test_refusal_record_still_needs_todo_and_evidence()
    print("OK")
