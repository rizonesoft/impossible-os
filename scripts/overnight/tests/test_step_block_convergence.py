#!/usr/bin/env python3
"""A CONVERGED review kind satisfies its step instead of forcing a blanket SKIP.

Observed 2026-08-07 closing the todo-metadata-layer section 18.
`review_convergence should-redispatch <slice> adversarial` returned CONVERGED --
inputs unchanged since that kind's last verdict, so re-dispatching would burn a
full round to re-derive the same answer -- while `skill_step_block` still
demanded a step-5 observation for the same kind.

Both gates were individually right and neither knew about the other, so the only
way through was `SKIP_SKILL_STEP_BLOCK=1`, which suppresses EVERY step check
rather than the one contradicted. That is the shape that erodes a gate: when the
escape hatch is wider than the exception, reaching for it becomes routine and it
stops being evidence of anything.

A convergence record is minted from a REAL dispatch's fingerprint, so it is
positive evidence the review happened -- satisfying the step, not bypassing it.
"""
from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOK = REPO / ".claude/hooks/skill_step_block.py"


def _load():
    if str(HOOK.parent) not in sys.path:
        sys.path.insert(0, str(HOOK.parent))
    spec = importlib.util.spec_from_file_location("skill_step_block", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _root_with(state):
    d = pathlib.Path(tempfile.mkdtemp())
    (d / ".claude/state").mkdir(parents=True)
    (d / ".claude/state/review-convergence.json").write_text(
        json.dumps(state), encoding="utf-8")
    return d


def test_a_converged_kind_is_reported():
    mod = _load()
    root = _root_with({"todo/x.md#18": {
        "adversarial": {"fp": "abc", "ts": 1},
        "perf": {"fp": "def", "ts": 2}}})
    got = mod._converged_kinds(str(root), {"todo": "todo/x.md", "section": 18})
    assert got == {"adversarial", "perf"}, got


def test_a_record_without_a_fingerprint_is_not_convergence():
    """Only a record minted from a real dispatch counts. An empty or
    placeholder entry must not satisfy a step."""
    mod = _load()
    root = _root_with({"todo/x.md#18": {"adversarial": {"ts": 1}}})
    assert mod._converged_kinds(str(root), {"todo": "todo/x.md", "section": 18}) == set()


def test_a_different_section_does_not_leak():
    """Convergence is per SLICE. Section 17's verdict must not satisfy 18."""
    mod = _load()
    root = _root_with({"todo/x.md#17": {"adversarial": {"fp": "abc", "ts": 1}}})
    assert mod._converged_kinds(str(root), {"todo": "todo/x.md", "section": 18}) == set()


def test_missing_context_fails_toward_STILL_GATING():
    """No section/todo, no state file, malformed JSON -- all must yield the
    empty set, so the step check stays in force. Failing the other way would
    silently disable step enforcement."""
    mod = _load()
    root = _root_with({"todo/x.md#18": {"adversarial": {"fp": "a", "ts": 1}}})
    assert mod._converged_kinds(str(root), {}) == set()
    assert mod._converged_kinds(str(root), {"todo": "todo/x.md"}) == set()
    assert mod._converged_kinds("/nonexistent", {"todo": "t", "section": 1}) == set()


if __name__ == "__main__":
    test_a_converged_kind_is_reported()
    test_a_record_without_a_fingerprint_is_not_convergence()
    test_a_different_section_does_not_leak()
    test_missing_context_fails_toward_STILL_GATING()
    print("PASS: a converged kind satisfies its step, and only that step")
