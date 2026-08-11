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
  todo-section-order.py --check-placement    exit 1 on sections after the tail

With no paths, walks todo/**/*.md.

EXIT 2 IS A REFUSAL, not a finding: the document ends inside an unclosed fence
or HTML comment, so the shared tracker masks everything past the opener and no
walk over it can be trusted. The three rewrite modes say so on stderr and touch
nothing; `--check-placement` reports it on stdout, where lint Check 22b counts
it as an error.
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

# The shared fence tracker (section 38, wrapping the section-36 primitive).
# BOTH heading scans in this file take their mask from it: `parse`, which feeds
# the pure-move rewrite, and `sections_after_closing`, which feeds the blocking
# lint Check 22b. Two independent fence-blind walks over one document is the
# exact drift the shared tracker exists to end -- and here it would have let a
# `## N.` written inside a fenced EXAMPLE split a real section block.
sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    import todo_fence as _fence
except ImportError as _exc:                                  # pragma: no cover
    sys.stderr.write(f"todo-section-order: cannot import the shared fence "
                     f"tracker: {_exc}\n")
    raise

# A section number is BOUNDED, and the bound is the point. `(\d+)` went straight
# into `int()`, and CPython refuses a string conversion over 4,300 digits -- so a
# heading carrying a 5,000-digit number raised ValueError, exited 1 with an EMPTY
# stdout, and lint Checks 22/22b (which discard stderr and erase the status with
# `|| true`) read the crash as CLEAN. Reproduced before fixing (Codex
# adversarial, section 41 post-commit, [medium]). Nine digits cannot be reached
# by any real roadmap and cannot overflow anything downstream; a longer run does
# not match at all, and `_OVERLONG_SECTION_RE` then NAMES it rather than letting
# it silently stop being a heading.
SECTION_RE = re.compile(r"^## (\d{1,9})\.")
_OVERLONG_SECTION_RE = re.compile(r"^## \d{10,}\.")
ANY_H2_RE = re.compile(r"^## ")

# File-level CLOSING MATTER. A numbered section is allowed to be relocated in
# front of these, because they belong at the end of the file by convention. Any
# OTHER non-numbered `## ` heading between numbered sections is a structure this
# tool does not understand, and it refuses rather than guessing -- surveyed
# 2026-07-30 across todo/**: 219 "OS Comparison", 209 "Verification", 90 "Unit
# Tests", 24 "History", and a handful of one-off headings that must NOT be
# treated as movable boundaries.
# Measured across todo/** (2026-08-02): the four canonical blocks plus the
# reference/appendix headings real files actually use after their sections.
# parse() REFUSES a file carrying an unrecognised `## ` heading between the
# first and last numbered section -- a deliberate "I cannot model this shape"
# guard -- so an unlisted appendix silently made `--fix` a no-op on files that
# genuinely needed repair (TODO-07 lsp-mcp, TODO-04 system-logging). Extend
# this set only with headings that are genuinely trailing matter; the pure-move
# proof in reorder() is the backstop, not this list.
CLOSING_MATTER = {"OS Comparison", "Unit Tests", "Verification", "History",
                  "Completed (Reference)", "Codex Adversarial Review",
                  "Format Quick Reference"}


def overlong_section(lines, mask):
    """1-based line number of an over-long `## N.` heading, or None.

    Reported rather than ignored: the bounded `SECTION_RE` makes such a heading
    stop being a section, and silently dropping a heading from a REORDER is how
    a block gets reparented. Masked lines are exempt -- a fenced example may
    legitimately contain anything.
    """
    for i, line in enumerate(lines):
        if not mask[i] and _OVERLONG_SECTION_RE.match(line):
            return i + 1
    return None


def scan(text: str):
    """`(lines, mask, unclosed_fence, unclosed_comment)` -- ONE scan per document.

    Threaded through `parse` and `sections_after_closing` rather than recomputed
    by each, so `reorder` (which calls both) and `_check_placement` pay for the
    walk once and, more importantly, cannot end up judging the same document by
    two different masks.
    """
    return _fence.scan_text(text)


def parse(text: str, sc=None):
    """(head, [(num, block)], tail) or None when the shape is unsafe to touch.

    REFUSES a document ending inside an unclosed fence or HTML comment. The mask
    hides everything from the opener to EOF, so the sections after it simply
    vanish -- and this function's answer drives a file REWRITE, where vanished
    sections would be silently dropped or reparented. `None` is this function's
    existing "I cannot model this shape" verdict, and every caller already
    treats it as do-nothing.
    """
    lines, mask, unclosed_fence, unclosed_comment = sc or scan(text)
    if unclosed_fence or unclosed_comment:
        return None
    starts = [i for i, l in enumerate(lines)
              if not mask[i] and SECTION_RE.match(l)]
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
        if mask[i]:
            continue
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


