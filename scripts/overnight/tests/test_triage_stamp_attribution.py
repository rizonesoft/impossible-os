#!/usr/bin/env python3
# v14 close-out (2026-08-16): `sequencer_triage.section_stamps()` kept a
# private column-zero `## N.` grammar after the producer and gates moved onto
# the shared projection, so for `## 1. Root` / `- ## 2. Nested` / stamps under
# the nested heading it attributed BOTH stamps to section 1 -- an unstamped
# shipped root read DONE and the stamped nested section read NEEDS_WORK, in
# the oracle every sequencer phase classifies from (v14 finding, Codex
# consistency review of the todo-metadata-layer work, [high]).
#
# These are the REFUSAL-direction controls for the shared-projection adoption:
# each case is one that must NOT attribute (or must NOT stop attributing).
# Revert the adoption and the nested/fence/closing-matter cases fail.
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / ".claude/hooks"))
import sequencer_triage as st  # noqa: E402

FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)


def stamps_of(text):
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as fh:
        fh.write(text)
        path = fh.name
    try:
        return st.section_stamps(path)
    finally:
        pathlib.Path(path).unlink(missing_ok=True)


# 1. THE HEADLINE CASE: a nested heading owns the stamps under it. The old
#    grammar assigned both stamps to section 1.
s = stamps_of(
    "## 1. Root\n"
    "- [x] shipped item\n"
    "- ## 2. Nested\n"
    "> **Verified:** 2026-08-16\n"
    "> **Quality reviewed:** 2026-08-16\n")
check("nested: section 2 carries both stamps", s.get(2) == {"V", "Q"})
check("nested: section 1 carries none", s.get(1) == set())

# 2. REFUSAL: a fenced example never stamps.
s = stamps_of(
    "## 1. Root\n"
    "```\n"
    "> **Verified:** fenced example\n"
    "```\n")
check("fence: fenced stamp does not attribute", s.get(1) == set())

# 3. REFUSAL: closing matter ends the last numbered section -- a stamp after
#    `## Unit Tests` must not attribute to section 1.
s = stamps_of(
    "## 1. Root\n"
    "- [x] item\n"
    "## Unit Tests\n"
    "> **Verified:** stray stamp in closing matter\n")
check("closing matter: stamp does not attribute", s.get(1) == set())

# 4. REGRESSION GUARD: the ordinary two-line physical stamp block yields BOTH
#    kinds (the projection strips `> ` from continuation lines; stamps must
#    stay on physical lines or `Quality reviewed` silently drops).
s = stamps_of(
    "## 3. Plain\n"
    "> **Verified:** 2026-08-16\n"
    "> **Quality reviewed:** 2026-08-16\n")
check("physical: two-line block yields V and Q", s.get(3) == {"V", "Q"})

# 5. REFUSAL: a blockquoted heading is a quoted example, not a boundary.
s = stamps_of(
    "## 4. Real\n"
    "> ## 99. Quoted example heading\n"
    "> **Verified:** 2026-08-16\n")
check("blockquote: quoted heading is no boundary", 99 not in s)
check("blockquote: stamp still reaches section 4", "V" in s.get(4, set()))

# 6. Deferred + awaiting token attribution under a NESTED section, the
#    collect_blockers walk shape.
s = stamps_of(
    "## 5. Root\n"
    "- ## 6. Nested deferral\n"
    "> **Deferred:** awaiting-hardware\n")
check("nested deferral: D+DR on section 6", s.get(6) == {"D", "DR"})
check("nested deferral: none on section 5", s.get(5) == set())

# 7. file_lifecycle: preamble stamps found; a FENCED `## 1.` does not end the
#    preamble early.
with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as fh:
    fh.write("# Title\n"
             "```\n"
             "## 1. fenced example -- not a section start\n"
             "```\n"
             "> **Validated:** 2026-08-16\n"
             "> **Gap-audited:** 2026-08-16\n"
             "## 1. Real first section\n"
             "> **Validated:** past the preamble, must not count twice\n")
    lp = fh.name
try:
    lc = st.file_lifecycle(lp)
    check("lifecycle: preamble stamps found past a fenced heading",
          lc == {"validated": True, "gap_audited": True})
finally:
    pathlib.Path(lp).unlink(missing_ok=True)

if FAILS:
    print("test_triage_stamp_attribution: FAIL")
    for f in FAILS:
        print(f"  - {f}")
    sys.exit(1)
print("test_triage_stamp_attribution: OK (10 checks)")
