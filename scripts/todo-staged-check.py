#!/usr/bin/env python3
"""Commit-time backstop for TODO item length and prose wrapping.

WHY A SECOND LAYER EXISTS. `todo_item_line_length.py` is a PreToolUse hook and
it works -- it blocks an over-cap checklist item on Edit/Write, with a useful
message. It is simply invisible to the write path both the operator and the
unattended runner actually use most: a `python3 - <<'PY' ... PY` heredoc, or a
`python3 /tmp/rewrite.py`, executed through Bash. No Edit, no Write, no hook.

MEASURED 2026-07-28. Repo-wide there are ~1,407 checklist lead lines over the
250-char cap. Almost all predate the hook, but FIVE landed AFTER it shipped, in
exactly two commits, and both wrote TODO content through python scripts:

    8ac3c976  (operator)  290, 325, 337 chars
    c64bec7c  (runner)    350, 359 chars

So the cap is enforced against the careful path and unenforced against the
common one. Everything reaches a commit, which is why the backstop belongs
here.

ONLY ADDED LINES ARE JUDGED. The ~1,407 legacy violations are untouched: this
reads `git diff --cached` and considers only lines the commit ADDS. A cleanup
of the legacy set is a separate, deliberate piece of work; failing every commit
until it happens would just teach everyone to pass the opt-out.

Usage:
  todo-staged-check.py              check staged todo/**.md (pre-commit)
  todo-staged-check.py --selftest
Opt-out: SKIP_TODO_STAGED_CHECK=1
Stdlib only.
"""
from __future__ import annotations

import os
import re
import subprocess
import pathlib
import sys

CAP = 250
_ITEM = re.compile(r"^ *- \[[ xX/]\] ")


def _staged_todo_files():
    try:
        out = subprocess.run(
            ["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"],
            capture_output=True, text=True, check=False).stdout
    except OSError:
        return []
    return [f for f in out.split("\n")
            if f.strip().startswith("todo/") and f.strip().endswith(".md")]


def _fence():
    """The shared fence tracker (section 38), memoised on the function."""
    mod = getattr(_fence, "_mod", None)
    if mod is None:
        import importlib.util
        src = pathlib.Path(__file__).resolve().parent / "todo_fence.py"
        spec = importlib.util.spec_from_file_location("todo_fence", src)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _fence._mod = mod
    return mod


def _staged_snapshot():
    """`(docs, tree_id)` from `todo_fence.staged_docs`, or `(None, reason)`.

    THE SNAPSHOT HAS TO MATCH THE COORDINATES. `_added_lines` derives every line
    number from `git diff --cached`, i.e. from the INDEX, while every structural
    walk below used to read the WORKING TREE. Those are the same file only when
    nothing is partially staged; under partial staging an unstaged edit shifts
    every later line, so a staged coordinate indexes a different line -- and
    once a fence mask is derived from that snapshot, the mismatch also decides
    which lines count as fenced (Codex design review, section 38, [high]).

    THE ENUMERATION IS SHARED WITH `lint.sh`, deliberately. Each gate reading
    the index its own way is how they came to disagree about three real cases:
    git C-QUOTES a non-ASCII path in `--name-only` output so one gate never saw
    the file, an unmerged entry fell back to worktree bytes here and refused
    there, and an undecodable blob raised here and was skipped there. Two
    mechanisms for one invariant is exactly the drift the shared tracker exists
    to end (Codex consistency, section 38 review, [medium]).
    """
    try:
        return _fence().staged_docs(_git_root()), None
    except Exception as exc:                  # StagedSnapshotError or OSError
        return None, str(exc)


def _git_root():
    """The repo this gate is judging: the CWD one, not the script's own.

    `_staged_todo_files` and `_added_lines` both shell out without `-C`, so
    they already speak to the invoking repository -- a pre-commit hook runs at
    its root. Resolving the snapshot from `__file__` instead would read THIS
    checkout's index while the coordinates came from the caller's, which is the
    same generation mismatch one directory over (caught by this gate's own
    fixtures, which stage into a scratch repo).
    """
    return "."


def _post_image_lines(text):
    """`(lines, mask, unclosed_reason)` for one staged document."""
    scan = _fence().scan_text(text)
    return scan.lines, scan.mask, scan.unclosed_reason()