def is_ordered(text: str, sc=None) -> bool:
    p = parse(text, sc)
    if p is None:
        return True                      # nothing we can judge
    nums = [n for n, _ in p[1]]
    return nums == sorted(nums)


def sections_after_closing(text: str, sc=None):
    """`## N.` sections that sit AFTER the closing matter, in file order.

    A SEPARATE predicate from `is_ordered` on purpose. Sections appended past
    `## OS Comparison` / `## Unit Tests` / `## Verification` / `## History` are
    still NUMERICALLY ordered among themselves, so the sorted-numbers test is
    silent about them -- measured 2026-08-01 on TODO-04, where sections 53-59
    sat after all three closing blocks and `lint.sh` reported 0 errors. This is
    the exact shape CLAUDE.md cites from TODO-06 ("sections 12-13 appended
    AFTER its OS Comparison / Unit Tests / Verification / History blocks"), so
    the doctrine named it while the checker could not see it.

    Kept out of `--check` deliberately: that mode is wired to a lint ERROR that
    blocks commits, and promoting a pre-existing violation to blocking would
    wedge whatever run is mid-section in that file. Report first, repair at a
    boundary, promote after.
    """
    lines, mask, _uf, _uc = sc or scan(text)
    starts = [i for i, l in enumerate(lines)
              if not mask[i] and SECTION_RE.match(l)]
    if not starts:
        return []
    # Only a recognised heading that FOLLOWS the first numbered section is
    # closing matter. Anchoring this wrongly flagged 8 files where a heading
    # like `## Important Notes` sits in the FRONT matter: every section then
    # looks "after the closing matter" (measured 2026-08-02, immediately after
    # widening the vocabulary -- the scan caught the error on its next run).
    first_closing = None
    for i in range(starts[0] + 1, len(lines)):
        if mask[i]:
            continue
        l = lines[i]
        if ANY_H2_RE.match(l) and not SECTION_RE.match(l):
            if l[3:].strip() in CLOSING_MATTER:
                first_closing = i
                break
    if first_closing is None:
        return []
    out = []
    for i in range(first_closing, len(lines)):
        if mask[i]:
            continue
        m = SECTION_RE.match(lines[i])
        if m:
            out.append((int(m.group(1)), i + 1, lines[i].strip()))
    return out


def reorder(text: str, sc=None):
    """Reordered text, or None if unsafe / already ordered."""
    sc = sc or scan(text)
    p = parse(text, sc)
    if p is None:
        return None
    head, blocks, tail = p
    nums = [n for n, _ in blocks]
    # "Already ordered" must mean ORDER **and** PLACEMENT. Numeric ascent alone
    # short-circuited the rebuild for a file whose sections were appended past
    # the closing matter -- they ascend among themselves, so `--fix` reported
    # nothing to do and silently left the defect in place. Measured 2026-08-02
    # on three files (TODO-04 usermode x10, TODO-07 lsp-mcp x1, TODO-04
    # system-logging x1). The rebuild below always emits head + sorted sections
    # + tail, so it repairs placement for free once it is allowed to run.
    if nums == sorted(nums) and not sections_after_closing(text, sc):
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
    """Explicit paths, or the repository's own `todo/` corpus.

    SYMLINKS ARE NOT PART OF THE CORPUS. A symlinked `todo/` -- or a symlinked
    `*.md` inside it -- makes this walk read whatever the link points at, so a
    root replaced with a link to `$HOME` or `/` turns two blocking lint checks
    (22 and 22b run this tool) into an unbounded traversal of somewhere else
    entirely. Guarding only `lint.sh`'s own Check 19 left that escape open
    through this tool's glob (Codex adversarial, section 41 round 8, [medium]).
    Explicit paths are still honoured as given: naming a file is a deliberate
    act, and `main()` refuses what it cannot read.
    """
    paths = [a for a in argv if not a.startswith("-")]
    if paths:
        return [Path(p) for p in paths]
    root = Path("todo")
    if root.is_symlink() or not root.is_dir():
        return []
    # `os.walk(followlinks=False)` rather than a recursive glob. Filtering the
    # final candidate with `is_symlink()` catches `todo/x.md -> elsewhere` and
    # MISSES an ancestor: `todo/link -> /outside` made the walk enumerate
    # `todo/link/ext.md`, so Checks 22/22b could traverse an unbounded external
    # tree and `--fix` could rewrite a file outside the repository entirely
    # (Codex adversarial, section 41 post-commit, [high]; reproduced before
    # fixing). Pruning `dirs` in place is what stops the descent.
    out = []
    for base, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = [d for d in dirs if not Path(base, d).is_symlink()]
        for name in files:
            p = Path(base, name)
            if name.endswith(".md") and p.is_file() and not p.is_symlink():
                out.append(p)
    return sorted(out)


