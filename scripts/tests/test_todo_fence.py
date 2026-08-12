#!/usr/bin/env python3
"""Fence-awareness fixtures for the four TODO GATE parsers.

Each of these four can REFUSE something -- a commit, or `phase FIXPOINT` -- and
before section 38 each read raw Markdown with no fence state, so a literal
example inside a ``` block could produce a blocking verdict about text that is
a code sample.

HOW THE CONTROL WORKS, and why it is not a `sed` mutation. Every walk now takes
a `mask` argument, so passing `[False] * len(lines)` reproduces the fence-BLIND
walk exactly as it shipped before this section -- the control IS the old code
path, not an approximation of it. That also means it cannot rot: a `sed`-anchor
mutation guard silently stopped mutating anything when the function under it was
rewritten (recorded in overnight-runner-improvements-v13, from the cache-producer
section), and an argument the function actually reads has no equivalent failure
mode.

Every positive fixture below is paired with that control, so a fixture that
passes because it asserts nothing is caught: the control MUST produce the
finding the fence-aware walk suppresses.

    python3 scripts/tests/test_todo_fence.py
"""
from __future__ import annotations

import importlib.util
import itertools
import gc
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time
import tracemalloc

REPO = pathlib.Path(__file__).resolve().parents[2]

# The section glyph, built rather than typed: a bare glyph-plus-digit in a code
# file is refused by the repo's no-bare-section-refs rule, and these fixtures
# need real Implementation Order and OS Comparison rows to parse.
S = "§"

_FAILS = []


def check(name, cond):
    if not cond:
        _FAILS.append(name)


def load(name, relpath):
    src = REPO / relpath
    spec = importlib.util.spec_from_file_location(name, src)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# --------------------------------------------------------------------------
# A TODO whose fence contains one of EVERY structure the four gates parse: a
# numbered heading, an open item, an Implementation Order row, both stamps, a
# Deferred stamp and an OS Comparison heading. Nothing in the fence may reach
# any verdict.
# --------------------------------------------------------------------------
FIXTURE = "\n".join([
    "---",
    "id: 00-infrastructure/TODO-99-fixture",
    "---",
    "",
    "# TODO-99: Fence Fixture",
    "",
    "## Implementation Order",
    "",
    "| S | Order | Section | Deliverable | Depends On | Status |",
    "| - | :-: | :-: | - | - | :-: |",
    "| x |  1  | " + S + "1 | The real section | -- |  [x]   |",
    "",
    "## 1. The Real Section",
    "",
    "> **Spawned-by:** root",
    "",
    "Prose.",
    "",
    "- [x] the real, done item",
    "",
    "An example of what a closed section looks like:",
    "",
    "```markdown",
    "## Implementation Order",
    "| S | Order | Section | Deliverable | Depends On | Status |",
    "| - | :-: | :-: | - | - | :-: |",
    "| x |  2  | " + S + "99 | Fenced example deliverable | -- |  [x]   |",
    "## 99. A Fenced Example Section",
    "- [ ] a fenced example item",
    "> **Verified:** 2026-08-01 | commit `abc` | 1/1",
    "> **Quality reviewed:** 2026-08-01 | Codex",
    "> **Deferred:** [blocked] a fenced example stamp",
    "## OS Comparison",
    "```",
    "",
    "> **Notes:**",
    "> - shipped the real section",
    "> **Test runner:** none",
    "> **Verified:** 2026-08-01 | commit `abc` | 1/1",
    "> **Quality reviewed:** 2026-08-01 | Codex",
    "",
    "## OS Comparison",
    "",
    "| S | Feature | Win11 | Linux | Impossible OS |",
    "| - | - | - | - | - |",
    "| x | " + S + "1 real section | n/a | n/a | done |",
    "",
])

UNCLOSED_FENCE = "# T\n\n```\n## 1. Hidden\n- [ ] hidden work\n"
UNCLOSED_COMMENT = "# T\n\n<!--\n## 1. Hidden\n"


def _write(dirpath, text=FIXTURE, name="TODO-99-fixture.md"):
    p = pathlib.Path(dirpath) / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")
    return p


# --------------------------------------------------------------------------
# 1. The shim itself.
# --------------------------------------------------------------------------
def test_shim():
    tf = load("todo_fence", "scripts/todo_fence.py")
    _sc = tf.scan_text(FIXTURE)
    lines, mask = _sc.lines, _sc.mask
    uf = _sc.terminal is not None and _sc.terminal.kind == 'fence'
    uc = _sc.terminal is not None and _sc.terminal.kind == 'comment'
    fenced = [l for i, l in enumerate(lines) if mask[i]]
    check("shim: the fenced example heading is masked",
          any(l.startswith("## 99.") for l in fenced))
    check("shim: the real heading is NOT masked",
          not any(l.startswith("## 1.") for l in fenced))
    check("shim: a balanced document reports no unclosed fence", not uf)
    check("shim: a balanced document reports no unclosed comment", not uc)
    check("shim: unclosed_reason is None for a well-formed document",
          tf.unclosed_reason(None) is None and _sc.unclosed_reason() is None)
    # THE MESSAGE COMES FROM A REAL SCAN, not from synthesised flags. The old
    # shape of this test passed booleans straight to `unclosed_reason`, so it
    # asserted the WORDING without ever proving a document produces those
    # flags; a scan that stopped reporting an unclosed construct would have
    # kept it green. Section 42 replaced the pair with a tagged terminal, and
    # the terminal has to come from somewhere, so the fixtures now do.
    _fence_term = tf.scan_text("a\n```\nnever closed\n").terminal
    check("shim: an unclosed FENCE names the fence and its opening line",
          _fence_term.kind == "fence"
          and "fenced code block" in tf.unclosed_reason(_fence_term)
          and "line 2" in tf.unclosed_reason(_fence_term))
    _comment_term = tf.scan_text("a\n<!--\nnever closed\n").terminal
    check("shim: an unclosed COMMENT names the comment, not the fence",
          _comment_term.kind == "comment"
          and "fenced code block" not in tf.unclosed_reason(_comment_term))
    # A FENCE OPENED FIRST WINS, and it is now a property of the scan rather
    # than a precedence rule between two independent booleans: inside a fence a
    # `<!--` is literal text, so only one construct can ever be open.
    check("shim: a `<!--` inside a fence leaves the FENCE as the terminal",
          tf.scan_text("a\n```\n<!--\n").terminal.kind == "fence")
    # The four EOF-consuming types section 42 added each report themselves,
    # which is the whole reason the contract carries a kind.
    for _src, _kind, _num in (("<script>\nx\n", "script", 1),
                              ("<?php\nx\n", "pi", 3),
                              ("<!DOCTYPE\nx\n", "declaration", 4),
                              ("<![CDATA[\nx\n", "cdata", 5)):
        _t = tf.scan_text(_src).terminal
        check(f"shim: an unclosed type-{_num} block reports kind {_kind}",
              _t is not None and _t.kind == _kind and _t.number == _num)
    # ...and the two blank-line-terminated types are NOT unterminated at EOF.
    # CommonMark ends them there, so a terminal value would make the producer
    # refuse a legal document.
    check("shim: a type-6 block running to EOF is well-formed, not terminal",
          tf.scan_text("<details>\nx\n").terminal is None)
    check("shim: unmasked() yields ORIGINAL indices",
          [i for i, _ in tf.unmasked(lines, mask)]
          == [i for i, m in enumerate(mask) if not m])

    # mask_text must preserve BOTH the line count and every line's length, so a
    # caller's character offsets keep pointing at the same character.
    masked = tf.mask_text(FIXTURE)
    orig_lines, new_lines = FIXTURE.split("\n"), masked.split("\n")
    check("mask_text: line count preserved", len(orig_lines) == len(new_lines))
    check("mask_text: every line length preserved",
          all(len(a) == len(b) for a, b in zip(orig_lines, new_lines)))
    check("mask_text: total length preserved", len(masked) == len(FIXTURE))
    check("mask_text: the fenced heading is gone", "## 99." not in masked)
    check("mask_text: the real heading survives",
          "## 1. The Real Section" in masked)

    # An unclosed opener: fail-closed (masks to EOF) AND reported.
    _s2 = tf.scan_text(UNCLOSED_FENCE)
    m2 = _s2.mask
    uf2 = _s2.terminal is not None and _s2.terminal.kind == 'fence'
    check("shim: an unclosed fence is REPORTED, not just masked", uf2)
    check("shim: an unclosed fence masks through EOF", all(m2[2:]))
    _s3 = tf.scan_text(UNCLOSED_COMMENT)
    uf3 = _s3.terminal is not None and _s3.terminal.kind == 'fence'
    uc3 = _s3.terminal is not None and _s3.terminal.kind == 'comment'
    check("shim: an unclosed comment is reported", uc3 and not uf3)


# --------------------------------------------------------------------------
# 2. THE MASKING HAZARD: blanking lines must not let a pattern JOIN two real
#    ones. Reproduced 2026-08-11; the fix is line-local classes in lint.sh.
# --------------------------------------------------------------------------
def test_mask_text_cannot_join_lines():
    tf = load("todo_fence", "scripts/todo_fence.py")

    doc = "\n".join(["##", "```", "x", "```", "12. Title", ""])
    masked = tf.mask_text(doc)
    newline_form = re.compile(r"^##\s+12\.\s+(.+?)$", re.MULTILINE)
    line_local = re.compile(r"^##[ \t]+12\.[ \t]+(.+?)$", re.MULTILINE)
    # CONTROL: the hazard must actually exist, or this fixture proves nothing.
    check("hazard control: the newline-matching form DOES fabricate a heading",
          bool(newline_form.search(masked)))
    check("hazard control: that form matches NOTHING before masking",
          not newline_form.search(doc))
    check("line-local form does not fabricate a heading",
          not line_local.search(masked))
    check("line-local form still matches a REAL heading",
          bool(line_local.search("## 12. Real Title")))

    two = "| a | b\n| 7 | Deliv | dep | [x] |\n"
    io_newline = re.compile(
        r"^\|[^|]*\|[^|]*\|\s*" + S + r"?\s*(\d+)\s*\|([^|]+)\|[^|]*\|\s*\[x\]\s*\|",
        re.MULTILINE)
    io_local = re.compile(
        r"^\|[^|\n]*\|[^|\n]*\|[ \t]*" + S + r"?[ \t]*(\d+)[ \t]*\|([^|\n]+)\|"
        r"[^|\n]*\|[ \t]*\[x\][ \t]*\|", re.MULTILINE)
    check("hazard control: a newline-matching negated class DOES span a line",
          bool(io_newline.search(two)))
    check("the line-local negated class cannot span a newline",
          not io_local.search(two))
    check("the line-local negated class still matches a real 6-column row",
          bool(io_local.search("| x |  3  | 7 | Deliverable | dep | [x] |\n")))

    # And the SHIPPED lint.sh must be using the line-local forms.
    block = _lint_check_block((REPO / "scripts/lint.sh").read_text(encoding="utf-8"))
    check("lint.sh Check 10/11 block extracted for the pattern audit", bool(block))
    # CODE lines only -- the block's own comments discuss the hazard by name,
    # and matching those would make this assertion pass or fail on prose.
    code = "\n".join(l for l in block.split("\n")
                     if not l.lstrip().startswith("#"))
    check("lint.sh Check 10/11 uses no newline-spanning negated pipe class",
          block and "[^|]" not in code.replace("[^|\\n]", ""))
    check("lint.sh Check 10/11 body-end sentinel is line-local",
          block and 'BODY_END_RE = re.compile(r"^##[ \\t]+\\S"' in block)


# --------------------------------------------------------------------------
# 3. Gate 1: todo-reachability.py (holds `phase FIXPOINT`).
# --------------------------------------------------------------------------
def test_reachability():
    reach = load("todo_reachability", "scripts/todo-reachability.py")
    tf = load("todo_fence", "scripts/todo_fence.py")
    _s = tf.scan_text(FIXTURE)
    lines, mask = _s.lines, _s.mask
    blind = [False] * len(lines)

    # `_sections` takes the whole ScanResult since section 47 (it needs
    # `leaf_views` and `in_blockquote`, not just the mask). The fence-blind
    # CONTROL therefore shadows the mask on a wrapper rather than passing a
    # second array, so it still isolates fence-blindness alone.
    class _Blind:
        def __init__(self, s):
            self._s, self.mask = s, blind

        def __getattr__(self, k):
            return getattr(self._s, k)

    aware_secs = {h.n for h, _, _ in reach._sections(_s)}
    blind_secs = {h.n for h, _, _ in reach._sections(_Blind(_s))}
    check("reachability: fence-aware sees only the real section",
          aware_secs == {1})
    check("reachability CONTROL: the fence-blind walk invents section 99",
          99 in blind_secs)

    # The fenced example row carries order 2 and section marker 99; the real
    # row carries order 1 and marker 1, so a fence-aware walk sees only {1}.
    check("reachability: fence-aware sees only the real table row",
          reach._io_rows(_s) == {1})
    check("reachability CONTROL: the fence-blind walk invents the example row",
          reach._io_rows(_Blind(_s)) == {1, 99})

    # THE SECTION COLUMN DECIDES, and ONLY when the header declares one. The
    # old rule took the first all-digit cell of the first two columns -- the
    # ORDER column in the six-column layout -- while the caller compares the
    # result against SECTION numbers; they diverge the moment a section is
    # re-sequenced. Collecting BOTH is not a safe middle: membership is the
    # integrity check, so an order number would satisfy it for an unrelated
    # section. And scanning the WHOLE row for a marker is wrong too, because
    # `Depends On` carries markers.
    reseq = "\n".join([
        "## Implementation Order",
        "| S | Order | Section | Deliverable | Dep | Status |",
        "| - | :-: | :-: | - | - | :-: |",
        "| x |  39   | " + S + "41 | A re-sequenced section | " + S + "7 |  [x]   |",
        ""])
    _r = tf.scan_text(reseq)
    rl, rm = _r.lines, _r.mask
    got = reach._io_rows(_r)
    check("reachability: a Section column yields the marker alone",
          got == {41})
    check("reachability: the order number does NOT also satisfy membership",
          39 not in got)
    check("reachability: a Depends On marker is not read as the row's section",
          7 not in got)

    # No Section column: the digit fallback stands, and the dependency marker
    # must still not be mistaken for the row's own section. This is the layout
    # most of the corpus uses (TODO-01, TODO-04).
    legacy = "\n".join([
        "## Implementation Order",
        "| S | Order | Deliverable | Depends On | Status |",
        "| - | :-: | - | - | :-: |",
        "| x |  45   | Reader leases | " + S + "38 |  [x]   |",
        ""])
    _l = tf.scan_text(legacy)
    ll, lm = _l.lines, _l.mask
    check("reachability: no Section column -> the order number is used",
          reach._io_rows(_l) == {45})

    # A table with NO header row at all. Its single data row must still count:
    # treating "the first row after the heading" as a header positionally ate
    # it, which silently disarmed the cache-coverage refusal in test_build.sh's
    # routed-reader fixture -- a gate going quiet, found by that suite.
    headerless = "\n".join([
        "## Implementation Order",
        "",
        "| x | 1 | a thing | -- | [x] |",
        ""])
    _h = tf.scan_text(headerless)
    hl, hm = _h.lines, _h.mask
    check("reachability: a header-less table still yields its data row",
          reach._io_rows(_h) == {1})

    # A "Section" header that holds the section TITLE, not its number. This is
    # the older four-column layout and it is most of the corpus; keying on the
    # header word alone made 1,522 titles unparseable and refused nearly every
    # section in those files.
    titled = "\n".join([
        "## Implementation Order",
        "| #   | Section | Tag | Dep | Mark |",
        "| --- | ------- | --- | --- | ---- |",
        "| 1   | Win32 Type Definitions | `[Sonnet]` | -- | x |",
        ""])
    _t = tf.scan_text(titled)
    tl, tm = _t.lines, _t.mask
    check("reachability: a Section column holding a TITLE falls back to the number",
          reach._io_rows(_t) == {1})

    # A legend table BEFORE the real one, inside the same heading. The header
    # must not latch for the whole section: a non-table line ends the table.
    twotables = "\n".join([
        "## Implementation Order",
        "| Legend | Section | Meaning |",
        "| - | - | - |",
        "| x | shipped | done |",
        "",
        "| S | Order | Section | Deliverable | Dep | Status |",
        "| - | :-: | :-: | - | - | :-: |",
        "| x |  39   | " + S + "41 | A re-sequenced section | -- |  [x]   |",
        ""])
    _w = tf.scan_text(twotables)
    wl, wm = _w.lines, _w.mask
    check("reachability: a preceding legend table does not latch the column",
          41 in reach._io_rows(_w))

    # A fenced `- [ ]` and a fenced stamp must not reach the body scans either.
    # `_sections` yields the CLASSIFICATION since section 43, not a bare number:
    # an over-long heading has no usable number and still delimits its section,
    # and a caller has to be able to tell those apart.
    # `_sections` yields body INDICES since section 47, because the body's
    # matchers do not share a projection: items read `views`, the `^> \*\*`
    # stamps read physical lines. These assertions project the same way `audit`
    # does.
    def _body(scan, n):
        return next((b for h, _, b in reach._sections(scan) if h.n == n), None)

    # `audit` skips masked lines inside its streaming pass, so a caller
    # reading the BOUNDS has to skip them too -- the bounds are a range, not
    # a pre-filtered list.
    body = [_s.views[j] for j in range(*_body(_s, 1)) if not _s.mask[j]]
    check("reachability: no fenced open item in the real section's body",
          not any("a fenced example item" in b for b in body))
    check("reachability: no fenced Deferred stamp in the real section's body",
          not any("a fenced example stamp" in b for b in body))
    # Fence-blind, the fenced heading TERMINATES the real section and the
    # example item is attributed to the phantom section 99 instead -- which is
    # the `no-io-row` verdict this gate used to raise over a code sample.
    _b99 = _body(_Blind(_s), 99)
    blind_99 = (None if _b99 is None else
                [_s.views[j] for j in range(*_b99)])   # blind mask: nothing masked
    check("reachability CONTROL: fence-blind, the item lands under phantom 99",
          blind_99 is not None
          and any("a fenced example item" in b for b in blind_99))

    with tempfile.TemporaryDirectory() as d:
        p = _write(d)
        check("reachability: a clean fixture yields no finding",
              reach.audit(str(p), root=d) == [])
        p.write_text(UNCLOSED_FENCE, encoding="utf-8")
        got = reach.audit(str(p), root=d)
        check("reachability: an unclosed fence is REFUSED, not reported clean",
              len(got) == 1 and got[0][0] == "unclosed-fence")
        p.write_text(UNCLOSED_COMMENT, encoding="utf-8")
        got = reach.audit(str(p), root=d)
        check("reachability: an unclosed comment is refused separately",
              len(got) == 1 and got[0][0] == "unclosed-comment")


# --------------------------------------------------------------------------
# 4. Gate 2: todo-orphan-check.py (backs lint Check 23, ERRORs at commit).
# --------------------------------------------------------------------------
def test_orphan_check():
    orph = load("todo_orphan_check", "scripts/todo-orphan-check.py")
    tri = orph._triage(REPO)
    fence = orph._fence()

    with tempfile.TemporaryDirectory() as d:
        p = _write(d)
        check("orphan-check: the clean fixture reports no orphan",
              orph.scan_file(p, tri, fence) == [])

        # CONTROL: unfence the example and it becomes a REAL orphan -- an open
        # item under a section carrying both stamps and no Deferred stamp. If
        # this does not fire, the clean result above proves nothing.
        unfenced = "\n".join([
            "# T", "",
            "| S | Order | Section | Deliverable | Dep | Status |",
            "| - | :-: | :-: | - | - | :-: |",
            "| x |  1  | " + S + "1 | The real section | -- |  [x]   |",
            "",
            "## 1. The Real Section",
            "- [x] the real, done item",
            "- [ ] a fenced example item",
            "> **Verified:** 2026-08-01 | commit `abc` | 1/1",
            "> **Quality reviewed:** 2026-08-01 | Codex",
            ""])
        p.write_text(unfenced, encoding="utf-8")
        got = orph.scan_file(p, tri, fence)
        check("orphan-check CONTROL: unfenced, the same item IS an orphan",
              got and got[0][0] == 1
              and any("fenced example item" in i for i in got[0][2]))

        # ...and fencing that very item silences it again.
        refenced = unfenced.replace(
            "- [ ] a fenced example item",
            "```markdown\n- [ ] a fenced example item\n```")
        p.write_text(refenced, encoding="utf-8")
        check("orphan-check: fencing the same item silences the finding",
              orph.scan_file(p, tri, fence) == [])

        # A malformed document is named, not silently passed.
        p.write_text(UNCLOSED_FENCE, encoding="utf-8")
        check("orphan-check: an unclosed fence is reported as UNSCANNABLE",
              "unclosed fenced code block" in orph.malformed(p, fence))
        check("orphan-check: scan_file refuses to scan a malformed file",
              orph.scan_file(p, tri, fence) == [])
        p.write_text(FIXTURE, encoding="utf-8")
        check("orphan-check: a well-formed file reports no malformation",
              orph.malformed(p, fence) == "")


# --------------------------------------------------------------------------
# 5. Gate 3: todo-staged-check.py (refuses a staged commit).
#    Also pins the INDEX-vs-worktree snapshot fix: the walks must read the
#    staged post-image, because that is where the added-line numbers come from.
# --------------------------------------------------------------------------
def _git(cwd, *args):
    return subprocess.run(["git"] + list(args), cwd=cwd,
                          capture_output=True, text=True, check=False)


def _run_staged(cwd):
    env = dict(os.environ)
    env.pop("SKIP_TODO_STAGED_CHECK", None)
    return subprocess.run([sys.executable,
                           str(REPO / "scripts/todo-staged-check.py")],
                          cwd=cwd, capture_output=True, text=True, env=env)


def test_staged_check():
    stg = load("todo_staged_check", "scripts/todo-staged-check.py")
    tf = load("todo_fence", "scripts/todo_fence.py")
    _s = tf.scan_text(FIXTURE)
    lines, mask = _s.lines, _s.mask
    vis = ["" if mask[i] else l for i, l in enumerate(lines)]
    # The SECTION-HEADING view the gate builds since section 47: the leaf
    # projection with fenced AND blockquoted lines blanked. The fence-blind
    # controls below pass the UNBLANKED projection, so each one still isolates
    # exactly the blindness it is named for.
    sec_vis = ["" if (mask[i] or _s.in_blockquote(i)) else l
               for i, l in enumerate(_s.leaf_views)]

    check("staged-check: fenced headings do not count toward the section cap",
          stg._section_total(sec_vis) == 1)
    check("staged-check CONTROL: fence-blind, the fenced example counts too",
          stg._section_total(_s.leaf_views) == 2)

    added_all = [(i + 1, l) for i, l in enumerate(lines)]
    masked_nos = {i + 1 for i, m in enumerate(mask) if m}
    added_vis = [(n, t) for n, t in added_all if n not in masked_nos]
    check("staged-check: a fenced `## N.` is not an added section",
          [t for _, t in stg.added_sections(added_vis, sec_vis)]
          == ["## 1. The Real Section"])
    check("staged-check CONTROL: fence-blind, the fenced heading is one too",
          len(stg.added_sections(added_all, _s.leaf_views)) == 2)
    check("staged-check: no fenced park is reported",
          stg._park_into_shipping_section(vis, sec_vis, added_vis) == [])

    # THE SNAPSHOT. Stage one version, leave a DIFFERENT one in the worktree,
    # and confirm the gate judged the STAGED bytes. The worktree-only copy adds
    # an over-cap item; a worktree-reading gate would refuse the commit for
    # content that is not in it.
    with tempfile.TemporaryDirectory() as d:
        _git(d, "init", "-q", ".")
        _git(d, "config", "user.email", "t@t")
        _git(d, "config", "user.name", "t")
        todo = pathlib.Path(d) / "todo" / "00-infrastructure"
        todo.mkdir(parents=True)
        f = todo / "TODO-99-fixture.md"
        f.write_text("# T\n", encoding="utf-8")
        _git(d, "add", "-A")
        _git(d, "commit", "-qm", "base")

        f.write_text(FIXTURE, encoding="utf-8")
        _git(d, "add", "-A")                        # <- this is what is staged
        over_cap_line = "- [ ] " + "z" * 400
        f.write_text(FIXTURE + over_cap_line + "\n", encoding="utf-8")
        r = _run_staged(d)
        check("staged-check: an UNSTAGED over-cap item does not refuse",
              r.returncode == 0)
        check("staged-check: the unstaged item is not even mentioned",
              "z" * 40 not in (r.stdout + r.stderr))

        _git(d, "add", "-A")                        # now it IS staged
        r2 = _run_staged(d)
        check("staged-check CONTROL: staged, the over-cap item DOES refuse",
              r2.returncode == 1)

        # An unclosed fence in a staged file refuses rather than passing unread.
        f.write_text(UNCLOSED_FENCE, encoding="utf-8")
        _git(d, "add", "-A")
        r3 = _run_staged(d)
        check("staged-check: an unclosed fence refuses the commit",
              r3.returncode == 1 and "cannot be checked" in r3.stderr)


# --------------------------------------------------------------------------
# 6. Gate 4: lint.sh Checks 10/11 -- run the SHIPPED embedded block against a
#    fixture corpus. Extraction failing is itself a test failure, so this
#    cannot pass by quietly testing nothing.
# --------------------------------------------------------------------------
def _lint_check_block(lint_text):
    """The Check 10/11 heredoc from lint.sh, or "" if it cannot be located.

    Anchored on the `LINT_OUT=` assignment, which is unique -- lint.sh has five
    `python3 - <<'PYEOF'` blocks and taking "the only one" would silently return
    nothing. The caller asserts on a non-empty result, so a lint.sh refactor
    that moves this block FAILS the test rather than skipping it.
    """
    lines = lint_text.split("\n")
    blocks = []
    start = None
    for i, l in enumerate(lines):
        if start is None:
            if "<<'PYEOF'" in l:
                start = i
        elif l.strip() == "PYEOF":
            blocks.append("\n".join(lines[start + 1:i]))
            start = None
    # Identified by CONTENT, not by position. lint.sh has five PYEOF heredocs
    # and the `LINT_OUT=` assignment now wraps across continuation lines, so
    # both "the only one" and "the first one" are wrong -- the first silently
    # returned nothing the moment the assignment grew a backslash.
    owned = [b for b in blocks if "LINT_LAND_DATE" in b]
    return owned[0] if len(owned) == 1 else ""


