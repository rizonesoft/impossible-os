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
from pathlib import Path

# The shared fence tracker (section 38, wrapping the section-36 primitive). It
# decides which lines are VERBATIM regions -- fenced code, its delimiters, and
# HTML block comments -- so this tool and every gate parser agree about where
# markdown structure stops. `fence_mask` documents the delimiter line being
# masked too as being "a correct VERBATIM-REGION marker for the tools that
# rewrite files rather than merely read them", which is this one.
sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    import todo_fence as _fence
except ImportError as _exc:                                  # pragma: no cover
    sys.stderr.write(f"todo-reflow: cannot import the shared fence tracker: "
                     f"{_exc}\n")
    raise

MIN_RUN = 3
BAND = 22
LO, HI = 78, 138

_BULLET = re.compile(r"^(\s*)([-*+]\s+|\d+[.)]\s+)")

# THE CONTAINER-FENCE FALLBACK IS RETIRED (section 39). This tool used to carry
# a SECOND, looser fence walk beside the shared mask -- `fence_step` run on the
# line with its indent stripped -- because `cache_schema.fence_mask` applied
# CommonMark's 0-3-space bound to the PHYSICAL line and so could not see a fence
# indented five spaces under `100. docs`. The union was deliberately
# conservative and deliberately temporary: its own comment said to retire it
# when section 39 made the shared tracker container-aware, and that is what this
# change did.
#
# WHY IT COULD NOT SIMPLY BE LEFT IN PLACE. Stripping the indent made the opener
# a HEURISTIC SUPERSET -- it cannot tell a container fence from an indented CODE
# BLOCK whose content happens to contain a ``` line, which is exactly the
# distinction the 0-3-space bound draws. Left beside a tracker that now draws
# that line correctly, it would keep this tool answering a DIFFERENT fence
# question from every other consumer, and would keep suppressing real repairs
# behind root indented code (Codex design review, section 39, [medium]). The
# container-aware mask is strictly better on both counts: it claims the
# container-indented blocks the fallback was there to protect, and it claims
# nothing for a root 4-space block that merely contains a literal delimiter.
#
# THE AMBIGUITY REFUSAL WENT WITH IT, and that is the same decision rather than
# a second one: `process()` refused a file when the indent-stripped opener had
# actually suppressed a repair, because such an opener was a GUESS and a run
# would otherwise certify a file whose reading it could not defend. There is no
# longer a guess to report. An unclosed FENCE is still refused, through the
# shared `unclosed_reason()` contract every gate uses.


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


def verbatim_mask(lines, mask=None):
    """`True` per line where reflow must copy the line through untouched.

    THE SHARED MASK, AND NOTHING ELSE, since section 39 made it container-aware.
    This wrapper survives the fallback it used to union in because it is where
    the tool STATES that its verbatim regions are exactly the shared tracker's
    -- the property that keeps a repair tool from rewriting a region a gate
    parser reads as opaque. A future divergence would be a bug here, not a
    second walk to add back.

    `mask` stays injectable for the reason the gate parsers take one
    (`scripts/tests/test_todo_fence.py`): a fixture can pass a deliberately
    wrong mask -- `[False] * len(lines)` -- and prove the tracker is what
    protects a block, so the test cannot pass by asserting nothing.
    """
    if mask is None:
        # THE PROSE PROJECTION, not the structural one. This tool lints
        # HARD-WRAPPED PROSE, and the 30 live `<details>` lines under `todo/`
        # are exactly the prose it exists to check -- so CommonMark HTML block
        # types 6 and 7 stay VISIBLE here while every structural reader (and
        # `format-md-tables.py`, which rewrites what it sees) hides them. One
        # boolean mask could not serve both, which is what made this a section
        # rather than a flag (Codex design review, section 42, [high]).
        mask = _fence.fence_scan(lines).prose_mask()
    return mask


def reflow(text: str, vmask=None) -> str:
    lines = text.split("\n")
    mask = verbatim_mask(lines) if vmask is None else vmask
    out, buf = [], []

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
    for i, line in enumerate(lines):
        if mask[i]:
            # A verbatim region: fenced code and its delimiters, an HTML block
            # comment, or a container-indented fence. Ending the prose buffer
            # here is what stops a paragraph being joined ACROSS the region.
            flush()
            out.append(line)
            prev_blank, in_icode = False, False
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
    except UnicodeDecodeError as exc:
        # A DECODE FAILURE IS A REFUSAL, not a traceback. Uncaught it exited 1,
        # which is this tool's "found something to reflow" code -- so a
        # non-UTF-8 file produced rc 1 with EMPTY stdout, and lint Check 19
        # read that as zero findings and reported nothing (Codex adversarial,
        # section 41 round 4, [medium]; reproduced). Exit 2 is the documented
        # "left untouched" contract and the consumer already surfaces it.
        print(f"{path}: REFUSED -- not valid UTF-8 ({exc}). Left untouched.",
              file=sys.stderr)
        return 2
    # A MALFORMED DOCUMENT IS REFUSED WHOLE, not merely masked. The shared
    # tracker masks an unclosed fence or comment through EOF, which protects
    # everything AFTER the opener and nothing before it -- so hard-wrapped prose
    # earlier in the file would still be rewritten, `_norm()` would accept it
    # (joining preserves content), and the run would report a successful repair
    # of a file nobody can parse (Codex design review, section 41, [medium]).
    # Refusing reuses this function's existing exit-2 contract, and `main` turns
    # any refusal into a process-level 2, so `--check`, `--diff` and `--write`
    # all say the same thing.
    _scan = _fence.scan_text(original)
    shared_mask = _scan.prose_mask()
    reason = _scan.unclosed_reason()
    if reason:
        print(f"{path}: REFUSED -- {reason}. Left untouched.", file=sys.stderr)
        return 2
    # ONE MASK, ONE REFLOW (section 39). This used to compute a second, looser
    # mask and refuse the file when the two disagreed about a repair, because
    # the looser one was a guess about what an indented delimiter meant. The
    # shared tracker now answers that question correctly, so there is one mask,
    # no comparison, and no refusal to make -- and the ~54ms second reflow that
    # guarded it (about 19% of a command the pre-commit hook runs) is gone with
    # it.
    new = reflow(original, vmask=shared_mask)
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
            # Atomic, and race-checked against the bytes we read at the top of
            # this function -- see `todo_fence.replace_atomically`.
            _fence.replace_atomically(path, new, original)
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