def _added_lines(path):
    """[(lineno_in_new_file, text)] for lines this commit ADDS."""
    try:
        diff = subprocess.run(["git", "diff", "--cached", "-U0", "--", path],
                              capture_output=True, text=True, check=False).stdout
    except OSError:
        return []
    out, lineno = [], 0
    for line in diff.split("\n"):
        m = re.match(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@", line)
        if m:
            lineno = int(m.group(1))
            continue
        if line.startswith("+++") or line.startswith("---"):
            continue
        if line.startswith("+"):
            out.append((lineno, line[1:]))
            lineno += 1
        elif not line.startswith("-"):
            lineno += 1
    return out


def over_cap(added):
    return [(n, len(t), t) for n, t in added if _ITEM.match(t) and len(t) > CAP]


def wrapped_block(added):
    """Apparent fill column if the ADDED lines look hard-wrapped, else 0.

    The wrap rule had the same PreToolUse-only bypass this file exists to close
    (todo_wrap_reminder never sees a python3-via-Bash write), so it is checked
    here too. WARN, not block, unlike the item cap: the cap is long-standing
    policy already blocked at Edit, whereas the wrap rule is new (2026-07-28)
    and a blocking gate on it could refuse the unattended runner's own TODO
    edits, where a wedge costs a night.
    """
    try:
        sys.path.insert(0, os.path.join(
            os.path.dirname(os.path.abspath(__file__))))
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "_todo_reflow", os.path.join(os.path.dirname(
                os.path.abspath(__file__)), "todo-reflow.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except Exception:
        return 0                      # fail-open: never break a commit on this
    # PROSE only. Feeding every added line to the detector made the RECOMMENDED
    # repair trip the warning: converting an over-cap body into indented
    # sub-bullets adds a run of similar-width `- ` lines, which is exactly the
    # narrow-band-of-widths signature `_hard_wrapped` looks for. Observed
    # 2026-07-30 on TODO-04 -- the commit that split the 4,865-char QEMU-reap
    # item warned "hard-wrapped at ~111 columns" about its own fix. A bullet is
    # never hard-wrapped prose, and `_classify` already knows that (bullets,
    # headings, tables, quotes and fences all classify as `struct`).
    #
    # prev_blank=False deliberately: in a diff the preceding line is unknown,
    # and False keeps an indented continuation line classified as prose (the
    # thing we DO want to wrap-check) rather than as an indented code block.
    block = [t for _, t in added if mod._classify(t, False) == "prose"]
    for i in range(len(block)):
        for j in range(i + 3, len(block) + 1):
            if mod._hard_wrapped(block[i:j]):
                return max(len(x.rstrip()) for x in block[i:j - 1])
    return 0



# OS COMPARISON LAST-COLUMN CAP (2026-08-08). The `Impossible OS` column is a
# CLAIM column, and it had grown paragraphs: cells reached 219 display columns
# and dragged whole rows past 400, which is unreadable in the raw source these
# files are read in. All 223 tables were repaired to <= 80 in one pass; this
# keeps them there.
#
# Enforced on ADDED lines, because the drift arrives one row at a time. A
# corpus-wide check would pass on the day a single over-cap row lands and only
# notice once someone re-measures; intersecting the over-cap cells with the
# lines this commit adds catches it at the commit that introduces it.
#
# 80 is the operator's number, chosen against the corpus: it is the p99 of that
# column (2,593 cells, p50 31, p90 61) so it binds on outliers without touching
# ordinary entries, and it is 20 narrower than the widest cell that prompted it.
#
# NOT a padding cap. `format-md-tables.py` was given a --max-pad once and it was
# reverted the same day: capping PADDING leaves an over-cap cell unpadded, so
# its pipe juts out and the column stops aligning. This caps CONTENT, which is
# the only version that both narrows the table and keeps it aligned.
OS_COMPARISON_CELL_CAP = 80


def _oscomp_over_cap(lines, added):
    """[(lineno, width, cell)] for OS Comparison claim cells this commit
    ADDS that exceed the cap. Takes the staged post-image (fenced lines already
    blanked) for table structure and intersects with the added line numbers, so
    legacy rows are never judged and a fenced example table is never judged at
    all."""
    try:
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "_fmt", str(pathlib.Path(__file__).resolve().parent / "format-md-tables.py"))
        fmt = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fmt)
    except Exception:
        return []                     # fail-open: never break a commit on this
    added_nos = {n for n, _ in added}
    out = []
    for start, end in fmt._find_tables(lines):
        rows = [fmt._split_row(l) for l in lines[start:end]]
        if "Impossible OS" not in " ".join(rows[0]):
            continue
        ncols = len(rows[0])
        for i, row in enumerate(rows):
            if i == 1 or len(row) != ncols:
                continue
            lineno = start + 1 + i
            if lineno not in added_nos:
                continue
            # EVERY claim column, not just the last (2026-08-09). The commit
            # gate mirrored lint Check 27, which scanned `row[-1]` only -- so
            # prose that would be refused in the Impossible OS column landed in
            # the Win11 or Linux one instead, and one such row pads every column
            # in its table (TODO-25 held at 358 columns for content needing 217).
            for col, cell in enumerate(row):
                if col == 0:
                    continue          # decorative glyph column
                w = fmt._dwidth(cell)
                if w > OS_COMPARISON_CELL_CAP:
                    out.append((lineno, w, cell))
    return out