def _run_lint_block(block, corpus_text):
    """Run the extracted Check 10/11 block over a synthetic `todo/` corpus.

    CWD supplies the corpus and argv[1] supplies the repo root, exactly as
    lint.sh invokes it -- which is what makes the fixture corpus synthetic while
    the tool under test stays the real one.
    """
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        _write(root / "todo" / "00-infrastructure", corpus_text)
        script = root / "_c1011.py"
        script.write_text(block, encoding="utf-8")
        return subprocess.run([sys.executable, str(script), str(REPO)], cwd=d,
                              capture_output=True, text=True)


def test_lint_checks_10_11():
    block = _lint_check_block((REPO / "scripts/lint.sh").read_text(encoding="utf-8"))
    check("lint 10/11: the embedded block was extracted", bool(block))
    if not block:
        return

    r = _run_lint_block(block, FIXTURE)
    check("lint 10/11: the clean fixture raises no error", r.returncode == 0)
    check("lint 10/11: the fenced example row raises no stamp-region error",
          "section-99" not in r.stdout)

    # CONTROL: the same row UNFENCED, over a section whose stamp region is
    # missing Notes and Test runner, MUST raise -- otherwise the fixture above
    # proves nothing.
    unfenced = "\n".join([
        "# T", "",
        "| S | Order | Section | Deliverable | Dep | Status |",
        "| - | :-: | :-: | - | - | :-: |",
        "| x |  2  | " + S + "99 | Fenced example deliverable | -- |  [x]   |",
        "",
        "## 99. A Fenced Example Section",
        "- [ ] a fenced example item",
        "> **Verified:** 2026-08-01 | commit `abc` | 1/1",
        ""])
    r2 = _run_lint_block(block, unfenced)
    check("lint 10/11 CONTROL: unfenced, the example section IS reported",
          "section-99" in r2.stdout)

    # An unclosed fence is named rather than read as a file with no IO rows.
    r3 = _run_lint_block(
        block, "# T\n\n```\n| x | 1 | " + S + "1 | d | -- | [x] |\n")
    check("lint 10/11: an unclosed fence is reported, not silently clean",
          "cannot be checked" in r3.stdout)


# --------------------------------------------------------------------------
# 7. Checks 10/11 under the COMMIT GATE judge the bytes the commit carries.
#    Fence-awareness gave the pre-existing worktree read two new steering
#    routes -- an unstaged fence can MASK a staged violation, and an unstaged
#    unclosed fence can REFUSE a commit that does not contain it. Neither is
#    reachable against a fence-blind check, so both are pinned here.
# --------------------------------------------------------------------------
_VIOLATION = "\n".join([
    "---", "schema_version: 1", "id: f", "domain: 00-infrastructure",
    "status: draft", 'title: "f"', "---", "",
    "# TODO-98 fixture", "",
    "## Implementation Order", "",
    "| Star | Order | Section | Deliverable | Depends | Status |",
    "| ---- | :---: | :---: | --- | --- | :----: |",
    "| star |   1   |  " + S + "1 | Sample row | -- |  [x]   |", "",
    "## 1. Sample section", "",
    "Body missing the Notes block on purpose.", "",
    "> **Test runner:** `scripts/foo.bat` | 0 failures", "",
    "> **Verified:** 2026-12-31 | commit `abc1234` | 1/1 items | build OK",
    "> **Quality reviewed:** 2026-12-31 | Codex 1x | 0 fixed | scope: N/A", "",
])


def _lint_repo(d):
    """A scratch repo holding lint.sh plus the dependency closure it loads."""
    root = pathlib.Path(d)
    (root / "scripts" / "todo-graph").mkdir(parents=True)
    (root / "todo" / "00-infrastructure").mkdir(parents=True)
    (root / "src").mkdir()
    for rel in ("scripts/lint.sh", "scripts/todo_fence.py",
                "scripts/todo-graph/cache_schema.py"):
        (root / rel).write_text((REPO / rel).read_text(encoding="utf-8"),
                                encoding="utf-8")
    (root / "src" / "stub.c").write_text("/* stub */\n", encoding="utf-8")
    _git(d, "init", "-q", ".")
    _git(d, "config", "user.email", "t@t")
    _git(d, "config", "user.name", "t")
    return root


def _run_lint(d, staged_scope=True):
    env = dict(os.environ)
    if staged_scope:
        env["LINT_GATE_SCOPE_STAGED"] = "1"
    else:
        env.pop("LINT_GATE_SCOPE_STAGED", None)
    return subprocess.run(["bash", "scripts/lint.sh"], cwd=d,
                          capture_output=True, text=True, env=env)


def test_lint_staged_scope():
    # (a) The violation is STAGED; the worktree then wraps it in a fence.
    #     The commit still contains the violation, so it must still be caught.
    with tempfile.TemporaryDirectory() as d:
        root = _lint_repo(d)
        f = root / "todo" / "00-infrastructure" / "TODO-98-f.md"
        f.write_text(_VIOLATION, encoding="utf-8")
        _git(d, "add", "-A")
        r_staged = _run_lint(d)
        check("staged scope CONTROL: the staged violation is caught to begin with",
              "section-1" in r_staged.stdout and "Check 10" in r_staged.stdout)
        # Now hide it in the WORKTREE only.
        f.write_text(_VIOLATION.replace("## 1. Sample section",
                                        "```markdown\n## 1. Sample section")
                     .replace("scope: N/A\n", "scope: N/A\n```\n"),
                     encoding="utf-8")
        r_hidden = _run_lint(d)
        check("an UNSTAGED fence cannot hide a STAGED Check 10 violation",
              "section-1" in r_hidden.stdout)

    # (b) An unstaged unclosed fence in a file the commit does NOT touch must
    #     not refuse the commit -- the wedge the gate scoping exists to stop.
    with tempfile.TemporaryDirectory() as d:
        root = _lint_repo(d)
        other = root / "todo" / "00-infrastructure" / "TODO-97-other.md"
        other.write_text("# T\n\n| star | 1 | " + S + "1 | d | -- | [x] |\n",
                         encoding="utf-8")
        _git(d, "add", "-A")
        _git(d, "commit", "-qm", "base")
        touched = root / "todo" / "00-infrastructure" / "TODO-96-mine.md"
        touched.write_text("# Mine\n", encoding="utf-8")
        _git(d, "add", "todo/00-infrastructure/TODO-96-mine.md")
        other.write_text("# T\n\n```\nnever closed\n", encoding="utf-8")
        r = _run_lint(d)
        check("an unstaged unclosed fence in an UNTOUCHED file does not refuse",
              "cannot be checked" not in r.stdout)
        # CONTROL: the same malformed file, when the commit DOES carry it, is
        # refused -- otherwise (b) would pass by never checking anything.
        _git(d, "add", "-A")
        r2 = _run_lint(d)
        check("staged scope CONTROL: staged, the unclosed fence IS refused",
              "cannot be checked" in r2.stdout)

    # (c) A staged-NEW violating file DELETED from the worktree without staging
    #     the deletion. The blob is still in the commit, so it must still be
    #     judged -- a filesystem walk would never visit it at all.
    with tempfile.TemporaryDirectory() as d:
        root = _lint_repo(d)
        (root / "todo" / "00-infrastructure" / "seed.md").write_text(
            "# seed\n", encoding="utf-8")
        _git(d, "add", "-A")
        _git(d, "commit", "-qm", "base")
        f = root / "todo" / "00-infrastructure" / "TODO-94-ghost.md"
        f.write_text(_VIOLATION, encoding="utf-8")
        _git(d, "add", "-A")
        f.unlink()                       # gone from disk, still in the index
        r = _run_lint(d)
        check("a staged file deleted from the worktree is still judged",
              "section-1" in r.stdout)

    # (d) A non-ASCII path whose worktree copy HIDES the staged violation in a
    #     fence. Git C-QUOTES such a path in `--name-only` output, so a scheme
    #     that decides "read the index for this one" by matching against that
    #     output silently misses it and reads the masking worktree copy
    #     instead. Enumerating the index NUL-separated has no such step.
    #     Written this way deliberately: without the unstaged masking edit the
    #     case passes under the old code too, and would prove nothing.
    with tempfile.TemporaryDirectory() as d:
        root = _lint_repo(d)
        f = root / "todo" / "00-infrastructure" / "TODO-93-café.md"
        f.write_text(_VIOLATION, encoding="utf-8")
        _git(d, "add", "-A")
        f.write_text(_VIOLATION.replace("## 1. Sample section",
                                        "```markdown\n## 1. Sample section")
                     .replace("scope: N/A\n", "scope: N/A\n```\n"),
                     encoding="utf-8")
        r = _run_lint(d)
        check("a non-ASCII path cannot hide a staged violation in the worktree",
              "section-1" in r.stdout)


# --------------------------------------------------------------------------
# 8. The Check 24 aggregator must COUNT the refusal record, not just parse it.
#    reachability's new unclosed-* kind passes the rc-vs-shape pair, so a
#    counter that only knows open-in-done / open-in-deferred drops it and the
#    gate reports nothing about a file the tool refused to read.
# --------------------------------------------------------------------------
def test_check24_counts_the_refusal():
    lint = (REPO / "scripts/lint.sh").read_text(encoding="utf-8")
    check("lint Check 24 aggregates the unclosed-fence kind",
          "'unclosed-fence','unclosed-comment'" in lint.replace(", ", ","))

    reach = load("todo_reachability", "scripts/todo-reachability.py")
    with tempfile.TemporaryDirectory() as d:
        p = _write(d, UNCLOSED_FENCE)
        kinds = {k for k, _, _ in reach.audit(str(p), root=d)}
    check("...and that is exactly the kind audit() emits", kinds == {"unclosed-fence"})


# --------------------------------------------------------------------------
# 9. The two MUTATING repair tools (section 41). A gate adopter is right when
#    its verdict is unchanged; these two are right when the FILE is unchanged,
#    so every fixture asserts byte-identity and the control asserts that the
#    fence-blind walk moves a byte.
# --------------------------------------------------------------------------
# Shapes the pre-adoption `strip().startswith("```")` toggle gets wrong. Each
# body is three clustered ~90-column lines breaking mid-sentence -- the exact
# hard-wrap signature `_hard_wrapped` fires on -- so a walk that fails to see
# the fence WILL join them.
_WRAPPED_BODY = [
    "a fenced example line that is long enough to look like a wrapped line at about ninety cols",
    "another fenced example line that is long enough to look like a wrapped line at ninety cols",
    "a third fenced example line that is long enough to look like a wrapped line at ninety cols",
    "end.",
]
#
# The blank lines INSIDE each fence are load-bearing, not decoration. Without
# them the blind walk treats the delimiter and the `end.` line as part of the
# same prose run, and `_hard_wrapped` rejects the run on the short lines --
# so the control passes, the fixture looks green, and it is asserting nothing.
# Caught by the control on first run; the widths above were 72-73 and below the
# tool's own 78-column floor for the same reason.
TILDE_FENCE = "# T\n\nIntro on one line.\n\n~~~text info string\n\n" + \
    "\n".join(_WRAPPED_BODY) + "\n\n~~~\n\ntail on one line.\n"
# A ```` block whose body contains a SHORTER ``` run: the closer must repeat the
# opener's character at no less than its length, or the inner run ends the block
# and the real closer re-opens a fence over the rest of the file.
NESTED_FENCE = "# T\n\nIntro on one line.\n\n````markdown\n```\n" + \
    "\n".join(_WRAPPED_BODY) + "\n```\n````\n\ntail on one line.\n"
# A fenced block carrying a recognised CLOSING-MATTER heading, followed by a
# real numbered section. Fence-blind, the fenced `## OS Comparison` becomes the
# closing matter and section 2 reads as filed after it.
PLACEMENT_FIXTURE = (
    "# T\n\n## 1. First\n\nAn example of a closed file:\n\n"
    "```markdown\n## OS Comparison\n| a | b |\n```\n\n"
    "## 2. Second\n\nbody\n\n## OS Comparison\n\ntail\n"
)


def _lint_check19_wrap_block(lint_text):
    """The Check 19 hard-wrap SHELL block from lint.sh, or "" if not locatable.

    Anchored on content (`LINT19_WRAP_ERR=` ... `rm -f "$LINT19_WRAP_ERR"`) for
    the reason `_lint_check_block` states: a lint.sh refactor that moves this
    block must FAIL the test rather than silently skip it, which is why the
    caller asserts the extraction is non-empty.
    """
    lines = lint_text.split("\n")
    start = rm = end = None
    for i, l in enumerate(lines):
        s = l.strip()
        if start is None:
            if s == "LINT19_WRAP=0":
                start = i
        elif rm is None:
            if s == 'rm -f "$LINT19_WRAP_ERR"':
                rm = i
        elif s == "fi":
            # The `fi` closing the empty-corpus guard, which the block needs to
            # be balanced shell. Anchoring on `rm -f` alone returned a fragment
            # that bash could not run at all -- and every fixture then failed
            # for that reason rather than for anything under test.
            end = i
            break
    return "\n".join(lines[start:end + 1]) if end is not None else ""


def _run_lint19_wrap(block, corpus_text, tool=True, corpus=True, raw=None,
                     symlink=False, env=None):
    """`(stderr+stdout, ERRORS)` from running that block over a synthetic corpus.

    `scripts` is SYMLINKED to the real tree so the tool under test is the real
    `todo-reflow.py` while the corpus is synthetic -- the same split
    `_run_lint_block` uses. `tool=False` omits the script (the scratch-repo
    shape that has no dependency closure), `corpus=False` omits every todo file,
    and `raw` writes bytes instead of text.
    """
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        # A SYMLINKED todo/ passes `[ -d ]` but is not traversed by a find that
        # lacks -H, so the scan silently enumerates nothing.
        todo_dir = (root / "_real_todo") if symlink else (root / "todo")
        if symlink:
            todo_dir.mkdir(parents=True, exist_ok=True)
            os.symlink(todo_dir, root / "todo")
        if corpus is True:
            p = todo_dir / "00-infrastructure" / "TODO-99-fixture.md"
            p.parent.mkdir(parents=True, exist_ok=True)
            if raw is not None:
                p.write_bytes(raw)
            else:
                p.write_text(corpus_text, encoding="utf-8")
        elif corpus == "empty":
            todo_dir.mkdir(parents=True, exist_ok=True)
        # corpus is False -> todo/ does not exist AT ALL, which is the shape
        # several tt_install_lint scratch repos have and is NOT the same test as
        # an empty directory: under `set -euo pipefail` the missing path aborted
        # the whole linter.
        if tool:
            os.symlink(REPO / "scripts", root / "scripts")
        else:
            (root / "scripts").mkdir(parents=True, exist_ok=True)
        script = root / "_c19.sh"
        # `set -euo pipefail` MATCHES lint.sh:17. Without it this harness was a
        # false equivalence: the production abort it is meant to detect cannot
        # happen in a shell that does not set -e, so the fixture passed while
        # the real gate died.
        script.write_text(
            'set -euo pipefail\nRED=""\nNC=""\nERRORS=0\nREPO_ROOT="$1"\n' +
            block + '\necho "ERRORS=$ERRORS"\n', encoding="utf-8")
        e = dict(os.environ)
        e.update(env or {})
        r = subprocess.run(["bash", str(script), str(root)], cwd=d,
                           capture_output=True, text=True, env=e)
        out = r.stdout + r.stderr
        n = 0
        for line in r.stdout.split("\n"):
            if line.startswith("ERRORS="):
                n = int(line.split("=", 1)[1])
        return out, n


def _pre_adoption_mask(lines):
    """The fence-BLIND walk `todo-reflow.py` shipped before section 41.

    A faithful transcription of the deleted state machine -- `incode` toggled by
    `line.strip().startswith("```")`, no tilde fences, no closer char/length
    match, no CommonMark indent bound -- kept in the TEST rather than the tool
    because the tool must not carry a second definition of "am I inside a fence".
    It is the control every fixture below is paired with: if the fixture passes
    under this too, it is asserting nothing.
    """
    out, incode = [], False
    for line in lines:
        if line.strip().startswith("```"):
            out.append(True)
            incode = not incode
            continue
        out.append(incode)
    return out


def test_reflow():
    rf = load("todo_reflow", "scripts/todo-reflow.py")
    for name, src in (("tilde fence", TILDE_FENCE),
                      ("nested longer fence", NESTED_FENCE)):
        lines = src.split("\n")
        check(f"reflow leaves a {name} byte-identical", rf.reflow(src) == src)
        # CONTROL: the pre-adoption walk MUST move a byte on this shape, or the
        # fixture is passing for a reason that has nothing to do with the fence.
        check(f"...and the fence-blind walk rewrites the {name}",
              rf.reflow(src, vmask=_pre_adoption_mask(lines)) != src)

    # An unclosed fence is REFUSED whole, not partially reflowed: the prose
    # BEFORE the opener is inside the file the mask cannot vouch for.
    with tempfile.TemporaryDirectory() as d:
        p = _write(d, "\n".join(_WRAPPED_BODY) + "\n\n```\nunclosed\n")
        before = p.read_text(encoding="utf-8")
        rc = rf.process(str(p), "write")
        # READ BACK INSIDE the tempdir's lifetime -- outside it the path is gone
        # and the comparison raises (or, guarded, passes vacuously).
        after = p.read_text(encoding="utf-8")
    check("reflow refuses an unclosed fence (rc 2)", rc == 2)
    check("...and writes nothing", after == before)

    # THE CONTAINER-INDENTED FENCE, now claimed by the SHARED tracker rather
    # than by this tool's retired fallback (section 39). NO BLANK LINE before
    # the delimiter -- with one, the indented-code-block path in `_classify`
    # would protect the body anyway and the fixture would prove nothing about
    # the tracker. Measured both shapes; only this one moves.
    deep = "- 1. item\n     ```\n     " + "\n     ".join(_WRAPPED_BODY) + \
           "\n     ```\n"
    # THE HOSTILE SHAPE: a container-indented ```` block carrying a SHORTER ```
    # run. A bare toggle closes on the inner run, reflows the body, and re-opens
    # on the real closer -- the first cut of the retired fallback did exactly
    # that, which is why the container phase delegates to `fence_step` for the
    # closer-length rule instead of re-deriving it.
    hostile = "100. docs\n     ````markdown\n     ```\n     " + \
        "\n     ".join(_WRAPPED_BODY) + "\n     ```\n     ````\n"
    for name, src in (("container-indented fence", deep),
                      ("container fence with a shorter inner run", hostile)):
        lines = src.split("\n")
        mask_only = rf._fence.fence_scan(lines).mask
        check(f"reflow leaves a {name} byte-identical", rf.reflow(src) == src)
        # The mask claims the delimiters AND the body -- every line but the
        # unfenced marker line and the trailing empty one.
        check(f"the shared mask alone now claims the whole {name}",
              all(mask_only[1:-1]) and not mask_only[0])
        # CONTROL: a deliberately WRONG mask must join the body, or the two
        # assertions above would pass on a tool that never reflows anything.
        check(f"CONTROL: with an all-False mask the {name} body IS joined",
              rf.reflow(src, vmask=[False] * len(lines)) != src)
        # NO LONGER REFUSED. The refusal existed because an indent-stripped
        # opener was a GUESS; the tracker now decides it the way a renderer
        # does, so the file is simply clean and is written through untouched.
        with tempfile.TemporaryDirectory() as d:
            p = _write(d, src)
            rc = rf.process(str(p), "write")
            after = p.read_text(encoding="utf-8")
        check(f"--write accepts the {name} rather than refusing it", rc == 0)
        check(f"...and leaves the {name} on disk untouched", after == src)

    # THE FALSE OPENER IS GONE WITH THE FALLBACK. A ROOT 4-space block carrying
    # a literal ``` used to open the indent-stripped walk and mask the real
    # hard-wrapped prose after it, so the tool refused the file. CommonMark says
    # that block is an indented CODE BLOCK and its delimiter is literal text, so
    # the tracker claims nothing, the prose after it is visible, and the repair
    # is reported instead of being hidden behind a refusal.
    prose = [
        "a prose line that is long enough to look like a wrapped line at about ninety columns",
        "another prose line that is long enough to look like a wrapped line at ninety columns",
        "a third prose line that is long enough to look like a wrapped line at ninety columns",
        "end.",
    ]
    for name, indent in (("space-indented", "    "), ("tab-indented", "\t")):
        src = ("# T\n\nintro on one line.\n\n" + indent + "```\n" + indent +
               "a literal fence inside an indented code block\n\n" +
               "\n".join(prose) + "\n")
        lines = src.split("\n")
        mask_only = rf._fence.fence_scan(lines).mask
        check(f"the {name} root code block opens NO fence in the tracker",
              not any(mask_only))
        check(f"...so the {name} file's hard-wrapped prose is still repairable",
              rf.reflow(src) != src)
        with tempfile.TemporaryDirectory() as d:
            p = _write(d, src)
            rc = rf.process(str(p), "check")
            after = p.read_text(encoding="utf-8")
        check(f"...and --check REPORTS the {name} repair (rc 1), not a refusal",
              rc == 1)
        check("...and writes nothing", after == src)

    # A root indented code block that merely CONTAINS a delimiter is inert: it
    # was inert before (the fallback opened but suppressed nothing) and it is
    # inert now for a better reason -- nothing opened at all.
    inert = "# T\n\nintro on one line.\n\n    ```\n    a literal fence\n"
    with tempfile.TemporaryDirectory() as d:
        p = _write(d, inert)
        rc = rf.process(str(p), "check")
    check("a root indented code block carrying a delimiter is NOT refused",
          rc == 0)

    # PERF GUARD: with the fallback and its ambiguity comparison retired there
    # is ONE mask and ONE reflow per document, unconditionally -- the second
    # reflow that guarded the comparison is gone. Counted rather than timed: a
    # wall-clock assertion on a loaded host is a flake, and the property is
    # structural.
    calls = []
    real = rf.reflow
    try:
        rf.reflow = lambda *a, **k: (calls.append(1), real(*a, **k))[1]
        with tempfile.TemporaryDirectory() as d:
            p = _write(d, "# T\n\nordinary one-line prose.\n")
            rf.process(str(p), "check")
    finally:
        rf.reflow = real
    check("a document is reflowed exactly once", len(calls) == 1)

    # THE PRODUCTION CONSUMER, not just process(). A refusal the lint discards
    # is a refusal nobody sees -- Check 19 sent stderr to /dev/null and turned
    # every nonzero exit into success, so the tool returned 2 and the gate
    # reported nothing (Codex adversarial, section 41 round 3).
    # THE REFUSING FIXTURE IS NOW AN UNCLOSED FENCE. It used to be a root
    # indented code block carrying a literal delimiter, which the retired
    # fallback treated as an ambiguous opener; that shape is legal CommonMark
    # and is no longer refused by anything. An unclosed fence still is, through
    # the `unclosed_reason()` contract every gate shares, so it is what proves
    # the lint surfaces a refusal.
    hidden = ("# T\n\nintro on one line.\n\n" + "\n".join(prose) +
              "\n\n```\nunclosed\n")
    block = _lint_check19_wrap_block(
        (REPO / "scripts/lint.sh").read_text(encoding="utf-8"))
    check("lint 19: the wrap block was extracted", bool(block))
    err, errors = _run_lint19_wrap(block, hidden)
    check("lint Check 19 reports the refusal as an error",
          "Check 19" in err and "REFUSED" in err)
    check("...and counts it", errors == 1)
    # CONTROL: an ordinary hard-wrapped file must NOT become an error, or the
    # fixture is only proving that any input trips the new branch.
    clean_wrap = "# T\n\n" + "\n".join(prose) + "\n"
    err2, errors2 = _run_lint19_wrap(block, clean_wrap)
    check("CONTROL: an ordinary hard-wrapped file is not an error",
          errors2 == 0 and "REFUSED" not in err2)

    # The three shapes that made rc alone the wrong test. A CRASH must not wear
    # the findings code, a repo with no todo files must not be invoked at all,
    # and a scratch repo missing the tool must say so rather than report a
    # style finding it never computed.
    _, errors3 = _run_lint19_wrap(block, "", raw=b"x\xff\xfey\n")
    check("a non-UTF-8 todo file is an error, not a silent rc 1", errors3 == 1)
    err4, errors4 = _run_lint19_wrap(block, "", corpus="empty")
    check("an empty todo corpus is skipped, not invoked with no FILE args",
          errors4 == 0 and "REFUSED" not in err4)
    # The sentinel is the assertion here: under `set -euo pipefail` an aborted
    # block prints NOTHING, and "no error was reported" reads identical to
    # "the block ran and found nothing". Reaching `ERRORS=` proves it ran.
    err6, errors6 = _run_lint19_wrap(block, "", corpus=False)
    check("an ABSENT todo/ directory does not abort the linter",
          "ERRORS=" in err6 and errors6 == 0)
    err5, errors5 = _run_lint19_wrap(block, clean_wrap, tool=False)
    check("a missing todo-reflow.py is reported, not counted as zero findings",
          errors5 >= 1)
    check("...and the message names the check", "Check 19" in err5)

    # A SYMLINKED todo/ passes `[ -d ]` but a find without -H does not traverse
    # it, so the scan would enumerate nothing and the sentinel alone would still
    # say ERRORS=0. The assertion is therefore that the scan RAN: the hostile
    # corpus must still be refused through the symlink.
    err7, errors7 = _run_lint19_wrap(block, hidden, symlink=True)
    check("a symlinked todo/ is REFUSED, not followed and not silently skipped",
          errors7 == 1 and "symlink" in err7)
    # An unusable TMPDIR must be a named error, not an abort that takes the rest
    # of the linter with it -- the sentinel is what distinguishes the two.
    err8, errors8 = _run_lint19_wrap(block, clean_wrap,
                                     env={"TMPDIR": "/nonexistent-lint19-xyz"})
    check("an unusable TMPDIR is reported, not an abort",
          "ERRORS=" in err8 and errors8 >= 1 and "mktemp failed" in err8)


