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

# THE CONTAINER-FENCE FALLBACK, and why the shared mask alone is not yet enough
# for this tool. `cache_schema.fence_mask` applies CommonMark's 0-3-space bound
# to the PHYSICAL line, but CommonMark applies it after stripping the enclosing
# list-container prefix, so a fence indented five spaces under `100. docs` is
# not recognised at all -- a KNOWN LIMIT owned by section 39
# (`cache_schema.py:859`). The pre-adoption walk toggled on any stripped line
# starting with three backticks, so it happened to protect exactly those blocks;
# dropping it outright would expose their bodies to the prose heuristic, and
# `_norm()` cannot catch that because joining lines preserves content (Codex
# design review, section 41, [high]). 14 such delimiter lines sit in 5 corpus
# files today.
#
# It is a UNION with the mask, never a replacement: a line is verbatim when the
# MASK says so OR this fallback is open. A union can only make the tool MORE
# conservative than either rule alone, which is the only direction a repair tool
# that rewrites files may move. Retire it when section 39 makes the shared
# tracker container-aware.
#
# IT RUNS THE SHARED `fence_step`, on the line with its container indent
# removed -- it is NOT a second matcher. A first cut WAS a bare toggle on any
# 3+ run, and it closed a five-space-indented ```` block on the ``` line inside
# it: the body then reflowed, the real closer re-opened the fallback, and
# `_norm()` accepted the result because joining lines preserves content (Codex
# adversarial, section 41, [high], reproduced before fixing). Delegating gets
# the closer character/length rule, the blank-tail rule and the
# backtick-info-string rule from one definition.
#
# AN UNCLOSED LOOSE FENCE IS REFUSED BEHAVIOURALLY, not syntactically, and that
# distinction is the whole argument. Stripping the indent makes this opener a
# HEURISTIC SUPERSET: it cannot tell a container fence from an indented CODE
# BLOCK whose content happens to contain a ``` line, which is exactly what the
# 0-3-space bound separates -- so refusing on the syntax alone fails closed on
# legal documents. The first cut refused NOTHING for that reason, and that was
# worse: an unclosed opener masks every later line, so real hard-wrapped prose
# after it is hidden, `new == original`, and `--check` certifies as CLEAN a file
# it could not read (Codex adversarial, section 41 round 2, [medium]; verified
# by reproducing it before changing anything). A silent false negative in a gate
# is not a safer trade than a loud false positive.
#
# So `process()` refuses exactly when the ambiguity has a CONSEQUENCE: the loose
# state is still open at EOF **and** the fallback actually suppressed a repair
# the shared mask alone would have made. An inert false opener changes no
# outcome and is not reported; an ambiguous one is named, and BOTH readings of
# it want a human -- an unclosed container fence should be closed, and an
# indented code block should not be swallowing the prose after it. Measured
# 2026-08-11: 0 of 281 corpus files end inside an unclosed loose fence, so the
# extra pass costs nothing today.


def _loose_step(state, line: str):
    """`fence_step` with the container indent removed. See the block above."""
    return _fence.fence_step(state, line.lstrip(" \t"))


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


def verbatim_scan(lines, mask=None):
    """`(vmask, loose_open)` -- the mask plus the fallback's TERMINAL state.

    The terminal flag is returned for the same reason `cache_schema.fence_scan`
    returns its own: a state still open at EOF has masked everything after its
    opener, and a caller that cannot see that reports success over a document it
    could not read. `process()` is the caller that acts on it.
    """
    vmask, loose = _verbatim(lines, mask)
    return vmask, loose is not None


def verbatim_mask(lines, mask=None):
    """`True` per line where reflow must copy the line through untouched.

    The union described at `_loose_step`: the shared tracker's mask, OR an open
    loose-fence state for the container-indented delimiters the tracker cannot
    see yet. Returned as one list so the walk in `reflow` has a single question
    to ask per line instead of two interleaved state machines -- which is what
    the pre-adoption code got wrong, testing its fence flag BEFORE its
    indented-code-block flag, so a ``` inside an indented code block toggled it.

    `mask` is injectable for the SAME reason the four gate parsers take one
    (`scripts/tests/test_todo_fence.py`): passing `[False] * len(lines)` isolates
    the fallback from the shared tracker, so a fixture can prove which of the two
    is doing the work and cannot pass by asserting nothing.
    """
    return _verbatim(lines, mask)[0]


def _verbatim(lines, mask=None):
    """`(vmask, terminal loose state)`. See `verbatim_scan` / `verbatim_mask`."""
    if mask is None:
        mask, _uf, _uc = _fence.fence_scan(lines)
    loose = None
    out = []
    for i, line in enumerate(lines):
        # THE FALLBACK ONLY SEES WHAT THE MASK DID NOT CLAIM. Letting it also
        # step on masked lines would let a ``` written INSIDE a ````-fenced
        # block open it, and it would then stay open past the real closer and
        # silently suppress reflow for the rest of the file. Restricted this
        # way it can only ever describe the container-indented delimiters the
        # tracker does not model, which is the whole of its job.
        if mask[i]:
            out.append(True)
            continue
        nxt = _loose_step(loose, line)
        # Verbatim when we were inside before the line OR are inside after it,
        # which covers the content and BOTH delimiters in one expression --
        # the same formulation `cache_schema.fence_scan` uses for the mask.
        out.append(loose is not None or nxt is not None)
        loose = nxt
    return out, loose


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
    lines, shared_mask, unclosed_fence, unclosed_comment = \
        _fence.scan_text(original)
    reason = _fence.unclosed_reason(unclosed_fence, unclosed_comment)
    if reason:
        print(f"{path}: REFUSED -- {reason}. Left untouched.", file=sys.stderr)
        return 2
    vmask, loose_open = verbatim_scan(lines, shared_mask)
    new = reflow(original, vmask=vmask)
    # THE AMBIGUOUS CONTAINER FENCE, refused whenever it COSTS something -- see
    # the `_loose_step` block. Comparing the two reflows is what makes this
    # behavioural: if they agree, the fallback suppressed nothing and the file is
    # genuinely clean; if they differ, this run would otherwise report a clean
    # file while hiding a repair behind a delimiter it cannot prove is one.
    #
    # NOT CONDITIONED ON THE FENCE STAYING OPEN. It first was, and that left the
    # same hole one shape over: two indented code blocks each carrying a matching
    # literal delimiter CLOSE the loose state, so the terminal flag is clear
    # while everything between them was still masked (Codex adversarial, section
    # 41 round 7, [medium]). The terminal state now only chooses the WORDING,
    # because an author fixes an unclosed delimiter differently from a paired
    # one. Measured across all 281 corpus files: 5 rely on the fallback and 0 of
    # them change verdict, so the broader rule refuses nothing that exists today.
    #
    # GUARDED ON THE MASKS, not run unconditionally. Equal masks deterministically
    # produce equal output, so the second reflow was pure waste on the 276 of 281
    # corpus files whose fallback claims nothing -- measured at ~54ms, about 19%
    # of a command that runs in the pre-commit hook (Codex perf, section 41
    # post-commit, [medium]). Comparing the masks first preserves the proof
    # exactly: where they are equal there is nothing for the comparison to find.
    if vmask != shared_mask and reflow(original, vmask=shared_mask) != new:
        which = ("opens a fence that never closes" if loose_open
                 else "pairs with a later one to form a fence")
        print(f"{path}: REFUSED -- an indented ``` or ~~~ delimiter {which}, "
              f"and it is hiding hard-wrapped prose. Close it, or de-indent it "
              f"below four spaces if it is meant as code. Left untouched.",
              file=sys.stderr)
        return 2
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
