#!/usr/bin/env python3
"""Refuse a commit that adds a line holding a Unicode en or em dash.

The dash rule (CLAUDE.md "Code Style: ASCII Dashes") was enforced only by
Claude's edit hook, which binds no other lane. This check runs from
.githooks/pre-commit, so it binds every lane that commits. It reads only the
lines the commit ADDS: the 2026-11-21 tree still holds dashes in about 225
older files, which are not swept, and an untouched line is never refused.

Generated files are exempt: their text is copied from sources this check has
already judged (todo/implementation-plan.md copies TODO titles).

Exit 0 when clean, 1 with one line per hit, 2 when the staged diff cannot be
read. SKIP_DASH_CHECK=1 skips it.
"""
import os
import subprocess
import sys

DASHES = (chr(0x2013), chr(0x2014))
GENERATED = {"todo/implementation-plan.md", "COUNT.md"}


def hits(diff: str) -> list:
    out, path, line = [], None, 0
    for raw in diff.splitlines():
        if raw.startswith("+++ "):
            name = raw[4:]
            path = name[2:] if name.startswith("b/") else None
            continue
        if raw.startswith("@@"):
            # @@ -a,b +c,d @@: added lines count up from c
            try:
                line = int(raw.split("+", 1)[1].split(" ", 1)[0].split(",", 1)[0])
            except (IndexError, ValueError):
                line = 0
            continue
        if raw.startswith("+") and path:
            if path not in GENERATED and any(d in raw for d in DASHES):
                out.append((path, line))
            line += 1
    return out


def main() -> int:
    if os.environ.get("SKIP_DASH_CHECK") == "1":
        return 0
    try:
        diff = subprocess.run(
            ["git", "diff", "--cached", "-U0", "--no-color", "--no-ext-diff", "--diff-filter=ACMR"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, check=True,
        ).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"[dash-check] the staged diff could not be read: {exc}", file=sys.stderr)
        return 2
    found = hits(diff)
    for path, line in found:
        print(f"[dash-check] {path}:{line} adds a Unicode en or em dash; rewrite it with a comma, colon, "
              "parentheses or a full stop", file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