def test_section_order():
    so = load("todo_section_order", "scripts/todo-section-order.py")

    # THE FENCE-BLIND CONTROL, rebuilt on the section-42 `ScanResult`. It is a
    # real scan with the mask deliberately cleared, so the test still cannot
    # pass by asserting nothing: the control MUST see the fenced heading that
    # the fence-aware call suppresses.
    def blind(t):
        r = so._fence.fence_scan(t.split("\n"))
        n = len(r.lines)
        r.mask = [False] * n
        r.codes = bytearray(n)      # every line ordinary; `kinds` derives from this
        r.terminal = None
        return r

    bad = so.sections_after_closing(PLACEMENT_FIXTURE, blind(PLACEMENT_FIXTURE))
    check("CONTROL: fence-blind placement sees the fenced closing heading",
          [n for n, _, _ in bad] == [2])
    check("fence-aware placement is clean",
          so.sections_after_closing(PLACEMENT_FIXTURE) == [])

    # The same fenced heading must not split a block during a REPAIR. Sections
    # are out of order, so `reorder` genuinely runs rather than short-circuiting.
    doc = ("# T\n\n## 1. A\n\nbody a\n\n```markdown\n## 9. Fenced example\n"
           "fenced body\n```\n\n## 3. C\n\nbody c\n\n## 2. B\n\nbody b\n\n"
           "## OS Comparison\n\ntail\n")
    out = so.reorder(doc)
    check("reorder still repairs a real out-of-order file", out is not None)
    check("...as a pure move (same bytes)", len(out) == len(doc))
    check("...keeping the fenced example inside section 1",
          "## 1. A\n\nbody a\n\n```markdown\n## 9. Fenced example\n"
          "fenced body\n```\n" in out)
    check("...and numbering the real sections only",
          [n for n, _ in so.parse(out)[1]] == [1, 2, 3])
    check("CONTROL: the fence-blind parse sees the fenced heading as a section",
          [n for n, _ in so.parse(doc, blind(doc))[1]] == [1, 9, 3, 2])

    # CONTAINMENT: neither a symlinked corpus root nor a symlinked *.md inside
    # it is part of the corpus, or Checks 22/22b traverse wherever it points.
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "real").mkdir()
        _write(root / "todo", "# T\n", name="a.md")
        outside = root / "outside.md"
        outside.write_text("# outside\n", encoding="utf-8")
        os.symlink(outside, root / "todo" / "linked.md")
        cwd = os.getcwd()
        try:
            os.chdir(root)
            names = {p.name for p in so._targets([])}
        finally:
            os.chdir(cwd)
        check("a symlinked *.md inside todo/ is not a target",
              names == {"a.md"})
        os.rename(root / "todo", root / "real_todo")
        os.symlink(root / "real_todo", root / "todo")
        try:
            os.chdir(root)
            linked_root = so._targets([])
        finally:
            os.chdir(cwd)
        check("a symlinked todo/ root yields no targets at all",
              linked_root == [])

    # A symlinked ANCESTOR is the shape a final-component filter misses: the
    # walk enumerated todo/link/ext.md and --fix could rewrite a file outside
    # the repository entirely.
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "todo").mkdir()
        (root / "todo" / "real.md").write_text("# T\n", encoding="utf-8")
        (root / "outside").mkdir()
        (root / "outside" / "ext.md").write_text("# X\n", encoding="utf-8")
        os.symlink(root / "outside", root / "todo" / "link")
        cwd = os.getcwd()
        try:
            os.chdir(root)
            names = {p.name for p in so._targets([])}
        finally:
            os.chdir(cwd)
        check("a symlinked SUBDIRECTORY is not descended into",
              names == {"real.md"})

    # An over-long section number crashed `int()` and exited 1 with empty
    # stdout, which lint Check 22 read as clean.
    huge = "## " + "9" * 5000 + ". Huge\nx\n\n## 2. B\ny\n\n## OS Comparison\nt\n"
    with tempfile.TemporaryDirectory() as d:
        p = _write(d, huge)
        rc = so.main(["--check", str(p)])
        placement = so._check_placement([p])
    check("an over-long section number REFUSES (rc 2), not ValueError", rc == 2)
    check("...and --check-placement reports it as a finding", placement == 1)

    # THE WRITE IS ATOMIC AND RACE-CHECKED. A rewrite computed from bytes that
    # have since changed on disk must refuse rather than overwrite the newer
    # content with a repair of the older.
    doc = ("# T\n\n## 1. A\nbody a\n\n## 3. C\nbody c\n\n## 2. B\nbody b\n\n"
           "## OS Comparison\ntail\n")
    with tempfile.TemporaryDirectory() as d:
        p = _write(d, doc)
        new = so.reorder(doc)
        p.write_text(doc + "\nconcurrent edit\n", encoding="utf-8")
        raised = False
        try:
            so._fence.replace_atomically(p, new, doc)
        except OSError:
            raised = True
        after = p.read_text(encoding="utf-8")
    check("a concurrent edit makes the atomic write REFUSE", raised)
    check("...and the concurrent edit survives", after.endswith("concurrent edit\n"))
    # And the happy path really does replace, leaving no temp file behind.
    with tempfile.TemporaryDirectory() as d:
        p = _write(d, doc)
        so._fence.replace_atomically(p, "# replaced\n", doc)
        left = sorted(x.name for x in p.parent.iterdir())
        after = p.read_text(encoding="utf-8")
    check("the atomic write replaces the file", after == "# replaced\n")
    check("...and leaves no temp file behind", left == [p.name])

    for name, src in (("unclosed fence", UNCLOSED_FENCE),
                      ("unclosed comment", UNCLOSED_COMMENT)):
        check(f"parse refuses an {name}", so.parse(src) is None)
        check(f"reorder refuses an {name}", so.reorder(src) is None)
    with tempfile.TemporaryDirectory() as d:
        p = _write(d, UNCLOSED_FENCE)
        before = p.read_text(encoding="utf-8")
        rc = so.main(["--fix", str(p)])
        hits = so._check_placement([p])
        after = p.read_text(encoding="utf-8")
    check("--fix refuses an unclosed fence (rc 2)", rc == 2)
    check("...and writes nothing", after == before)
    check("--check-placement reports it rather than passing silently", hits == 1)


def test_container_aware_fences():
    """Section 39: the 0-3-space rule lands AFTER the container prefix.

    Every expectation here was taken from a real CommonMark parser before it was
    written down, not from reading the spec and guessing. The oracle is not
    imported at runtime -- `markdown-it-py` is not a dependency of this repo, and
    a test that silently skips when an optional package is missing is a test that
    reports success for doing nothing.
    """
    sys.path.insert(0, str(REPO / "scripts"))
    import todo_fence as tf

    def mask_of(src):
        return tf.scan_text(src).mask

    # THE NAMED CASE. A five-space fence under `100. docs` is a real block, so
    # the `- [ ]` inside it is an EXAMPLE and must not reach the item index.
    src = "100. docs\n     ```\n     - [ ] not an item\n     ```\n\n## 9. Real\n"
    m = mask_of(src)
    check("a 5-space fence under an ordered marker opens a real block",
          m[1] and m[2] and m[3])
    check("...so its `- [ ]` content is masked, not indexed", m[2])
    check("...and the heading after it is still structure", not m[5])

    # BLANK LINE BETWEEN THE MARKER AND THE FENCE (Codex design review, [high]).
    # A blank line does NOT close a list item, so this is the same block one
    # line down. Getting this wrong leaves the fence unopened and leaks the item.
    src = ("100. docs\n\n     ```\n     - [ ] not an item\n     ```\n\n"
           "## 9. Real\n")
    m = mask_of(src)
    check("a blank line does not close a list container before its fence",
          m[2] and m[3] and m[4])
    check("...and the heading after it survives", not m[6])

    # THE ROOT 4-SPACE MARKER IS STILL AN INDENTED CODE BLOCK, which is the
    # regression the container work most easily causes: widening the bound to
    # "any indent" would break this, and section 35's `overindented_fence`
    # fixture pins the same property from the producer side.
    src = "# T\n\n    ```\n    - [ ] literal\n    ```\n\n## 9. Real\n"
    check("a 4-space fence marker at root opens nothing", not any(mask_of(src)))

    # A LIST ITEM WHOSE CONTENT IS ITSELF OVER-INDENTED. 5+ spaces after the
    # marker put the content indent at marker+1, so the remainder is an indented
    # code block and the delimiter stays literal.
    src = "-      ```\n       still code\n"
    check("5+ spaces after a marker leave an indented code block, not a fence",
          not any(mask_of(src)))

    # BLOCKQUOTES ARE CONTAINERS TOO -- 27 of the corpus lines this change
    # claimed were blockquote-prefixed, not list-indented.
    src = "> ```\n> - [ ] not an item\n> ```\n\n## 9. Real\n"
    m = mask_of(src)
    check("a fence inside a blockquote opens a real block", m[0] and m[1] and m[2])
    check("...and the heading after it is still structure", not m[4])

    # NESTED CONTAINERS, both orders.
    src = "> - item\n>   ```\n>   - [ ] not an item\n>   ```\n"
    m = mask_of(src)
    check("a fence nested in a list inside a blockquote opens", m[1] and m[2])

    # A LEAF BLOCK DIES WITH ITS CONTAINER -- the section-36 round-3 shape. The
    # blockquote ends at the unprefixed line, so CommonMark ends the fence there
    # rather than letting it swallow the rest of the document.
    src = "> ```\n> code\n\n## 9. Real\n"
    m = mask_of(src)
    check("leaving a blockquote closes the fence inside it", m[0] and m[1])
    check("...so a later heading is NOT swallowed", not m[3])
    check("...and the document does not read as ending inside a fence",
          tf.scan_text(src).terminal is None)

    # THE TERMINAL FLAG STILL FIRES for a genuinely unclosed container fence,
    # which is what stops the container work from turning a loud refusal into a
    # silent erasure.
    src = "100. docs\n     ```\n     code\n"
    check("an unclosed container-indented fence sets unclosed_fence",
          tf.scan_text(src).terminal is not None)

    # AND THE CLOSER RULES ARE STILL THE LEAF'S. A shorter run inside a longer
    # container-indented block must not close it.
    src = "100. docs\n     ````\n     ```\n     code\n     ````\n\n## 9. Real\n"
    m = mask_of(src)
    check("a shorter inner run does not close a container-indented fence",
          all(m[1:5]) and not m[6])

    # ---- The four findings from this section's adversarial round ----

    # [high] EVERY mask consumer, not just the mask. `checklist_item_leads` ran
    # the leaf matcher on physical lines, so it returned a fenced example as a
    # real item -- and `--fix-line-numbers` consumes that, so a stamp could be
    # rewritten to point at documentation.
    sys.path.insert(0, str(REPO / "scripts" / "todo-graph"))
    import cache_schema as cs
    src = "100. docs\n     ```\n     - [ ] Phantom target\n     ```\n"
    check("checklist_item_leads sees the container-indented fence",
          list(cs.checklist_item_leads(src)) == [])
    check("CONTROL: a real item at root is still found",
          list(cs.checklist_item_leads("## 1. T\n\n- [ ] Real\n")) == [3])

    # [high] A TAB ON THE CUT. The space-only fast path sliced `cols`
    # characters and handed back a tab that re-expands from column 0, so a real
    # fence went unseen and the builder indexed its `- [x]` content.
    src = "- item\n  \t```\n  \t- [x] Fake -> `fake_fn()`\n  \t```\n"
    m = mask_of(src)
    check("a tab straddling a list content indent still opens the fence",
          m[1] and m[2] and m[3])
    check("...so the fenced `- [x]` is not a stamped item",
          list(cs.checklist_item_leads(src)) == [])

    # [medium] LIST INTERRUPTION. Only a bullet or `1.` may interrupt a
    # paragraph, so `10.` after a prose line is paragraph text. Opening a
    # container there made the next indented line read as a fence and the
    # document report as ending inside an unclosed one -- a REFUSAL of a
    # perfectly valid file.
    src = "para\n10. faux\n    ```\n    trailing prose\n"
    _sc = tf.scan_text(src)
    lines, mask = _sc.lines, _sc.mask
    uf = _sc.terminal is not None and _sc.terminal.kind == 'fence'
    uc = _sc.terminal is not None and _sc.terminal.kind == 'comment'
    check("an ordered marker other than 1 cannot interrupt a paragraph",
          not any(mask))
    check("...and the document is NOT reported as unclosed", not uf)
    check("CONTROL: `1.` may interrupt, and its fence is real",
          any(mask_of("para\n1. real\n   ```\n   code\n   ```\n")))
    check("CONTROL: a bullet may interrupt too",
          any(mask_of("para\n- real\n  ```\n  code\n  ```\n")))
    check("CONTROL: after a BLANK line, `10.` opens normally",
          any(mask_of("para\n\n10. real\n    ```\n    code\n    ```\n")))

    # ---- Round 2: three of these were regressions in the round-1 fixes ----

    # [high] A tab ANYWHERE in the retained leading run, not just on the cut.
    # Slicing shifts every retained character by `cols`, so a tab re-expands
    # from the wrong origin whenever `cols % 4 != 0`. The first repair patched
    # only `rest[0]` and this shape still masked nothing.
    for label, src in (
        ("spaces-then-tab", "- item\n   \t```\n   \t- [ ] Phantom\n   \t```\n"),
        ("tab on the cut", "- item\n  \t```\n  \t- [x] Fake\n  \t```\n"),
    ):
        m = mask_of(src)
        check(f"a {label} list indent still opens the fence", all(m[1:4]))
        check(f"...and the {label} fenced item is not indexed",
              list(cs.checklist_item_leads(src)) == [])
    check("CONTROL: consecutive tabs are an indented code block, not a fence",
          not any(mask_of("- item\n\t\t```\n\t\tcode\n\t\t```\n")))

    # [high] AN ORDERED SIBLING IS NOT AN INTERRUPTION. The paragraph `first`
    # lives inside item 1; line 2 is indented too little to continue it, so the
    # item closes and `2.` opens a sibling. A flat `para_open` flag rejected it
    # and the fence below went unopened -- the erasure direction.
    src = "1. first\n2. second\n    ```\n    - [ ] Phantom\n    ```\n"
    m = mask_of(src)
    check("an ordered SIBLING opens, and its fence with it", all(m[2:5]))
    check("...so its fenced item is not indexed",
          list(cs.checklist_item_leads(src)) == [])
    check("a third sibling behaves the same",
          any(mask_of("1. a\n2. b\n3. c\n   ```\n   code\n   ```\n")))

    # [high] AN INLINE COMMENT SPANNING LINES is not an HTML block, so the mask
    # has no concept of it; `checklist_item_leads` keeps its own latch for that
    # case. Routing the walk through the mask left the latch with no reader.
    src = "- [ ] Real <!-- comment\n      - [ ] Phantom\n      continues -->\n"
    check("a checklist line inside a multi-line inline comment is not an item",
          list(cs.checklist_item_leads(src)) == [1])

    # ---- Round 3: real block state, not an approximation of it ----

    # [high] LAZY CONTINUATION. A paragraph line may omit its container prefix
    # entirely and still belong to the item, so a shorter container match does
    # not always mean the container closed.
    src = "10. first\nlazy continuation\n    ```\n    - [ ] Phantom\n    ```\n"
    check("a lazy paragraph continuation keeps its list container",
          all(mask_of(src)[2:5]))
    check("...so the fence inside it is not a phantom item",
          list(cs.checklist_item_leads(src)) == [])

    # [high] PARAGRAPH STATE, not "the line was non-blank". A setext underline
    # closes the paragraph above it and an ATX heading replaces it, so the list
    # that follows is NOT interrupting anything.
    for label, src in (
        ("setext underline",
         "Title\n=====\n10. item\n    ```\n    - [ ] Phantom\n    ```\n"),
        ("ATX heading", "# Title\n10. item\n    ```\n    - [ ] Phantom\n    ```\n"),
        ("thematic break", "para\n---\n10. item\n    ```\n    code\n    ```\n"),
    ):
        check(f"a list after a {label} opens normally", any(mask_of(src)))

    # [medium] A BLANK-FIRST LIST ITEM closes on the following blank, so what
    # comes after is root indented code -- not the item's content, and not an
    # unclosed fence the producer should refuse.
    _sc = tf.scan_text("10.\n\n    ```\n    literal\n")
    lines, mask = _sc.lines, _sc.mask
    uf = _sc.terminal is not None and _sc.terminal.kind == 'fence'
    uc = _sc.terminal is not None and _sc.terminal.kind == 'comment'
    check("a blank-first list item does not swallow the indented run",
          not any(mask))
    check("...and the document is not reported as unclosed", not uf)
    check("CONTROL: a FILLED item still continues across a blank",
          any(mask_of("10. x\n\n    ```\n    code\n    ```\n")))

    # [high] AN INLINE CODE SPAN IS NOT A COMMENT OPENER.
    check("a backticked `<!--` does not latch the comment state",
          list(cs.checklist_item_leads("- [ ] First `<!--`\n- [ ] Second\n"))
          == [1, 2])

    # [medium] THE SHIM SHARES THE PRODUCER'S NEWLINE CONTRACT. A CRLF document
    # with a CLOSED fence must not be reported unclosed by the gates while the
    # builder accepts it.
    crlf = "a\r\n```\r\ncode\r\n```\r\n"
    check("the shim agrees with the producer about a CRLF document",
          tf.scan_text(crlf).terminal is None
          and cs.scan_text(crlf).terminal is None)

    # ---- Round 4: classes the stateful oracle alphabet could not generate ----

    # [high] A LINK REFERENCE DEFINITION is not a paragraph, so a list after it
    # is not interrupting anything.
    src = "[foo]: /url\n10. item\n    ```\n    - [ ] Phantom\n    ```\n"
    check("a list after a link reference definition opens normally",
          all(mask_of(src)[2:5]))
    check("...so its fenced item is not indexed",
          list(cs.checklist_item_leads(src)) == [])

    # [high] NESTED CONTENT FILLS EVERY ENCLOSING ITEM. A blank-first outer item
    # whose first content is another container stayed provisional, so the next
    # blank closed it and took the open fence with it.
    for label, src in (
        ("list", "10.\n    - child\n\n    ```\n    - [ ] Phantom\n    ```\n"),
        ("blockquote", "10.\n    > q\n\n    ```\n    code\n    ```\n"),
    ):
        check(f"a nested {label} fills its blank-first outer item",
              all(mask_of(src)[3:6]))
    check("CONTROL: a blank-first item with NO content still closes",
          not any(mask_of("10.\n\n    ```\n    literal\n")))

    # [medium] CODE SPANS ARE DELIMITER-RUN AWARE. A doubled or tripled backtick
    # run around `<!--` is a literal span, not a comment opener.
    for label, doc in (
        ("double", "- [ ] First ``<!--``\n- [ ] Second\n"),
        ("triple", "- [ ] A ```<!--```\n- [ ] B\n"),
    ):
        check(f"a {label}-backtick span holding an opener does not latch",
              list(cs.checklist_item_leads(doc)) == [1, 2])
    # CONTROL, and note the shape: the hidden line must be an INDENTED
    # continuation of the same item. This control was first written with a
    # sibling `- [ ] P` at column 0 and passed, because the latch was not yet
    # block-scoped -- so the control was pinning the round-5 [high] defect
    # rather than the behaviour. A test written against unverified behaviour
    # inherits its bugs.
    check("CONTROL: a REAL unclosed comment still latches over its own item",
          list(cs.checklist_item_leads(
              "- [ ] R <!-- c\n      - [ ] P\n      -->\n")) == [1])

    # [medium] BOM PARITY with the producer: the helper takes `scan_text`, so a
    # BOM before a first-line fence is normalised away for both.
    check("a BOM-prefixed fence hides its item from the helper too",
          list(cs.checklist_item_leads("﻿```\n- [ ] Phantom\n```\n")) == [])

    # ---- Round 5: marker forms the alphabet had not sampled ----

    # [high] A TAB AFTER THE MARKER expands from the MARKER's column, not from
    # column 0, so `-\titem` has content indent 4 and not 5.
    src = "-\titem\n    ```\n    - [ ] Phantom\n    ```\n"
    check("a tab after a bullet marker gives the right content indent",
          all(mask_of(src)[1:4]))
    check("...so the fence under it is not a phantom item",
          list(cs.checklist_item_leads(src)) == [])

    # [high] AN ORDERED MARKER INTERRUPTS BY VALUE, not by spelling: `01.` is
    # the number 1 and may interrupt a paragraph exactly as `1.` does.
    for label, doc in (("01.", "para\n01. item\n    ```\n    code\n    ```\n"),
                       ("001.", "para\n001. i\n     ```\n     code\n     ```\n")):
        check(f"a zero-padded {label} may interrupt a paragraph", any(mask_of(doc)))
    check("CONTROL: `02.` still may NOT interrupt",
          not any(mask_of("para\n02. item\n    ```\n    trailing\n")))

    # [high] THE COMMENT LATCH IS BLOCK-SCOPED. An unmatched `<!--` in an item's
    # inline text is literal, so it cannot hide a SIBLING item -- which would
    # make `--fix-line-numbers` report a valid target missing.
    check("an unclosed inline comment does not hide the next sibling item",
          list(cs.checklist_item_leads(
              "- [ ] Real <!-- unclosed\n- [ ] Target\ncontinues -->\n")) == [1, 2])
    check("CONTROL: an indented continuation of the SAME item is still hidden",
          list(cs.checklist_item_leads(
              "- [ ] Real <!-- comment\n      - [ ] Phantom\n      ends -->\n")) == [1])
    check("CONTROL: a blank line ends the latch",
          list(cs.checklist_item_leads("- [ ] R <!-- c\n\n- [ ] T\n")) == [1, 3])

    # [medium] The single-line link-reference bound is OWNED, not merely stated:
    # section 42 carries the item the code comment names.
    _s42 = (REPO / "todo/00-infrastructure/TODO-06-todo-metadata-layer.md"
            ).read_text(encoding="utf-8").split("## 42.")[-1].split("\n---")[0]
    check("section 42 owns the multi-line link-reference-definition gap",
          "MULTI-LINE link reference definitions" in _s42)

    # ---- Round 6: the two classes the 2.56M-document alphabet could not reach ----

    # [high] A TAB STOP IS PHYSICAL. Inside a nested container the absolute
    # column must survive prefix stripping, or a tab expands from the wrong
    # origin and the scan invents a fence -- reporting a VALID document as
    # ending inside an unclosed one, which makes the builder refuse it.
    _sc = tf.scan_text("- a\n  -\tb\n    \t```\n    \tliteral\n")
    lines, mask = _sc.lines, _sc.mask
    uf = _sc.terminal is not None and _sc.terminal.kind == 'fence'
    uc = _sc.terminal is not None and _sc.terminal.kind == 'comment'
    check("a tab inside a NESTED container expands from its physical column",
          not any(mask))
    check("...so a valid nested-tab document is not refused", not uf)

    # [high] THE LATCH BOUNDARY IS CONTENT-INDENT + 4, exercised across the
    # whole band rather than at its two ends. CommonMark keeps two list items
    # for indents 0-5 and folds to one at 6, where the line becomes indented
    # code and can only be a lazy paragraph continuation.
    for sp in range(0, 6):
        doc = f"- [ ] Real <!-- unclosed\n{' ' * sp}- [ ] Target\ncontinues -->\n"
        check(f"an unclosed inline comment does not hide a sibling at indent {sp}",
              list(cs.checklist_item_leads(doc)) == [1, 2])
    for sp in (6, 8):
        doc = f"- [ ] Real <!-- unclosed\n{' ' * sp}- [ ] Target\ncontinues -->\n"
        check(f"...but a continuation at indent {sp} IS still hidden",
              list(cs.checklist_item_leads(doc)) == [1])

    # ---- Round 7 ----

    # [high] A TAB AFTER `>` is worth `4 - (col % 4)` columns from the marker's
    # ABSOLUTE end, so a fixed three spaces is right only at column 0.
    for label, doc in (("root", ">\tq\n"), ("nested", "> - a\n>  -\tb\n")):
        _sc = tf.scan_text(doc)
        check(f"a tab after a {label} blockquote marker parses without inventing"
              f" a block",
              not any(_sc.mask) and _sc.terminal is None)

    # [high] THE TERMINAL FLAG IS HONEST, INCLUDING WHERE THE MASK IS NOT. The
    # lazy-continuation residual (section 42) can report a valid document as
    # ending inside a fence. Suppressing the flag after a lazy line was tried
    # and REVERTED: it also suppressed GENUINE unclosed fences, publishing an
    # EOF-erased document instead of refusing it. These four pin the direction
    # of that trade so it cannot be quietly re-softened.
    for label, doc in (
            ("root", "# T\n\n```\nunclosed\n"),
            ("container", "100. docs\n     ```\n     code\n"),
            ("immediately after a lazy line",
             "10. first\nlazy continuation\n    ```\n    unclosed\n"),
            ("unrelated, later in a file with a lazy line",
             "10. a\nlazy\n\npara\n\n```\nunclosed\n")):
        check(f"a real unclosed fence {label} is still reported",
              tf.scan_text(doc).terminal is not None)

    # [medium] CODE-SPAN MASKING IS LINEAR, asserted DETERMINISTICALLY. The
    # first cut rescanned the whole remaining string for every UNMATCHED
    # backtick run, which is quadratic on input the repository controls --
    # `--fix-line-numbers` reaches it on files accepted up to 16 MiB, so one
    # line could stall a repair for minutes (5.6s on the shape below).
    #
    # NOT A WALL-CLOCK BOUND. The first version of this test called
    # `perf_counter()` and required under a second while claiming to test
    # complexity class -- which is neither, and is exactly the flake that can
    # block a commit on a loaded runner. It failed in the reviewer's own
    # sandbox, which is how the contradiction surfaced. Counting executed lines
    # with `settrace` is host-independent: it measures WORK.
    def _work(fn, *a):
        n = 0
        def tracer(frame, event, arg):
            nonlocal n
            if event == "line":
                n += 1
            return tracer
        sys.settrace(tracer)
        try:
            out = fn(*a)
        finally:
            sys.settrace(None)
        return out, n

    # BOUNDED PER BYTE, which is the actual linearity claim. Comparing two RUN
    # COUNTS does not work: run k carries k backticks, so the byte length grows
    # quadratically with the run count and a linear implementation legitimately
    # costs ~14x more work for 4x the runs. Measured on this input: the linear
    # version does 2.3 executed lines per byte, the quadratic one 283.9 -- so a
    # bound of 8 accepts the former and rejects the latter by two orders of
    # magnitude, which is what makes this a real guard rather than a number
    # that happens to pass today.
    big = "".join("`" * k + "x" * 20 for k in range(1, 401)) + "<!--"
    big_masked, w_big = _work(cs.mask_code_spans, big)
    check("code-span masking work is linear in input size",
          w_big < 8 * len(big))
    check("...preserving length", len(big_masked) == len(big))
    check("...and leaving its unmatched runs as literal text",
          "`" in big_masked)

    # [high] AN ESCAPED BACKTICK IS LITERAL TEXT, so it cannot open a span and
    # cannot hide a real `<!--`. Odd backslashes escape; even ones do not.
    check("an escaped backtick does not open a code span",
          "<!--" in cs.mask_code_spans("- [ ] Real \\`<!-- c\\`"))
    check("...so the comment it opens still hides the lines after it",
          list(cs.checklist_item_leads(
              "- [ ] Real \\`<!-- c\\`\n      - [ ] Phantom\n      ends -->\n"))
          == [1])
    check("CONTROL: an EVEN backslash run leaves the delimiter live",
          "<!--" not in cs.mask_code_spans("- [ ] Real \\\\`<!--`"))

    # ---- Round 11 ----

    # [high] ESCAPING IS AN OPENER RULE ONLY. Inside a span everything is
    # literal, so a backslash-prefixed run still CLOSES the span it is in.
    # Ignoring it ran the span on to a later backtick and swallowed a real
    # `<!--`.
    check("an escaped-looking run still closes the span it is inside",
          "<!-- c" in cs.mask_code_spans("- [ ] Real `x \\` <!-- c`"))
    check("...so the comment it exposes hides the item after it",
          list(cs.checklist_item_leads(
              "- [ ] Real `x \\` <!-- c`\n      - [ ] Phantom\n      -->\n")) == [1])

    # [medium] BOTH PROBES ANSWER "IS THIS CODE?" THE SAME WAY. The lead-stop
    # cut kept the old single-backtick regex after the comment probe moved on,
    # so a doubled-run span documenting the marker truncated a real item name.
    check("a doubled-run span holding a stop marker does not cut the lead",
          list(cs.checklist_item_leads(
              "- [ ] Prefix ``-> XREF:`` Tail\n").values())
          == ["- [ ] Prefix ``-> XREF:`` Tail"])
    check("CONTROL: a REAL reference tail is still cut",
          list(cs.checklist_item_leads("- [ ] Name -> XREF: other\n").values())
          == ["- [ ] Name "])

    # [high] A CONTINUATION LINE CAN OPEN A COMMENT. Note the shape: the hidden
    # item must be INDENTED. The review's example put it at column 0, where
    # markdown-it renders two list items and escapes the opener as literal text
    # -- a sibling ends the paragraph, so there is no comment to be inside.
    check("an indented continuation line's comment hides the item under it",
          list(cs.checklist_item_leads(
              "- [ ] Real\n      cont <!-- c\n      - [ ] Phantom\n      -->\n")) == [1])
    check("CONTROL: at column 0 the later item is REAL and stays indexed",
          list(cs.checklist_item_leads(
              "- [ ] Real\n      cont <!-- c\n- [ ] Phantom\n-->\n")) == [1, 3])

    # [medium] BOUNDED MEMORY on a dense-run line. Per-run tuples plus a
    # per-length index measured 95 bytes of Python objects per input byte, so a
    # permitted 16 MiB line was ~1.5 GiB.
    dense = "`x" * 100000
    tracemalloc.start()
    cs.mask_code_spans(dense)
    peak = tracemalloc.get_traced_memory()[1]
    tracemalloc.stop()
    check("dense-run masking stays bounded in memory", peak < 20 * len(dense))

    # ---- Round 12 ----

    # [high] A CONTINUATION OPENER IS BOUNDED BY ITS ITEM, not by its own
    # indent. Swept across the whole band, because using the opener line's
    # indent erased REAL nested items at 1-5 spaces -- the same mistake the
    # item-line path had already fixed, reintroduced one branch over.
    for sp in range(0, 6):
        doc = (f"- [ ] Real\n{' ' * sp}continuation <!-- c\n"
               f"{' ' * sp}- [ ] Phantom\n{' ' * sp}-->\n")
        check(f"a continuation opener at indent {sp} does not erase a real item",
              list(cs.checklist_item_leads(doc)) == [1, 3])
    for sp in (6, 8):
        doc = (f"- [ ] Real\n{' ' * sp}continuation <!-- c\n"
               f"{' ' * sp}- [ ] Phantom\n{' ' * sp}-->\n")
        check(f"...but at indent {sp} the item IS inside the comment",
              list(cs.checklist_item_leads(doc)) == [1])

    # [medium] A BACKSLASH ESCAPES ONE BACKTICK, not a whole run: the remaining
    # run of a `\``-prefixed sequence still opens a span.
    check("an escaped FIRST backtick leaves the rest of the run eligible",
          "<!--" not in cs.mask_code_spans("- [ ] Prefix \\``<!--` Tail"))
    check("...so the item name is not truncated at it",
          list(cs.checklist_item_leads("- [ ] Prefix \\``<!--` Tail\n").values())
          == ["- [ ] Prefix \\``<!--` Tail"])
    check("CONTROL: a single escaped backtick is still literal",
          "<!--" in cs.mask_code_spans("- [ ] Real \\`<!--\\`"))