def _check_placement(paths) -> int:
    hits = 0
    for path in paths:
        try:
            text = Path(path).read_text(encoding="utf-8")
        except OSError as exc:
            hits += 1
            print(f"{path}: cannot check placement -- unreadable ({exc})")
            continue
        except UnicodeDecodeError as exc:
            # SAME FAIL-OPEN, one function over. A bare `except: continue` here
            # meant a file this gate could not read passed it, which is the
            # exact shape the comment below refuses for unclosed fences.
            hits += 1
            print(f"{path}: cannot check placement -- not valid UTF-8 ({exc})")
            continue
        sc = scan(text)
        # AN UNPARSEABLE DOCUMENT IS A FINDING, not a clean file. The mask hides
        # everything past an unclosed opener, so `sections_after_closing` would
        # return `[]` and this gate would go SILENT on exactly the malformed
        # document it should be loudest about -- fail-open, which is what
        # `todo_fence`'s module docstring warns a gate must never do with the
        # mask alone. Printed on stdout because lint Check 22b counts one error
        # per line it reads there.
        reason = _fence.unclosed_reason(*sc[2:])
        if reason is None:
            ln = overlong_section(sc[0], sc[1])
            if ln is not None:
                reason = (f"line {ln} carries a `## N.` heading whose number "
                          f"exceeds 9 digits, so it is not a section this tool "
                          f"can order")
        if reason:
            hits += 1
            print(f"{path}: cannot check placement -- {reason}")
            continue
        bad = sections_after_closing(text, sc)
        if bad:
            hits += 1
            names = ", ".join(f"section {n} (line {ln})" for n, ln, _ in bad[:6])
            more = "" if len(bad) <= 6 else f" +{len(bad) - 6} more"
            print(f"{path}: {len(bad)} section(s) after the closing matter: {names}{more}")
    return 1 if hits else 0


def main(argv) -> int:
    if "--check-placement" in argv:
        return _check_placement(_targets(argv))
    mode = ("fix" if "--fix" in argv else
            "diff" if "--diff" in argv else "check")
    rc = 0
    refused = False
    for path in _targets(argv):
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            # AN UNREADABLE TARGET IS A REFUSAL, not a skip. Continuing here
            # returned rc 0 from `--check`, `--diff` and `--fix` for a path that
            # was never scanned -- and the clean-corpus gate in
            # `scripts/test-tooling.sh` reads rc 0 as proof that EVERY target
            # was (Codex adversarial, section 41 round 7, [medium]).
            print(f"{path}: REFUSED -- unreadable ({exc}). Left untouched.",
                  file=sys.stderr)
            refused = True
            continue
        except UnicodeDecodeError as exc:
            # A DECODE FAILURE IS THE SAME REFUSAL as an unclosed fence, not a
            # traceback. Uncaught it exited 1, which is this tool's "sections
            # out of order" code, so a non-UTF-8 file reported findings it never
            # computed (Codex adversarial, section 41 round 6, [medium]).
            print(f"{path}: REFUSED -- not valid UTF-8 ({exc}). Left untouched.",
                  file=sys.stderr)
            refused = True
            continue
        sc = scan(text)
        # `reorder` returns None for BOTH "already ordered" and "malformed", and
        # the loop below treats None as success -- so without this a file nobody
        # can parse produced a silent rc 0 from `--check`, `--diff` and `--fix`
        # alike (Codex design review, section 41, [medium]). Same exit-2 contract
        # `todo-reflow.py` uses for the same condition.
        reason = _fence.unclosed_reason(*sc[2:])
        if reason is None:
            ln = overlong_section(sc[0], sc[1])
            if ln is not None:
                reason = (f"line {ln} carries a `## N.` heading whose number "
                          f"exceeds 9 digits, so it is not a section this tool "
                          f"can order")
        if reason:
            print(f"{path}: REFUSED -- {reason}. Left untouched.",
                  file=sys.stderr)
            refused = True
            continue
        new = reorder(text, sc)
        if new is None:
            continue
        nums = [n for n, _ in parse(text, sc)[1]]
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
            try:
                # Atomic, and race-checked against `text` as read above. A bare
                # `write_text` truncated first AND let its OSError escape as an
                # uncaught traceback (rc 1, this tool's "out of order" code).
                _fence.replace_atomically(path, new, text)
            except OSError as exc:
                print(f"{path}: REFUSED -- cannot write ({exc}).",
                      file=sys.stderr)
                refused = True
                continue
            print(f"{path}: reordered {nums} -> {sorted(nums)}")
    return 2 if refused else rc


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
