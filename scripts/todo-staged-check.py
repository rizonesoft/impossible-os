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


def wrapped_block(added):
    """Apparent fill column if the ADDED lines look hard-wrapped, else 0.

    The wrap rule had the same PreToolUse-only bypass this file exists to close
    (todo_wrap_reminder never sees a python3-via-Bash write), so it is checked
    here too. WARN, not block, unlike the item cap: the cap is long-standing
    policy already blocked at Edit, whereas the wrap rule is new (2026-07-28)
    and a blocking gate on it could refuse the unattended runner's own TODO
    edits, where a wedge costs a night.
    """
    try:
        sys.path.insert(0, os.path.join(
            os.path.dirname(os.path.abspath(__file__))))
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "_todo_reflow", os.path.join(os.path.dirname(
                os.path.abspath(__file__)), "todo-reflow.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except Exception:
        return 0                      # fail-open: never break a commit on this
    # PROSE only. Feeding every added line to the detector made the RECOMMENDED
    # repair trip the warning: converting an over-cap body into indented
    # sub-bullets adds a run of similar-width `- ` lines, which is exactly the
    # narrow-band-of-widths signature `_hard_wrapped` looks for. Observed
    # 2026-07-30 on TODO-04 -- the commit that split the 4,865-char QEMU-reap
    # item warned "hard-wrapped at ~111 columns" about its own fix. A bullet is
    # never hard-wrapped prose, and `_classify` already knows that (bullets,
    # headings, tables, quotes and fences all classify as `struct`).
    #
    # prev_blank=False deliberately: in a diff the preceding line is unknown,
    # and False keeps an indented continuation line classified as prose (the
    # thing we DO want to wrap-check) rather than as an indented code block.
    block = [t for _, t in added if mod._classify(t, False) == "prose"]
    for i in range(len(block)):
        for j in range(i + 3, len(block) + 1):
            if mod._hard_wrapped(block[i:j]):
                return max(len(x.rstrip()) for x in block[i:j - 1])
    return 0


def main(argv) -> int:
    if "--selftest" in argv:
        return _selftest()
    if os.environ.get("SKIP_TODO_STAGED_CHECK") == "1":
        return 0
    files = _staged_todo_files()
    if not files:
        return 0
    bad, wrapped = [], []
    for f in files:
        added = _added_lines(f)
        for n, ln, text in over_cap(added):
            bad.append((f, n, ln, text))
        col = wrapped_block(added)
        if col:
            wrapped.append((f, col))
    for f, col in wrapped:
        sys.stderr.write(
            "[todo-staged-check WARN] %s: this commit adds prose hard-wrapped at "
            "~%d columns. todo/ prose is one paragraph per physical line. Repair: "
            "python3 scripts/todo-reflow.py --write %s\n" % (f, col, f))
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

    # --- wrapped_block must not fire on the RECOMMENDED repair (2026-07-30) --
    # Splitting an over-cap body into indented sub-bullets adds a run of
    # similar-width `- ` lines -- the same narrow-band signature a fill column
    # has. Before the prose filter at wrapped_block(), the commit that split
    # TODO-04's 4,865-char QEMU-reap item warned about its own fix.
    subs = [(i, "        - " + w) for i, w in enumerate([
        "unlocked shared temp files letting a concurrent process publish a key",
        "the key being blind to `ABI_CLANG` overrides and some more text here",
        "header concatenation ambiguous across a sorted-adjacent boundary now",
        "a wrapper-identity check stopping at file bytes not a behavior print"])]
    check("sub-bullet run is not a fill column", wrapped_block(subs) == 0)
    # ... while a genuine fill column is still caught.
    real = [(i, t) for i, t in enumerate((
        "`task_exec()` has no transactional commit point: it mutates the image and THEN performs\n"
        "fallible work, so a late failure returns `-1` to a caller that iretqs back into an image\n"
        "that no longer exists. It also replaces the stack base with a fresh guarded kernel stack\n"
        "and never reclaims the old one.").split("\n"))]
    check("genuine hard wrap still caught", wrapped_block(real) > 0)

    if fails:
        sys.stderr.write("todo-staged-check selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("todo-staged-check selftest OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