def test_one_heading_rule_section43():
    """One `## N.` classifier, and nothing may define a second one.

    Section 43. Three copies were deleted in favour of `classify_heading`; the
    fourth (`.claude/hooks/sequencer_triage.py`) is control-plane and an
    unattended run may not edit it, so it is an ALLOWLISTED residual here rather
    than an untested claim -- when an operator retires it, this list shrinks and
    the test says so.
    """
    fence = load("todo_fence_s43", "scripts/todo_fence.py")
    cs = load("cache_schema_s43", "scripts/todo-graph/cache_schema.py")

    # --- the tagged contract -------------------------------------------------
    ok = fence.classify_heading("## 7. Title")
    check("s43: a plain heading classifies ok",
          (ok.kind, ok.n, ok.title) == ("ok", 7, "Title"))
    check("s43: a bare `## 5.` keeps its optional title as None",
          fence.classify_heading("## 5.").title is None)
    check("s43: CommonMark's 1-3 space indent is a heading",
          fence.classify_heading("   ## 9. X").n == 9)
    check("s43: four spaces is not a heading",
          fence.classify_heading("    ## 9. X").kind == "none")
    check("s43: a non-heading line classifies none",
          fence.classify_heading("- [x] item").kind == "none")

    # An UNREPRESENTABLE number is matched and reported, never dropped: the
    # match is what keeps it delimiting its section. Both causes are covered --
    # too many digits (which used to raise ValueError out of `int()`) and a
    # value past the schema ceiling.
    long_head = "## " + "9" * 5000 + ". Over long"
    lr = fence.classify_heading(long_head)
    check("s43: a 5000-digit heading is over-long, not a crash",
          lr.kind == "over-long" and lr.n is None and lr.digits == 5000)
    over = fence.classify_heading("## 70000. Past the ceiling")
    check("s43: a value past the schema ceiling is over-long too",
          over.kind == "over-long" and over.n is None)
    check("s43: the report names the digit count for a long run",
          "5000 digits" in fence.heading_report(3, lr))
    check("s43: the report names the VALUE for an in-length overflow",
          "value above" in fence.heading_report(3, over))
    # The report must never echo the digit RUN -- a multi-megabyte heading would
    # otherwise be printed in full onto a serial line or a lint summary.
    check("s43: the report does not echo the digit run",
          len(fence.heading_report(3, lr)) < 400)

    # --- the producer refuses, and does not re-parent -------------------------
    build = load("build_s43", "scripts/todo-graph/build.py")
    body = ("# T\n\n## Implementation Order\n\n"
            "| Order | Section | Deliverable | Depends On | Status |\n"
            "| :---: | :-----: | --- | --- | :---: |\n"
            "| 1 | " + S + "1 | thing | -- | [x] |\n\n"
            "## 1. Real\n\n- [x] first item\n\n"
            "## " + "9" * 5000 + ". Over long\n\n- [x] second item\n")
    sc = build.scan_body(body)
    heads = build._walk_section_headings(sc.leaf_views, sc.mask, sc.in_blockquote)
    errs = build._walk_unusable_headings(sc.leaf_views, sc.mask, sc.in_blockquote)
    items = build._walk_stamped_items(sc.views, sc.leaf_views, sc.mask,
                                      "todo/x/TODO-01-x.md", sc.in_blockquote)
    check("s43: the producer records the usable heading only",
          [h[0] for h in heads] == [1])
    check("s43: the producer carries an unusable-heading error",
          [e[0] for e in errs] == ["unusable-heading"])
    # THE CONTROL THAT MATTERS. The item below the unusable heading must NOT be
    # filed under section 1 -- that re-parenting is the silent misattribution
    # the whole rule exists to refuse, and it is what a bounded MATCH produced.
    check("s43: the item below an unusable heading is not re-parented to 1",
          [i.get("section_n") for i in items] == [1])

    # --- START and END must agree about indent ------------------------------
    # Section 43's own adversarial review: `classify_heading` was widened to
    # CommonMark's 0-3 indent while every consumer still ENDED a section on a
    # column-0 `## `. Both halves of the desynchronisation are pinned here.
    so = load("section_order_s43", "scripts/todo-section-order.py")
    reach = load("reach_s43", "scripts/todo-reachability.py")
    interleaved = ("# T\n\n## 2. Two\n\nbody two\n\n   ## Notes\n\n"
                   "notes body\n\n   ## 1. One\n\nbody one\n")
    check("s43: an indented non-numbered H2 between sections REFUSES the parse",
          so.parse(interleaved) is None)
    # The one that actually damages a file: `reorder` REWROTE this document and
    # carried the `## Notes` block along with the section above it.
    check("s43: ...so the mutating reorder leaves it untouched",
          so.reorder(interleaved) is None)
    adjacent = ("# T\n\n## 1. One\n\n- [ ] item one\n\n"
                "   ## 2. Two\n\n- [ ] item two\n")
    asc = fence.scan_text(adjacent)
    bodies = {h.n: [asc.views[j] for j in range(*bounds)
                    if asc.views[j].strip().startswith("- [")]
              for h, _, bounds in reach._sections(asc)}
    check("s43: adjacent indented sections have DISJOINT bodies",
          bodies.get(1) == ["- [ ] item one"]
          and bodies.get(2) == ["- [ ] item two"])
    check("s43: the boundary rule accepts the same indent as the start rule",
          fence.is_h2("   ## Notes") and fence.is_h2("## 1. X")
          and not fence.is_h2("    ## Too deep"))
    # AND THE TITLE MUST BE CUT WITH THE SAME RULE. Round 2: `is_h2` accepted
    # 0-3 spaces while the callers still sliced `line[3:]`, so an indented
    # `## OS Comparison` became `# OS Comparison`, stopped matching the
    # closing-matter set, and `sections_after_closing` -- which lint Check 22b
    # calls DIRECTLY -- reported nothing for a section placed after the tail.
    check("s43: h2_title strips the permitted indent, not a fixed 3 columns",
          all(fence.h2_title(pad + "## OS Comparison") == "OS Comparison"
              for pad in ("", " ", "  ", "   ")))
    placed = ("# T\n\n## 1. One\n\nbody\n\n{pad}## OS Comparison\n\n"
              "table\n\n## 2. After the tail\n\nbody two\n")
    check("s43: a section after INDENTED closing matter is still reported",
          all(so.sections_after_closing(placed.format(pad=pad),
                                        so.scan(placed.format(pad=pad)))
              for pad in ("", " ", "  ", "   ")))

    # --- no second grammar, checked at the AST ------------------------------
    # A grep control is evaded by a composed or dynamically-built pattern; the
    # AST sees the string wherever `re.compile` is actually called.
    # DISCOVERED, NOT LISTED, and it catches THREE shapes rather than one. The
    # first version enumerated nine files and looked only for numeric matchers,
    # so it passed while `build.py` and `todo-reachability.py` still decided
    # boundaries with `startswith("## ")` and cut titles with a fixed `[3:]`
    # slice -- the very drift the section claims to have ended (Codex
    # consistency, section 43 post-ship, [high]). A hand-maintained file list
    # is the same failure mode as a hand-maintained inventory: it is only ever
    # as current as the last person to remember it.
    import ast
    allowed = {"scripts/todo-graph/cache_schema.py"}
    # NAMED RESIDUALS, each one a file an unattended run may not edit. Both
    # `.claude/hooks/` and `scripts/overnight/` are control plane by CLAUDE.md,
    # so section 43 consolidated everything it was ALLOWED to and listed the
    # rest rather than pretending the closure was complete. Shrinking this set
    # is an operator-authorised change; growing it is a regression, which is
    # why it is spelled out here instead of being a silent skip.
    residual = {".claude/hooks/sequencer_triage.py",
                "scripts/overnight/section_slice.py",
                "scripts/overnight/section-manifest.py"}
    heading_pat = re.compile(r"##\s*\\?\(?\\d")
    offenders = []
    candidates = sorted(
        str(q.relative_to(REPO)) for q in (REPO / "scripts").rglob("*.py"))
    for rel in candidates:
        if rel in allowed or rel in residual:
            continue
        # TEST FILES ARE EXEMPT, and the reason is not convenience: a fixture
        # BUILDS markdown containing `## ` rather than parsing it, and this very
        # function contains the literals it searches for. Exempting them keeps
        # the check aimed at consumers that DECIDE what a heading is.
        base = rel.rsplit("/", 1)[-1]
        if base.startswith("test_") or "/tests/" in rel:
            continue
        path = REPO / rel
        src = path.read_text(encoding="utf-8", errors="replace")
        try:
            tree = ast.parse(src)
        except SyntaxError:
            continue
        srclines = src.split("\n")
        for node in ast.walk(tree):
            # Shape 1 + 2: a heading regex, or any `## `-anchored literal that
            # carries a digit matcher.
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                v = node.value
                if (v.lstrip("^").startswith("## ") and "\\d" in v) or heading_pat.search(v):
                    offenders.append(f"{rel}:{node.lineno} (grammar)")
                continue
            # Shape 3: `<expr>.startswith("## ")` -- a boundary decision made
            # without the shared rule. The `## <Title>` prefix tests that name a
            # SPECIFIC heading are the same defect: they miss the legal indent.
            if (isinstance(node, ast.Call)
                    and isinstance(node.func, ast.Attribute)
                    and node.func.attr == "startswith"
                    and node.args
                    and isinstance(node.args[0], ast.Constant)
                    and isinstance(node.args[0].value, str)
                    and node.args[0].value.startswith("## ")):
                offenders.append(f"{rel}:{node.lineno} (boundary)")
                continue
            # Shape 4: a fixed-offset title slice on a line known to be a
            # heading -- `l[3:]` reads one character into an indented title.
            if (isinstance(node, ast.Subscript)
                    and isinstance(node.slice, ast.Slice)
                    and isinstance(node.slice.lower, ast.Constant)
                    and node.slice.lower.value == 3
                    and node.slice.upper is None):
                ctx = srclines[node.lineno - 1] if node.lineno <= len(srclines) else ""
                if "##" in ctx or "h2" in ctx.lower() or "heading" in ctx.lower():
                    offenders.append(f"{rel}:{node.lineno} (title slice)")
    check("s43: no consumer decides headings without the shared rule (AST, "
          + f"{len(candidates)} files); found " + ", ".join(offenders),
          not offenders)
    # EVERY RESIDUAL IS REAL, not a stale note: when an operator retires one,
    # this fails and the set above must shrink. That is the only mechanism that
    # stops the exemption list outliving the exemption.
    for rel in sorted(residual):
        q = REPO / rel
        if not q.exists():
            check(f"s43: residual {rel} no longer exists -- remove it from the "
                  f"allowlist", False)
            continue
        body = q.read_text(encoding="utf-8", errors="replace")
        still = ("SECTION_HEADING_RE" in body or "## (" in body
                 or 'startswith("## ' in body)
        check(f"s43: residual {rel} still hand-rolls a heading rule (shrink "
              f"the allowlist when it is retired)", still)

    # --- the TRACKED baseline's own data contract ---------------------------
    # Section 43 post-ship. The helper validates a per-owner map when it is
    # given one, but "the repo's baseline HAS one, and it sums" is a property of
    # THIS FILE, not of any single run -- a commit deleting the key would
    # otherwise pass ordinary lint and surface later as an rc 7 at someone
    # else's scoped commit, which is the deferred blame the map was added to
    # remove (Codex consistency, section 43 post-ship, [high]). Enforcing it in
    # the helper instead was tried and reverted: fixtures legitimately hand it
    # three-key baselines, and a synthetic tree uses the same default path, so
    # the rule could be scoped neither by caller nor by path.
    import json as _json
    base_p = REPO / "scripts/lint/stub-lint-baseline.json"
    base = _json.loads(base_p.read_text(encoding="utf-8"))
    by = base.get("by_owner")
    check("s43: the tracked baseline carries a per-owner map",
          isinstance(by, dict) and bool(by))
    if isinstance(by, dict) and by:
        ok_shape = all(
            isinstance(v, dict)
            and isinstance(v.get("occurrences"), int)
            and not isinstance(v.get("occurrences"), bool)
            and isinstance(v.get("resolved"), int)
            and not isinstance(v.get("resolved"), bool)
            and 0 <= v["resolved"] <= v["occurrences"]
            for v in by.values())
        check("s43: every per-owner entry is a sane (occurrences, resolved) pair",
              ok_shape)
        check("s43: per-owner occurrences sum to the recorded total",
              sum(v["occurrences"] for v in by.values()) == base.get("total"))
        check("s43: per-owner resolved sum to the recorded resolved",
              sum(v["resolved"] for v in by.values()) == base.get("resolved"))
        check("s43: every per-owner key is a repo-relative todo path",
              all(k.startswith("todo/") and ".." not in k for k in by))

    # --- one name for an unreadable document, two exit codes on purpose ------
    # ONE NAME ACROSS THREE TOOLS for an unreadable document. The producer and
    # the reachability gate already filed it under `terminal_category(...)`;
    # validate.py named it in prose alone, so a reader grepping `unclosed-fence`
    # found two of the three places it occurs. A generic category was tried here
    # first and REVERTED: it would have replaced the specific terminal that
    # sections 38-39 deliberately shared with the producer, trading a real
    # parity property for a vaguer one.
    vsrc = (REPO / "scripts/todo-graph/validate.py").read_text(encoding="utf-8")
    check("s43: validate.py's refusal carries the shared terminal category",
          "_refuse(f\"{_cs.terminal_category(_scan.terminal)}" in vsrc)
    # And the token is the SAME for the same document in the gate and the
    # producer, checked against a real terminal rather than asserted in prose.
    unclosed = fence.scan_text("# T\n\n## 1. X\n\n```\nnever closed\n")
    check("s43: an unclosed document has a terminal to categorise",
          unclosed.terminal is not None)
    check("s43: gate and producer categorise it identically",
          fence.terminal_category(unclosed.terminal)
          == cs.terminal_category(unclosed.terminal) == "unclosed-fence")
    check("s43: cache_schema's ceiling is what bounds the classifier",
          cs._HEADING_DIGIT_LIMIT == len(str(cs._MAX_SECTION_N)))


# --------------------------------------------------------------------------
# 15. Section 47: the GATE CONSUMERS agree with the PRODUCER about what a
#     section is. ONE fixture drives build.py, todo-reachability.py,
#     todo-staged-check.py and validate.py; each claim is paired with a control
#     proving the physical-line classifier disagreed.
# --------------------------------------------------------------------------
# `- ## 2. Nested` is a list item containing a real h2 to CommonMark, so the
# producer records section 2 and files the indented items under it. Every gate
# below used to classify headings from PHYSICAL lines, so each saw one section
# and filed section 2's items under section 1.
_S47 = "\n".join([
    "# Fixture",
    "",
    "## Implementation Order",
    "",
    "| S | Order | Section | Deliverable | Dep | Status |",
    "| - | :-: | :-: | - | - | :-: |",
    "| x |  1   | " + S + "1 | Root section    | - |  [ ]   |",
    "| x |  2   | " + S + "2 | Nested section  | - |  [x]   |",
    "",
    "## 1. Root",
    "",
    "- [ ] root open item",
    "",
    "- ## 2. Nested",
    "  - [x] `nested_helper()` shipped under the nested heading",
    "  - [ ] still open under the nested heading",
    "",
    "> **Verified:** 2026-08-12 nested section",
    "> **Quality reviewed:** 2026-08-12",
    "",
    "[jump](#2-nested)",
    ""])

# The SAME document with a QUOTED heading added. A `> ## 99.` is an EXAMPLE:
# `leaf_views` turns it into a real heading, so every consumer that adopts the
# projection must also carry `in_blockquote` or it invents a section here.
_S47_QUOTED = _S47.replace("- [ ] root open item",
                           "- [ ] root open item\n\n> ## 99. Quoted example")


def test_section_context_closure_section47():
    reach = load("reach_s47", "scripts/todo-reachability.py")
    stg = load("staged_s47", "scripts/todo-staged-check.py")
    bld = load("build_s47", "scripts/todo-graph/build.py")
    val = load("validate_s47", "scripts/todo-graph/validate.py")
    fence = load("fence_s47", "scripts/todo_fence.py")

    _s = fence.scan_text(_S47)

    # -- 1. THE PRODUCER is the reference: sections {1, 2}.
    prod = {n for n, _ in bld.extract_section_headings(_S47)}
    check("s47: the producer sees both the root and the nested section",
          prod == {1, 2})

    # -- 2. REACHABILITY agrees, and files the item under the RIGHT section.
    secs = {h.n for h, _, _ in reach._sections(_s)}
    check("s47: reachability agrees with the producer on the section set",
          secs == prod)
    owned = {h.n: [_s.views[j] for j in range(*b)
                   if _s.views[j].lstrip().startswith("- [")]
             for h, _, b in reach._sections(_s)}
    check("s47: the nested section owns its own items",
          any("nested_helper" in b for b in owned.get(2, []))
          and not any("nested_helper" in b for b in owned.get(1, [])))
    # CONTROL: the physical-line classifier this section replaced saw ONE
    # section and swept the nested items into it.

    class _Physical:
        """The same scan projected onto PHYSICAL lines -- the pre-section-47
        classifier, which is what has to disagree for the fix to mean
        anything."""

        def __init__(self, s):
            self._s = s
            self.leaf_views = s.lines
            self.views = s.lines

        def __getattr__(self, k):
            return getattr(self._s, k)

    phys = _Physical(_s)
    phys_secs = {h.n for h, _, _ in reach._sections(phys)}
    check("s47 CONTROL: the physical-line classifier saw only section 1",
          phys_secs == {1})
    phys_owned = {h.n: [phys.views[j] for j in range(*b)
                        if phys.views[j].lstrip().startswith("- [")]
                  for h, _, b in reach._sections(phys)}
    check("s47 CONTROL: ...and swept the nested item under section 1",
          any("nested_helper" in b for b in phys_owned.get(1, [])))

    # -- 3. THE MISATTRIBUTION IS THE DEFECT, and it is attribution rather than
    # silence. With the stamps landing in the nested section's body, the fixed
    # walk reports `open-in-done` against section 2 -- the section that is
    # actually DONE -- while the physical-line walk reported it against section
    # 1, whose Implementation Order row is still open. A gate naming the wrong
    # section sends the repair to the wrong place.
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "TODO-99-s47.md"
        p.write_text(_S47, encoding="utf-8")
        found = reach.audit(str(p), root=d)
    done_secs = {sec for kind, sec, _ in found if kind == "open-in-done"}
    check("s47: `open-in-done` is attributed to the DONE nested section",
          done_secs == {2})

    # -- 4. STAGED-CHECK counts the nested section and demands its provenance.
    def _views(text):
        s = fence.scan_text(text)
        vis = ["" if s.mask[i] else l for i, l in enumerate(s.lines)]
        sec_vis = ["" if (s.mask[i] or s.in_blockquote(i)) else l
                   for i, l in enumerate(s.leaf_views)]
        return s, vis, sec_vis

    s2, vis, sec_vis = _views(_S47)
    check("s47: the staged section total counts the nested section",
          stg._section_total(sec_vis) == 2)
    check("s47 CONTROL: on physical lines the nested section was uncountable",
          stg._section_total(vis) == 1)
    added = [(i + 1, l) for i, l in enumerate(s2.lines)]
    check("s47: a nested `- ## N.` is an ADDED section (so it hits the cap)",
          2 in {stg._is_section(sec_vis[n - 1]) and n
                for n, _ in stg.added_sections(added, sec_vis)} or
          len(stg.added_sections(added, sec_vis)) == 2)
    # The path is used ONLY by `_file_is_new` (a new file's sections are roots
    # by construction and exempt), so it must name a blob that exists in HEAD
    # for the provenance check to engage at all.
    _tracked = "todo/00-infrastructure/TODO-06-todo-metadata-layer.md"
    check("s47: the nested section must declare its provenance",
          any("2. Nested" in head for _, head in
              stg._sections_missing_provenance(_tracked, vis, sec_vis, added)))
    check("s47 CONTROL: on physical lines it escaped the provenance check",
          not any("2. Nested" in head for _, head in
                  stg._sections_missing_provenance(_tracked, vis, vis, added)))

    # -- 5. A QUOTED heading is NOT a section, in either consumer. This is the
    # regression the fix would have introduced: `leaf_views` alone promotes
    # `> ## 99.` into a heading, so the blockquote predicate is load-bearing.
    qs = fence.scan_text(_S47_QUOTED)
    check("s47: a quoted `> ## 99.` is not a section to the producer",
          99 not in {n for n, _ in bld.extract_section_headings(_S47_QUOTED)})
    check("s47: ...nor to reachability",
          99 not in {h.n for h, _, _ in reach._sections(qs)})
    _, qvis, qsec = _views(_S47_QUOTED)
    check("s47: ...nor to the staged section count",
          stg._section_total(qsec) == 2)
    check("s47 CONTROL: without the blockquote predicate it would count 3",
          stg._section_total(["" if qs.mask[i] else l
                              for i, l in enumerate(qs.leaf_views)]) == 3)

    # -- 6. VALIDATE resolves the nested heading's anchor -- through
    # `check_in_file_anchor`, not just the slug helper. An earlier version of
    # this test asserted `"3-missing" not in slugs` and called that the
    # dead-anchor control; the fixture contained no such link and the checker
    # was never invoked, so the assertion held even with dead-anchor reporting
    # switched off entirely (Codex test-coverage, section 47, [medium]).
    slugs = val._heading_slugs(ev[1] for ev in val._scan_markdown(_s)
                               if ev[0] == "heading")
    check("s47: the nested heading's anchor resolves",
          "2-nested" in slugs)
    check("s47 CONTROL: on physical lines that anchor was reported dead",
          "2-nested" not in val._heading_slugs(
              ev[1] for ev in val._scan_markdown(phys) if ev[0] == "heading"))

    def _anchor_findings(text):
        """`check_in_file_anchor` over a one-file snapshot: `{file_path: text}`."""
        rel = "todo/00-x/TODO-99-s47.md"
        return val.check_in_file_anchor([{"file_path": rel}], {rel: text})

    check("s47: the live `#2-nested` link raises no dead-anchor finding",
          not any("2-nested" in f.detail for f in _anchor_findings(_S47)))
    _dead = _anchor_findings(_S47 + "\n[dead](#3-missing)\n")
    check("s47 CONTROL: a genuinely dead anchor IS reported",
          any("3-missing" in f.detail for f in _dead))
    # A heading inside a blockquote IS a real anchor target: GitHub renders it
    # and gives it a slug. This is the ONE place the closure deliberately
    # differs from the graph-context walks above.
    qslugs = val._heading_slugs(ev[1] for ev in val._scan_markdown(qs)
                                if ev[0] == "heading")
    check("s47: a quoted heading still anchors (GitHub renders it)",
          "99-quoted-example" in qslugs)

    # -- 7. STAMPS STAY ON PHYSICAL LINES, and this is the control that proves
    # why. `views` keeps the blockquote marker only on the line that OPENS the
    # quote, so in the ordinary two-line stamp block `> **Verified:**` still
    # matches while the `> **Quality reviewed:**` CONTINUATION under it does
    # not. A fully stamped section would read verified-but-unreviewed and
    # silently stop being DONE -- a partial, asymmetric corruption in the gate
    # that holds `phase FIXPOINT` open.
    check("s47: both stamps match on physical lines",
          any(reach.VERIFIED_RE.match(l) for l in _s.lines)
          and any(reach.QUALITY_RE.match(l) for l in _s.lines))
    check("s47 CONTROL: on `views` the continuation stamp silently stops "
          "matching",
          any(reach.VERIFIED_RE.match(v) for v in _s.views)
          and not any(reach.QUALITY_RE.match(v) for v in _s.views))

    # -- 8. AN OVER-LONG NESTED HEADING IS REFUSED, NOT DROPPED. The producer
    # side of this was pinned in section 45; reachability's was not, and a walk
    # that DROPS the heading re-parents its items under the previous section --
    # a wrong answer rather than a missing one (Codex test-coverage, section 47,
    # [medium]).
    _long = "\n".join([
        "## Implementation Order",
        "",
        "| S | Order | Section | Deliverable | Dep | Status |",
        "| - | :-: | :-: | - | - | :-: |",
        "| x |  1   | " + S + "1 | Root | - |  [ ]   |",
        "",
        "## 1. Root",
        "",
        "- [ ] root item",
        "",
        "- ## 12345678901. Too many digits to be a section number",
        "  - [ ] item under the unusable heading",
        ""])
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "TODO-99-long.md"
        p.write_text(_long, encoding="utf-8")
        long_found = reach.audit(str(p), root=d)
    check("s47: an over-long NESTED heading is reported, not dropped",
          any(k == "unusable-heading" for k, _, _ in long_found))
    # ...and its child item is NOT re-parented onto section 1, which is the
    # damage dropping the heading would do.
    _ls = fence.scan_text(_long)
    _lowned = {h.n: [_ls.views[j] for j in range(*b)
                     if _ls.views[j].lstrip().startswith("- [")]
               for h, _, b in reach._sections(_ls) if h.kind == "ok"}
    check("s47: the unusable heading's item is not re-parented onto section 1",
          not any("under the unusable heading" in b
                  for b in _lowned.get(1, [])))
    check("s47 CONTROL: the physical-line walk saw no unusable heading at all",
          not any(h.kind == "over-long"
                  for h, _, _ in reach._sections(_Physical(_ls))))

    # -- 9. THE FENCE MASK STILL WINS OVER THE PROJECTION. `leaf_views`
    # transforms a nested `- ## N.` whether or not it is fenced, so the mask has
    # to suppress it independently -- the composite case the unfenced fixture
    # above cannot reach (Codex test-coverage, section 47, [medium]).
    _fenced_nested = "\n".join([
        "## 1. Root",
        "",
        "```",
        "- ## 99. Fenced nested example",
        "```",
        "",
        "> - ## 98. Quoted nested example",
        ""])
    _fs = fence.scan_text(_fenced_nested)
    _fsecs = {h.n for h, _, _ in reach._sections(_fs) if h.kind == "ok"}
    check("s47: a FENCED nested heading is not a section",
          _fsecs == {1})
    check("s47: a QUOTED nested heading is not a section either",
          98 not in _fsecs)
    check("s47: the producer agrees on the same composite document",
          {n for n, _ in bld.extract_section_headings(_fenced_nested)} == {1})
    _, _fvis, _fsec = _views(_fenced_nested)
    check("s47: neither counts toward the staged section total",
          stg._section_total(_fsec) == 1)
    # THE VALIDATOR SPLITS HERE, and that is the documented divergence rather
    # than an inconsistency: the FENCED heading is not rendered by GitHub and
    # must not anchor, while the QUOTED one IS rendered with a slug and must.
    # This assertion is what proves the two rules are independent -- the mask
    # suppresses, the blockquote predicate (deliberately absent here) does not.
    _vslugs = val._heading_slugs(ev[1] for ev in val._scan_markdown(_fs)
                                 if ev[0] == "heading")
    check("s47: the FENCED nested heading is not a validator anchor",
          "99-fenced-nested-example" not in _vslugs)
    check("s47: the QUOTED nested heading IS an anchor (GitHub renders it)",
          "98-quoted-nested-example" in _vslugs)
    # CONTROLS: remove the fence and remove the quote INDEPENDENTLY, and each
    # heading must become visible -- proving the suppression came from the mask
    # and the predicate rather than from the projection failing to see them.
    _unfenced = _fenced_nested.replace("```\n", "")
    check("s47 CONTROL: unfenced, the same nested heading IS a section",
          99 in {h.n for h, _, _ in reach._sections(fence.scan_text(_unfenced))
                 if h.kind == "ok"})
    _unquoted = _fenced_nested.replace("> - ## 98.", "- ## 98.")
    check("s47 CONTROL: unquoted, the same nested heading IS a section",
          98 in {h.n for h, _, _ in reach._sections(fence.scan_text(_unquoted))
                 if h.kind == "ok"})


