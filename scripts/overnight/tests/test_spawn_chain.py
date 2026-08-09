#!/usr/bin/env python3
"""Spawn-chain sensor: tell recursion apart from decomposition.

The split predictor scores a section's SIZE and says nothing about its ORIGIN --
and origin is where the expensive failure lives. TODO-07 ran sections 20->26,
seven consecutive links, each created from the PREVIOUS section's review finding
another edge case in the same teardown path. Branching factor 1.0 for days, and
every counter read healthy the whole time: sections shipped, reviews passed, the
IO table filled. Nothing could see that section N existed only because of N-1.

COMPLETION-FIRST IS NOT WEAKENED. A discovered gap is still always filed. This
decides only WHERE -- a new section, or a parked `- [/]` in the parent naming its
blocker. Both keep the work visible; only one grows the file.
"""
from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
TOOL = REPO / "scripts/overnight/section-manifest.py"


def _load():
    spec = importlib.util.spec_from_file_location("section_manifest", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _chain(pairs):
    """pairs: [(section, parent_or_None, kind)] -> file lines"""
    out = []
    for n, par, kind in pairs:
        out.append(f"## {n}. thing {n}")
        if par is not None:
            out.append(f"> **Spawned-by:** §{par} ({kind})")
    return out


def test_review_chain_accumulates_depth():
    """The TODO-07 shape: each section spawned by the previous one's review."""
    sm = _load()
    lines = _chain([(20, None, ""), (21, 20, "review"), (22, 21, "review"),
                    (23, 22, "review"), (24, 23, "review")])
    ch = sm.spawn_chains(lines)
    assert ch[21][2] == 1, ch
    assert ch[22][2] == 2, ch
    assert ch[23][2] == 3, ch
    assert ch[24][2] == 4, ch


def test_a_split_is_not_recursion():
    """Decomposition at authoring time is healthy and ADDS no depth of its own:
    same work, correctly partitioned, where the two halves carry opposite
    failure modes. A split off a ROOT therefore stays at 0.

    But it PROPAGATES the depth it inherits, and that half is load-bearing.
    Updated 2026-08-09: a split used to RESET the count, which let a cascade run
    forever under the limit -- eleven splits interleaved with nine review-spawns
    held the deepest chain in the metadata-layer TODO at 2 against a limit of 3,
    so the sensor never fired once. Splitting a review-spawned section does not
    make its successors less recursive; it multiplies the surface that spawns
    the next one."""
    sm = _load()
    ch = sm.spawn_chains(_chain([(10, None, ""), (11, 10, "split")]))
    assert ch[11][1] == "split" and ch[11][2] == 0, ch
    # A split in the MIDDLE of a review chain CARRIES it forward. Reverting the
    # propagation makes the last two assertions read 0 and 1 -- which is the
    # exact blindness this test now exists to prevent.
    ch2 = sm.spawn_chains(_chain([(1, None, ""), (2, 1, "review"),
                                  (3, 2, "split"), (4, 3, "review")]))
    assert ch2[2][2] == 1 and ch2[3][2] == 1 and ch2[4][2] == 2, ch2
    # ...and a chain that alternates review/split still reaches the limit, which
    # is what the old reset made impossible.
    ch3 = sm.spawn_chains(_chain([(1, None, ""), (2, 1, "review"),
                                  (3, 2, "split"), (4, 3, "review"),
                                  (5, 4, "split"), (6, 5, "review")]))
    assert ch3[6][2] >= sm.SPAWN_CHAIN_LIMIT, ch3


def test_depth_reaches_the_manifest_where_the_decision_is_made():
    """The counter was correct and consulted by NOBODY: its only readers were
    two CLI verbs nothing calls automatically, so a run could sit at depth 3 and
    never learn it. The manifest (and through it the section-pack summary a
    fresh worker reads to orient) now carries `spawn_chain`, and a chain at the
    limit carries the note. INFORMATION, not a gate -- refusing here would push
    a legitimate finding into a park, and parks in closed sections are the 60
    stranded items this whole change set exists to stop creating."""
    import json, subprocess, sys, tempfile, os
    body = ("# T\n\n## Implementation Order\n\n| x | 1 | s1 | d | -- | [x] |\n\n"
            "## 1. Root\n\n- [x] a\n\n"
            "## 2. Two\n\n> **Spawned-by:** section 1 (review)\n\n- [x] a\n\n"
            "## 3. Three\n\n> **Spawned-by:** section 2 (review)\n\n- [x] a\n\n"
            "## 4. Four\n\n> **Spawned-by:** section 3 (review)\n\n- [ ] work\n")
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "todo/00-infrastructure").mkdir(parents=True)
        (root / "todo/00-infrastructure/TODO-01-x.md").write_text(body, encoding="utf-8")
        def manifest(n):
            r = subprocess.run(
                [sys.executable, str(HERE.parent / "section-manifest.py"),
                 "todo/00-infrastructure/TODO-01-x.md", str(n), "--project", str(root)],
                capture_output=True, text=True, cwd=str(root))
            return json.loads(r.stdout)
        assert manifest(1)["spawn_chain"] is None, "a root must report no chain"
        two = manifest(2)
        assert two["spawn_chain"]["review_depth"] == 1, two["spawn_chain"]
        assert "note" not in two["spawn_chain"], "a shallow chain needs no nag"
        four = manifest(4)
        assert four["spawn_chain"]["review_depth"] == 3, four["spawn_chain"]
        assert "user_impact" in four["spawn_chain"]["note"], four["spawn_chain"]


