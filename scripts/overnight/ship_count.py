#!/usr/bin/env python3
"""How many sections a range of commits shipped. Frozen with the supervisor.

A section ships when its Implementation Order row turns `[x]`. That is the repo's own
definition of a section ship (CLAUDE.md), and it is what a stamp count got wrong: a
reopened section replaces its stamps when it ships again, so counting stamps by net
change read the 2026-11-08 ship of `00-infrastructure/TODO-04` s56 as zero.
Correcting a stamp touches no row, so it still counts as nothing.
"""
from __future__ import annotations

import re
import subprocess

_FILE_RE = re.compile(r"^\+\+\+ b/(.*)$")
_ROW_RE = re.compile(r"^([+-])\|.*?\|\s*(\d+)\s*\|.*\|\s*\[([ x/])\]\s*\|\s*$")


def ships_in_diff(diff: str) -> int:
    """Rows that went from open or in progress to `[x]`, per file and section."""
    before, after = {}, {}
    current = None
    for line in diff.splitlines():
        m = _FILE_RE.match(line)
        if m:
            current = m.group(1)
            continue
        m = _ROW_RE.match(line)
        if m and current:
            key = (current, int(m.group(2)))
            (before if m.group(1) == "-" else after)[key] = m.group(3)
    return sum(1 for key, status in after.items()
               if status == "x" and before.get(key, "x") != "x")


def ships_between(root, base: str, tip: str, timeout: int = 30) -> int:
    r = subprocess.run(["git", "-C", str(root), "diff", "--no-color", "--unified=0",
                        f"{base}..{tip}", "--", "todo"],
                       capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip()[:200])
    return ships_in_diff(r.stdout)
