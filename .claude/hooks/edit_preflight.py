#!/usr/bin/env python3
"""Deterministic Edit preflight: fail stale edits with a re-anchor hint.

102 of 544 Edit/Write attempts failed in run-20260710, most on stale
old_string (the file moved under the edit, or the string was paraphrased from
memory) -- each failure costing a blind retry round. The harness already
rejects these; this hook rejects them EARLIER with the remediation the retry
actually needs: the closest real line(s) in the current file, so the next
attempt anchors on current content instead of another from-memory variant.

Checks (Edit tool only; Write/MultiEdit pass through):
  0 matches   -> EDIT-STALE block + the closest-matching real lines
  2+ matches without replace_all -> EDIT-AMBIGUOUS block + the count

Fail-open on any error (a broken preflight must never block real edits).
--selftest exercises the match logic.
"""
from __future__ import annotations

import difflib
import json
import sys

MAX_HINT_LINES = 3


def _closest_lines(needle: str, content: str) -> list:
    """The file lines closest to the FIRST substantive line of the stale
    old_string -- the re-anchor hint."""
    target = next((ln for ln in needle.splitlines() if ln.strip()), "").strip()
    if not target:
        return []
    lines = content.splitlines()
    stripped = [ln.strip() for ln in lines]
    best = difflib.get_close_matches(target, [s for s in stripped if s],
                                     n=1, cutoff=0.4)
    if not best:
        return []
    idx = stripped.index(best[0])
    lo = max(0, idx - 1)
    return [f"  {i + 1}: {lines[i]}"[:200]
            for i in range(lo, min(len(lines), lo + MAX_HINT_LINES))]


def check(tool_input: dict) -> tuple[bool, str]:
    """Pure check: (allow, message)."""
    path = tool_input.get("file_path") or ""
    old = tool_input.get("old_string")
    if not path or not old:
        return True, ""
    try:
        content = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return True, ""  # missing file: let the tool report it
    n = content.count(old)
    if n == 1 or (n >= 1 and tool_input.get("replace_all")):
        return True, ""
    if n == 0:
        hint = _closest_lines(old, content)
        hint_txt = ("Closest current content:\n" + "\n".join(hint) + "\n") \
            if hint else ""
        return False, (
            f"[EDIT-STALE] old_string not found in {path} (0 matches) -- the "
            f"file content differs from what the edit assumes. {hint_txt}"
            "Re-read a slice around the target and re-anchor on CURRENT "
            "content; do not retry a paraphrased variant. "
            "Details: docs/infrastructure/hook-codes.md#edit-stale")
    return False, (
        f"[EDIT-AMBIGUOUS] old_string occurs {n}x in {path} without "
        "replace_all -- extend the string with surrounding context to make it "
        "unique, or set replace_all for a deliberate global change. "
        "Details: docs/infrastructure/hook-codes.md#edit-stale")


def _selftest() -> int:
    import tempfile
    import os
    fails = []
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as tf:
        tf.write("int frob(void) {\n    return level + 1;\n}\n"
                 "int frob2(void) {\n    return level + 1;\n}\n")
        p = tf.name
    try:
        ok, _ = check({"file_path": p, "old_string": "int frob(void) {"})
        if not ok:
            fails.append("unique match blocked")
        ok, msg = check({"file_path": p, "old_string": "return levl + 1;"})
        if ok or "EDIT-STALE" not in msg or "level + 1" not in msg:
            fails.append(f"stale string not caught with hint: {msg[:120]}")
        ok, msg = check({"file_path": p, "old_string": "    return level + 1;"})
        if ok or "EDIT-AMBIGUOUS" not in msg:
            fails.append("ambiguous match not caught")
        ok, _ = check({"file_path": p, "old_string": "    return level + 1;",
                       "replace_all": True})
        if not ok:
            fails.append("replace_all blocked")
        ok, _ = check({"file_path": "/nonexistent/x.c", "old_string": "x"})
        if not ok:
            fails.append("missing file blocked (should pass through)")
    finally:
        os.unlink(p)
    if fails:
        print("\n".join("FAIL: " + f for f in fails), file=sys.stderr)
        return 1
    print("edit_preflight selftest OK")
    return 0


def main() -> int:
    if "--selftest" in sys.argv:
        return _selftest()
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if d.get("tool_name") != "Edit":
        return 0
    try:
        allow, msg = check(d.get("tool_input") or {})
    except Exception:
        return 0  # fail-open
    if allow:
        return 0
    sys.stderr.write(msg)
    return 2


if __name__ == "__main__":
    sys.exit(main())