def test_root_is_a_declarable_provenance():
    """`(root)` had to become sayable before provenance could be REQUIRED --
    otherwise a genuinely independent section has to invent a parent. It
    contributes 0 depth, exactly as silence did, so it changes no verdict; what
    it buys is that "nothing spawned this" is a claim rather than a default.
    Added 2026-08-09 alongside the staged-check that refuses an undeclared
    section."""
    sm = _load()
    ch = sm.spawn_chains(["## 1. a", "> **Spawned-by:** root"])
    assert ch == {}, ch
    # a root parent still anchors a real chain below it
    ch2 = sm.spawn_chains(["## 1. a", "> **Spawned-by:** root",
                           "## 2. b", "> **Spawned-by:** section 1 (review)"])
    assert ch2[2][2] == 1, ch2


def test_unmarked_sections_are_silent():
    """Additive by construction: every pre-existing section has no marker, so the
    sensor reports nothing until sections start declaring provenance. A sensor
    that lit up the whole corpus on day one would be ignored on day two."""
    sm = _load()
    assert sm.spawn_chains(["## 1. a", "## 2. b", "- [ ] item"]) == {}


def test_cycles_do_not_hang():
    """A malformed self- or mutual-reference stops at 0. A sensor must never be
    the thing that hangs the run."""
    sm = _load()
    ch = sm.spawn_chains(_chain([(5, 5, "review")]))
    assert ch[5][2] <= 2, ch
    ch2 = sm.spawn_chains(_chain([(6, 7, "review"), (7, 6, "review")]))
    assert all(v[2] <= 3 for v in ch2.values()), ch2


def test_continuation_waiver_demands_the_two_that_bite():
    """'needed' is not a justification. A chain link that cannot name what a user
    hits, or why a park in the parent would lose something, IS the TODO-07
    shape."""
    sm = _load()
    ok, missing = sm.validate_continuation_waiver({"user_impact": "x"})
    assert not ok and "not_parkable" in missing and "severity_trend" in missing
    ok, _ = sm.validate_continuation_waiver({
        "user_impact": "cold-start hangs on a non-dumpable child",
        "not_parkable": "the parent already shipped; a park has no owner",
        "severity_trend": "1 High this round vs 8H+11M last",
        "surface": "scripts/lsp-mcp/bridge.py",
    })
    assert ok
    ok, _ = sm.validate_continuation_waiver({
        "user_impact": "  ", "not_parkable": "x",
        "severity_trend": "y", "surface": "z"})
    assert not ok, "whitespace must not satisfy a field"
    assert not sm.validate_continuation_waiver("cohesive")[0]


def test_cli_exits_nonzero_only_past_the_limit():
    sm = _load()
    with tempfile.TemporaryDirectory() as d:
        shallow = pathlib.Path(d) / "TODO-99-a.md"
        shallow.write_text("\n".join(_chain([(1, None, ""), (2, 1, "review")])))
        r = subprocess.run([sys.executable, str(TOOL), "spawn-chain", str(shallow)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stdout
        assert json.loads(r.stdout)["over_limit"] == []

        deep = pathlib.Path(d) / "TODO-99-b.md"
        deep.write_text("\n".join(_chain(
            [(1, None, ""), (2, 1, "review"), (3, 2, "review"), (4, 3, "review")])))
        r = subprocess.run([sys.executable, str(TOOL), "spawn-chain", str(deep)],
                           capture_output=True, text=True)
        assert r.returncode == 1, r.stdout
        assert 4 in json.loads(r.stdout)["over_limit"], r.stdout


if __name__ == "__main__":
    test_review_chain_accumulates_depth()
    test_a_split_is_not_recursion()
    test_unmarked_sections_are_silent()
    test_cycles_do_not_hang()
    test_continuation_waiver_demands_the_two_that_bite()
    test_cli_exits_nonzero_only_past_the_limit()
    print("PASS: spawn-chain sensor (recursion vs decomposition)")
