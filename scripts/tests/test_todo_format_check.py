#!/usr/bin/env python3
"""Fixtures for .claude/hooks/todo_format_check.py, the TODO authoring check
that warns on a fenced C block over 15 lines and on a `(N.M Title)` checklist
label. Each case runs the real hook with a Write event, as the harness sends
it, and asserts whether it warns. The fence cases are the shapes its old
regex judged wrongly; fence state now comes from the shared tracker.

    python3 scripts/tests/test_todo_format_check.py
"""
import json
import pathlib
import subprocess
import sys

HOOK = pathlib.Path(__file__).resolve().parents[2] / ".claude" / "hooks" / "todo_format_check.py"
BODY = "\n".join(f"int x{i};" for i in range(16))
T3, T4 = "`" * 3, "`" * 4
CASES = [
    ("a long C block warns", f"{T3}c\n{BODY}\n{T3}\n", True),
    ("a short C block is silent", f"{T3}c\nint a;\n{T3}\n", False),
    ("a long tilde-fenced C block warns", f"~~~c\n{BODY}\n~~~\n", True),
    ("a C block inside an HTML comment is silent", f"<!--\n{T3}c\n{BODY}\n{T3}\n-->\n", False),
    ("a shorter inner run does not end a four-backtick block",
     f"{T4}c\nint a;\n{T3}\n{BODY}\n{T4}\n", True),
    ("the spec-block-ok opt-out is honoured",
     f"<!-- spec-block-ok: wire format -->\n{T3}c\n{BODY}\n{T3}\n", False),
    ("a long non-C block is silent", f"{T3}text\n{BODY}\n{T3}\n", False),
    ("two blocks written back to back stay two", f"{T3}c\nint a;\n{T3}\n{T3}c\n{BODY}\n{T3}\n", True),
    ("a C block after a blockquoted fence ends by container exit still warns",
     f"> {T3}text\n> example\n{T3}c\n{BODY}\n{T3}\n", True),
    ("a C block after a list-contained fence ends by container exit still warns",
     f"- item\n  {T3}text\n  example\n{T3}c\n{BODY}\n{T3}\n", True),
    ("an indented delimiter does not close a C block",
     f"{T3}c\nint a;\n    {T3}\n{BODY}\n{T3}\n", True),
    ("an N.M label warns", "- [ ] (1.2 Foo) bar\n", True),
    ("an N.M label inside a fence is silent", f"{T3}\n- [ ] (1.2 Foo) bar\n{T3}\n", False),
]


def warns(text):
    event = {"tool_name": "Write", "tool_input": {"file_path": "/repo/todo/x/TODO-01.md", "content": text}}
    proc = subprocess.run([sys.executable, str(HOOK)], input=json.dumps(event),
                          capture_output=True, text=True, timeout=60)
    if proc.returncode != 0:
        raise SystemExit(f"hook exited {proc.returncode}: {proc.stderr[-300:]}")
    return bool(proc.stdout.strip())


def main():
    bad = 0
    for label, text, want in CASES:
        ok = warns(text) == want
        bad += not ok
        print(f"{'PASS' if ok else 'FAIL'} {label}")
    print(f"{len(CASES) - bad}/{len(CASES)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
