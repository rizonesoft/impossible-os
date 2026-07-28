#!/usr/bin/env python3
# block-via: warning-only (never blocks -- reflowing is the author's call)
"""PreToolUse (Edit|Write on todo/**.md): hard-wrapped prose is the wrong shape.

`todo/` prose is authored ONE PARAGRAPH PER LINE and wrapped by the reader's
editor. Hard-wrapping to a fill column makes a section visibly inconsistent
with the file around it and turns every later edit into a reflow.

MEASURED 2026-07-28, TODO-21 after two sections were authored at a 120-col fill:

    sections 19-20 :  73 lines, avg 102, max  121
    rest of file   : 542 lines, avg 209, max 3655

The operator's rule, same day: "I thought we did away with hard wrapping lines,
especially in todos ... it is wrong compared to the other sections in most
todos. Also, we need to prevent this behaviour for the future."

WHY THE OBVIOUS CHECK IS NOT THE CHECK. "A paragraph must be one line" was
measured first and REJECTED: 236 of 255 files under todo/ contain multi-line
prose blocks, so that rule flags essentially the whole tree and would be
ignored within a day. The signature of a HARD WRAP is not "several lines", it
is several lines whose widths cluster in a narrow band and whose breaks land
mid-sentence -- the fingerprint of a fill column. That detector flags 36 files,
which matches the known offenders (the 120-col files authored recently, and
~34 older ones wrapped near 85).

WARNING-ONLY, deliberately, for two reasons. The content is CORRECT -- only its
shape is off, and blocking correct prose to enforce a layout preference trades
real friction for cosmetics. And a blocking gate here would fire on the
unattended runner's own TODO edits, where a wedge costs a whole night.

BUDGETED via `_advisory_budget` (cap 2/session): the habit is learned in one or
two reminders, and an unbudgeted nag becomes the cost problem T1-4 measured.

Selftest: python3 todo_wrap_reminder.py --selftest
"""
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

MIN_RUN = 3          # fewer lines than this says nothing about a fill column
BAND = 22            # allowed spread among the non-final line widths
LO, HI = 78, 138     # plausible fill columns

_MSG = (
    "[todo-wrap -- not a block] This edit hard-wraps prose at ~{col} columns. "
    "`todo/` prose is authored ONE PARAGRAPH PER LINE and wrapped by the "
    "reader's editor; a fill column makes the section inconsistent with the "
    "file around it (measured 2026-07-28 on TODO-21: sections 19-20 averaged "
    "102 chars/line against 209 for the rest of the file) and turns every later "
    "edit into a reflow. Re-emit each paragraph as a single physical line. The "
    "250-char cap on `- [ ]`/`- [x]`/`- [/]` LEAD lines still applies -- keep "
    "the lead short and put the body on an indented continuation line, also "
    "unwrapped."
)


def _is_prose(line: str) -> bool:
    s = line.strip()
    if not s:
        return False
    if s.startswith(("#", "-", "*", "|", ">", "```")):
        return False
    if re.match(r"^\d+[.)]", s):
        return False
    if line.startswith("    "):          # indented code block
        return False
    return True


def _blocks(lines):
    cur, incode = [], False
    for line in lines:
        if line.strip().startswith("```"):
            incode = not incode
            if cur:
                yield cur
                cur = []
            continue
        if incode:
            continue
        if _is_prose(line):
            cur.append(line)
        else:
            if cur:
                yield cur
            cur = []
    if cur:
        yield cur


def hard_wrapped(block) -> int:
    """Return the apparent fill column, or 0 if this block is not hard-wrapped."""
    if len(block) < MIN_RUN:
        return 0
    # The final line of a wrapped paragraph is short by nature; judging it would
    # break every correctly-wrapped block.
    head = [len(x.rstrip()) for x in block[:-1]]
    if len(head) < MIN_RUN - 1:
        return 0
    if not all(LO <= n <= HI for n in head):
        return 0
    if max(head) - min(head) > BAND:
        return 0
    # A wrapped line usually does NOT end a sentence. Requiring most breaks to
    # be mid-sentence separates a fill column from a run of short standalone
    # statements that merely happen to be similar lengths.
    mid = sum(1 for x in block[:-1]
              if not x.rstrip().endswith((".", ":", "!", "?", "|")))
    if mid < max(2, int(0.6 * len(head))):
        return 0
    return max(head)