# SECTION-COUNT CAP (2026-08-02). Enforced on GROWTH past the cap, not on
# existence above it, so a file already oversized can finish its outstanding
# work while being forbidden to absorb more.
#
# WHY THESE NUMBERS, measured across all 232 TODO files: median 9 sections,
# p90 16, and 32 for the largest file other than TODO-04 -- which had reached
# 63, roughly double the next and 7x the median. A cap of 99 was considered and
# rejected because it sits above every file in the repo, so it would never bind
# and the growth it exists to stop would continue unchecked.
#
# WHAT THIS MUST NOT DO. It must never tempt the run to SWALLOW a discovered
# gap to stay under a number. "Finishing the listed checklist items is not
# enough if the feature is still obviously incomplete" is the rule that makes
# this runner produce real completeness instead of checklist theatre; a cap
# that suppressed filing would trade a visible large file for invisible missing
# work, which is strictly worse. So the cap changes the DESTINATION of a new
# section, never the decision to file one: at the cap, the gap goes to the
# domain-correct TODO with a reciprocal XREF.
# RAISED 40 -> 50 (2026-08-10). The cap was calibrated 2026-08-02 against a
# corpus whose largest file OTHER than the usermode-test-framework TODO was 32
# sections; the todo-metadata-layer TODO is now 39, so the distribution moved and
# a 40 threshold fires constantly on a file where the honest answer to "does this
# belong in a domain-correct TODO?" is yes, it belongs here. A warning always
# answered the same way stops being read.
#
# The HARD cap deliberately stays at 60. The usermode-test-framework TODO sits at
# 62 -- over it, which is what stops it growing, enforced on growth rather than
# existence. Raising the hard cap would hand the one genuinely oversized file in
# the repo fresh slots, and CLAUDE.md already rejected a cap of 99 on the grounds
# that a number above every file can never bind.
SECTION_SOFT_CAP = 50
SECTION_HARD_CAP = 60
_SECTION_RE = re.compile(r"^## (\d+)\.")


def _section_total(lines):
    """Sections in the staged post-image, counting only UNFENCED headings.

    A fenced `## 99.` example used to count toward the cap, so a file could be
    pushed over the soft or hard cap by a code sample (section 38).
    """
    return sum(1 for line in lines if _SECTION_RE.match(line))


def added_sections(added):
    """New `## N.` headings this commit introduces.

    `_added_lines` yields (lineno, text) pairs -- NOT the (n, len, text) triples
    that `over_cap` produces. Getting that wrong is why the first version of
    this raised IndexError on every call.
    """
    return [(lineno, text.strip()) for lineno, text in added
            if _SECTION_RE.match(text)]


