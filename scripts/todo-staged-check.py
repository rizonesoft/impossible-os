#!/usr/bin/env python3
"""Commit-time backstop for TODO item length and prose wrapping.

WHY A SECOND LAYER EXISTS. `todo_item_line_length.py` is a PreToolUse hook and
it works -- it blocks an over-cap checklist item on Edit/Write, with a useful
message. It is simply invisible to the write path both the operator and the
unattended runner actually use most: a `python3 - <<'PY' ... PY` heredoc, or a
`python3 /tmp/rewrite.py`, executed through Bash. No Edit, no Write, no hook.

MEASURED 2026-07-28. Repo-wide there are ~1,407 checklist lead lines over the
250-char cap. Almost all predate the hook, but FIVE landed AFTER it shipped, in
exactly two commits, and both wrote TODO content through python scripts:

    8ac3c976  (operator)  290, 325, 337 chars
    c64bec7c  (runner)    350, 359 chars

So the cap is enforced against the careful path and unenforced against the
common one. Everything reaches a commit, which is why the backstop belongs
here.

ONLY ADDED LINES ARE JUDGED. The ~1,407 legacy violations are untouched: this
reads `git diff --cached` and considers only lines the commit ADDS. A cleanup
of the legacy set is a separate, deliberate piece of work; failing every commit
until it happens would just teach everyone to pass the opt-out.

Usage:
  todo-staged-check.py              check staged todo/**.md (pre-commit)
  todo-staged-check.py --selftest
Opt-out: SKIP_TODO_STAGED_CHECK=1
Stdlib only.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

CAP = 250
_ITEM = re.compile(r"^ *- \[[ xX/]\] ")


def _staged_todo_files():
    try:
        out = subprocess.run(
            ["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"],
            capture_output=True, text=True, check=False).stdout
    except OSError:
        return []
    return [f for f in out.split("\n")
            if f.strip().startswith("todo/") and f.strip().endswith(".md")]


def _added_lines(path):
    """[(lineno_in_new_file, text)] for lines this commit ADDS."""
    try:
        diff = subprocess.run(["git", "diff", "--cached", "-U0", "--", path],
                              capture_output=True, text=True, check=False).stdout
    except OSError:
        return []
    out, lineno = [], 0
    for line in diff.split("\n"):
        m = re.match(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@", line)
        if m:
            lineno = int(m.group(1))
            continue
        if line.startswith("+++") or line.startswith("---"):
            continue
        if line.startswith("+"):
            out.append((lineno, line[1:]))
            lineno += 1
        elif not line.startswith("-"):
            lineno += 1
    return out


def over_cap(added):
    return [(n, len(t), t) for n, t in added if _ITEM.match(t) and len(t) > CAP]


def main(argv) -> int:
    if "--selftest" in argv:
        return _selftest()
    if os.environ.get("SKIP_TODO_STAGED_CHECK") == "1":
        return 0
    files = _staged_todo_files()
    if not files:
        return 0
    bad = []
    for f in files:
        for n, ln, text in over_cap(_added_lines(f)):
            bad.append((f, n, ln, text))
    if not bad:
        return 0
    sys.stderr.write(
        "\n[todo-staged-check] %d NEW checklist item(s) exceed the %d-char cap.\n"
        "The PreToolUse hook missed these because the file was written through a\n"
        "script (python3 heredoc / rewrite) rather than Edit/Write -- that bypass\n"
        "is why this commit-time check exists.\n\n" % (len(bad), CAP))
    for f, n, ln, text in bad[:10]:
        sys.stderr.write("  %s:%d  %d chars (over by %d)\n      %s...\n"
                         % (f, n, ln, ln - CAP, text.strip()[:96]))
    sys.stderr.write(
        "\nItems are scannable one-line summaries. File:line citations, commit\n"
        "hashes, review round counts and investigation logs belong in the COMMIT\n"
        "MESSAGE or an indented continuation line, not the bullet. Aim for <= 230\n"
        "so small edits keep fitting.\n"
        "Check one line: python3 .claude/hooks/todo_item_line_length.py --check \"<line>\"\n"
        "Override (last resort): SKIP_TODO_STAGED_CHECK=1 git commit ...\n\n")
    return 1


def _selftest() -> int:
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    long_item = "- [ ] " + "x" * 300
    short_item = "- [ ] " + "x" * 100
    check("flags an over-cap added item",
          len(over_cap([(1, long_item)])) == 1)
    check("passes a short item", over_cap([(1, short_item)]) == [])
    check("indented item still judged",
          len(over_cap([(1, "      - [ ] " + "x" * 300)])) == 1)
    check("[x] and [/] judged too",
          len(over_cap([(1, "- [x] " + "y" * 300),
                        (2, "- [/] " + "z" * 300)])) == 2)
    # Prose and continuation lines are NOT checklist items and are exempt --
    # the cap is on the scannable bullet, not on body text.
    check("long prose exempt", over_cap([(1, "w" * 400)]) == [])
    check("long continuation exempt", over_cap([(1, "      " + "w" * 400)]) == [])
    check("table row exempt", over_cap([(1, "| " + "w" * 400 + " |")]) == [])
    # Boundary.
    check("exactly at cap passes",
          over_cap([(1, "- [ ] " + "x" * (CAP - 6))]) == [])
    check("one over cap fails",
          len(over_cap([(1, "- [ ] " + "x" * (CAP - 5))])) == 1)

    if fails:
        sys.stderr.write("todo-staged-check selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("todo-staged-check selftest OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
