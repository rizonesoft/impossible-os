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


def main():
    test_shim()
    test_mask_text_cannot_join_lines()
    test_reachability()
    test_orphan_check()
    test_staged_check()
    test_lint_checks_10_11()
    test_lint_staged_scope()
    test_check24_counts_the_refusal()
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
