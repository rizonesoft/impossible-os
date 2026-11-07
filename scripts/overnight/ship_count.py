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
_STATUS_RE = re.compile(r"^\[([ x/])\]$")


def _row(line: str):
    """(sign, section, status) for a changed Implementation Order row, or None.

    Read by cell, because layouts differ: most tables start with a badge column, and
    some put the section number first (`09-desktop-shell/TODO-14`, 30 rows in all)."""
    if not line[:2] in ("+|", "-|"):
        return None
    cells = [c.strip() for c in line[1:].strip().strip("|").split("|")]
    number = next((c for c in cells[:2] if c.isdigit()), None)
    status = next((m.group(1) for c in reversed(cells) for m in [_STATUS_RE.match(c)] if m), None)
    if number is None or status is None:
        return None
    return line[0], int(number), status


def ships_in_diff(diff: str) -> int:
    """Rows one commit turned from open or in progress to `[x]`, per file and section."""
    before, after = {}, {}
    current = None
    for line in diff.splitlines():
        m = _FILE_RE.match(line)
        if m:
            current = m.group(1)
            continue
        row = _row(line)
        if row and current:
            sign, number, status = row
            (before if sign == "-" else after)[(current, number)] = status
    return sum(1 for key, status in after.items()
               if status == "x" and before.get(key, "x") != "x")


def ships_between(root, base: str, tip: str, timeout: int = 30) -> int:
    """Ships along the commits from base to tip, one commit at a time. Endpoints alone
    miss a section reopened and shipped again inside the range."""
    r = subprocess.run(["git", "-C", str(root), "rev-list", "--reverse", "--first-parent",
                        f"{base}..{tip}"], capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip()[:200])
    total = 0
    for sha in r.stdout.split():
        d = subprocess.run(["git", "-C", str(root), "diff", "--no-color", "--unified=0",
                            f"{sha}~1", sha, "--", "todo"],
                           capture_output=True, text=True, timeout=timeout)
        if d.returncode != 0:
            raise RuntimeError(d.stderr.strip()[:200])
        total += ships_in_diff(d.stdout)
    return total