# --------------------------------------------------------------------------
# 16. Section 47: the staged gate REFUSES a document whose git coordinates and
#     scan coordinates cannot agree.
# --------------------------------------------------------------------------
def test_staged_coordinate_refusal_section47():
    stg = load("staged_coord_s47", "scripts/todo-staged-check.py")
    # `git diff` delimits on LF alone; `cache_schema.normalize_newlines` also
    # maps lone CR, VT, FF, NEL and U+2028/9 to `\n`. One git line then becomes
    # several projection lines and every lineno-keyed check addresses unrelated
    # text. 0 of 281 corpus files contain one, so this refuses a shape that does
    # not occur rather than changing a live verdict.
    for name, brk in (("lone CR", "\r"), ("vertical tab", "\v"),
                      ("form feed", "\f"), ("NEL", "\x85"),
                      ("line separator", "\u2028")):
        text = "## 1. One\n\nbody" + brk + "more\n\n- ## 2. Two\n"
        lines, mask, leaves, in_bq, reason = stg._post_image_lines(text)
        check("s47: a %s makes the staged document unscannable" % name,
              bool(reason) and lines is None)
    ok = "## 1. One\n\nbody\n\n- ## 2. Two\n"
    lines, mask, leaves, in_bq, reason = stg._post_image_lines(ok)
    check("s47 CONTROL: a plain LF document is still scanned",
          not reason and lines is not None and leaves is not None)
    crlf = "## 1. One\r\n\r\nbody\r\n"
    check("s47 CONTROL: CRLF is normalised, not refused (git counts it as one)",
          not stg._post_image_lines(crlf)[4])

    # EVERY break the production regex accepts is covered, and the WHOLE tuple
    # is asserted -- a refusal that returned real `lines` with a reason set
    # would let a caller walk the unprojected document anyway (Codex
    # test-coverage, section 47, [medium]).
    for name, brk in (("FS", "\x1c"), ("GS", "\x1d"), ("RS", "\x1e"),
                      ("paragraph separator", "\u2029")):
        got = stg._post_image_lines("## 1. One\n\nbody" + brk + "more\n")
        check("s47: a %s is refused too" % name,
              bool(got[4]) and got[:4] == (None, None, None, None))
    check("s47: the refusal returns the FULL null tuple, not just null lines",
          stg._post_image_lines("a\rb")[:4] == (None, None, None, None))

    # `added_sections` indexes `heads_view` BY GIT LINE NUMBER, so its bounds
    # are a real index path: line 0 and line > len must be dropped rather than
    # wrap or raise, and a heading on the FIRST and LAST line must survive.
    hv = ["## 1. One", "body", "  ## 2. Two"]
    check("s47: an out-of-range added lineno is dropped, not wrapped",
          stg.added_sections([(0, "x"), (99, "y")], hv) == [])
    check("s47 CONTROL: line 1 and the LAST line are both still returned",
          [n for n, _ in stg.added_sections(
              [(1, "## 1. One"), (3, "- ## 2. Two")], hv)] == [1, 3])


def test_logical_section_addition_section47():
    """A section can be created by a CONTAINER edit that never touches the
    heading line; the cap and provenance controls must still see it."""
    stg = load("staged_logical_s47", "scripts/todo-staged-check.py")
    fence = load("fence_logical_s47", "scripts/todo_fence.py")

    # HEAD has an indented `## 2.` that is an indented CODE BLOCK -- not a
    # section. The commit adds ONLY the list opener above it, which turns the
    # unchanged line into list content whose projection is a real heading.
    head = "# T\n\nprose\n\n    ## 2. Nested\n"
    staged = "# T\n\nprose\n\n- opener\n    ## 2. Nested\n"
    hs, ss = fence.scan_text(head), fence.scan_text(staged)
    head_secs = {fence.classify_heading(l).n
                 for i, l in enumerate(hs.leaf_views)
                 if not hs.mask[i] and not hs.in_blockquote(i)
                 and fence.classify_heading(l).kind == "ok"}
    staged_secs = {fence.classify_heading(l).n
                   for i, l in enumerate(ss.leaf_views)
                   if not ss.mask[i] and not ss.in_blockquote(i)
                   and fence.classify_heading(l).kind == "ok"}
    check("s47: the container edit alone creates a producer-visible section",
          head_secs == set() and staged_secs == {2})

    sec_vis = ["" if (ss.mask[i] or ss.in_blockquote(i)) else l
               for i, l in enumerate(ss.leaf_views)]
    # Only the opener (line 5) is in the diff; the heading is line 6.
    added = [(5, "- opener")]
    check("s47 CONTROL: the line-based set does not contain the heading line",
          6 not in {n for n, _ in added})

    with tempfile.TemporaryDirectory() as d:
        _git(d, "init", "-q", ".")
        _git(d, "config", "user.email", "t@t")
        _git(d, "config", "user.name", "t")
        rel = "todo/00-x/TODO-99-logical.md"
        fp = pathlib.Path(d) / rel
        fp.parent.mkdir(parents=True, exist_ok=True)
        fp.write_text(head, encoding="utf-8")
        _git(d, "add", rel)
        _git(d, "commit", "-q", "-m", "base")
        fp.write_text(staged, encoding="utf-8")
        _git(d, "add", rel)
        cwd = os.getcwd()
        try:
            os.chdir(d)
            head_nos = stg._head_section_numbers(rel)
            widened = stg._section_additions(rel, ss.lines, sec_vis, added)
        finally:
            os.chdir(cwd)

    # A COUNTER, not a set: section identity is occurrence-aware so a
    # renumbering or a delete-and-re-add (same count, different lines) does not
    # read as an addition.
    check("s47: HEAD's projected section counts are read for comparison",
          not head_nos)
    check("s47: the untouched heading line joins the section additions",
          6 in {n for n, _ in widened})
    check("s47: ...so the nested section now counts as ADDED",
          [n for n, _ in stg.added_sections(widened, sec_vis)] == [6])
    check("s47 CONTROL: without the logical delta it counted as nothing",
          stg.added_sections(added, sec_vis) == [])
    check("s47: the line-based checks keep the UNWIDENED set",
          len(added) == 1 and added[0][0] == 5)


def test_section_identity_shapes_section47():
    """Section identity is OCCURRENCE-aware, so the shapes that look like an
    addition and are not stay silent (Codex adversarial, section 47, [high])."""
    stg = load("staged_identity_s47", "scripts/todo-staged-check.py")
    fence = load("fence_identity_s47", "scripts/todo_fence.py")

    def _run(head, staged, added):
        s = fence.scan_text(staged)
        sec_vis = ["" if (s.mask[i] or s.in_blockquote(i)) else l
                   for i, l in enumerate(s.leaf_views)]
        with tempfile.TemporaryDirectory() as d:
            _git(d, "init", "-q", ".")
            _git(d, "config", "user.email", "t@t")
            _git(d, "config", "user.name", "t")
            rel = "todo/00-x/TODO-99-id.md"
            fp = pathlib.Path(d) / rel
            fp.parent.mkdir(parents=True, exist_ok=True)
            fp.write_text(head, encoding="utf-8")
            _git(d, "add", rel)
            _git(d, "commit", "-q", "-m", "base")
            fp.write_text(staged, encoding="utf-8")
            _git(d, "add", rel)
            cwd = os.getcwd()
            try:
                os.chdir(d)
                out = stg._section_additions(rel, s.lines, sec_vis, added)
            finally:
                os.chdir(cwd)
        return [n for n, _ in stg.added_sections(out, sec_vis)]

    # 1. SAME-NUMBER REPLACEMENT. HEAD already has a section 2; the commit
    # deletes it and adds a different one. Against a number SET this looked
    # like "2 already exists" and the new section escaped every control.
    same = _run("# T\n\n## 2. Old two\n\nold body\n",
                "# T\n\n## 2. New two\n\nnew body\n",
                [(3, "## 2. New two")])
    check("s47: a same-number replacement still counts as an added section",
          same == [3])

    # 2. RENUMBERING. One section, renamed 2 -> 3. The count of sections is
    # unchanged, so nothing was created and no provenance is owed... but the
    # heading line IS in the diff, so it stays line-based and is reported.
    # That is the pre-existing behaviour and is deliberately not changed here.
    renum = _run("# T\n\n## 2. Two\n\nbody\n",
                 "# T\n\n## 3. Two\n\nbody\n",
                 [(3, "## 3. Two")])
    check("s47: a renumbering is reported from the diff, not invented twice",
          renum == [3])

    # 3. DELETE-AND-RE-ADD at a different position, same number, same count.
    # The multiset difference is zero, so the logical walk adds NOTHING; only
    # the genuinely-added line is reported.
    moved = _run("# T\n\n## 2. Two\n\nbody\n\n## 5. Five\n",
                 "# T\n\n## 5. Five\n\n## 2. Two\n\nbody\n",
                 [(5, "## 2. Two")])
    check("s47: a move does not invent a second addition",
          moved == [5])

    # 4. AN UNCHANGED FILE adds nothing at all -- the control proving the
    # logical walk is silent when the commit created no section.
    quiet = _run("# T\n\n## 2. Two\n\nbody\n",
                 "# T\n\n## 2. Two\n\nbody edited\n",
                 [(5, "body edited")])
    check("s47 CONTROL: an edit that creates no section adds no section",
          quiet == [])


def test_baseline_fail_closed_section47():
    """A HEAD blob that EXISTS but cannot be read must refuse, not silently
    fall back to the pre-section-47 controls (Codex adversarial, [medium])."""
    stg = load("staged_baseline_s47", "scripts/todo-staged-check.py")
    check("s47: the unreadable-baseline error type exists and is distinct",
          issubclass(stg._BaselineUnreadable, RuntimeError))
    with tempfile.TemporaryDirectory() as d:
        _git(d, "init", "-q", ".")
        _git(d, "config", "user.email", "t@t")
        _git(d, "config", "user.name", "t")
        rel = "todo/00-x/TODO-99-base.md"
        fp = pathlib.Path(d) / rel
        fp.parent.mkdir(parents=True, exist_ok=True)
        fp.write_text("# T\n\n## 1. One\n", encoding="utf-8")
        _git(d, "add", rel)
        _git(d, "commit", "-q", "-m", "base")
        cwd = os.getcwd()
        try:
            os.chdir(d)
            present = stg._head_section_numbers(rel)
            absent = stg._head_section_numbers("todo/00-x/TODO-99-nope.md")
            # An oversized historical blob is refused rather than buffered into
            # the blocking hook.
            big = pathlib.Path(d) / "todo/00-x/TODO-99-big.md"
            big.write_text("x" * (stg._MAX_BASELINE_BYTES + 1),
                           encoding="utf-8")
            _git(d, "add", "todo/00-x/TODO-99-big.md")
            _git(d, "commit", "-q", "-m", "big")
            try:
                stg._head_section_numbers("todo/00-x/TODO-99-big.md")
                over = False
            except stg._BaselineUnreadable:
                over = True
        finally:
            os.chdir(cwd)
    check("s47: a readable HEAD blob yields its projected counts",
          present is not None and present.get(1) == 1)
    check("s47 CONTROL: a genuinely ABSENT blob is None (a new file), not an "
          "error", absent is None)
    check("s47: an oversized HEAD blob is REFUSED, not buffered",
          over)


def main():
    test_shim()
    test_container_aware_fences()
    test_mask_text_cannot_join_lines()
    test_reachability()
    test_orphan_check()
    test_staged_check()
    test_lint_checks_10_11()
    test_lint_staged_scope()
    test_check24_counts_the_refusal()
    test_reflow()
    test_section_order()
    test_html_blocks_section42()
    test_one_heading_rule_section43()
    test_container_phase_section45()
    test_html_block_type7_section46()
    test_section_context_closure_section47()
    test_staged_coordinate_refusal_section47()
    test_logical_section_addition_section47()
    test_section_identity_shapes_section47()
    test_baseline_fail_closed_section47()
    test_raw_html_is_not_rewritten_section48()
    if _FAILS:
        sys.stderr.write("test_todo_fence FAIL (%d):\n  - %s\n"
                         % (len(_FAILS), "\n  - ".join(_FAILS)))
        return 1
    if _SKIPS:
        sys.stderr.write("test_todo_fence SKIPPED %d check(s):\n  - %s\n"
                         % (len(_SKIPS), "\n  - ".join(_SKIPS)))
    print("test_todo_fence OK (%d assertions%s)"
          % (_ASSERTED[0],
             ", %d SKIPPED" % len(_SKIPS) if _SKIPS else ""))
    return 0


class _RxCharSet:
    """One parsed atom of the oracle pattern: ranges, `\\s`, and negation."""

    def __init__(self, ranges=(), negate=False, named=()):
        self.ranges = tuple(ranges)
        self.negate = negate
        self.named = tuple(named)

    def key(self):
        return (self.ranges, self.negate, self.named)

    def mask(self, ncp, ws):
        """This atom's membership over every code point, as `bytes`."""
        m = bytearray(ncp)
        for lo, hi in self.ranges:
            a, b = ord(lo), ord(hi)
            m[a:b + 1] = b"\x01" * (b - a + 1)
        for n in self.named:
            # Only `\s` appears in the oracle pattern. Anything else would be
            # silently mis-modelled, so refuse it rather than guess.
            if n != "s":
                raise AssertionError("unsupported named class \\%s" % n)
            m = bytearray(x | y for x, y in zip(m, ws))
        if self.negate:
            m = bytearray(1 - x for x in m)
        return bytes(m)


class _RxParser:
    """Recursive-descent parser for the subset the oracle pattern uses.

    Deliberately STRICT: every construct it does not model raises rather than
    being skipped, so a future markdown-it-py that widens the pattern breaks
    this proof loudly instead of proving something weaker than it claims.
    """

    def __init__(self, pat):
        self.p = pat
        self.i = 0

    def peek(self):
        return self.p[self.i] if self.i < len(self.p) else None

    def parse(self):
        node = self.alt()
        assert self.i == len(self.p), "unparsed tail %r" % (self.p[self.i:],)
        return node

    def alt(self):
        branches = [self.concat()]
        while self.peek() == "|":
            self.i += 1
            branches.append(self.concat())
        return ("alt", branches) if len(branches) > 1 else branches[0]

    def concat(self):
        items = []
        while True:
            c = self.peek()
            if c is None or c in "|)":
                break
            items.append(self.piece())
        return ("cat", items)

    def piece(self):
        atom = self.atom()
        while self.peek() in ("*", "+", "?"):
            q = self.p[self.i]
            self.i += 1
            atom = ({"*": "star", "+": "plus", "?": "opt"}[q], atom)
        return atom

    def atom(self):
        c = self.p[self.i]
        if c == "(":
            assert self.p[self.i:self.i + 3] == "(?:", "only (?: groups"
            self.i += 3
            node = self.alt()
            assert self.p[self.i] == ")"
            self.i += 1
            return node
        if c == "[":
            return ("set", self.charclass())
        if c in "^$":
            # `^` is start-of-input and `$` is end-of-input: both are structural
            # here because the pattern is matched against ONE line, which never
            # contains a newline. The exhaustive cross-check against Python's
            # own `re` below is what pins that reading.
            self.i += 1
            return ("anchor", None)
        if c == "\\":
            self.i += 1
            e = self.p[self.i]
            self.i += 1
            if e == "s":
                return ("set", _RxCharSet(named=("s",)))
            assert e not in "SdDwWbBAZ", "unsupported escape \\%s" % e
            return ("set", _RxCharSet(ranges=((e, e),)))
        self.i += 1
        return ("set", _RxCharSet(ranges=((c, c),)))

    def _classchar(self):
        c = self.p[self.i]
        if c != "\\":
            self.i += 1
            return c, None
        self.i += 1
        e = self.p[self.i]
        self.i += 1
        if e == "s":
            return None, "s"
        if e == "x":
            v = chr(int(self.p[self.i:self.i + 2], 16))
            self.i += 2
            return v, None
        assert e not in "SdDwW", "unsupported class escape \\%s" % e
        return e, None

    def charclass(self):
        assert self.p[self.i] == "["
        self.i += 1
        negate = self.peek() == "^"
        if negate:
            self.i += 1
        ranges, named, first = [], [], True
        while True:
            if self.p[self.i] == "]" and not first:
                self.i += 1
                break
            first = False
            lo, name = self._classchar()
            if name is not None:
                named.append(name)
                continue
            if self.peek() == "-" and self.p[self.i + 1] != "]":
                self.i += 1
                hi, hname = self._classchar()
                assert hname is None, "range endpoint cannot be a class"
                ranges.append((lo, hi))
            else:
                ranges.append((lo, lo))
        return _RxCharSet(ranges, negate, named)


def _rx_build(node, eps, trans, start):
    """Thompson construction; returns the state reached after `node`."""
    kind = node[0]
    if kind == "cat":
        s = start
        for item in node[1]:
            s = _rx_build(item, eps, trans, s)
        return s
    if kind == "alt":
        eps.append([])
        trans.append([])
        end = len(eps) - 1
        for br in node[1]:
            eps.append([])
            trans.append([])
            s = len(eps) - 1
            eps[start].append(s)
            eps[_rx_build(br, eps, trans, s)].append(end)
        return end
    if kind == "set":
        eps.append([])
        trans.append([])
        end = len(eps) - 1
        trans[start].append((node[1], end))
        return end
    if kind == "anchor":
        return start
    eps.append([])
    trans.append([])
    loop = len(eps) - 1
    eps[start].append(loop)
    inner = _rx_build(node[1], eps, trans, loop)
    eps.append([])
    trans.append([])
    end = len(eps) - 1
    if kind == "opt":
        # `?`: the inner path, or nothing.
        inner2 = inner
        eps[inner2].append(end)
        eps[loop].append(end)
        return end
    eps[inner].append(loop)
    if kind == "star":
        eps[loop].append(end)
    elif kind == "plus":
        eps[inner].append(end)
    else:
        raise AssertionError(kind)
    return end


def _rx_closure(eps, states):
    out, stack = set(states), list(states)
    while stack:
        for t in eps[stack.pop()]:
            if t not in out:
                out.add(t)
                stack.append(t)
    return frozenset(out)


