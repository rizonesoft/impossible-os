#!/usr/bin/env python3
"""Unwrap hard-wrapped prose in markdown, one paragraph per physical line.

`todo/` prose is authored ONE PARAGRAPH PER LINE and wrapped by the reader's
editor. Sections authored to a fill column read as inconsistent with the file
around them and turn every later edit into a reflow. `todo_wrap_reminder.py`
stops NEW hard-wrapping at authoring time; this repairs what already exists and
is wired into `validate-todo-file`.

DETERMINISTIC ON PURPOSE. Reflowing by hand (or by model) over a long TODO is
exactly the operation where a paragraph silently loses a clause. This joins
lines and changes nothing else, and `--check` verifies that the reflow is
content-preserving by comparing whitespace-normalised text before and after --
if that comparison fails the file is left untouched.

WHAT IS A HARD WRAP (and what is not). "A paragraph must be one line" was
measured and rejected: 236 of 255 files under todo/ contain multi-line prose
blocks, so that rule rewrites the whole tree. A fill column is identified by
several consecutive lines whose widths cluster in a narrow band AND whose
breaks land mid-sentence. Everything else -- bullets, tables, headings, fenced
and indented code, short two-line notes, and runs of complete sentences -- is
left exactly as found.

Usage:
  todo-reflow.py --check FILE...     report only, exit 1 if any file would change
  todo-reflow.py --diff FILE...      print a unified diff, change nothing
  todo-reflow.py --write FILE...     rewrite in place
  todo-reflow.py --selftest
Stdlib only.
"""
from __future__ import annotations

import difflib
import re
import sys

MIN_RUN = 3
BAND = 22
LO, HI = 78, 138

_BULLET = re.compile(r"^(\s*)([-*+]\s+|\d+[.)]\s+)")


def _classify(line: str, prev_blank: bool) -> str:
    """'blank' | 'struct' | 'prose' -- struct is anything we must not touch.

    `prev_blank` disambiguates the one genuinely ambiguous case: a 4+-space
    indented line. In markdown that is an indented CODE BLOCK only when a blank
    line precedes it; running on from a non-blank line it is the continuation
    of the preceding bullet or paragraph. TODO checklist items are written
    exactly that way -- a short `- [ ]` lead (the 250-char cap applies to it)
    followed by indented body lines -- and those bodies are 23 of the 53 wrapped
    lines in TODO-21 sections 19-20, so treating every indent as code would
    leave nearly half the problem unrepaired.
    """
    s = line.strip()
    if not s:
        return "blank"
    if s.startswith(("#", "|", ">", "```")):
        return "struct"
    if _BULLET.match(line):
        return "struct"
    if line.startswith("    ") and prev_blank:
        return "struct"                      # indented code block
    return "prose"


def _indent(line: str) -> str:
    return line[:len(line) - len(line.lstrip())]


def _hard_wrapped(block) -> bool:
    if len(block) < MIN_RUN:
        return False
    head = [len(x.rstrip()) for x in block[:-1]]
    if len(head) < MIN_RUN - 1:
        return False
    if not all(LO <= n <= HI for n in head):
        return False
    if max(head) - min(head) > BAND:
        return False
    mid = sum(1 for x in block[:-1]
              if not x.rstrip().endswith((".", ":", "!", "?", "|")))
    return mid >= max(2, int(0.6 * len(head)))


def reflow(text: str) -> str:
    lines = text.split("\n")
    out, buf, incode = [], [], False

    def flush():
        if not buf:
            return
        if _hard_wrapped(buf):
            # Join on single spaces, keeping the FIRST line's indent so an
            # indented continuation under a bullet stays indented.
            joined = _indent(buf[0]) + " ".join(x.strip() for x in buf)
            out.append(joined)
        else:
            out.extend(buf)
        buf.clear()

    prev_blank = True                        # start of file behaves like a blank
    in_icode = False                         # inside an INDENTED code block
    for line in lines:
        if line.strip().startswith("```"):
            flush()
            incode = not incode
            out.append(line)
            prev_blank, in_icode = False, False
            continue
        if incode:
            out.append(line)
            prev_blank = False
            continue
        blank = not line.strip()
        indented = line.startswith("    ")
        # An indented code block OPENS only after a blank line, and then runs
        # until the first non-blank line that is not indented. Tracking it as a
        # block (rather than per-line) matters: after its first line prev_blank
        # is False, so a per-line test would call every subsequent code line
        # prose and reflow the block. Caught by the selftest, which is why the
        # indented-code case is pinned there.
        if in_icode:
            if blank or indented:
                flush()
                out.append(line)
                prev_blank = blank
                continue
            in_icode = False
        elif indented and prev_blank and not blank:
            in_icode = True
            flush()
            out.append(line)
            prev_blank = False
            continue
        kind = _classify(line, prev_blank)
        prev_blank = (kind == "blank")
        if kind == "prose":
            # An indent CHANGE ends the paragraph: a differently-indented line
            # belongs to a different block (e.g. a new continuation level).
            if buf and _indent(line) != _indent(buf[0]):
                flush()
            buf.append(line)
        else:
            flush()
            out.append(line)
    flush()
    return "\n".join(out)


