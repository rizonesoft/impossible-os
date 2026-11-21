#!/usr/bin/env python3
"""Refuse a commit that adds a line holding a Unicode en or em dash.

The dash rule (CLAUDE.md "Code Style: ASCII Dashes") was enforced only by
Claude's edit hook, which binds no other lane. This check runs from
.githooks/pre-commit, so it binds every lane that commits. It reads only the
lines the commit ADDS: the 2026-11-21 tree still holds dashes in about 225
older files, which are not swept, and an untouched line is never refused.

Generated files are exempt: their text is copied from sources this check has
already judged (todo/implementation-plan.md copies TODO titles).

No patch text is parsed. Paths come from NUL-separated --name-only, added
line numbers from hunk headers only, and line text from the staged blob split
on LF, so no diff display setting, file name or file content can change what
is read.

Exit 0 when clean, 1 with one line per hit, 2 when the staged changes cannot
be read. SKIP_DASH_CHECK=1 skips it.
"""
import os
import subprocess
import sys

DASHES = (chr(0x2013).encode(), chr(0x2014).encode())
GENERATED = {"todo/implementation-plan.md", "COUNT.md"}
TIMEOUT = 60


def _git(*args) -> bytes:
    return subprocess.run(["git", *args], capture_output=True, timeout=TIMEOUT, check=True).stdout


def added_line_numbers(patch: bytes) -> list:
    """Line numbers (1-based, in the staged file) a -U0 diff says were added.

    Only the `@@ -a,b +c,d @@` headers are read: every content line of a patch
    starts with +, - or space, so a header is the only line starting with @@,
    and its counts do not depend on any diff display setting."""
    out = []
    for raw in patch.split(b"\n"):
        if not raw.startswith(b"@@ "):
            continue
        try:
            start, _, count = raw.split(b" ")[2][1:].partition(b",")
            first, n = int(start), int(count) if count else 1
        except (IndexError, ValueError):
            continue
        out.extend(range(first, first + n))
    return out


def hits() -> list:
    found = []
    for raw in _git("diff", "--cached", "--name-only", "-z", "--diff-filter=ACMRT").split(b"\0"):
        if not raw:
            continue
        path = raw.decode("utf-8", "surrogateescape")
        if path in GENERATED:
            continue
        patch = _git("diff", "--cached", "-U0", "--no-color", "--no-ext-diff", "--no-textconv",
                     "--", ":(literal)" + path)
        wanted = added_line_numbers(patch)
        if not wanted:
            continue                                     # binary, or nothing added
        lines = _git("cat-file", "blob", ":" + path).split(b"\n")   # LF only, as git counts
        for n in wanted:
            if 0 < n <= len(lines) and any(d in lines[n - 1] for d in DASHES):
                found.append((path, n))
    return found


def main() -> int:
    if os.environ.get("SKIP_DASH_CHECK") == "1":
        return 0
    try:
        found = hits()
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"[dash-check] the staged changes could not be read: {exc}", file=sys.stderr)
        return 2
    for path, line in found:
        print(f"[dash-check] {path}:{line} adds a Unicode en or em dash; rewrite it with a comma, colon, "
              "parentheses or a full stop", file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