def test_html_block_type7_section46():
    """Type 7's DFA is PROVED equivalent to the oracle, not sampled against it.

    Three prior recognisers each passed their own sampling and then diverged on
    ambiguity the reference resolves by global backtracking, so agreement over
    any finite set of strings is not the bar here. Both machines are finite, so
    equivalence is DECIDABLE: determinise the oracle's own pattern and walk the
    product to fixpoint (Codex design review, section 46, [high]).
    """
    tf = load("todo_fence_s46", "scripts/todo_fence.py")
    cs = tf._cache_schema()
    ncp = 0x110000

    try:
        from markdown_it.common.html_re import HTML_OPEN_CLOSE_TAG_STR
    except ImportError:
        _SKIPS.append("section 46 equivalence proof (markdown-it-py not installed)")
        HTML_OPEN_CLOSE_TAG_STR = None

    if HTML_OPEN_CLOSE_TAG_STR is not None:
        pattern = HTML_OPEN_CLOSE_TAG_STR + r"\s*$"
        eps, trans = [[]], [[]]
        end = _rx_build(_RxParser(pattern).parse(), eps, trans, 0)

        atoms = {}
        for edges in trans:
            for cset, _ in edges:
                atoms.setdefault(cset.key(), cset)
        atoms = list(atoms.values())

        # ---- Soundness: the shipped class partition IS the oracle's own. ----
        # A finite alphabet may stand in for all of Unicode only if every atom
        # either machine can test is CONSTANT on each class. Checked over every
        # code point, mechanically -- not over a hand-picked sample, which is
        # how the earlier attempts' alphabets missed the NBSP case.
        ws = bytes(map(str.isspace, map(chr, range(ncp))))
        masks = [a.mask(ncp, ws) for a in atoms]
        cls = bytes(map(cs._tag_class, map(chr, range(ncp))))
        pairs = set(zip(zip(*masks), cls))
        sigs = {sig for sig, _ in pairs}
        kinds = {k for _, k in pairs}
        check("s46 the class partition is exactly the oracle's atom partition",
              len(pairs) == len(sigs) == len(kinds) == cs._TAG_CLASSES)
        reps = {}
        for k in kinds:
            reps[k] = chr(cls.index(k))
        # `is_complete_tag_line` does NOT call `_tag_class` in its loop -- it
        # indexes a 256-entry table and decides everything above that inline.
        # The proof below is about `_tag_class`, so the fast path has to be
        # proved identical to it or the proof covers code nothing runs.
        fast = bytes((cs._TAG_CLASS_TABLE[cp] if cp < 256 else
                      (cs._T_WS_WIDE if chr(cp).isspace() else cs._T_UNQ))
                     for cp in range(ncp))
        check("s46 the scan's fast classification path IS `_tag_class`, over "
              "every code point", fast == cls)

        # ---- Determinise the oracle over that alphabet. ----
        s0 = _rx_closure(eps, {0})
        idx, order, otrans = {s0: 0}, [s0], []
        i = 0
        while i < len(order):
            cur, row = order[i], []
            for k in range(cs._TAG_CLASSES):
                c = reps[k]
                nxt = set()
                for s in cur:
                    for cset, t in trans[s]:
                        hit = any(lo <= c <= hi for lo, hi in cset.ranges)
                        if not hit and "s" in cset.named:
                            hit = c.isspace()
                        if hit != cset.negate:
                            nxt.add(t)
                if not nxt:
                    row.append(-1)
                    continue
                nxt = _rx_closure(eps, nxt)
                j = idx.get(nxt)
                if j is None:
                    j = idx[nxt] = len(order)
                    order.append(nxt)
                row.append(j)
            otrans.append(row)
            i += 1
        oacc = [end in s for s in order]

        # ---- Product reachability to fixpoint. BFS, so the first divergence
        # found is the SHORTEST distinguishing string. ----
        strans, sacc = cs._TAG_DFA_TRANS, cs._TAG_DFA_ACCEPT
        seen, queue, qi, bad = {(0, 0): ""}, [(0, 0)], 0, None
        while qi < len(queue):
            a, b = queue[qi]
            qi += 1
            av = sacc[a] if a >= 0 else False
            bv = oacc[b] if b >= 0 else False
            if av != bv and bad is None:
                bad = seen[(a, b)]
            for k in range(cs._TAG_CLASSES):
                na = strans[a][k] if a >= 0 else -1
                nb = otrans[b][k] if b >= 0 else -1
                if na < 0 and nb < 0:
                    continue
                if (na, nb) not in seen:
                    seen[(na, nb)] = seen[(a, b)] + reps[k]
                    queue.append((na, nb))
        check("s46 the DFA is EQUIVALENT to the oracle over all of Unicode "
              "(product reachability, %d states%s)"
              % (len(seen), "" if bad is None else ", counterexample %r" % bad),
              bad is None)

        # ---- Independent control: the compiled reference vs Python's own
        # `re`. If `_RxParser`/`_rx_build` were wrong, the product check above
        # would be proving equivalence to the WRONG machine, so this is what
        # keeps the proof from being circular. It also settles `$` vs
        # end-of-input, since the alphabet carries `\n`.
        #
        # EXHAUSTION ALONE IS NOT ENOUGH HERE, and the first cut of this test
        # got that wrong: the shortest tag carrying an attribute is `<a b>` at
        # FIVE characters, so an exhaustive sweep to length 4 never reaches
        # `attribute*`, `\s+`, a value, a quoted group or a repeat -- which is
        # the exact grammar the three earlier attempts died on (Codex
        # test-coverage, section 46, [medium]). The sweep is kept for its
        # density near the empty string and joined by two targeted generators.
        rx = re.compile(pattern)
        alpha = [reps[k] for k in sorted(reps)]
        words = []
        for n in range(5):
            words.extend("".join(t) for t in itertools.product(alpha, repeat=n))

        # (a) TRANSITION COVER of the compiled reference: the shortest word
        # traversing every reachable (state, class) edge. A mis-compiled group
        # or quantifier changes the machine's shape, so covering its edges is
        # what exercises the compilation rather than the alphabet.
        cover, seen_st, frontier = [], {0: ""}, [0]
        while frontier:
            st = frontier.pop()
            for k in range(cs._TAG_CLASSES):
                nx = otrans[st][k]
                if nx < 0:
                    continue
                cover.append(seen_st[st] + reps[k])
                if nx not in seen_st:
                    seen_st[nx] = seen_st[st] + reps[k]
                    frontier.append(nx)
        words.extend(cover)

        # (b) MULTIPLE LOOP ITERATIONS, which no shortest-path word forces:
        # every attribute form crossed with every tail at 0..4 repeats.
        for form in ("a", "a=b", "a='b'", 'a="b"', "a=b/", "a =b", "a= b",
                     "a  =  b", "_:x.y=b", "a=b c", "a='b c'", 'a="b\'c"'):
            for tail in (">", "/>", " >", "  />", "", "=>"):
                for k in range(5):
                    words.append("<x" + (" " + form) * k + tail)
            for k in range(3):
                words.append("</x" + (" " + form) * k + ">")

        mismatch = next((w for w in words
                         if bool(rx.search(w)) != cs.is_complete_tag_line(w)),
                        None)
        check("s46 %d control words (exhaustive <=4 + %d-edge transition cover"
              " + attribute repeats) agree with `re`%s"
              % (len(words), len(cover),
                 "" if mismatch is None else " -- first %r" % mismatch),
              mismatch is None)
        check("s46 the control reaches the attribute grammar at all",
              any(len(w) > 4 and " " in w and "=" in w for w in words))

        # (c) MUTATION CONTROL: a compiler that cannot detect a WRONG pattern
        # proves nothing about the right one. Dropping the `*` from
        # `attribute*` must make the compiled machine disagree with `re`.
        meps, mtrans = [[]], [[]]
        mutated = pattern.replace(")?)*\\s*", ")?)\\s*", 1)
        mend = _rx_build(_RxParser(mutated).parse(), meps, mtrans, 0)
        probe = "<x a=b c=d>"
        macc = _rx_closure(meps, {0})
        for ch in probe:
            nxt = set()
            for s in macc:
                for cset, t in mtrans[s]:
                    hit = any(lo <= ch <= hi for lo, hi in cset.ranges)
                    if not hit and "s" in cset.named:
                        hit = ch.isspace()
                    if hit != cset.negate:
                        nxt.add(t)
            macc = _rx_closure(meps, nxt) if nxt else frozenset()
        check("s46 MUTATION CONTROL: a mangled `attribute*` compiles to a "
              "machine that rejects %r, so the compiler is not vacuous" % probe,
              mutated != pattern and mend not in macc
              and bool(rx.search(probe)))

    # ---- The boundary cases the design review named. ----
    for word, want in (("<a>", True), ("<a/>", True), ("</a>", True),
                       ("</a-b>", True), ("</a >", True), ("<a >", True),
                       ("</>", False), ("</9a>", False), ("</a/>", False),
                       ("<a/b>", False), ("<>", False), ("<a", False),
                       ("<a b>", True), ("<a b=c>", True), ("<a b = c>", True),
                       ("<a b=>", False), ("<a b = >", False),
                       ("<a b='c'd>", False), ("<a b='c' d>", True),
                       ("<a b=c/>", True), ("<a b=/>", True),
                       ("<a _x:y.z=1>", True), ("<a .x=1>", False),
                       ("<a b=`>", False), ("<a>x", False), ("<a> ", True)):
        check("s46 %r is%s a complete tag" % (word, "" if want else " NOT"),
              cs.is_complete_tag_line(word) is want)

    # ---- Non-ASCII whitespace in EVERY grammar position. These 19 code points
    # are simultaneously `\s` AND legal unquoted content, which is the exact
    # ambiguity a single-path scanner cannot resolve: after `=` a NBSP is both
    # the `\s*` run and the first character of a value. ----
    for cp in (" ", "", " ", "　"):
        for shape in ("<a%sb>", "<a b%s=c>", "<a b=%sc>", "<a b=c%s>",
                      "<a>%s", "</a%s>", "<a%s/>"):
            word = shape % cp
            want = bool(re.match(r"^(?:<[A-Za-z][A-Za-z0-9\-]*"
                                 r"(?:\s+[a-zA-Z_:][a-zA-Z0-9:._-]*"
                                 r"(?:\s*=\s*(?:[^\"'=<>`\x00-\x20]+"
                                 r"|'[^']*'|\"[^\"]*\"))?)*\s*/?>"
                                 r"|</[A-Za-z][A-Za-z0-9\-]*\s*>)\s*$", word))
            check("s46 %r agrees with the oracle grammar" % word,
                  cs.is_complete_tag_line(word) is want)

    # ---- Behaviour in the scan: opening, closing, precedence, projections. ----
    r = tf.scan_text("<custom-widget>\n## 99. Fake\n- [x] Fake item\n\n"
                     "## 1. Real\n")
    check("s46 type 7 hides a heading and an item inside it",
          r.mask[0] and r.mask[1] and r.mask[2])
    check("s46 the block ends at the blank line, which stays unmasked",
          not r.mask[3] and not r.mask[4])
    check("s46 a type-7 block running to EOF is well-formed, never a terminal",
          tf.scan_text("<custom-widget>\nx\n").terminal is None)
    # PRECEDENCE: `<pre>` is a complete open tag too. Matching it as type 7
    # would give it a blank-line end rule instead of `</pre>`, so the row order
    # is load-bearing rather than cosmetic.
    pre = tf.scan_text("<pre>\n\n## 99. Fake\n</pre>\n\n## 1. Real\n")
    check("s46 `<pre>` stays type 1: a blank line does NOT end it",
          pre.mask[2] and pre.kinds[2] == "script")
    det = tf.scan_text("<details>\nx\n")
    check("s46 `<details>` stays type 6, not the type-7 catch-all",
          det.kinds[0] == "tag-block")
    # A FOUR-COLUMN INDENT IS CODE, NOT HTML -- the oracle's own
    # `is_code_block` guard, which type 7 inherits with every other row.
    check("s46 a 4-space-indented complete tag opens no block",
          not tf.scan_text("    <custom-widget>\nplain\n").mask[0])
    # The projection seam: structural readers and the table formatter must not
    # see inside, the reflow lint must.
    pr = tf.scan_text("<custom-widget>\nprose inside\n\n## 1. Real\n")
    check("s46 the structural mask HIDES a type-7 block", pr.mask[1])
    # BOTH projections hide type 7, unlike type 6 -- see `PROSE_VISIBLE_KINDS`.
    # The rewriter is the one consumer that would look inside, and it can
    # destroy a complete-tag opener, so it is not shown one.
    check("s46 the PROSE mask hides it too, so the rewriter never sees it",
          pr.prose_mask()[1])
    _pd = tf.scan_text("<details>\nprose inside\n\n## 1. Real\n")
    check("s46 CONTROL: type 6 IS prose-visible, so the two kinds differ here",
          _pd.mask[1] and not _pd.prose_mask()[1])

    # ---- CONTAINERS, FENCES AND EOF. The root-level fixtures above do not
    # reach any of these, and the EOF one asserted only `terminal is None` --
    # which is ALSO true when the feature is reverted, because then no block
    # opens at all. Every assertion here names the kind or the mask, so a
    # revert fails it (Codex test-coverage, section 46, [medium]). ----
    eof = tf.scan_text("<custom-widget>\nx\n")
    check("s46 a type-7 block running to EOF is masked AND never a terminal",
          eof.terminal is None and eof.kinds[0] == "complete-tag"
          and eof.mask[0] and eof.mask[1])
    for label, doc, hidden, shown in (
            ("in a list item", "- <x>\n  ## 99. F\n\n- next\n", (0, 1), (3,)),
            ("in a blockquote", "> <x>\n> ## 99. F\n\n## 1. R\n", (0, 1), (3,)),
            ("after a container marker ends a paragraph",
             "para\n- <x>\n  ## 99. F\n", (1, 2), (0,)),
            ("under-indented blank ends it",
             "- <x>\n  ## 99. F\n\n## 1. R\n", (0, 1), (3,))):
        rr = tf.scan_text(doc)
        check("s46 type 7 %s: the block is hidden" % label,
              all(rr.mask[i] for i in hidden))
        check("s46 type 7 %s: real structure after it is visible" % label,
              all(not rr.mask[i] for i in shown))
    fenced = tf.scan_text("```\n<custom-widget>\n```\n## 1. R\n")
    check("s46 a complete tag INSIDE a fence stays fence, not type 7",
          fenced.kinds[1] == "fence" and not fenced.mask[3])
    check("s46 a bare `<` and an empty line open nothing",
          not any(tf.scan_text("<\nplain\n").mask)
          and not any(tf.scan_text("\n\n").mask))
    check("s46 a final tag with no trailing newline still opens a block",
          tf.scan_text("prose\n\n<custom-widget>").kinds[-1] == "complete-tag")

    # ---- THE TWO CONSUMERS, RUN FOR REAL. The mask/prose_mask assertions
    # above only inspect the projection; they never invoke either tool, so a
    # wiring regression could make `format-md-tables.py` rewrite literal pipe
    # rows or make `todo-reflow.py` ignore type-7 prose with every other
    # section-46 assertion still green (Codex test-coverage, section 46,
    # [high]). The formatter half lives beside its own suite in
    # test_format_md_tables.py; the reflow half is here. ----
    rf = load("todo_reflow_s46", "scripts/todo-reflow.py")
    # THE OPENER MUST BE IN `todo-reflow.py`'s OWN 78-138 HARD-WRAP BAND, or
    # this fixture proves nothing. A short `<custom-widget>` can never enter a
    # run, so an earlier version of this test concluded the projection was
    # inert -- a conclusion true only of that fixture. At 95 characters the
    # opener is a valid complete tag AND a plausible wrapped line, and the
    # rewriter joins it into the paragraph, destroying the block and
    # republishing the `## 99.` and `- [x]` it was hiding (Codex
    # re-adversarial, section 46, [high]).
    _long_open = '<custom-widget data-x="' + "a" * 70 + '">'
    _hidden_struct = "## 99. Fake\n- [x] Fake item\n"
    _corrupt = (_long_open + "\n" + "\n".join(_WRAPPED_BODY[:3]) + "\n"
                + _hidden_struct)
    check("s46 the in-band opener really is a complete tag and in the band",
          cs.is_complete_tag_line(_long_open) and 78 <= len(_long_open) <= 138)
    _after = tf.scan_text(rf.reflow(_corrupt))
    check("s46 the REWRITER may not touch a type-7 block at all",
          rf.reflow(_corrupt) == _corrupt)
    check("s46 ...so the structure it hides stays hidden after a reflow",
          _after.kinds[0] == "complete-tag"
          and all(_after.mask[i] for i, l in enumerate(_after.lines)
                  if l.startswith("## 99.") or l.startswith("- [x] Fake")))
    # CONTROL: with type 7 prose-VISIBLE, this same document is corrupted --
    # which is what the assertions above are protecting against, and what makes
    # them a real gate rather than a restatement of the current code.
    _vis = [not (c in cs._PROSE_HIDDEN_CODES
                 or c == cs._CODE_BY_KIND["complete-tag"])
            for c in tf.scan_text(_corrupt).codes]
    check("s46 CONTROL: prose-VISIBLE type 7 lets the rewriter destroy it",
          rf.reflow(_corrupt, vmask=_vis) != _corrupt)
    # Type 6 stays visible and that is deliberate: its opener is name-anchored,
    # so the same join leaves the block open and hides the same structure.
    _t6 = ('<details data-x="' + "a" * 76 + '">\n'
           + "\n".join(_WRAPPED_BODY[:3]) + "\n" + _hidden_struct)
    _t6r = tf.scan_text(rf.reflow(_t6))
    check("s46 type 6 remains prose-visible, and survives the join it invites",
          _t6r.kinds[0] == "tag-block"
          and all(_t6r.mask[i] for i, l in enumerate(_t6r.lines)
                  if l.startswith("## 99.") or l.startswith("- [x] Fake")))

    # Accepted forms that only the equivalence proof reached: each must also
    # OPEN the scan-level row, which the proof says nothing about. `<x a=!>`
    # is the only behavioural input that exercises the `_T_UNQ` class.
    for word in ("</a  >", '<x a="v"/>', "<x  a>", "<x a/>", "<x a  =v>",
                 "<x a />", "<x a >", "<x a=!>"):
        rw = tf.scan_text(word + "\nbody\n\n## 1. R\n")
        check("s46 %r opens a type-7 block through the scan" % word,
              rw.kinds[0] == "complete-tag" and rw.mask[1]
              and not rw.mask[3])

    # ---- Oracle mask parity on generated type-7 fixtures. ----
    try:
        from markdown_it import MarkdownIt
    except ImportError:
        _SKIPS.append("section 46 fixture parity (markdown-it-py not installed)")
        return
    md = MarkdownIt("commonmark")
    for body in ("<custom-widget>", "</custom-widget>", "<x a=1 b='2' c=\"3\">",
                 "<x a=1/>", "<x a=1>", "<x a= >", "<x> ",
                 "<x a=b/c>", "<custom-widget attr>"):
        for lead in ("", "para\n"):
            doc = lead + body + "\nhidden line\n\n## 1. Real\n"
            ours = tf.scan_text(doc)
            want = set()
            for tok in md.parse(doc):
                if tok.type == "html_block" and tok.map:
                    want.update(range(tok.map[0], tok.map[1]))
            got = {i for i, k in enumerate(ours.kinds)
                   if k in cs.ALL_HIDDEN_KINDS}
            # markdown-it includes the terminating blank in a block's map; the
            # scan deliberately leaves that blank unmasked so an enclosing
            # container still closes on it, so compare the NON-blank lines.
            want = {i for i in want if ours.lines[i].strip(" \t")}
            check("s46 mask parity with markdown-it-py on %r (lead=%r)"
                  % (body, lead), want == got)


def test_html_blocks_section42():
    """CommonMark HTML blocks 1-7, the terminal contract, and the projections."""
    tf = load("todo_fence_s42", "scripts/todo_fence.py")
    # THE SHIM'S OWN MODULE OBJECT, not a second load of the same source. A
    # separate `load()` here produced a DIFFERENT `UnclosedDocument` class, so
    # `except` missed the exception the shim actually raises -- the exact
    # two-module-objects hazard `todo_fence._cache_schema()` is written to avoid.
    cs42 = tf._cache_schema()

    # ---- Types 1-5: EOF-consuming. Balanced closes, unterminated REFUSES. ----
    # Each pair is (opener, closer). The BALANCED document must publish its real
    # heading; the UNTERMINATED one must produce a terminal naming that exact
    # construct rather than a silently erased document.
    for opener, closer, kind, num in (
            ("<script>", "</script>", "script", 1),
            ("<?php", "?>", "pi", 3),
            ("<!DOCTYPE html>", None, "declaration", 4),
            ("<![CDATA[", "]]>", "cdata", 5),
            ("<!--", "-->", "comment", 2)):
        if closer is None:
            # A declaration ends on `>`, which its own opener already carries.
            balanced = f"# T\n\n{opener}\n\n## 1. Real\n"
        else:
            balanced = f"# T\n\n{opener}\n## 99. Fake\n{closer}\n\n## 1. Real\n"
        rb = tf.scan_text(balanced)
        check(f"type {num}: a balanced block closes and leaves no terminal",
              rb.terminal is None)
        check(f"type {num}: the real heading after a balanced block is visible",
              any(l.startswith("## 1.") and not rb.mask[i]
                  for i, l in enumerate(rb.lines)))
        if closer is not None:
            check(f"type {num}: a heading INSIDE the block is masked",
                  all(rb.mask[i] for i, l in enumerate(rb.lines)
                      if l.startswith("## 99.")))
        unterminated = f"# T\n\n{opener.replace('>', '') if closer is None else opener}\n## 99. Fake\n"
        ru = tf.scan_text(unterminated)
        check(f"type {num}: an unterminated block reports its own kind",
              ru.terminal is not None and ru.terminal.kind == kind
              and ru.terminal.number == num)
        # NAMES THE CONSTRUCT, and this assertion used to be vacuous: it was
        # `kind in msg OR "fenced" not in msg`, whose right half is true for
        # every HTML message, so the left half was never actually required
        # (Codex consistency, section 42, [low]).
        _msg = ru.unclosed_reason()
        check(f"type {num}: the refusal message names the construct",
              kind in _msg and "fenced code block" not in _msg
              and f"type {num}" in _msg)
        # CONTROL: the erasure this refusal exists to prevent is real -- every
        # line past the opener is masked, so without the terminal the document
        # would publish as an empty node rather than as a refusal.
        check(f"type {num}: CONTROL -- the unterminated block does mask to EOF",
              all(ru.mask[i] for i in range(3, len(ru.lines) - 1)))
        # `require_closed` is the projection the mutating tools take.
        try:
            ru.require_closed()
            check(f"type {num}: require_closed raises on an unterminated doc", False)
        except cs42.UnclosedDocument:
            check(f"type {num}: require_closed raises on an unterminated doc", True)

    # ---- Types 6 and 7: blank-line terminated, so NEVER a terminal. ----
    for tag, num in (("details", 6),):
        doc = f"# T\n\n<{tag}>\n## 99. Fake\n- [x] Fake item\n\n## 1. Real\n"
        r = tf.scan_text(doc)
        check(f"type {num}: `<{tag}>` hides the heading inside it",
              all(r.mask[i] for i, l in enumerate(r.lines)
                  if l.startswith("## 99.") or l.startswith("- [x] Fake")))
        check(f"type {num}: the block ends at the blank line, not at EOF",
              any(l.startswith("## 1.") and not r.mask[i]
                  for i, l in enumerate(r.lines)))
        check(f"type {num}: running to EOF is well-formed, never a terminal",
              tf.scan_text(f"<{tag}>\nx\n").terminal is None)
        # THE BLANK LINE ITSELF MUST NOT BE MASKED -- it is the same blank that
        # closes an enclosing list item, so swallowing it would keep a container
        # alive past its end.
        blank_idx = [i for i, l in enumerate(r.lines) if l == ""]
        check(f"type {num}: the terminating blank line stays unmasked",
              all(not r.mask[i] for i in blank_idx))

    # TYPE 7 SHIPPED IN SECTION 46, and this probe was rewritten rather than
    # deleted. It asserted "an unknown tag name opens NO block" and would still
    # PASS unchanged -- for a completely different reason (type 7 cannot
    # interrupt a paragraph) -- which is the shape that turns a real control
    # into a tautology. Both reasons are now pinned separately.
    rp = tf.scan_text("some prose\n<custom-widget>\nstill prose\n")
    check("type 7 does NOT interrupt an open paragraph", not any(rp.mask))
    r7 = tf.scan_text("<custom-widget>\nhidden\n\n## 1. Real\n")
    check("type 7: an unknown tag name DOES open a block outside a paragraph",
          r7.mask[0] and r7.mask[1] and not r7.mask[3])
    # The replacement control for "type 6 is name-driven": type 6 matches on
    # the NAME and needs no complete tag, type 7 needs a complete tag and
    # accepts any name. An unclosed unknown tag is therefore matched by
    # neither, which is what keeps the two rules distinguishable.
    r6 = tf.scan_text("some prose\n<details>\nhidden\n")
    check("type 6: a KNOWN block name does interrupt a paragraph",
          r6.mask[1])
    check("type 6 needs no `>`: `<details` alone still opens a block",
          tf.scan_text("<details\nhidden\n").mask[0])
    check("...and `<custom-widget` (no `>`) opens neither 6 nor 7",
          not any(tf.scan_text("<custom-widget\nplain\n").mask))
    # NBSP IS NOT A BLANK LINE. `str.strip()` says it is, which ended a
    # `<details>` block early and leaked the heading and item after it.
    # ...at EVERY place the rule is asked. Fixing only the HTML branch MOVED
    # the bug into the container phase rather than closing it: each shape below
    # leaked real structure until `_is_blank` became the single predicate.
    leak = tf.scan_text("<details>\n\u00a0\n## 99. Fake\n- [x] leaked\n\n")
    check("a lone NBSP does NOT terminate a type-6 block (root)",
          all(leak.mask[:4]))
    nested = tf.scan_text("- item\n  <details>\n  \u00a0\n  ## 99. Fake\n"
                          "  - [x] leaked\n")
    check("...nor a list-CONTAINED one, where the container phase asks first",
          all(nested.mask[1:5]))
    # `- ` + NBSP is a list item with CONTENT, not an empty marker. Reading it
    # as empty refused to open the container, so a nested block outlived a
    # later outdented heading and that section's work was re-parented.
    dash = tf.scan_text("para\n- \u00a0\n  <details>\n  hidden\n## 2. Real\n")
    check("a `- ` + NBSP marker opens a real list container",
          dash.mask[2] and dash.mask[3] and not dash.mask[4])

    # ---- The projection seam (design review [high]). ----
    doc = "# T\n\n<details>\nprose inside\n\n## 1. Real\n"
    r = tf.scan_text(doc)
    prose = r.prose_mask()
    check("structural mask HIDES a type-6 block",
          r.mask[2] and r.mask[3])
    check("prose mask SHOWS it, so the reflow lint still checks that prose",
          not prose[2] and not prose[3])
    check("the two projections agree about a FENCE (only type 6 differs)",
          [m for m in tf.scan_text("```\nx\n```\n").prose_mask()]
          == [m for m in tf.scan_text("```\nx\n```\n").mask])

    # ---- The positional-unpack guard (design review [high]). ----
    try:
        _a, _b, _c = tf.scan_text("x\n")
        check("a stale 3-tuple unpack fails LOUDLY rather than silently", False)
    except TypeError:
        check("a stale 3-tuple unpack fails LOUDLY rather than silently", True)

    # ---- Container-stripped section headings (item 7, CLOSED in section 45). ----
    B = load("build_s42", "scripts/todo-graph/build.py")
    # Section 42 left this hole open ON PURPOSE, because closing it for the
    # heading walk alone made that walk disagree with `_walk_stamped_items` and
    # an indented `[x]` landed under the PREVIOUS section. Section 45 closed it
    # the only way that is an improvement: the WHOLE closure takes the same
    # projection, so both walks see section 2 and the item is filed under it.
    # markdown-it-py reads the same three headings from this document.
    doc = "# T\n\n## 1. Root\n\n- item\n\n    ## 2. Nested\n\n    - [x] Nested item\n"
    sc = B.scan_body(doc)
    heads = [n for n, _ in B._walk_section_headings(sc.leaf_views, sc.mask, sc.in_blockquote)]
    check("the heading walk SEES a container-indented section (section 45)",
          heads == [1, 2])
    items = B._walk_stamped_items(sc.views, sc.leaf_views, sc.mask, "t.md",
                                  sc.in_blockquote)
    check("...and no item is attributed to a section the heading walk denies",
          all(i["section_n"] in heads for i in items))
    check("...the nested item is filed under the nested section, not the previous",
          [i["section_n"] for i in items] == [2])
    # The MEMORY invariant the reverted projection broke is still pinned: no
    # per-line stripped STRING array is retained. `conts` is one packed byte per
    # line and `views` derives the rest, returning `lines` itself untouched when
    # no line carries a container prefix.
    check("ScanResult retains no per-line `stripped` string array",
          not hasattr(sc, "stripped")
          and "stripped" not in cs42.ScanResult.__slots__)
    check("...the projection costs one byte per line",
          isinstance(sc.conts, bytearray) and len(sc.conts) == len(sc.lines))
    _flat = B.scan_body("## 1. A\n\n- [x] `f()` x\n\n## 2. B\n")
    check("...and a container-free document shares `lines` rather than copying",
          _flat.views is _flat.lines)

    # ---- An OVER-LONG section number still DELIMITS a section. ----
    # Section 42 bounded the digit run to `\d{1,9}` to stop the `int()` crash
    # (CPython refuses a conversion over 4,300 digits) and REVERTED it: bounding
    # the match makes the heading stop being a heading, so every item after it
    # is silently attributed to the PREVIOUS section. This pins the property
    # that was chosen instead -- the heading still delimits -- so a future
    # re-bound has to fail here rather than land quietly. Matching AND reporting
    # is section 43's work, across all four copies of the grammar.
    over = "## 1. First\n\n- [x] `a()` one\n\n## 12345678901. Over\n\n- [x] `b()` two\n"
    os_ = B.scan_body(over)
    check("an over-long section number is still MATCHED as a heading",
          all(cs42.SECTION_HEADING_RE.match(l)
              for l in os_.lines if l.startswith("## ")))
    # SECTION 43 STRENGTHENED THE OUTCOME, not the property. This used to expect
    # the second item under section 12345678901 -- which the heading walk had
    # matched but the cache schema pins to 0-65535, so the producer was emitting
    # a node that its own validator would reject. The property being defended is
    # unchanged and still checked first: the item must not be re-parented to
    # section 1. What changed is that an unrepresentable number now records
    # NOTHING and carries an error, instead of recording a value that cannot
    # survive validation.
    over_items = B._walk_stamped_items(os_.views, os_.leaf_views, os_.mask,
                                       "t.md", os_.in_blockquote)
    check("...so the item after it is NOT re-parented to the previous section",
          [i["section_n"] for i in over_items] == [1])
    check("...and the node carries an error rather than dropping it silently",
          [e[0] for e in B._walk_unusable_headings(os_.leaf_views, os_.mask, os_.in_blockquote)]
          == ["unusable-heading"])

    _corpus_differential(tf, cs42)