def _norm(s: str) -> str:
    """Whitespace-normalised text, for proving the reflow preserved content."""
    return re.sub(r"\s+", " ", s).strip()


def process(path: str, mode: str) -> int:
    """Return 1 if the file changed (or would), 0 otherwise, 2 on refusal."""
    try:
        with open(path, encoding="utf-8") as fh:
            original = fh.read()
    except OSError as exc:
        print(f"{path}: cannot read ({exc})", file=sys.stderr)
        return 2
    new = reflow(original)
    if new == original:
        return 0
    if _norm(new) != _norm(original):
        print(f"{path}: REFUSED -- reflow would change content, not just line "
              f"breaks. Left untouched.", file=sys.stderr)
        return 2
    if mode == "diff":
        sys.stdout.writelines(difflib.unified_diff(
            original.splitlines(True), new.splitlines(True),
            fromfile=path, tofile=path + " (reflowed)"))
    elif mode == "write":
        try:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(new)
        except OSError as exc:
            print(f"{path}: cannot write ({exc})", file=sys.stderr)
            return 2
        print(f"{path}: reflowed")
    else:
        print(f"{path}: hard-wrapped prose (would reflow)")
    return 1


def _selftest() -> int:
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    wrapped = (
        "`task_exec()` has no transactional commit point: it mutates the process image and THEN performs\n"
        "fallible work, so a late failure returns `-1` to a caller that iretqs back into an image that no\n"
        "longer exists. It also replaces the stack base with a fresh guarded kernel stack and never\n"
        "reclaims the old one.\n"
    )
    got = reflow(wrapped)
    check("joins to one line", len([l for l in got.split("\n") if l.strip()]) == 1)
    check("content preserved", _norm(got) == _norm(wrapped))

    # Things that must survive untouched.
    for name, src in (
        ("one-line para", "A single long paragraph already on one physical line, left alone entirely.\n"),
        ("two-line note", "A short note about the thing.\nAnd a second line continuing it.\n"),
        ("table", "| a | b |\n| --- | --- |\n| 1 | 2 |\n"),
        ("heading", "## 19. A Heading\n\nBody on one line.\n"),
        ("bullets", "- item one\n- item two\n- item three\n"),
        ("sentences", "The first sentence here is about eighty-five characters long and ends properly.\n"
                      "The second sentence here is about eighty-five characters long and ends fine.\n"
                      "The third sentence here is about eighty-five characters long and ends fine.\n"
                      "Short tail.\n"),
    ):
        check(f"untouched: {name}", reflow(src) == src)

    fenced = ("```\nsome code line that is long enough to look like a wrap at about ninety columns ok\n"
              "another code line that is long enough to look like a wrap at about ninety cols ok\n"
              "a third code line that is long enough to look like a wrap at ninety-odd columns k\n```\n")
    check("untouched: fenced code", reflow(fenced) == fenced)

    # An INDENTED CODE BLOCK (blank line before it) must survive untouched,
    # while an indented bullet continuation (no blank line) gets reflowed. This
    # pair is the whole reason _classify takes prev_blank.
    icode = ("Some intro paragraph on one line.\n"
             "\n"
             "    some indented code that is long enough to look like a wrapped line at ninety\n"
             "    another indented code line long enough to look like a wrapped line at ninety\n"
             "    a third indented code line long enough to look like a wrapped line ninety ok\n"
             "    end.\n")
    check("untouched: indented code block", reflow(icode) == icode)

    # An indented continuation under a bullet keeps its indent.
    ind = ("- [ ] **Item lead**\n"
           "  the body of this item is hard wrapped across several lines at about a hundred and ten\n"
           "  columns which is exactly the shape we are repairing here in this particular instance\n"
           "  and it continues once more to make the run long enough to be detected as a fill col\n"
           "  end.\n")
    out = reflow(ind)
    check("bullet lead untouched", out.split("\n")[0] == "- [ ] **Item lead**")
    check("continuation keeps indent", out.split("\n")[1].startswith("  "))
    check("continuation joined", len([l for l in out.split("\n") if l.strip()]) == 2)
    check("indent content preserved", _norm(out) == _norm(ind))

    # Idempotence: reflowing twice equals reflowing once.
    check("idempotent", reflow(reflow(wrapped)) == reflow(wrapped))

    if fails:
        sys.stderr.write("todo-reflow selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("todo-reflow selftest OK")
    return 0


def main(argv) -> int:
    if "--selftest" in argv:
        return _selftest()
    mode = "check"
    files = []
    for a in argv:
        if a == "--write":
            mode = "write"
        elif a == "--diff":
            mode = "diff"
        elif a == "--check":
            mode = "check"
        elif a.startswith("-"):
            print(f"unknown option {a}", file=sys.stderr)
            return 2
        else:
            files.append(a)
    if not files:
        print(__doc__.split("Usage:")[1].strip(), file=sys.stderr)
        return 2
    changed = refused = 0
    for f in files:
        rc = process(f, mode)
        changed += rc == 1
        refused += rc == 2
    if refused:
        return 2
    return 1 if (changed and mode == "check") else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
