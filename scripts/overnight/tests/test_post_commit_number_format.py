#!/usr/bin/env python3
"""COUNT.md's thousands separator must group EVERY three digits, not just one.

WHY. The post-commit hook formats every headline number in COUNT.md through a
one-line `fmt()` sed. The original expression anchored on `\\>$` (end of line):

    fmt() { echo "$1" | sed ':a;s/\\B[0-9]\\{3\\}\\>$/,&/;ta'; }

After inserting the first comma the loop could no longer match, because the
comma it had just written is itself a word boundary and defeats the leading
`\\B`. So it emitted exactly ONE separator and was correct for every number up
to six digits -- which is every number the project had ever produced.

It broke the day the line count crossed a million, and reported `1006,165`
instead of `1,006,165` in the repo's most-read file. That is the whole hazard
of this bug class: the code is not wrong until the data grows past the point
the author happened to test, and then it is wrong in public.

This test pins the behaviour at magnitudes on both sides of that cliff so a
later "simplification" of the sed cannot quietly reintroduce it.
"""
from __future__ import annotations

import pathlib
import re
import subprocess
import sys

HOOK = pathlib.Path(__file__).resolve().parents[3] / ".githooks" / "post-commit"

# (input, expected). The 7- and 9-digit cases are the regression; the rest
# guard the boundaries the fix must not disturb.
CASES = [
    ("0", "0"),
    ("1", "1"),
    ("999", "999"),
    ("1000", "1,000"),
    ("12345", "12,345"),
    ("144054", "144,054"),          # last magnitude the old code got right
    ("999999", "999,999"),
    ("1000000", "1,000,000"),       # first magnitude it got wrong
    ("1006165", "1,006,165"),       # the value that actually shipped broken
    ("1146298", "1,146,298"),
    ("28000000", "28,000,000"),
    ("123456789", "123,456,789"),
]


def extract_fmt(text):
    """Pull the fmt() definition out of the hook so we test the REAL one.

    Re-implementing it here would test this file against itself, which is the
    same mistake that let a reversed-constant bug pass its own unit suite
    elsewhere in this tree.
    """
    m = re.search(r"^fmt\(\)\s*\{.*?\}\s*$", text, re.MULTILINE | re.DOTALL)
    return m.group(0) if m else None


def main():
    if not HOOK.exists():
        print("FAIL: %s not found" % HOOK)
        return 1

    fmt_def = extract_fmt(HOOK.read_text(encoding="utf-8"))
    if not fmt_def:
        print("FAIL: could not locate fmt() in %s -- was it renamed?" % HOOK)
        return 1

    failures = []
    for value, expected in CASES:
        proc = subprocess.run(
            ["bash", "-c", '%s\nfmt "%s"' % (fmt_def, value)],
            capture_output=True, text=True,
        )
        got = proc.stdout.strip()
        if got != expected:
            failures.append((value, expected, got))

    for value, expected, got in failures:
        print("FAIL: fmt(%s) -> %r, expected %r" % (value, got, expected))

    if failures:
        print("\n%d/%d cases failed. A separator that groups only the final "
              "three digits is correct until the number reaches seven digits, "
              "then wrong forever." % (len(failures), len(CASES)))
        return 1

    print("PASS: fmt() groups correctly across %d magnitudes" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
