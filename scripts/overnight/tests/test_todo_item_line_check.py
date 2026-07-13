#!/usr/bin/env python3
"""C1: todo_item_line_length.py --check gives a one-call fit query {len, cap,
overage, ok} so a rewrite is confirmed without a blind manual len() recount, and
the block message points at --check (not the old `python3 -c print(len(...))`)
and carries the C2 "file is UNCHANGED -> reuse old_string" note."""
import json
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOK = REPO / ".claude/hooks/todo_item_line_length.py"


def _check(line):
    r = subprocess.run([sys.executable, str(HOOK), "--check", line],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def test_check_reports_fit():
    ok = _check("- [ ] short item")
    assert ok["ok"] is True and ok["overage"] == 0 and ok["cap"] == 250
    long = _check("- [ ] " + "x" * 260)
    assert long["ok"] is False and long["len"] == 266 and long["overage"] == 16


def test_check_respects_commit_whitelist():
    # `Commit:` items are whitelisted -> ok even when long.
    res = _check("- [x] Commit: " + "y" * 300)
    assert res["ok"] is True and res["whitelisted"] is True


def test_check_non_item_is_ok():
    assert _check("just prose, not a checklist item")["is_checklist_item"] is False


def test_block_message_points_at_check_and_reuse_old_string():
    payload = {"tool_name": "Edit",
               "tool_input": {"file_path": "todo/x.md",
                              "new_string": "- [ ] " + "z" * 260}}
    r = subprocess.run([sys.executable, str(HOOK)], input=json.dumps(payload),
                       text=True, capture_output=True, env={**os.environ})
    assert r.returncode == 2
    assert "--check" in r.stderr, "block message must point at the --check mode"
    assert "file is UNCHANGED" in r.stderr, "block message must carry the C2 reuse-old_string note"
    assert "print(len(" not in r.stderr, "block message must NOT recommend the manual len() recount"


if __name__ == "__main__":
    test_check_reports_fit()
    test_check_respects_commit_whitelist()
    test_check_non_item_is_ok()
    test_block_message_points_at_check_and_reuse_old_string()
    print("PASS: todo_item_line_length --check (C1/C2)")
