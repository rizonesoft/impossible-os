#!/usr/bin/env python3
"""Keep `## N.` section BODIES in numeric order within a TODO file.

WHY. Sections were being filed dependency-adjacent rather than appended: a
section discovered during review of an earlier one was inserted directly after
its parent and given the next free number, so TODO-04's bodies ran
`... 28, 32, 29, 30, 31, 35, 39, 33, 36, 37, 34, 38`. The Implementation Order
table stayed correct and every tool kept working (`section_slice.py` matches by
NUMBER, not position), so nothing broke -- but a reader scrolling for a section
walks straight past it and concludes it is missing. That happened, and cost real
operator time.

The convention this enforces: **a new section takes the next free number AND its
body goes last.** Numeric order and physical order are then the same thing, and
"where is section N" has exactly one answer.

PURE MOVE, or nothing. The rewrite is refused unless the multiset of section
blocks is byte-identical before and after, and the preamble and trailing
non-numbered blocks (`## OS Comparison`, `## Unit Tests`, ...) are untouched. It
also refuses outright when a non-numbered `##` heading sits BETWEEN numbered
sections, because then "the tail" is ambiguous and moving blocks could reparent
prose. Same discipline as `todo-reflow.py`: a repair tool that can only do one
narrow thing is one you can run without reading the diff.

  todo-section-order.py --check [paths...]   exit 1 if any file is out of order
  todo-section-order.py --fix   [paths...]   rewrite in place (pure move)
  todo-section-order.py --diff  [paths...]   show what --fix would do

With no paths, walks todo/**/*.md.
"""
from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

SECTION_RE = re.compile(r"^## (\d+)\.")
ANY_H2_RE = re.compile(r"^## ")

# File-level CLOSING MATTER. A numbered section is allowed to be relocated in
# front of these, because they belong at the end of the file by convention. Any
# OTHER non-numbered `## ` heading between numbered sections is a structure this
# tool does not understand, and it refuses rather than guessing -- surveyed
# 2026-07-30 across todo/**: 219 "OS Comparison", 209 "Verification", 90 "Unit
# Tests", 24 "History", and a handful of one-off headings that must NOT be
# treated as movable boundaries.
CLOSING_MATTER = {"OS Comparison", "Unit Tests", "Verification", "History"}


def parse(text: str):
    """(head, [(num, block)], tail) or None when the shape is unsafe to touch."""
    lines = text.split("\n")
    starts = [i for i, l in enumerate(lines) if SECTION_RE.match(l)]
    if len(starts) < 2:
        return None
    first, last = starts[0], starts[-1]
    # Every non-numbered `## ` between the first and last numbered section must
    # be recognised CLOSING MATTER; anything else is a structure this tool does
    # not model, so it refuses. Observed once for real: TODO-06 had sections 12
    # and 13 appended AFTER its OS Comparison / Unit Tests / Verification /
    # History blocks, i.e. past the end of the file's closing matter.
    closing = []
    for i in range(first, len(lines)):
        if ANY_H2_RE.match(lines[i]) and not SECTION_RE.match(lines[i]):
            title = lines[i][3:].strip()
            if i < last and title not in CLOSING_MATTER:
                return None
            closing.append(i)
    # Boundaries: every `## ` heading (numbered or closing) starts a new block.
    heads = sorted(starts + closing) + [len(lines)]
    blocks, closing_blocks = [], []
    for k, s in enumerate(heads[:-1]):
        blk = tuple(lines[s:heads[k + 1]])
        m = SECTION_RE.match(lines[s])
        if m:
            blocks.append((int(m.group(1)), blk))
        else:
            closing_blocks.append(blk)
    tail_at = closing[0] if closing else len(lines)
    # LINE LISTS, not joined strings. Concatenating joined strings drops the
    # single "\n" that sat between two slices, so the rebuilt file came out two
    # bytes short and the pure-move length check (correctly) refused it.
    tail = tuple(x for blk in closing_blocks for x in blk)
    return (tuple(lines[:first]), blocks, tail)


def is_ordered(text: str) -> bool:
    p = parse(text)
    if p is None:
        return True                      # nothing we can judge
    nums = [n for n, _ in p[1]]
    return nums == sorted(nums)


def reorder(text: str):
    """Reordered text, or None if unsafe / already ordered."""
    p = parse(text)
    if p is None:
        return None
    head, blocks, tail = p
    nums = [n for n, _ in blocks]
    if nums == sorted(nums):
        return None
    ordered = sorted(blocks, key=lambda nb: nb[0])
    out_lines = list(head)
    for _, blk in ordered:
        out_lines.extend(blk)
    out_lines.extend(tail)
    out = "\n".join(out_lines)
    # PURE-MOVE proof: same blocks, same head, same tail, same total bytes.
    check = parse(out)
    if check is None:
        return None
    if sorted(b for _, b in check[1]) != sorted(b for _, b in blocks):
        return None
    if check[0] != head or check[2] != tail:
        return None
    if len(out) != len(text):
        return None
    return out


def _targets(argv):
    paths = [a for a in argv if not a.startswith("-")]
    if paths:
        return [Path(p) for p in paths]
    return [Path(p) for p in sorted(glob.glob("todo/**/*.md", recursive=True))]


def main(argv) -> int:
    mode = ("fix" if "--fix" in argv else
            "diff" if "--diff" in argv else "check")
    rc = 0
    for path in _targets(argv):
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            continue
        new = reorder(text)
        if new is None:
            continue
        nums = [n for n, _ in parse(text)[1]]
        if mode == "check":
            print(f"{path}: sections out of order: {nums}")
            rc = 1
        elif mode == "diff":
            import difflib
            sys.stdout.writelines(difflib.unified_diff(
                text.splitlines(True), new.splitlines(True),
                str(path), str(path) + " (ordered)"))
            rc = 1
        else:
            path.write_text(new, encoding="utf-8")
            print(f"{path}: reordered {nums} -> {sorted(nums)}")
    return rc


def _selftest() -> int:
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    doc = ("# T\n\npre\n\n"
           "## 1. A\nbody a\n\n---\n\n"
           "## 3. C\nbody c\n\n---\n\n"
           "## 2. B\nbody b\n\n---\n\n"
           "## OS Comparison\ntail\n")
    out = reorder(doc)
    check("reorders", out is not None)
    check("numeric order", [n for n, _ in parse(out)[1]] == [1, 2, 3])
    check("pure move (same bytes)", len(out) == len(doc))
    check("tail preserved", out.endswith("## OS Comparison\ntail\n"))
    check("head preserved", out.startswith("# T\n\npre\n\n"))
    check("bodies intact", "body c" in out and "body b" in out)
    check("already-ordered is a no-op", reorder(
        "## 1. A\nx\n\n## 2. B\ny\n\n## Unit Tests\nz\n") is None)
    check("is_ordered agrees", not is_ordered(doc) and is_ordered(out))
    # Refuses when a non-numbered heading sits BETWEEN numbered sections.
    check("refuses interleaved h2", reorder(
        "## 1. A\nx\n\n## Notes\nn\n\n## 3. C\nz\n\n## 2. B\ny\n") is None)
    # Refuses a file with fewer than two sections.
    check("single section is a no-op", reorder("## 1. A\nx\n") is None)

    if fails:
        sys.stderr.write("todo-section-order selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("todo-section-order selftest OK")
    return 0


if __name__ == "__main__":
    sys.exit(_selftest() if "--selftest" in sys.argv else main(sys.argv[1:]))