# PARK-INTO-A-SHIPPING-SECTION (2026-08-09). A `- [/]` park is the sanctioned
# way to file work into a section that is ALREADY closed -- the owner-side
# stranded sweep is its re-open path. It is NOT a way to dispose of a review
# finding in the section you are stamping right now, and that is exactly when
# review findings arrive.
#
# MEASURED, which is why this is a refusal and not advice: `stranded_deferrals`
# reports 61 stranded candidates, and 60 of them "sit in DONE-parked sections
# (fixpoint never re-visits them -> will NOT flip naturally)". Three are marked
# `clean` -- the work was finished and nobody flipped the box. A park whose
# parent is stamped in the same commit joins that pile by construction.
#
# The author always has better options at that moment: fix it and record an
# `- [x]` item, file it in another component's OPEN section, or decide it fails
# the user-impact test and do not file it. All three keep the work reachable.
_PARK_RE = re.compile(r"^\s*- \[/\]")
_STAMP_RE = re.compile(r"^>\s*\*\*(Verified|Quality reviewed):\*\*")


def _park_into_shipping_section(lines, added):
    """[(lineno, section_heading, text)] for `- [/]` items this commit ADDS to a
    section it is ALSO stamping in the same commit.

    `lines` is the staged post-image with fenced lines blanked, so a fenced
    `> **Verified:**` cannot make a section look stamped-now and a fenced
    `- [/]` cannot look like a park (section 38).
    """
    added_nos = {n for n, _ in added}
    # section index -> (start, end) over the post-image
    bounds, cur, heads = [], None, {}
    for i, ln in enumerate(lines, 1):
        if _SECTION_RE.match(ln.strip()):
            if cur is not None:
                bounds.append((cur, i - 1))
            cur = i
            heads[i] = ln.strip()[:70]
    if cur is not None:
        bounds.append((cur, len(lines)))
    out = []
    for start, end in bounds:
        rng = range(start, end + 1)
        stamped_now = any(n in added_nos and _STAMP_RE.match(lines[n - 1])
                          for n in rng if n - 1 < len(lines))
        if not stamped_now:
            continue
        for n in rng:
            if n in added_nos and n - 1 < len(lines) and _PARK_RE.match(lines[n - 1]):
                out.append((n, heads.get(start, "?"), lines[n - 1].strip()[:90]))
    return out


# PROVENANCE ON EVERY NEW SECTION (2026-08-09). The spawn-chain sensor counts
# consecutive `(review)` links and demands an accountable waiver past three --
# but it can only count sections that DECLARE where they came from, and nothing
# required the declaration. Measured: 20 markers across 2,410 sections, all of
# them in one file, because that is where the run happened to be when the sensor
# shipped. An undeclared section is a root at depth 0, so the limit never fires
# and the gate binds only while someone keeps a habit.
#
# `(root)` is a legal answer and costs nothing at the verdict -- it contributes
# 0 depth exactly like silence did. The difference is that "nothing spawned
# this" becomes a claim someone made, instead of the default you get by writing
# no line at all.
#
# EXEMPT: a brand-new TODO file. Every section in a file this commit CREATES is
# a root by construction, and making `create-todo` stamp ten identical `(root)`
# lines on a scaffold is ceremony, not accountability.
_SPAWNED_BY_RE = re.compile(
    r"^>\s*\*\*Spawned-by:\*\*\s*(?:root\b|(?:\u00a7|section\s*)\d+\s*\((?:split|review)\))",
    re.I)


# USER IMPACT ON A REVIEW-SPAWNED SECTION (2026-08-09). Provenance made the
# cascade countable; this makes each link ANSWER FOR ITSELF at the moment it is
# created, which is the only moment the answer is cheap.
#
# Scoped to `(review)` deliberately. A `root` is a capability someone set out to
# build and a `(split)` is work already justified being partitioned -- neither is
# the shape that runs away. A section created FROM A REVIEW FINDING is, and the
# question that separates a worthwhile one from refinement is always the same:
# what does a user hit if this is not done?
#
# THE CONTENT IS NOT JUDGED, on purpose. "Nothing today; the parser miscounts
# only if a TODO ever fences a heading example" is a legitimate and useful
# answer -- and it is exactly the answer that talks its author out of creating
# the section. Demanding a WEIGHTY impact would teach people to invent one,
# which is strictly worse than an honest "nothing". The requirement is that the
# sentence gets written where a reviewer will read it.
#
# The worked example is the section that prompted this: a real defect in the
# section parser, surfaced by another section's test fixture, with ZERO live
# occurrences in 232 TODO files.
_USER_IMPACT_RE = re.compile(r"^>\s*\*\*User impact:\*\*\s*\S", re.I)
_REVIEW_SPAWN_RE = re.compile(
    r"^>\s*\*\*Spawned-by:\*\*\s*(?:\u00a7|section\s*)\d+\s*\(review\)", re.I)