def scan_text(text: str) -> int:
    for b in _blocks(text.split("\n")):
        col = hard_wrapped(b)
        if col:
            return col
    return 0


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _targets_todo(path: str) -> bool:
    if not path:
        return False
    p = path.replace("\\", "/")
    if not p.endswith(".md"):
        return False
    return "/todo/" in p or p.startswith("todo/")


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    if d.get("tool_name") not in ("Edit", "Write"):
        return 0
    ti = d.get("tool_input") or {}
    if not _targets_todo(str(ti.get("file_path") or "")):
        return 0
    text = str(ti.get("new_string") if d["tool_name"] == "Edit"
               else ti.get("content") or "")
    if not text:
        return 0
    col = scan_text(text)
    if not col:
        return 0
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import _advisory_budget
        if not _advisory_budget.should_emit(
                _repo_root(), "todo_wrap_reminder", cap=2,
                session_id=str(d.get("session_id") or "")):
            return 0
    except Exception:
        pass          # fail-open: a budget bug must never silence the hook
    print(json.dumps({"systemMessage": _MSG.format(col=col)}))
    return 0


def _selftest() -> int:
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    wrapped = (
        "`task_exec()` has no transactional commit point: it mutates the process image and THEN performs\n"
        "fallible work, so a late failure returns `-1` to a caller that iretqs back into an image that no\n"
        "longer exists. It also replaces `tasks[pid].stack_base` with a fresh guarded kernel stack and\n"
        "never reclaims the old one.\n"
    )
    check("detects a 120-col fill", scan_text(wrapped) > 0)

    one_line = (
        "`task_exec()` has no transactional commit point: it mutates the process image and THEN performs "
        "fallible work, so a late failure returns `-1` to a caller that iretqs back into an image that no "
        "longer exists.\n"
    )
    check("one-line paragraph is clean", scan_text(one_line) == 0)

    # Two-line prose is normal in 236/255 todo files and must never warn.
    check("two-line prose is clean", scan_text(
        "A short note about the thing.\nAnd a second line continuing it.\n") == 0)

    # Bullets, tables, headings and fenced code are not prose.
    check("bullets clean", scan_text(
        "- item one that is quite long and wraps near a hundred and ten columns or so here\n"
        "- item two that is quite long and wraps near a hundred and ten columns or so here\n"
        "- item three that is quite long and wraps near a hundred and ten columns or so\n") == 0)
    check("table clean", scan_text(
        "| a | b |\n| --- | --- |\n| 1 | 2 |\n") == 0)
    check("fenced code clean", scan_text(
        "```\nsome code line that is long enough to look like a wrap at about ninety columns ok\n"
        "another code line that is long enough to look like a wrap at about ninety cols ok\n"
        "a third code line that is long enough to look like a wrap at ninety-odd columns k\n```\n") == 0)

    # Sentences that each END a sentence are a list of statements, not a wrap.
    check("sentence-per-line clean", scan_text(
        "The first sentence here is about eighty-five characters long and ends properly.\n"
        "The second sentence here is about eighty-five characters long and ends properly.\n"
        "The third sentence here is about eighty-five characters long and ends properly.\n"
        "Short tail.\n") == 0)

    # Path targeting.
    check("todo path matches", _targets_todo("todo/02-kernel-core/TODO-21.md"))
    check("abs todo path matches", _targets_todo("/repo/todo/x/TODO-1.md"))
    check("non-todo md ignored", not _targets_todo("docs/infrastructure/x.md"))
    check("non-md ignored", not _targets_todo("todo/x.py"))

    # Never blocks.
    import contextlib
    import io
    for payload in (
            {"tool_name": "Edit", "session_id": "s", "tool_input":
                {"file_path": "todo/a.md", "new_string": wrapped}},
            {"tool_name": "Write", "session_id": "s2", "tool_input":
                {"file_path": "todo/a.md", "content": wrapped}},
            {"tool_name": "Bash", "tool_input": {"command": "ls"}}):
        old = sys.stdin
        try:
            sys.stdin = io.StringIO(json.dumps(payload))
            with contextlib.redirect_stdout(io.StringIO()):
                rc = main()
            check(f"never blocks: {payload['tool_name']}", rc == 0)
        finally:
            sys.stdin = old

    if fails:
        sys.stderr.write("todo_wrap_reminder selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("todo_wrap_reminder selftest OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(_selftest() if "--selftest" in sys.argv else main())
    except Exception:
        sys.exit(0)          # never break an edit over a layout preference
