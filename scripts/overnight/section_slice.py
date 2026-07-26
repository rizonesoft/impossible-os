#!/usr/bin/env python3
"""Shared TODO section slicer for the overnight control plane.

ONE implementation of "give me the body of `## N.` in this TODO file",
imported by section-manifest.py (SPLIT verdict) and section-pack.py
(evidence bundle). It lives here because those two had byte-identical
private copies and a fix landing in only one of them would produce an
oracle split-brain: the manifest and the pack would disagree about what
a section even contains.

THE BUG THIS FIXES (found 2026-07-17 in the bare-metal-hardening TODO's
terminal section; recurred 2026-07-25 twice in the kernel-resource-
accounting-quotas TODO as its tail was split):

Both copies terminated the slice only on the NEXT NUMBERED heading:

    for i, ln in enumerate(lines):
        m = SECTION_RE.match(ln)          # ^## (\\d+)\\.
        if m:
            if int(m.group(1)) == n and start is None:
                start = i
            elif start is not None:
                end = i; break
    return "\\n".join(lines[start:end])   # end is None for the LAST section

For the terminal `## N.` section of a file that next numbered heading
never arrives, `end` stays None, and `lines[start:None]` slices to EOF --
swallowing the file-level `## OS Comparison`, `## Unit Tests`,
`## Verification` and `## History` blocks into the section body.

Consequences measured on the live tree, all of them doctrine-gate
corruption rather than cosmetics:
  - bare-metal-hardening, terminal section: real body 4 open items;
    manifest reported 21 and emitted a false SPLIT-RECOMMENDED whose
    likely_files/symbols were scraped from the Verification block's prose.
  - kernel-resource-accounting-quotas, terminal section: reported 37 open
    items; after splitting it, the NEW terminal section reported 24 open
    plus 71 done (95 items, the entire remainder of the file) against a
    real body of 5 checklist lines.

Note the interaction with the split machinery: splitting a terminal
section does not escape the bug, it MOVES it to whichever section is now
last, so every split of a file's tail pays the false verdict again.

THE FIX: terminate on the next level-2 heading of ANY kind. Sections use
`### ` for their own sub-headings, so a bare `## ` boundary is safe and
needs no allowlist of known trailing heading names (an allowlist is what
scripts/lint.sh used, and it silently over-captured on the 4 TODO files
whose trailing heading was not one of the five it knew about).

Stdlib-only, no side effects at import: both callers add this directory
to sys.path and import it the same way they already import worktree_hash.
"""
from __future__ import annotations

import re

# Start of the section we want: `## 12. Title`.
SECTION_RE = re.compile(r"^## (\d+)\.\s*(.*)")

# End of ANY section: the next level-2 heading, numbered or not. `### Sub`
# does NOT match (the third character is `#`, not whitespace), so a
# section's own sub-headings stay inside its body.
SECTION_END_RE = re.compile(r"^##\s+\S")


def section_block(text: str, n: int) -> str:
    """Body of section `n`, from its `## n.` heading up to (not including)
    the next level-2 heading, or EOF when it is genuinely the last block in
    the file.

    Returns "" when the section is absent. The heading line itself is
    included, matching the behavior both callers already relied on."""
    lines = text.splitlines()
    start = None
    for i, ln in enumerate(lines):
        if start is None:
            m = SECTION_RE.match(ln)
            if m and int(m.group(1)) == n:
                start = i
            continue
        if SECTION_END_RE.match(ln):
            return "\n".join(lines[start:i])
    if start is None:
        return ""
    return "\n".join(lines[start:])


def _selftest() -> int:
    """`python3 section_slice.py --selftest` -- no pytest dependency, so the
    runner suite can call it directly."""
    doc = "\n".join([
        "# TODO-99 Example",
        "",
        "## 1. First",
        "- [ ] alpha",
        "",
        "## 2. Terminal section",
        "- [ ] beta",
        "- [x] gamma",
        "### 2.1 A sub-heading stays inside",
        "- [ ] delta",
        "",
        "## OS Comparison",
        "| a | b |",
        "",
        "## Unit Tests",
        "- [ ] not-part-of-section-2",
        "",
        "## Verification",
        "- [ ] also-not-part-of-section-2",
    ])
    failures = []

    def check(cond, msg):
        if not cond:
            failures.append(msg)

    b1 = section_block(doc, 1)
    check(b1.startswith("## 1. First"), "section 1 must start at its heading")
    check("alpha" in b1, "section 1 lost its own item")
    check("beta" not in b1, "section 1 leaked into section 2")

    b2 = section_block(doc, 2)
    check(b2.startswith("## 2. Terminal section"), "terminal heading missing")
    check("beta" in b2 and "gamma" in b2, "terminal section lost its items")
    check("delta" in b2, "a `### ` sub-heading must stay inside the body")
    check("OS Comparison" not in b2, "terminal section swallowed OS Comparison")
    check("Unit Tests" not in b2, "terminal section swallowed Unit Tests")
    check("Verification" not in b2, "terminal section swallowed Verification")
    check("not-part-of-section-2" not in b2,
          "terminal section swallowed trailing checklist items")
    check(b2.count("- [") == 3,
          f"terminal section item count wrong: {b2.count('- [')} != 3")

    check(section_block(doc, 99) == "", "absent section must return empty")

    # A file whose last numbered section really is the last block: slicing
    # to EOF is correct there and must not regress.
    tail = "## 1. Only\n- [ ] one\n- [ ] two"
    check(section_block(tail, 1).count("- [") == 2,
          "genuinely-final section must still reach EOF")

    # An unlisted trailing heading (the case lint.sh's allowlist missed).
    odd = "## 1. S\n- [ ] a\n## Bare Metal Testing Plan\n- [ ] b"
    check("- [ ] b" not in section_block(odd, 1),
          "unlisted trailing heading must still terminate the slice")

    for f in failures:
        print(f"FAIL: {f}")
    print(f"section_slice selftest: {'PASS' if not failures else 'FAIL'} "
          f"({len(failures)} failure(s))")
    return 1 if failures else 0


if __name__ == "__main__":
    import sys
    raise SystemExit(_selftest() if "--selftest" in sys.argv else 0)