def _corpus_differential(tf, cs42):
    """The markdown-it-py differential over the LIVE corpus, as a test.

    PERSISTED HERE BECAUSE SECTION 39's WAS NOT. That section's acceptance ran
    a generated differential of ~5.5M documents ad hoc, and nothing kept it --
    so its headline result (two residual type-6 files) could only be re-derived
    by rebuilding the harness from scratch, which is what section 42 had to do.
    A measurement that gates a section belongs in the suite that guards it.

    Section 42's own result is that the residual set is EMPTY: every line of
    every corpus file now agrees with the oracle about fenced code and HTML
    blocks. That is asserted POSITIVELY rather than as "the differential stayed
    nil", because going from two divergent files to zero is the change this
    section exists to make (Codex design review, [medium]).
    """
    try:
        from markdown_it import MarkdownIt
    except ImportError:
        # A MISSING ORACLE IS NOT A PASS. The first version of this called
        # `check(..., True)`, which counted a green assertion on a host that
        # never ran the comparison -- the gate then "passed" precisely because
        # it could not run, which is the same fail-silent shape the scanner
        # itself is built to refuse (Codex consistency, section 42, [medium]).
        # Recorded as a SKIP that the summary prints and that no assertion
        # count absorbs.
        _SKIPS.append("corpus differential vs markdown-it-py "
                      "(markdown-it-py not installed)")
        return
    md = MarkdownIt("commonmark")
    divergent = []
    for p in sorted((REPO / "todo").rglob("*.md")):
        text = cs42.normalize_newlines(p.read_text(encoding="utf-8"))
        r = cs42.scan_text(text)
        n = len(r.lines)
        oracle = [False] * n
        for t in md.parse(text):
            if t.type in ("fence", "html_block") and t.map:
                for i in range(t.map[0], min(t.map[1], n)):
                    oracle[i] = True
        ours = [bool(k) and k in cs42.ALL_HIDDEN_KINDS for k in r.kinds]
        if oracle != ours[:n]:
            divergent.append(p.name)
    check("corpus differential vs markdown-it-py: the residual set is EMPTY "
          f"(divergent files: {divergent[:3]})", not divergent)


def test_container_phase_section45():
    """Section 45: the container phase gets a cost model, an indent check, and
    ONE section context shared by the whole producer closure.

    Every masking expectation below was taken from `markdown-it-py` before it
    was written down, and the oracle is then re-run against those literals when
    it is importable. Both halves matter: the literals gate on a host with no
    oracle installed, and the oracle run proves the literals were not invented.
    """
    tf = load("todo_fence_s45", "scripts/todo_fence.py")
    cs = tf._cache_schema()
    B = load("build_s45", "scripts/todo-graph/build.py")

    _CONT_CUT = cs._CONT_CUT_MASK
    _CONT_BQ = cs._CONT_BQ

    def hidden(text):
        r = cs.scan_text(text)
        return [bool(k) and k in cs.ALL_HIDDEN_KINDS for k in r.kinds]

    def oracle(text, n):
        """markdown-it's fence/html_block line set, or None when unavailable."""
        try:
            from markdown_it import MarkdownIt
        except ImportError:
            return None
        out = [False] * n
        for t in MarkdownIt("commonmark").parse(text):
            if t.type in ("fence", "html_block") and t.map:
                for i in range(t.map[0], min(t.map[1], n)):
                    out[i] = True
        return out

    _oracle_ran = [0]

    def agrees(label, text, expected):
        got = hidden(text)[:len(expected)]
        check("s45 %s" % label, got == expected)
        o = oracle(text, len(expected))
        if o is None:
            return
        _oracle_ran[0] += 1
        check("s45 %s -- markdown-it agrees with that literal" % label,
              o == expected)

    # ---- The blank-line branch is no longer quadratic. ----
    # `"- " * N + "x"` opens N list containers and the N blank lines under it
    # each re-matched every one of them: 0.082s at 1,000 levels, 0.435s at
    # 2,000 and 1.372s at 4,000 -- 3 KB to 12 KB of input, far below the 16 MiB
    # per-TODO ceiling, stalling `build.py` and every migrated gate.
    def scan_secs(n):
        src = "- " * n + "x\n" + "\n" * n
        t0 = time.monotonic()
        cs.scan_text(src)
        return time.monotonic() - t0

    t1k, t4k = scan_secs(1000), scan_secs(4000)
    # RATIO, NOT AN ABSOLUTE, for the shape: 4x the nesting and 4x the blank
    # lines is 16x the work when quadratic and ~4x when linear. The bound is
    # generous (8x) because this runs on a loaded CI box, and it still fails by
    # a wide margin against the measured 16.7x regression.
    check("s45 blank-line matching scales near-linearly, not quadratically "
          "(4k/1k = %.1fx)" % (t4k / t1k if t1k else 0),
          t1k > 0 and t4k / t1k < 8)
    # ...and the absolute bound the section promised, which a uniformly slow
    # box would otherwise satisfy by making the ratio look fine.
    check("s45 a 4,000-level document scans in under 0.5s (%.3fs)" % t4k,
          t4k < 0.5)

    # ---- An under-indented blank ends a substring-terminated HTML block. ----
    # PRE-EXISTING and verified so: the same divergence reproduces with `<!--`
    # at `f3ea34fd6`, before section 42 widened the reach from one HTML type to
    # five. Left alone it ERASES -- the producer emitted headings 1 and 3, no
    # stamped item, and NO terminal error, because the next unindented line
    # closes the block by container exit.
    for opener in ("<!-- c", "<?pi", "<!DOC", "<![CDATA[x", "<script>"):
        src = ("## 1. Root\n- a\n  %s\n \n  ## 2. Real\n  - [x] `f()` Item\n"
               "## 3. Third\n" % opener)
        agrees("an under-indented blank ends a %r block" % opener,
               src, [False, False, True, False, False, False, False])
    # THE CONTROL THAT DATES THE DEFECT. `<!--` was already a container-alive
    # HTML leaf before section 42, so its agreeing here is what shows section 45
    # repaired a rule rather than section 42 having broken one.
    src = "## 1. Root\n- a\n  <!-- c\n \n  ## 2. Real\n  - [x] `f()` Item\n## 3. T\n"
    agrees("...and the `<!--` control, which predates section 42",
           src, [False, False, True, False, False, False, False])

    # ---- The four falsifiers for that scope. ----
    # A blank indented TO the content indent stays INSIDE the block.
    agrees("a blank at the content indent stays inside the block",
           "## 1. R\n- a\n  <?pi\n  \n  ## 2. X\n  ?>\n",
           [False, False, True, True, True, True])
    # A root-level type 1-5 block has blkIndent 0, so no blank is under it.
    agrees("a root-level block still survives a blank line",
           "## 1. R\n<?pi\n\n## 2. X\n?>\n## 3. T\n",
           [False, True, True, True, True, False])
    # A FENCE in the identical shape is NOT affected -- markdown-it keeps it
    # alive across the under-indented blank, so widening the rule to fences
    # would be a divergence, not a fix.
    agrees("an under-indented blank does NOT close a list-contained fence",
           "## 1. R\n- a\n  ```\n\n  ## 2. F\n  ```\n## 3. T\n",
           [False, False, True, True, True, True, False])
    # A bare `>` inside a blockquote-contained block leaves `rest` empty while
    # the LINE is not blank; guarding on `rest` would close the block here.
    agrees("a bare `>` inside a blockquote-contained block is not a blank",
           "## 1. R\n> <?pi\n>\n> ## 2. X\n> ?>\n",
           [False, True, True, True, True])

    # ---- The blank-line fast path equals the walk it replaced, exhaustively. ----
    # `min(first_unfilled, first_bq)` is the same answer only while both indices
    # hold their exact sentinels, and the failure is silent -- an index-valued
    # absent-blockquote sentinel closes every ordinary list on the next blank
    # line (Codex design review, [high]). So this is a DIFFERENTIAL against a
    # literal transcription of the pre-section-45 loop over every container
    # stack up to depth 4, rather than a handful of documents that happen to
    # exercise some of them.
    def reference_blank_walk(containers):
        """The pre-section-45 loop, blank-line path only."""
        matched = 0
        for kind, _arg, filled in containers:
            if kind == "bq":
                break            # `^ {0,3}>` cannot match a blank line
            if not filled:
                break
            matched += 1
        return matched

    kinds = (("li", 2, True), ("li", 2, False), ("bq", 0, True))
    stacks = [()]
    for _ in range(4):
        stacks += [s + (k,) for s in stacks for k in kinds]
    disagreed = []
    for stack in stacks:
        containers = [list(k) for k in stack]
        # The maintained invariant, spelled out: slot 0 is the first container
        # that is not yet filled, slot 1 the first blockquote, each `_NO_BQ`-ish
        # past the end when absent.
        first_unfilled = next((i for i, c in enumerate(containers) if not c[2]),
                              len(containers))
        first_bq = next((i for i, c in enumerate(containers) if c[0] == "bq"),
                        cs._NO_BQ)
        for blank in ("", " ", "   ", "\t", " \t "):
            got = cs._match_containers(containers, blank,
                                       [first_unfilled, first_bq])
            want = reference_blank_walk(containers)
            if got[0] != want:
                disagreed.append((stack, blank, got[0], want))
    check("s45 the O(1) blank path matches the stack walk over all %d stacks "
          "to depth 4 (%r)" % (len(stacks), disagreed[:2]), not disagreed)
    check("s45 ...and that sweep actually covered blockquote and provisional "
          "stops rather than only filled lists",
          any(reference_blank_walk([list(k) for k in s]) < len(s)
              for s in stacks))
    # END-TO-END, because the differential above pins the function while the
    # maintenance lives in its two callers. Each document below leaves the
    # indices in a different state -- fresh, post-filled-push, post-truncation,
    # blockquote-below-list, provisional -- and none of them opens a fence or an
    # HTML block, so a mis-matched container surfaces as a spurious mask.
    for label, src in (
            ("a blank on a fresh stack", "\n- a\n\n  b\n"),
            ("a blank under a filled list", "- a\n\n  b\n"),
            ("a blank after the stack truncated", "- a\nroot\n- b\n\n  c\n"),
            ("a blockquote below a filled list", "- a\n\n  > q\n\n  x\n"),
            ("a provisional item", "- \n\n  x\n"),
    ):
        check("s45 %s: nothing is masked" % label, not any(cs.scan_text(src).mask))

    # ---- The projection is lossless; there is NO physical-line fallback. ----
    # A TAB straddling the list-indent cut cannot be expressed as a character
    # offset, and falling back to the physical line would hide the heading while
    # the PURE-SLICE item under it stayed visible -- filing shipped work under
    # the previous section, which is exactly the misattribution section 42
    # reverted a narrower change to avoid.
    doc = "## 1. Root\n- a\n\t## 2. Real\n  - [x] `f()` Item\n"
    sc = B.scan_body(doc)
    check("s45 a tab-straddling prefix escapes into the sparse map, not a "
          "physical fallback",
          sc.views[2] == "  ## 2. Real" and 2 in sc.cont_exc)
    check("s45 ...so the heading is seen", cs.classify_heading(sc.views[2]).kind == "ok")
    heads = [n for n, _ in B._walk_section_headings(sc.leaf_views, sc.mask, sc.in_blockquote)]
    items = B._walk_stamped_items(sc.views, sc.leaf_views, sc.mask, "t.md",
                                  sc.in_blockquote)
    check("s45 ...and the item under it is filed under section 2, not section 1",
          heads == [1, 2] and [i["section_n"] for i in items] == [2])
    check("s45 ...while the physical line it replaced is not a heading at all",
          cs.classify_heading(sc.lines[2]).kind == "none")
    # ACROSS THE CORPUS the escape is exercised and still lossless. Measured
    # 2026-08-12: exactly 2 lines need it, both a tab inside a list-contained
    # Makefile example. The property pinned is that an escaped line resolves to
    # its STORED remainder and never to the physical line -- a count would break
    # the next time a TODO gains a tab, which is not a defect.
    _esc = 0
    for _p in sorted((REPO / "todo").rglob("*.md")):
        _sc = B.scan_body(_p.read_text(encoding="utf-8"))
        for _i, _rest in _sc.cont_exc.items():
            _esc += 1
            if _sc.views[_i] != _rest or _sc.views[_i] == _sc.lines[_i]:
                _esc = -10000
    check("s45 every corpus escape resolves to its stored remainder, never to "
          "the physical line (%d escaped lines)" % _esc, _esc >= 0)

    # ---- All FIVE walks share the projection, including the refusal. ----
    # FOUR spaces, not two: `classify_heading` accepts 0-3 leading spaces on a
    # physical line, so a 2-space indent would be visible to the old producer
    # too and the control below would prove nothing.
    doc = ("# T\n\n## 1. Root\n\n- item\n\n    ## 2. Nested\n\n"
           "    - [x] `g()` Nested\n")
    sc = B.scan_body(doc)
    check("s45 the heading walk sees the list-contained section",
          [n for n, _ in B._walk_section_headings(sc.leaf_views, sc.mask, sc.in_blockquote)] == [1, 2])
    check("s45 ...and the item walk files its indented item under THAT section",
          [i["section_n"]
           for i in B._walk_stamped_items(sc.views, sc.leaf_views, sc.mask,
                                          "t.md", sc.in_blockquote)] == [2])
    # THE CONTROL. Handed the PHYSICAL lines -- the pre-section-45 producer --
    # the same document attributes the item to section 1. That divergence is
    # what the shared projection removes.
    check("s45 ...where the physical-line producer attributes it to section 1",
          [n for n, _ in B._walk_section_headings(sc.lines, sc.mask, lambda _i: False)] == [1]
          and [i["section_n"]
               for i in B._walk_stamped_items(sc.lines, sc.lines, sc.mask,
                                              "t.md", lambda _i: False)] == [1])
    # THE FIFTH WALK. An over-long number inside a container is a heading the
    # other four now see; leaving this one on physical lines would drop the
    # section AND its error, which is the silent narrowing the refusal exists
    # to prevent.
    over = ("## 1. First\n\n- item\n\n    ## 12345678901. Over\n\n"
            "    - [x] `h()` two\n")
    os_ = B.scan_body(over)
    check("s45 a list-contained over-long heading is REFUSED, not dropped",
          [e[0] for e in B._walk_unusable_headings(os_.leaf_views, os_.mask, os_.in_blockquote)]
          == ["unusable-heading"])
    check("s45 ...and the item under it is not re-parented to section 1",
          [i["section_n"]
           for i in B._walk_stamped_items(os_.views, os_.leaf_views, os_.mask,
                                          "t.md", os_.in_blockquote)] == [])
    check("s45 ...while the physical-line walk would have missed the refusal",
          B._walk_unusable_headings(os_.lines, os_.mask, lambda _i: False) == [])

    # ---- The blockquote rejection survives losing the `>` marker. ----
    # `_walk_stamped_items` used to get this free from the `>` surviving in the
    # text it matched; on the stripped view the marker is gone, so a stamp
    # continuation carrying `[x]` would read as a shipped item.
    bq = "## 1. Root\n\n> **Verified:** 2026-01-01\n> - [x] `k()` not an item\n"
    sc = B.scan_body(bq)
    check("s45 a blockquoted `[x]` is still rejected after container stripping",
          B._walk_stamped_items(sc.views, sc.leaf_views, sc.mask, "t.md",
                                sc.in_blockquote) == [])
    check("s45 ...and the scan is what says so, not the surviving marker",
          sc.views[3] == "- [x] `k()` not an item" and sc.in_blockquote(3))

    # ---- A heading sharing the container-OPENING line (adversarial, [high]). ----
    # `views` keeps the marker a line opens, because the item walk matches on
    # it. That makes `- ## 2. Nested` -- a real h2 to CommonMark -- invisible to
    # `classify_heading`, so the item under it files against section 1, and the
    # over-long form recorded NO `unusable-heading` at all, failing open. The
    # leaf projection is the second view that closes both.
    doc = "## 1. Root\n- ## 2. Nested\n  - [x] `same_line_sym()` shipped\n"
    sc = B.scan_body(doc)
    check("s45 the leaf view consumes a marker the line itself opens",
          sc.leaf_views[1] == "## 2. Nested"
          and sc.views[1] == "- ## 2. Nested")
    check("s45 ...so a same-line container heading is a section",
          [n for n, _ in B._walk_section_headings(sc.leaf_views, sc.mask, sc.in_blockquote)]
          == [1, 2])
    check("s45 ...and its item is filed under it",
          [i["section_n"]
           for i in B._walk_stamped_items(sc.views, sc.leaf_views, sc.mask,
                                          "t.md", sc.in_blockquote)] == [2])
    # THE CONTENT MATCHER MUST NOT TAKE THE LEAF VIEW. `- [x] shipped` leafs to
    # `[x] shipped`, which the item regex refuses -- feeding one projection to
    # both questions loses every checklist item in a list.
    check("s45 ...while the leaf view of an item line drops its bullet",
          sc.leaf_views[2] == "[x] `same_line_sym()` shipped")
    over = "## 1. Root\n- ## 12345678901. Over\n  - [x] `s()` x\n"
    so = B.scan_body(over)
    check("s45 a same-line OVER-LONG heading now fails closed",
          [e[0] for e in B._walk_unusable_headings(so.leaf_views, so.mask, so.in_blockquote)]
          == ["unusable-heading"]
          and B._walk_unusable_headings(so.lines, so.mask, lambda _i: False) == [])
    check("s45 ...and takes its item with it rather than re-parenting",
          [i["section_n"]
           for i in B._walk_stamped_items(so.views, so.leaf_views, so.mask,
                                          "t.md", so.in_blockquote)] == [])
    _flat2 = B.scan_body("## 1. A\n\n## 2. B\n")
    check("s45 a document opening no container allocates no leaf array",
          _flat2.leafs is None and _flat2.leaf_views is _flat2.views)

    # ---- The packed-cut boundary at the escape value (test-coverage, [medium]). ----
    # Cuts 0-126 are inline and 127 is the escape, so a container prefix landing
    # exactly on the boundary is where an off-by-one silently swaps a real
    # offset for "consult the sparse map" -- or the reverse, which resolves to
    # the wrong slice of a valid line.
    for cut in (124, 126, 128, 130):
        # The containers must be OPEN from an earlier line for the prefix to be
        # MATCHED rather than opened, so line 1 nests `cut/2` list levels with
        # content and line 2 continues them at exactly `cut` columns.
        depth = cut // 2
        src = ("## 1. Root\n" + "- " * depth + "x\n"
               + " " * cut + "## 2. Nested\n")
        r = cs.scan_text(src)
        check("s45 a container prefix of %d characters projects exactly" % cut,
              r.views[2] == "## 2. Nested"
              and len(r.lines[2]) - len(r.views[2]) == cut)
        packed = r.conts[2] & _CONT_CUT
        check("s45 ...encoded inline below the escape and escaped at or above "
              "it (%d)" % cut,
              (packed == cut and 2 not in r.cont_exc) if cut < cs._CONT_ESCAPE
              else (packed == cs._CONT_ESCAPE and r.cont_exc[2] == "## 2. Nested"))
    # ...and the blockquote flag must survive the escape, since it shares the
    # byte: an escape that clobbered bit 7 would let a blockquoted `[x]` through.
    # A blockquote plus 64 list levels puts the MATCHED prefix at 130
    # characters, past the escape, on a line that is also a checklist item. If
    # the escape clobbered bit 7 the item would stop being blockquoted and a
    # stamp continuation would publish as shipped work.
    bqdeep = ("## 1. R\n> " + "- " * 64 + "x\n> " + " " * 128
              + "- [x] `q()` no\n")
    r = cs.scan_text(bqdeep)
    check("s45 the blockquote flag survives a cut past the escape value",
          (r.conts[2] & _CONT_CUT) == cs._CONT_ESCAPE and (r.conts[2] & _CONT_BQ)
          and r.cont_exc[2] == "- [x] `q()` no")
    _bqd = B.scan_body(bqdeep)
    check("s45 ...so the deep blockquoted item is still rejected",
          B._walk_stamped_items(_bqd.views, _bqd.leaf_views, _bqd.mask, "t.md",
                                _bqd.in_blockquote) == [])

    # ---- Lazy continuation, then the O(1) blank path (test-coverage, [medium]). ----
    # The lazy branch `continue`s BEFORE the truncation that clamps both stack
    # indices, so it is the one path that hands the fast blank path a stack it
    # did not just normalise. A regression here closes a list early and exposes
    # fenced content, or retains a blockquote and masks a real heading.
    agrees("a lazy continuation before a blank keeps the container alive",
           "## 1. R\n\n100. first\nlazy continuation\n\n     ```\n     - [ ] x\n"
           "     ```\n\n## 2. Real\n",
           [False, False, False, False, False, True, True, True, False, False])
    # The same lazy shape through a blockquote-in-list stack. Note the answer:
    # only the opener is masked, because the blank after it is under the list
    # content indent and now ENDS the block -- so `## 2.` is real structure.
    agrees("...and the same through a blockquote-in-list stack",
           "## 1. R\n\n- > quoted\n  > lazy\n\n  <?pi\n\n  ## 2. Hidden\n  ?>\n"
           "## 3. Real\n",
           [False, False, False, False, False, True, False, False, False, False])
    lazy = B.scan_body("## 1. R\n\n100. first\nlazy continuation\n\n"
                       "     ## 2. Nested\n\n     - [x] `lazy_sym()` shipped\n")
    check("s45 a heading after a lazy continuation is attributed correctly",
          [n for n, _ in B._walk_section_headings(lazy.leaf_views, lazy.mask, lazy.in_blockquote)]
          == [1, 2]
          and [i["section_n"]
               for i in B._walk_stamped_items(lazy.views, lazy.leaf_views,
                                              lazy.mask, "t.md",
                                              lazy.in_blockquote)] == [2])

    # ---- Blockquote-contained HTML exits on a physical blank (test-coverage). ----
    # A blank line cannot match `>`, so `matched` drops below `depth` and the
    # block must close on THAT branch, before the new under-indent comparison is
    # ever reached. The bare-`>` falsifier above keeps the blockquote matched
    # and so exercises the opposite path; this is the competing exit.
    agrees("a physical blank exits a blockquote-contained HTML block",
           "## 1. R\n> <?pi\n\n## 2. Real\n",
           [False, True, False, False])
    agrees("...and the same inside a list-plus-blockquote stack",
           "## 1. R\n- > <?pi\n\n## 2. Real\n",
           [False, True, False, False])
    bqx = B.scan_body("## 1. Root\n> <?pi\n\n## 2. Real\n\n- [x] `bq_sym()` shipped\n")
    check("s45 ...so the heading and item after it stay visible and attributed",
          [n for n, _ in B._walk_section_headings(bqx.leaf_views, bqx.mask, bqx.in_blockquote)]
          == [1, 2]
          and [i["section_n"]
               for i in B._walk_stamped_items(bqx.views, bqx.leaf_views,
                                              bqx.mask, "t.md",
                                              bqx.in_blockquote)] == [2])

    # ---- A blockquote is a QUOTED EXAMPLE, and stripping its marker must not
    # turn one into graph data (adversarial round 2, both [high]). ----
    # Every walk used to get this free from the `>` surviving in the physical
    # text it matched. The projections delete the marker, so the guarantee
    # inverted in three places at once, and the ONE predicate that restores it
    # has to see a blockquote the line OPENS -- the matched-container flag
    # cannot, which is exactly the quoted-heading case below.
    _S = "§"          # the section sign, built rather than written inline
    _XR = "-> XREF: [`TODO-99`](../09-x/TODO-99-y.md) %s1" % _S
    _ROW = "| :-: | :-: | - | - | :-: |\n|  1  | %s1 | %%s | -- | [x] |" % _S
    _HDR = "| Order | Section | Deliverable | Depends On | Status |"
    for label, doc, want in (
            ("a quoted Inputs block emits no dependency edge",
             "## 1. R\n\n> ## Inputs\n> - %s\n" % _XR,
             {"heads": [1], "items": [], "xrefs": 0, "io": 0}),
            ("a quoted heading is not a section and reparents nothing",
             "## 1. Real\n\n> ## 2. Quoted\n\n- [x] `actual()` shipped\n",
             {"heads": [1], "items": [1], "xrefs": 0, "io": 0}),
            ("a quoted Implementation Order table emits no rows",
             "## 1. R\n\n> ## Implementation Order\n>\n> %s\n> %s\n"
             % (_HDR, (_ROW % "Fake").replace("\n", "\n> ")),
             {"heads": [1], "items": [], "xrefs": 0, "io": 0}),
            # CONTROLS. The same constructs UNQUOTED must still land, or the
            # guard above would be indistinguishable from deleting the walks.
            ("CONTROL: a real Inputs block still emits its edge",
             "## Inputs\n\n- %s\n\n## 1. R\n" % _XR,
             {"heads": [1], "items": [], "xrefs": 1, "io": 0}),
            ("CONTROL: a real Implementation Order table still emits its row",
             "## Implementation Order\n\n%s\n%s\n" % (_HDR, _ROW % "Real"),
             {"heads": [], "items": [], "xrefs": 0, "io": 1}),
    ):
        sc = B.scan_body(doc)
        bq = sc.in_blockquote
        got = {
            "heads": [n for n, _ in
                      B._walk_section_headings(sc.leaf_views, sc.mask, bq)],
            "items": [i["section_n"] for i in
                      B._walk_stamped_items(sc.views, sc.leaf_views, sc.mask,
                                            "t.md", bq)],
            "xrefs": len(B._walk_inputs_xrefs(sc.views, sc.leaf_views, sc.mask,
                                              bq)),
            "io": len(B._walk_implementation_order(sc.views, sc.leaf_views,
                                                   sc.mask, bq)),
        }
        check("s45 %s (%r)" % (label, got), got == want)
    # The predicate must be LEAF-level. A blockquote opened on the same line is
    # invisible to the matched packing, which is what let the quoted heading
    # through in the first place.
    _q = B.scan_body("## 1. Real\n\n> ## 2. Quoted\n")
    check("s45 the blockquote predicate sees a marker the line itself opens",
          _q.in_blockquote(2))
    # ...and a LAZY continuation of a quoted paragraph is inside it too, though
    # it carries no `>` for either the packing or the old physical-line walks to
    # see. PRE-EXISTING -- the base producer emits the same edge -- but the one
    # predicate has to be true everywhere or it is not one predicate.
    for label, doc, walk, want in (
            ("a lazy blockquote XREF row emits no edge",
             "## Inputs\n\n> quoted paragraph\n| %s | example |\n" % _XR,
             "xrefs", 0),
            ("a lazy blockquote IO row emits no row",
             "## Implementation Order\n\n> quoted paragraph\n%s\n"
             % (_ROW % "Fake").splitlines()[-1], "io", 0),
            ("CONTROL: the same row unquoted still lands",
             "## Inputs\n\n| %s | real |\n" % _XR, "xrefs", 1),
    ):
        sc = B.scan_body(doc)
        bq = sc.in_blockquote
        got = (len(B._walk_inputs_xrefs(sc.views, sc.leaf_views, sc.mask, bq))
               if walk == "xrefs" else
               len(B._walk_implementation_order(sc.views, sc.leaf_views,
                                                sc.mask, bq)))
        check("s45 %s (%d)" % (label, got), got == want)

    # ---- The projection's memory budget, measured rather than argued. ----
    # An `array("i")` of offsets plus a separate flag array was the first design
    # and would add 83,886,206 bytes at the 16 MiB valid-input ceiling; one
    # packed byte per line adds 16,777,217 (Codex design review, [medium]).
    # MEASURED at the real ceilings on 2026-08-12: peak RSS 312,288 KB before
    # and 328,456 KB after on a 16 MiB newline-only document, +15.8 MB / +5.2%,
    # with `views is lines` throughout. Those runs take ~13s each, so what the
    # SUITE pins is the per-line constant they extrapolate from, at a size a
    # commit hook can afford.
    n = 200_000
    tracemalloc.start()
    _r = cs.scan_text("\n" * n)
    _peak = tracemalloc.get_traced_memory()[1]
    tracemalloc.stop()
    _views_shared = _r.views is _r.lines
    _cost = len(_r.conts)
    del _r
    check("s45 the projection costs one byte per line (%d for %d lines)"
          % (_cost, n + 1), _cost == n + 1)
    check("s45 ...and a newline-only document allocates no view at all",
          _views_shared)
    # The whole scan of a newline-only document stays within a few times the
    # line array itself; an offset array per line would show up here.
    check("s45 ...so peak traced allocation stays proportionate (%.1f MB)"
          % (_peak / 1e6), _peak < 60_000_000)

    # ---- The projection picks its representation by DENSITY. ----
    # The identity short-circuit alone requires EVERY byte to be zero, so one
    # container line anywhere forced two document-sized pointer lists: a 4 MiB
    # blank document projected to 106 MB peak RSS and the same document with
    # `- x` / `  y` appended to 180 MB, linear amplification that approaches
    # 640 MB at the 16 MiB ceiling. The newline-only case could never show it,
    # which is why the acceptance shape here is a LATE OPENER (Codex perf,
    # section 45 post-ship, [high]).
    _n = 200_000
    _cases = (
        ("no container at all -> `lines` itself", "\n" * _n, "list", True),
        ("one late opener -> a sparse overlay", "\n" * _n + "- x\n  y\n",
         "_Projection", False),
        ("a real TODO file -> the dense list", None, "list", False),
    )
    for label, src, want_type, want_identity in _cases:
        if src is None:
            r = B.scan_body((REPO / "todo/00-infrastructure"
                             / "TODO-06-todo-metadata-layer.md")
                            .read_text(encoding="utf-8"))
        else:
            r = cs.scan_text(src)
        v, lv = r.views, r.leaf_views
        check("s45 %s (%s)" % (label, type(v).__name__),
              type(v).__name__ == want_type
              and (v is r.lines) == want_identity)
        # WHICHEVER representation is chosen, the ANSWER is the same. An
        # overlay that indexed or iterated differently from the dense list
        # would be a silent producer divergence rather than a memory win.
        check("s45 ...and it indexes, iterates and measures like a list",
              len(v) == len(r.lines) and list(v) == [v[i] for i in range(len(v))]
              and len(lv) == len(r.lines))
        del r, v, lv
    # The overlay must carry the container-stripped line, not the physical one.
    _ov = cs.scan_text("\n" * _n + "- x\n  y\n")
    check("s45 the overlay carries the stripped remainder, not the raw line",
          _ov.views[-2] == "  y" and _ov.leaf_views[-2] == "  y"
          and _ov.views[0] == "")
    del _ov

    if not _oracle_ran[0]:
        _SKIPS.append("section 45 oracle agreement (markdown-it-py not installed)")