def _review_sections_missing_user_impact(path, lines, added):
    """[(lineno, heading)] for `(review)`-spawned sections this commit ADDS that
    carry no `> **User impact:**` line."""
    if _file_is_new(path):
        return []
    added_nos = {n for n, _ in added}
    starts = [i for i, ln in enumerate(lines, 1) if _SECTION_RE.match(ln.strip())]
    out = []
    for idx, start in enumerate(starts):
        if start not in added_nos:
            continue
        end = starts[idx + 1] - 1 if idx + 1 < len(starts) else len(lines)
        body = lines[start:end]
        if not any(_REVIEW_SPAWN_RE.match(b) for b in body):
            continue                   # root or split: not the runaway shape
        if not any(_USER_IMPACT_RE.match(b) for b in body):
            out.append((start, lines[start - 1].strip()[:70]))
    return out


def _file_is_new(path):
    """True when this commit CREATES the file (no HEAD blob)."""
    try:
        r = subprocess.run(["git", "cat-file", "-e", f"HEAD:{path}"],
                           capture_output=True)
        return r.returncode != 0
    except Exception:
        return False


def _sections_missing_provenance(path, lines, added):
    """[(lineno, heading)] for `## N.` sections this commit ADDS that carry no
    `> **Spawned-by:**` line."""
    if _file_is_new(path):
        return []
    added_nos = {n for n, _ in added}
    starts = [i for i, ln in enumerate(lines, 1) if _SECTION_RE.match(ln.strip())]
    out = []
    for idx, start in enumerate(starts):
        if start not in added_nos:
            continue                   # pre-existing section: not this commit's
        end = starts[idx + 1] - 1 if idx + 1 < len(starts) else len(lines)
        body = lines[start:end]
        if not any(_SPAWNED_BY_RE.match(b) for b in body):
            out.append((start, lines[start - 1].strip()[:70]))
    return out


