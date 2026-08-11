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
import os
import pathlib
import re
import subprocess
import sys
import tempfile
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
    lines, mask, uf, uc = tf.scan_text(FIXTURE)
    fenced = [l for i, l in enumerate(lines) if mask[i]]
    check("shim: the fenced example heading is masked",
          any(l.startswith("## 99.") for l in fenced))
    check("shim: the real heading is NOT masked",
          not any(l.startswith("## 1.") for l in fenced))
    check("shim: a balanced document reports no unclosed fence", not uf)
    check("shim: a balanced document reports no unclosed comment", not uc)
    check("shim: unclosed_reason is None when both flags are clear",
          tf.unclosed_reason(False, False) is None)
    check("shim: an unclosed FENCE names the fence",
          "fenced code block" in (tf.unclosed_reason(True, False) or ""))
    check("shim: an unclosed COMMENT names the comment, not the fence",
          "<!--" in (tf.unclosed_reason(False, True) or "")
          and "fenced code block" not in (tf.unclosed_reason(False, True) or ""))
    check("shim: the fence flag wins when both are set (one message, not two)",
          "fenced code block" in (tf.unclosed_reason(True, True) or ""))
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
    _, m2, uf2, _ = tf.scan_text(UNCLOSED_FENCE)
    check("shim: an unclosed fence is REPORTED, not just masked", uf2)
    check("shim: an unclosed fence masks through EOF", all(m2[2:]))
    _, _, uf3, uc3 = tf.scan_text(UNCLOSED_COMMENT)
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
    lines, mask, _, _ = tf.scan_text(FIXTURE)
    blind = [False] * len(lines)

    aware_secs = {n for n, _, _ in reach._sections(lines, mask)}
    blind_secs = {n for n, _, _ in reach._sections(lines, blind)}
    check("reachability: fence-aware sees only the real section",
          aware_secs == {1})
    check("reachability CONTROL: the fence-blind walk invents section 99",
          99 in blind_secs)

    # The fenced example row carries order 2 and section marker 99; the real
    # row carries order 1 and marker 1, so a fence-aware walk sees only {1}.
    check("reachability: fence-aware sees only the real table row",
          reach._io_rows(lines, mask) == {1})
    check("reachability CONTROL: the fence-blind walk invents the example row",
          reach._io_rows(lines, blind) == {1, 99})

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
    rl, rm, _, _ = tf.scan_text(reseq)
    got = reach._io_rows(rl, rm)
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
    ll, lm, _, _ = tf.scan_text(legacy)
    check("reachability: no Section column -> the order number is used",
          reach._io_rows(ll, lm) == {45})

    # A table with NO header row at all. Its single data row must still count:
    # treating "the first row after the heading" as a header positionally ate
    # it, which silently disarmed the cache-coverage refusal in test_build.sh's
    # routed-reader fixture -- a gate going quiet, found by that suite.
    headerless = "\n".join([
        "## Implementation Order",
        "",
        "| x | 1 | a thing | -- | [x] |",
        ""])
    hl, hm, _, _ = tf.scan_text(headerless)
    check("reachability: a header-less table still yields its data row",
          reach._io_rows(hl, hm) == {1})

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
    tl, tm, _, _ = tf.scan_text(titled)
    check("reachability: a Section column holding a TITLE falls back to the number",
          reach._io_rows(tl, tm) == {1})

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
    wl, wm, _, _ = tf.scan_text(twotables)
    check("reachability: a preceding legend table does not latch the column",
          41 in reach._io_rows(wl, wm))

    # A fenced `- [ ]` and a fenced stamp must not reach the body scans either.
    body = next(b for n, _, b in reach._sections(lines, mask) if n == 1)
    check("reachability: no fenced open item in the real section's body",
          not any("a fenced example item" in b for b in body))
    check("reachability: no fenced Deferred stamp in the real section's body",
          not any("a fenced example stamp" in b for b in body))
    # Fence-blind, the fenced heading TERMINATES the real section and the
    # example item is attributed to the phantom section 99 instead -- which is
    # the `no-io-row` verdict this gate used to raise over a code sample.
    blind_99 = next((b for n, _, b in reach._sections(lines, blind) if n == 99),
                    None)
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
    lines, mask, _, _ = tf.scan_text(FIXTURE)
    vis = ["" if mask[i] else l for i, l in enumerate(lines)]

    check("staged-check: fenced headings do not count toward the section cap",
          stg._section_total(vis) == 1)
    check("staged-check CONTROL: fence-blind, the fenced example counts too",
          stg._section_total(lines) == 2)

    added_all = [(i + 1, l) for i, l in enumerate(lines)]
    masked_nos = {i + 1 for i, m in enumerate(mask) if m}
    added_vis = [(n, t) for n, t in added_all if n not in masked_nos]
    check("staged-check: a fenced `## N.` is not an added section",
          [t for _, t in stg.added_sections(added_vis)]
          == ["## 1. The Real Section"])
    check("staged-check CONTROL: fence-blind, the fenced heading is one too",
          len(stg.added_sections(added_all)) == 2)
    check("staged-check: no fenced park is reported",
          stg._park_into_shipping_section(vis, added_vis) == [])

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
        mask_only = rf._fence.fence_scan(lines)[0]
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
        mask_only = rf._fence.fence_scan(lines)[0]
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

    blind = lambda t: (t.split("\n"), [False] * len(t.split("\n")), False, False)

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
        return tf.scan_text(src)[1]

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
          not tf.scan_text(src)[2])

    # THE TERMINAL FLAG STILL FIRES for a genuinely unclosed container fence,
    # which is what stops the container work from turning a loud refusal into a
    # silent erasure.
    src = "100. docs\n     ```\n     code\n"
    check("an unclosed container-indented fence sets unclosed_fence",
          tf.scan_text(src)[2])

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
    lines, mask, uf, uc = tf.scan_text(src)
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
    lines, mask, uf, uc = tf.scan_text("10.\n\n    ```\n    literal\n")
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
          tf.scan_text(crlf)[2] is False and cs.scan_text(crlf)[2] is False)

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
    lines, mask, uf, uc = tf.scan_text("- a\n  -\tb\n    \t```\n    \tliteral\n")
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
        lines, mask, uf, uc = tf.scan_text(doc)
        check(f"a tab after a {label} blockquote marker parses without inventing"
              f" a block", not any(mask) and not uf)

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
              tf.scan_text(doc)[2] is True)

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
    if _FAILS:
        sys.stderr.write("test_todo_fence FAIL (%d):\n  - %s\n"
                         % (len(_FAILS), "\n  - ".join(_FAILS)))
        return 1
    print("test_todo_fence OK (%d assertions)" % _ASSERTED[0])
    return 0


_ASSERTED = [0]
_check = check


def check(name, cond):                                   # noqa: F811
    _ASSERTED[0] += 1
    _check(name, cond)


if __name__ == "__main__":
    sys.exit(main())
