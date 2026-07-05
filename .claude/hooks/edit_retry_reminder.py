#!/usr/bin/env python3
# block-via: warning-only (PostToolUseFailure reminder; never blocks)
"""PostToolUseFailure (Edit): redirect a failed old_string match toward Grep.

Measured (run-20260704-213648.log, a single 14h overnight run): 28 "String to
replace not found in file" Edit failures. Each one is currently followed by a
blind whole-file re-read and a second guess at the exact current text -- a
known, long-standing model tendency ("file not read before edit"), not
something a hook can prevent outright. What a hook CAN do is make the retry
cheaper: instead of re-reading the whole file and guessing again, Grep a short
fragment of the attempted old_string to see the ACTUAL current text around
it, then build old_string from that. This does not fire on the "File has not
been read yet" freshness-gate failure -- that error is already fully
actionable on its own (it says exactly what to do next).

Selftest: python3 edit_retry_reminder.py --selftest
"""
from __future__ import annotations

import json
import sys

_NOT_FOUND_MARKER = "String to replace not found in file"


def _fragment(old_string: str, limit: int = 60) -> str:
    for line in old_string.splitlines():
        line = line.strip()
        if line:
            return line[:limit]
    return old_string.strip()[:limit]


def _error_text(tool_error) -> str:
    if isinstance(tool_error, dict):
        return str(tool_error.get("message") or "")
    return str(tool_error or "")


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    if d.get("hook_event_name") != "PostToolUseFailure":
        return 0
    if d.get("tool_name") not in ("Edit", "MultiEdit"):
        return 0
    err = _error_text(d.get("tool_error"))
    if _NOT_FOUND_MARKER not in err:
        return 0

    ti = d.get("tool_input") or {}
    old_string = ti.get("old_string")
    if not old_string and isinstance(ti.get("edits"), list) and ti["edits"]:
        first = ti["edits"][0]
        old_string = first.get("old_string") if isinstance(first, dict) else None
    if not isinstance(old_string, str) or not old_string.strip():
        return 0

    frag = _fragment(old_string)
    file_path = ti.get("file_path") or "the file"
    msg = (
        f"[edit-retry -- not a block] old_string did not match current content "
        f"in {file_path}. Before re-reading the whole file and guessing again, "
        f"Grep for a short distinctive fragment to see the ACTUAL current "
        f"text: pattern approximately \"{frag}\" -- then build old_string from "
        f"that exact match, not from memory of an earlier read."
    )
    print(json.dumps({"systemMessage": msg}))
    return 0


def _selftest() -> int:
    import contextlib
    import io
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    def run(payload):
        old = sys.stdin
        try:
            sys.stdin = io.StringIO(json.dumps(payload))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                main()
            return buf.getvalue().strip()
        finally:
            sys.stdin = old

    out = run({
        "hook_event_name": "PostToolUseFailure",
        "tool_name": "Edit",
        "tool_input": {"file_path": "/repo/x.c", "old_string": "  int foo(void) {\n"},
        "tool_error": "String to replace not found in file. String: ...",
    })
    check("edit-not-found-fires", "edit-retry" in out and "foo(void)" in out)

    out2 = run({
        "hook_event_name": "PostToolUseFailure",
        "tool_name": "Edit",
        "tool_input": {"file_path": "/repo/x.c", "old_string": "int foo(void) {\n"},
        "tool_error": "File has not been read yet. Read it first before writing to it.",
    })
    check("freshness-gate-silent", out2 == "")

    out3 = run({
        "hook_event_name": "PostToolUse",
        "tool_name": "Edit",
        "tool_input": {"file_path": "/repo/x.c", "old_string": "int foo(void) {\n"},
    })
    check("success-event-silent", out3 == "")

    out4 = run({
        "hook_event_name": "PostToolUseFailure",
        "tool_name": "Bash",
        "tool_input": {"command": "ls"},
        "tool_error": "String to replace not found in file",
    })
    check("non-edit-tool-silent", out4 == "")

    out5 = run({
        "hook_event_name": "PostToolUseFailure",
        "tool_name": "MultiEdit",
        "tool_input": {"file_path": "/repo/y.c",
                       "edits": [{"old_string": "  static void bar() {\n"}]},
        "tool_error": {"message": "String to replace not found in file. String: ..."},
    })
    check("multiedit-dict-error-fires", "bar()" in out5)

    if fails:
        sys.stderr.write("edit_retry_reminder selftest FAIL: " + "; ".join(fails) + "\n")
        return 1
    print("edit_retry_reminder selftest OK")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    sys.exit(main())