def main(argv) -> int:
    if "--selftest" in argv:
        return _selftest()
    if os.environ.get("SKIP_TODO_STAGED_CHECK") == "1":
        return 0
    files = _staged_todo_files()
    if not files:
        return 0
    # ONE index snapshot for the whole run, generation-pinned. Every structural
    # walk below reads from it, and `_added_lines`'s coordinates are re-bound to
    # the same generation afterwards -- two git invocations against a moving
    # index is how a mask from one generation ends up indexing another's lines.
    snap, snap_err = _staged_snapshot()
    if snap is None:
        sys.stderr.write(
            "\n[todo-staged-check] cannot read the staged index: %s\n"
            "\n  Refusing rather than judging the working tree instead: the\n"
            "  coordinates this gate uses come from the index, so a snapshot it\n"
            "  cannot trust would judge one generation's lines against\n"
            "  another's text.\n"
            "  Opt-out: SKIP_TODO_STAGED_CHECK=1 git commit ...\n" % snap_err)
        return 1
    docs, tree_id = snap
    bad, wrapped, oscomp, parked, noprov, noimpact = [], [], [], [], [], []
    unscannable = []
    # section-count cap: judged per file, on files this commit GROWS
    capped_hard, capped_soft = [], []
    for f in files:
        text = docs.get(f)
        if text is None:
            continue                   # staged deletion: nothing to judge
        lines, mask, unclosed = _post_image_lines(text)
        if unclosed:
            unscannable.append((f, unclosed))
            continue
        # Fenced and commented lines blanked IN PLACE, so every line NUMBER
        # still indexes the same line while a fenced heading, stamp, park or
        # table row can no longer match anything. Blanking is safe here because
        # every walk below matches per line; the whole-text callers that cannot
        # do this are handled in lint.sh.
        vis = ["" if mask[i] else l for i, l in enumerate(lines)]
        masked_nos = {i + 1 for i, m in enumerate(mask) if m}
        added = [(n, t) for n, t in _added_lines(f) if n not in masked_nos]
        for n, ln, text in over_cap(added):
            bad.append((f, n, ln, text))
        col = wrapped_block(added)
        if col:
            wrapped.append((f, col))
        for n, w, cell in _oscomp_over_cap(vis, added):
            oscomp.append((f, n, w, cell))
        for n, head, text in _park_into_shipping_section(vis, added):
            parked.append((f, n, head, text))
        for n, head in _sections_missing_provenance(f, vis, added):
            noprov.append((f, n, head))
        for n, head in _review_sections_missing_user_impact(f, vis, added):
            noimpact.append((f, n, head))
        new_secs = added_sections(added)
        if new_secs:
            total = _section_total(vis)
            if total > SECTION_HARD_CAP:
                capped_hard.append((f, total, new_secs))
            elif total >= SECTION_SOFT_CAP:
                capped_soft.append((f, total, new_secs))
    # RE-BIND THE COORDINATES. Everything above read one pinned generation, but
    # `_added_lines` shelled out to `git diff --cached` separately -- so a
    # `git add` landing in between would have paired this run's masks with
    # another generation's line numbers. Checking the tree id once at the end
    # costs one `write-tree` and turns that race into a refusal.
    try:
        if _fence().index_tree(_git_root()) != tree_id:
            sys.stderr.write(
                "\n[todo-staged-check] the index changed while this gate ran "
                "(a concurrent `git add`) -- refusing rather than reporting a "
                "verdict about two different generations. Re-run the commit.\n")
            return 1
    except Exception as exc:
        sys.stderr.write(
            "\n[todo-staged-check] cannot re-verify the index generation: %s\n"
            % exc)
        return 1
    if unscannable:
        for f, why in unscannable:
            sys.stderr.write(
                "\n[todo-staged-check] %s cannot be checked: %s\n" % (f, why))
        sys.stderr.write(
            "\n  Every structural walk in this gate stops at an unclosed\n"
            "  delimiter, so passing the file would mean passing it UNREAD --\n"
            "  and an unclosed opener added by this very commit would hide\n"
            "  every section, park and stamp below it. `build.py` refuses the\n"
            "  same documents rather than publishing the erasure.\n"
            "  Opt-out: SKIP_TODO_STAGED_CHECK=1 git commit ...\n")
        return 1
    for f, total, new_secs in capped_soft:
        sys.stderr.write(
            "[todo-staged-check WARN] %s now has %d sections (soft cap %d). "
            "Before adding more, ask whether the new work belongs in the "
            "domain-correct TODO rather than here -- a section about kernel "
            "logging or process lifecycle usually does, even when discovered "
            "while working this file.\n" % (f, total, SECTION_SOFT_CAP))

    for f, col in wrapped:
        sys.stderr.write(
            "[todo-staged-check WARN] %s: this commit adds prose hard-wrapped at "
            "~%d columns. todo/ prose is one paragraph per physical line. Repair: "
            "python3 scripts/todo-reflow.py --write %s\n" % (f, col, f))
    if parked:
        for f, n, head, text in parked:
            sys.stderr.write(
                "\n[todo-staged-check] %s:%d parks an item into a section this "
                "same commit is STAMPING:\n    %s\n    in: %s\n"
                % (f, n, text, head))
        sys.stderr.write(
            "\n  A `- [/]` park is how you file into a section that is ALREADY\n"
            "  closed. Parking into one you are closing RIGHT NOW strands it: the\n"
            "  fixpoint loop never revisits a DONE section, and the stranded sweep\n"
            "  currently reports 60 items in exactly that state -- three of them\n"
            "  already finished, with nobody left to tick the box.\n"
            "  You have three better dispositions, all reachable:\n"
            "    - fix it now and record it as `- [x]` (the default for a finding\n"
            "      inside this section's own surface);\n"
            "    - file `- [ ]` in the OPEN section of the component that owns it;\n"
            "    - decide it fails the user-impact test and do not file it.\n"
            "  Opt-out: SKIP_TODO_STAGED_CHECK=1 git commit ...\n")
        return 1

    if oscomp:
        for f, n, w, cell in oscomp:
            sys.stderr.write(
                "\n[todo-staged-check] %s:%d adds an OS Comparison cell of %d "
                "columns (cap %d):\n    %s\n"
                % (f, n, w, OS_COMPARISON_CELL_CAP, cell))
        sys.stderr.write(
            "\n  That column is a CLAIM, not a paragraph. Keep the verdict glyph and\n"
            "  a short phrase; move the justification into the section body or a line\n"
            "  beneath the table -- relocate it, do not delete it.\n"
            "  All 223 tables were repaired to <= %d on 2026-08-08; this keeps them there.\n"
            "  Opt-out: SKIP_TODO_STAGED_CHECK=1 git commit ...\n"
            % OS_COMPARISON_CELL_CAP)
        return 1

    if capped_hard:
        for f, total, new_secs in capped_hard:
            sys.stderr.write(
                "\n[todo-staged-check] %s has %d sections (hard cap %d) and this "
                "commit adds %d more.\n" % (f, total, SECTION_HARD_CAP, len(new_secs)))
            for n, text in new_secs[:5]:
                sys.stderr.write("      + %s\n" % text[:110])
            sys.stderr.write(
                "\nDO NOT DROP THE WORK. Completion-first still applies: a gap you\n"
                "found is real and must be filed. What changes at the cap is WHERE.\n"
                "File it in the domain-correct TODO (the one a reader would look in\n"
                "for that capability) with a reciprocal XREF back to this section,\n"
                "exactly as the CLAUDE.md 'file it immediately in the owning TODO'\n"
                "rule already prescribes. A `-part-2` file is NOT the answer: it\n"
                "recreates the same unbounded bucket under a new name.\n"
                "\nADD THE IMPLEMENTATION ORDER ROW, NOT JUST THE SECTION BODY.\n"
                "The triage oracle classifies from the IO table row and never reads\n"
                "the body. MEASURED 2026-08-02 against a real completed file: a new\n"
                "`## N.` body alone left TODO-05 classified DONE, so the work was\n"
                "invisible to every later pass; the SAME section with an IO row\n"
                "flipped the file to NEEDS_WORK and the oracle listed it as open.\n"
                "Filing into a completed TODO without the row is a black hole --\n"
                "the file-level twin of the stamped-section trap CLAUDE.md warns\n"
                "about. Row + body + reciprocal XREF, every time.\n"
                "\nAND SEARCH FIRST -- unreachable is not the same as absent.\n"
                "If this work already exists anywhere (a stamped section, a\n"
                "Deferred park, a section in the wrong place, another domain's\n"
                "TODO), REOPEN or REFERENCE it. Do NOT write a second section\n"
                "covering the same ground because the first one could not be\n"
                "reached: that doubles the review surface, splits one piece of\n"
                "work across two owners, and every counter will read as if the\n"
                "corpus improved. `grep -rn <capability> todo/` before filing.\n"
                "Override (last resort): SKIP_TODO_STAGED_CHECK=1 git commit ...\n\n")
        return 1

    if noprov:
        for f, n, head in noprov:
            sys.stderr.write(
                "\n[todo-staged-check] %s:%d adds a section with no provenance:\n    %s\n"
                % (f, n, head))
        sys.stderr.write(
            "\n  Add ONE line directly under the heading, naming where it came from:\n"
            "    > **Spawned-by:** root            <- nothing spawned it\n"
            "    > **Spawned-by:** section N (split)   <- decomposition of section N\n"
            "    > **Spawned-by:** section N (review)  <- created from section N's review\n"
            "\n  This is what the spawn-chain sensor counts. Without it a section is a\n"
            "  root at depth 0, so a review-spawn cascade never reaches the limit and\n"
            "  never has to justify itself -- which is how one file went from 9 to 35\n"
            "  sections in a week with the sensor live and silent throughout.\n"
            "  `root` is a fine answer and costs nothing; saying nothing is not.\n"
            "  A brand-new TODO file is exempt -- its sections are roots by construction.\n"
            "  Opt-out: SKIP_TODO_STAGED_CHECK=1 git commit ...\n")
        return 1

    if noimpact:
        for f, n, head in noimpact:
            sys.stderr.write(
                "\n[todo-staged-check] %s:%d is spawned from a REVIEW but does not say "
                "what a user hits:\n    %s\n" % (f, n, head))
        sys.stderr.write(
            "\n  Add one line in the section body:\n"
            "    > **User impact:** <what a user hits if this is NOT done>\n"
            "\n  Required only on `(review)`-spawned sections -- a `root` is a capability\n"
            "  you set out to build and a `(split)` is justified work being partitioned.\n"
            "  A section created FROM a review finding is the shape that runs away, and\n"
            "  this is the question that separates a worthwhile one from refinement.\n"
            "\n  \"Nothing today\" IS a legitimate answer, and often the useful one -- it is\n"
            "  the answer that talks you out of the section. The content is not judged;\n"
            "  demanding a weighty impact would only teach people to invent one. Write it\n"
            "  honestly, then decide: fix it in the section you are in and name it `- [x]`,\n"
            "  file `- [ ]` in the OPEN section of the component that owns it, or keep the\n"
            "  new section because the sentence you just wrote justifies it.\n"
            "  Opt-out: SKIP_TODO_STAGED_CHECK=1 git commit ...\n")
        return 1

    if not bad:
        return 0
    sys.stderr.write(
        "\n[todo-staged-check] %d NEW checklist item(s) exceed the %d-char cap.\n"
        "The PreToolUse hook missed these because the file was written through a\n"
        "script (python3 heredoc / rewrite) rather than Edit/Write -- that bypass\n"
        "is why this commit-time check exists.\n\n" % (len(bad), CAP))
    for f, n, ln, text in bad[:10]:
        sys.stderr.write("  %s:%d  %d chars (over by %d)\n      %s...\n"
                         % (f, n, ln, ln - CAP, text.strip()[:96]))
    sys.stderr.write(
        "\nItems are scannable one-line summaries. File:line citations, commit\n"
        "hashes, review round counts and investigation logs belong in the COMMIT\n"
        "MESSAGE or an indented continuation line, not the bullet. Aim for <= 230\n"
        "so small edits keep fitting.\n"
        "Check one line: python3 .claude/hooks/todo_item_line_length.py --check \"<line>\"\n"
        "Override (last resort): SKIP_TODO_STAGED_CHECK=1 git commit ...\n\n")
    return 1