def test_raw_html_is_not_rewritten_section48():
    """The REWRITER may not move the literal tag bytes it is deliberately shown.

    Type 6 is prose-visible so `todo-reflow.py` can lint the 30 live `<details>`
    prose lines under `todo/`; that same visibility let it JOIN an in-band
    opening tag into the paragraph below. The fix is a BOUNDARY, not a hide, so
    every fixture here asserts the EXACT expected output: the markup lines
    byte-identical AND the prose between them joined into one. An assertion set
    proving only the first would be satisfied by hiding type 6 entirely, which
    is the option this section rejected.

    THE EXPECTED OUTPUT IS WRITTEN OUT, NEVER DERIVED FROM THE MASK. An earlier
    version of this test built its expectation by calling
    `prose_html_boundary_mask()` -- the function under test -- so a line the
    mask stopped classifying simply vanished from the expectation and the
    assertion still passed (Codex test-coverage, section 48, [high]).
    """
    tf = load("todo_fence_s48", "scripts/todo_fence.py")
    rf = load("todo_reflow_s48", "scripts/todo-reflow.py")

    B = ["payload line one that is long enough to look like a wrapped prose line at about ninety co",
         "payload line two that is long enough to look like a wrapped prose line at about ninety co",
         "payload line three that is long enough to look like a wrapped prose line at ninety cols o"]
    body, joined = "\n".join(B), " ".join(B)
    # IN THE TOOL'S OWN 78-138 HARD-WRAP BAND, or the fixture proves nothing --
    # the section-46 lesson, where a 15-character opener could never enter a run
    # and the projection was therefore wrongly called inert.
    opener = '<details data-x="' + "a" * 76 + '">'
    cont = 'data-x="' + "a" * 70 + '">'
    check("s48 the fixture opener and continuation are inside the hard-wrap band",
          78 <= len(opener) <= 138 and 78 <= len(cont) <= 138
          and all(78 <= len(x) <= 138 for x in B))

    # Each case is (name, document, EXACT expected output).
    cases = [
        ("in-band opening tag",
         opener + "\n" + body + "\n",
         opener + "\n" + joined + "\n"),
        ("trailing closing tag",
         opener + "\n" + body + "\n</details>\n",
         opener + "\n" + joined + "\n</details>\n"),
        # A type-6 opener is NAME-anchored and needs no `>` on its own line, so
        # a tag spans a line ending in both directions. Protecting only the
        # opener would leave this continuation as the first line of a fresh run
        # (Codex design review, section 48, [high]).
        ("opening tag split across lines",
         "<details\n" + cont + "\n" + body + "\n",
         "<details\n" + cont + "\n" + joined + "\n"),
        ("closing tag split across lines",
         "<details>\n" + body + "\n</details\n>\n",
         "<details>\n" + joined + "\n</details\n>\n"),
        # A quoted attribute value spanning a newline, containing a `>` that is
        # NOT the terminator. Without quote state the tag ends early and the
        # rest of the value is reflowed as prose.
        ('attribute value spanning a newline (double-quoted)',
         '<details data-x="a > b' + "a" * 55 + '\nvalue continues here '
         + "b" * 55 + '">\n' + body + "\n</details>\n",
         '<details data-x="a > b' + "a" * 55 + '\nvalue continues here '
         + "b" * 55 + '">\n' + joined + "\n</details>\n"),
        ("attribute value spanning a newline (single-quoted)",
         "<details data-x='a > b" + "a" * 55 + "\nvalue continues here "
         + "b" * 55 + "'>\n" + body + "\n</details>\n",
         "<details data-x='a > b" + "a" * 55 + "\nvalue continues here "
         + "b" * 55 + "'>\n" + joined + "\n</details>\n"),
    ]
    # Constructs whose terminator is NOT `>`. Each carries an early `>` before
    # its real terminator, which a generic tag carry ends on -- leaving the rest
    # of the construct classified as prose and joined. The CDATA form was
    # byte-identical BEFORE this projection existed, so shipping the generic
    # terminator would have been a regression, not merely a gap (Codex
    # adversarial [medium] + test-coverage [high], section 48).
    # THE CONSTRUCT BODY IS INDENTED so it forms a run the tool would really
    # join. With the body at the same indent as a short `<details>` opener, the
    # 9-character opener falls in `_hard_wrapped`'s width window and the whole
    # document is protected by the WIDTH TEST rather than by this projection --
    # so the fixture passes against a tool that never learned about constructs
    # at all. The paired false-mask control below is what caught that.
    ibody = "\n".join("  " + l for l in B)
    for name, open_tag, close_tag in (
        ("comment", "<!-- a > b", "-->"),
        ("CDATA section", "<![CDATA[literal > continues", "]]>"),
        ("processing instruction", "<?php $a > $b", "?>"),
    ):
        doc = ("<details>\n" + open_tag + "\n" + ibody + "\n" + close_tag
               + "\n</details>\n")
        cases.append(("nested " + name + " (terminator is not `>`)", doc, doc))
    # Nested raw-text elements. Their CONTENT is never prose: joining three
    # in-band script lines puts the first line's `//` comment in front of every
    # statement after it (Codex adversarial, section 48, [high]).
    #
    # THESE LINES MUST BE IN THE 78-138 BAND. The first version of this fixture
    # used 74-76-character JavaScript, BELOW `LO=78`, so it could never enter a
    # run and the four raw-text assertions passed against a tool with no
    # raw-text handling at all -- the section-46 short-opener mistake repeated
    # inside its own regression test (Codex consistency, section 48, [high]).
    js = ["  var averylongidentifier = somefunction(a, b, c) + anotherfunction(d, e, f); // x",
          "  var bverylongidentifier = somefunction(a, b, c) + anotherfunction(d, e, f); ok1",
          "  var cverylongidentifier = somefunction(a, b, c) + anotherfunction(d, e, f); ok2"]
    check("s48 the raw-text fixture lines are inside the hard-wrap band",
          all(78 <= len(x) <= 138 for x in js))
    for el, inner in (("script", "\n".join(js)), ("pre", ibody),
                      ("style", ibody), ("textarea", ibody)):
        doc = ("<div>\n<" + el + ">\n" + inner + "\n</" + el + ">\n</div>\n")
        cases.append(("nested <" + el + "> content", doc, doc))
    # The three raw-text OPENER shapes that a first-match-wins scan got wrong,
    # each reproduced: a DIFFERENT element's closing tag inside a string
    # cancelled the opener, a second unmatched opener after a matched pair was
    # ignored, and an opener preceded by ordinary text was skipped entirely
    # because the scan was gated on the line already being markup (Codex
    # adversarial [high] + consistency [high], section 48).
    for name, opener in (
        ('a different element\'s closer in a string',
         '<script>const m = "</style>";'),
        ("a second unmatched opener", "<script></script><script>"),
        ("an opener preceded by text", "leading prose text here <script>"),
        # The opening TAG may itself span a line ending, and the element only
        # opens once it closes. Holding that pending name in a local rather
        # than in the carried state meant the line that closed the tag had
        # forgotten which element it belonged to (Codex re-adversarial,
        # section 48, [high]).
        ("a multiline opening tag", '<script\n type="text/javascript">'),
    ):
        doc = "<div>\n" + opener + "\n" + "\n".join(js) + "\n</script>\n</div>\n"
        cases.append(("raw-text opener: " + name, doc, doc))
    # A CLOSER MUST BE SYNTACTICALLY COMPLETE. Ordinary script text that merely
    # STARTS like the closing tag ended protection early, so the in-band lines
    # after it joined (Codex re-adversarial, section 48, [high]).
    # PADDED INTO THE BAND: at 29 characters the line sits below `LO=78` and
    # poisons the run's width test, so the document is protected by
    # `_hard_wrapped` and the fixture passes against a tool with no closer
    # validation at all. The paired false-mask control is what exposes that.
    # `</script bogus>` is the shape that survived the FIRST repair: the name
    # boundary matched and the generic tag parser then accepted arbitrary text
    # before the `>`, ending raw mode on ordinary script text. Only whitespace
    # is admitted there now (Codex re-adversarial, section 48, [high]).
    for text in ('  const s = "</script bogus";', '  const s = "</script-safe>";',
                 '  const s = "</script bogus>";', '  const s = "</style>";'):
        padded = text + " // " + "x" * (82 - len(text) - 4)
        check("s48 the incomplete-closer fixture is inside the band",
              78 <= len(padded) <= 138)
        doc = ("<div>\n<script>\n" + padded + "\n" + "\n".join(js)
               + "\n</script>\n</div>\n")
        cases.append(("incomplete closer in script text: " + text.strip()[:24],
                      doc, doc))

    for name, doc, want in cases:
        got = rf.reflow(doc)
        check("s48 %s: exact expected output" % name, got == want)
        # PAIRED CONTROL ON EVERY CASE, including the byte-identical ones. A
        # mask of all-False brings the corruption straight back, so no
        # assertion above can be passing because the tool stopped reflowing --
        # and the raw-text and construct cases, whose expected output EQUALS
        # their input, are exactly the ones that had no control and were
        # therefore passing against a tool with no raw-text handling at all
        # (Codex consistency, section 48, [high]).
        check("s48 %s: CONTROL a false mask corrupts it" % name,
              rf.reflow(doc, hmask=[False] * len(doc.split("\n"))) != got)

    # ---- NEGATIVE SIDE: what must NOT be treated as markup. 24 of the 30 live
    # type-6 lines are ordinary prose, so over-masking silently ends the lint
    # this seam exists to provide. ----
    for name, needle in (("a bare `a < b`", "a < b"),
                         ("a non-ASCII letter after `<`", "<\u00e9lan"),
                         ("a `>=` comparison", ">= 30 min")):
        pr = [l[:-len(needle)] + needle for l in B]
        doc = "<details>\n" + "\n".join(pr) + "\n</details>\n"
        sc = tf.scan_text(doc)
        hm = sc.prose_html_boundary_mask()
        check("s48 %s is NOT classified as markup" % name, not any(hm[1:4]))
        check("s48 %s still joins into one line" % name,
              rf.reflow(doc) == "<details>\n" + " ".join(pr) + "\n</details>\n")

    # A raw-text tag INSIDE an open comment is not an opener. Recognising it as
    # one set `raw` while `carry` was still `-->`; every later line then took
    # the raw branch and never fed the terminator back, so BOTH states stuck
    # and the rest of the block froze as markup -- silently disabling the lint
    # this projection exists to provide (Codex re-adversarial, section 48,
    # [medium]; the mask came back all ones). The assertion is the NEGATIVE
    # one: the prose after the comment must still reflow.
    wedge = "<details>\n<!-- literal <script>\n-->\n" + ibody + "\n</details>\n"
    wedge_mask = tf.scan_text(wedge).prose_html_boundary_mask()
    check("s48 a `<script>` inside an open comment does not wedge the walk",
          not any(wedge_mask[3:6]))
    check("s48 ...so the prose after that comment still reflows, and the "
          "comment lines are untouched",
          rf.reflow(wedge) == ("<details>\n<!-- literal <script>\n-->\n  "
                               + " ".join(B) + "\n</details>\n"))

    # A CLOSING tag may span a line ending too. Requiring a complete
    # `</name\s*>` on one physical line meant `</script\n>` never cleared the
    # raw state, so every later line stayed marked and the prose after it
    # silently stopped being linted (Codex re-adversarial, section 48,
    # [medium]). The assertion is the NEGATIVE one -- the prose AFTER the
    # closer must reflow -- with the script body still untouched.
    mlc = ("<div>\n<script>\n" + "\n".join(js) + "\n</script\n>\n" + ibody
           + "\n</div>\n")
    mlc_out = rf.reflow(mlc)
    check("s48 a closing tag split across lines still clears the raw state",
          "  " + " ".join(B) in mlc_out)
    check("s48 ...while the script body stays byte-identical",
          all(j in mlc_out.split("\n") for j in js))

    # A STRAY raw-text CLOSING tag, outside raw mode and after ordinary text,
    # is still markup. The fast-path run excluded `<script>` but not
    # `</script>` -- the lookahead sat before the optional `/` -- so the run
    # swallowed it silently, the line was left unmasked, and the tag was joined
    # into the prose (Codex re-adversarial, section 48, [medium]).
    stray_line = ("leading prose text that is long enough to look like a "
                  "wrapped line <a></script>")
    stray = "<div>\n" + stray_line + "\n" + "\n".join(B[:2]) + "\n</div>\n"
    check("s48 a stray `</script>` after text and a skippable tag is markup",
          bool(tf.scan_text(stray).prose_html_boundary_mask()[1]))
    check("s48 ...so that line is never joined into the prose below it",
          rf.reflow(stray).split("\n")[1] == stray_line)

    # State may not LEAK between two separate prose-visible blocks: an element
    # left open when the block ends must not silently protect the next one, or
    # one malformed block would disable the lint for the rest of the document.
    leak = ("<div>\n<script>\n" + "\n".join(js) + "\n\n<details>\n"
            + "\n".join(B) + "\n</details>\n")
    check("s48 an unclosed raw element does not leak into the NEXT block",
          " ".join(B) in rf.reflow(leak))

    # The projection must agree with the repo's OWN html grammar about what a
    # construct is. A one-character `/!?` test accepted `</3` and `<!lowercase`,
    # which the type-7 NFA and the scanner's declaration rule both reject, so
    # the reflow silently skipped prose it exists to repair (Codex consistency,
    # section 48, [medium]). Both directions are pinned.
    for lead in ("</3 ", "<!lowercase "):
        pr = [lead + l[len(lead):] for l in B]
        doc = "<details>\n" + "\n".join(pr) + "\n</details>\n"
        check("s48 `%s` is NOT a construct and its prose still joins" % lead.strip(),
              not any(tf.scan_text(doc).prose_html_boundary_mask()[1:4])
              and rf.reflow(doc) == "<details>\n" + " ".join(pr) + "\n</details>\n")
    for good in ("</div>", "<!DOCTYPE html>", "<!-- c -->", "<?php ?>", "<a>"):
        doc = "<details>\n" + good + "\n</details>\n"
        check("s48 `%s` IS still recognised as a construct" % good,
              bool(tf.scan_text(doc).prose_html_boundary_mask()[1]))

    # A tag under a CONTAINER prefix is still markup: 2 of the 30 live lines are
    # `> <details>` in a blockquote, which does not start with `<` physically.
    bq = tf.scan_text("> <details>\n> body\n")
    check("s48 a blockquoted opener is seen as markup through the leaf view",
          bq.prose_html_boundary_mask()[0])

    # ---- COST. These helpers run from `todo-reflow.py` under lint Check 19
    # over all 281 files at every commit, and this file carries a 2s producer
    # budget a single pathological line has blown before. Measured on
    # `monotonic_ns` (a wall-clock bracket can be satisfied by a clock step --
    # section 34). A 16 MiB line dense with `<` cost 10.378s when every `<`
    # reached `_construct_at` (Codex perf, section 48, [high]). ----
    MiB16 = 16 * 1024 * 1024
    # The REJECTED-CANDIDATE shapes are here on purpose. With a loose
    # `<[/!?A-Za-z]` filter every grammar-invalid candidate still reached
    # `_construct_at`, was rejected, and advanced the walk one character: a
    # 16 MiB line of `</3` -- ordinary prose the tool must accept -- cost
    # 4.732s, over this file's 2s producer budget (Codex re-adversarial,
    # section 48, [medium]).
    # ASSERTED AS SCALING, NOT AS A WALL-CLOCK BOUND. An absolute threshold
    # here measures the machine as much as the code: the same dense-`<a>` case
    # took 1.71s in a fresh process and 3.115s inside this suite, so a 3s bound
    # failed on correct code. Ratio across a 4x size step separates the
    # property actually under test -- linear (~4x) against the per-construct
    # Python walk and the quadratic tail copy (~16x) that rounds 4-6 found --
    # and it cannot be satisfied or broken by ambient load. Section 34 made the
    # same correction for the todo-cache budgets.
    # MEASURED IN A FRESH INTERPRETER, and that is not fastidiousness. Building
    # several 16 MiB payloads inside THIS process makes allocation dominate:
    # the same dense-`<a>` case measured 1.71s standalone, 3.115s in-suite
    # against an absolute bound, and a 24.9x "superlinear" ratio in-suite while
    # the regex under test is ~linear in isolation (0.090s at 1 MiB to 1.901s
    # at 16 MiB). Both in-suite forms would have been read as code defects.
    # A subprocess measures the code; this process cannot.
    probe = r'''
import sys, time, gc
sys.path.insert(0, "scripts")
import todo_fence as tf
UNITS = [("dense `<`", "<"), ("dense `</3`", "</3"), ("dense `<!x`", "<!x"),
         ("dense `<a>`", "<a>"), ("dense `<!A>`", "<!A>"),
         ("ordinary text", "a"), ("dense quoted tag", '<a b="c">'),
         ("malformed raw closer", "</script bogus>")]
MiB = 1024 * 1024
# The malformed-closer payload MUST sit inside an open <script>, or `raw` is
# never set, the closing-tag-in-progress branch is never reached, and the case
# silently measures generic tag scanning instead of the quadratic fix.
WRAP = {"malformed raw closer": ("<div>\n<script>\n", "\n</script>\n</div>\n")}
for name, unit in UNITS:
    head, tail = WRAP.get(name, ("<details>\n", "\n</details>\n"))
    times = []
    # 2 -> 8 MiB, not 4 -> 16. The RATIO is what separates linear from
    # quadratic, and it is size-independent, so the smaller pair keeps the
    # discrimination at a quarter of the allocation. This suite shares a
    # machine with timing-sensitive harnesses in the same pack.
    for size in (2 * MiB, 8 * MiB):
        doc = head + unit * (size // len(unit)) + tail
        t0 = time.monotonic_ns()
        tf.scan_text(doc).prose_html_boundary_mask()
        times.append((time.monotonic_ns() - t0) / 1e9)
        del doc
        gc.collect()
    print("%s\t%.4f\t%.4f" % (name, times[0], times[1]))
'''
    proc = subprocess.run([sys.executable, "-c", probe], capture_output=True,
                          text=True, cwd=os.getcwd())
    check("s48 the scaling probe ran (rc %d)" % proc.returncode,
          proc.returncode == 0 and proc.stdout.strip() != "")
    for row in proc.stdout.strip().splitlines():
        name, small, big = row.split("\t")
        small, big = float(small), float(big)
        ratio = big / small if small > 0 else 0.0
        # Linear over a 4x size step is ~4x; the per-construct Python walk and
        # the quadratic tail copy that rounds 4-6 found were ~16x and worse.
        check("s48 %s scales linearly (%.1fx over a 4x size step, %.2fs)"
              % (name, ratio, big), ratio < 8.0)

    # The malformed-closer shape -- the quadratic one rounds 4-6 produced -- is
    # covered by the subprocess scaling probe above, which wraps that payload
    # in an open `<script>` so it actually reaches the closing-tag-in-progress
    # branch. An earlier in-process version compared 2x size steps against a
    # 4.0 ratio, which leaves no separation margin at all: quadratic work over
    # a 2x step IS ~4x (Codex re-adversarial, section 48, [medium]).

    # A document with NO prose-visible block -- 279 of the 281 live files -- must
    # not walk anything or materialise `leaf_views`, and the mask is a compact
    # bytearray rather than a pointer-per-line list. The dense version added
    # 130,932 KiB of peak RSS on a size-valid 16 MiB document (Codex perf,
    # section 48, [high]).
    plain = tf.scan_text("\n".join(B * 2000))
    t0 = time.monotonic_ns()
    m = plain.prose_html_boundary_mask()
    dt = (time.monotonic_ns() - t0) / 1e9
    check("s48 a document with no type-6 block short-circuits (%.4fs)" % dt,
          dt < 0.25 and isinstance(m, bytearray) and not any(m))

    # ---- THE ODD-BREAK REFUSAL, section 47's remedy applied to this tool. ----
    def run(argv, text):
        with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False,
                                         encoding="utf-8", newline="") as fh:
            fh.write(text)
            tmp = fh.name
        try:
            before = open(tmp, "rb").read()
            rc = subprocess.run([sys.executable, "scripts/todo-reflow.py"]
                                + argv + [tmp], capture_output=True, text=True)
            return rc, before, open(tmp, "rb").read()
        finally:
            os.unlink(tmp)

    # EVERY reachable separator, not a single sample: an omission from the
    # pattern is exactly what this is here to catch (Codex test-coverage,
    # section 48, [medium]).
    for name, ch in (("VT", "\v"), ("FF", "\f"), ("FS", "\x1c"),
                     ("GS", "\x1d"), ("RS", "\x1e"), ("NEL", "\x85"),
                     ("U+2028", "\u2028"), ("U+2029", "\u2029")):
        for mode in ("--check", "--write"):
            rc, before, after = run([mode], "intro\n\na" + ch + "b\n")
            check("s48 %s under %s is REFUSED rc 2" % (name, mode),
                  rc.returncode == 2 and "non-LF line break" in rc.stderr)
            check("s48 %s under %s leaves the bytes untouched" % (name, mode),
                  before == after)

    # A REAL lone-CR file, read back through `process()`. It must NOT be
    # refused: text mode normalises it before either coordinate system sees it,
    # so the CR half of the pattern is unreachable here. Asserting this on the
    # literal `"a\nb\n"` (as an earlier version did) tests nothing at all --
    # it would stay green if reading stopped normalising (Codex test-coverage,
    # section 48, [medium]).
    rc, before, after = run(["--check"], "intro\n\na\rb\n")
    check("s48 a REAL lone-CR file is not refused as a non-LF break",
          rc.returncode != 2 and "non-LF line break" not in rc.stderr)
    check("s48 ...and that file really did carry a CR byte on disk",
          b"\r" in before)

    # CONTROL -- the two coordinate systems really do disagree on an odd break,
    # which is what makes the refusal a fix rather than a ritual.
    check("s48 CONTROL split('\\n') and the scan disagree on a VT break",
          len("a\vb\n".split("\n")) != len(tf.scan_text("a\vb\n").lines))


_ASSERTED = [0]
_SKIPS = []
_check = check


def check(name, cond):                                   # noqa: F811
    _ASSERTED[0] += 1
    _check(name, cond)


if __name__ == "__main__":
    sys.exit(main())