def _selftest() -> int:
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    long_item = "- [ ] " + "x" * 300
    short_item = "- [ ] " + "x" * 100
    check("flags an over-cap added item",
          len(over_cap([(1, long_item)])) == 1)
    check("passes a short item", over_cap([(1, short_item)]) == [])
    check("indented item still judged",
          len(over_cap([(1, "      - [ ] " + "x" * 300)])) == 1)
    check("[x] and [/] judged too",
          len(over_cap([(1, "- [x] " + "y" * 300),
                        (2, "- [/] " + "z" * 300)])) == 2)
    # Prose and continuation lines are NOT checklist items and are exempt --
    # the cap is on the scannable bullet, not on body text.
    check("long prose exempt", over_cap([(1, "w" * 400)]) == [])
    check("long continuation exempt", over_cap([(1, "      " + "w" * 400)]) == [])
    check("table row exempt", over_cap([(1, "| " + "w" * 400 + " |")]) == [])
    # Boundary.
    check("exactly at cap passes",
          over_cap([(1, "- [ ] " + "x" * (CAP - 6))]) == [])
    check("one over cap fails",
          len(over_cap([(1, "- [ ] " + "x" * (CAP - 5))])) == 1)

    # --- wrapped_block must not fire on the RECOMMENDED repair (2026-07-30) --
    # Splitting an over-cap body into indented sub-bullets adds a run of
    # similar-width `- ` lines -- the same narrow-band signature a fill column
    # has. Before the prose filter at wrapped_block(), the commit that split
    # TODO-04's 4,865-char QEMU-reap item warned about its own fix.
    subs = [(i, "        - " + w) for i, w in enumerate([
        "unlocked shared temp files letting a concurrent process publish a key",
        "the key being blind to `ABI_CLANG` overrides and some more text here",
        "header concatenation ambiguous across a sorted-adjacent boundary now",
        "a wrapper-identity check stopping at file bytes not a behavior print"])]
    check("sub-bullet run is not a fill column", wrapped_block(subs) == 0)
    # ... while a genuine fill column is still caught.
    real = [(i, t) for i, t in enumerate((
        "`task_exec()` has no transactional commit point: it mutates the image and THEN performs\n"
        "fallible work, so a late failure returns `-1` to a caller that iretqs back into an image\n"
        "that no longer exists. It also replaces the stack base with a fresh guarded kernel stack\n"
        "and never reclaims the old one.").split("\n"))]
    check("genuine hard wrap still caught", wrapped_block(real) > 0)

    if fails:
        sys.stderr.write("todo-staged-check selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("todo-staged-check selftest OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
