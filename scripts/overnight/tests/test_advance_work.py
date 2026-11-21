#!/usr/bin/env python3
"""Smoke test for advance-work.py -- the pre-Opus packet assembler.

advance-work orchestrates the real oracle/classify over the whole repo, so (like
test-launch.sh) it runs against the real tree with --no-pack for speed. It
asserts the packet is well-formed, not any specific cursor (that moves as the
repo progresses). The composed tools each have their own hermetic tests."""
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
AW = HERE.parent / "advance-work.py"
REPO = HERE.parent.parent.parent


def test_packet_is_well_formed():
    r = subprocess.run([sys.executable, str(AW), "--no-pack"],
                       capture_output=True, text=True, cwd=str(REPO), timeout=90)
    assert r.returncode == 0, r.stderr
    d = json.loads(r.stdout)
    assert d.get("action") in ("work", "fixpoint", "blocked", "unknown"), d
    if d["action"] == "work":
        assert d.get("cursor", {}).get("file"), d
        assert "sections_summary" in d, d
        # preflight verdict is always surfaced (cached stamp or 'not run')
        assert "preflight" in d, d


def _mod():
    import importlib.util
    spec = importlib.util.spec_from_file_location("advance_work", AW)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_cursor_disagreement_is_a_field_not_a_silent_choice():
    """v17 close-out (2026-08-29): the guard's explicit cursor said 60 while
    the oracle said 59 and nothing reconciled them. The packet now carries the
    disagreement; the oracle stays the answer."""
    m = _mod()
    todo = "todo/00-infrastructure/TODO-06-x.md"
    st = {"file": todo, "section_idx": 60, "section_source": "explicit"}
    d = m._cursor_disagreement(st, todo, {"n": 59, "class": "NEEDS_WORK"})
    assert d and d["cursor_section_idx"] == 60 and d["oracle_section"] == 59, d
    # Agreement, a derived cursor, another file, or no next section -> nothing.
    assert m._cursor_disagreement(st, todo, {"n": 60}) is None
    assert m._cursor_disagreement({**st, "section_source": "derived"}, todo, {"n": 59}) is None
    assert m._cursor_disagreement(st, "todo/other.md", {"n": 59}) is None
    assert m._cursor_disagreement(st, todo, None) is None
    assert m._cursor_disagreement({}, todo, {"n": 59}) is None


def test_the_packet_names_a_runnable_section_never_a_blocked_one():
    m = _mod()
    secs = [{"n": 1, "class": "NEEDS_WORK", "waiting": True}, {"n": 2, "class": "BLOCKED"},
            {"n": 3, "class": "NEEDS_WORK"}, {"n": 4, "class": "DONE"}]
    assert m._next_section(secs)["n"] == 3
    # Every open section waits: the first is named so the stall is visible.
    assert m._next_section(secs[:2])["n"] == 1
    # Only blocked and done work left: no section.
    assert m._next_section([secs[1], secs[3]]) is None


if __name__ == "__main__":
    test_packet_is_well_formed()
    test_cursor_disagreement_is_a_field_not_a_silent_choice()
    test_the_packet_names_a_runnable_section_never_a_blocked_one()
    print("PASS: advance-work")


def test_runnable_sections_go_in_plan_row_order_after_any_owing_only_review():
    m = _mod()
    secs = [{"n": 49, "class": "NEEDS_WORK", "plan_order": 7}, {"n": 52, "class": "NEEDS_WORK", "waiting": True},
            {"n": 60, "class": "NEEDS_WORK", "plan_order": 1}, {"n": 61, "class": "NEEDS_WORK", "plan_order": 2}]
    assert m._next_section(secs)["n"] == 60                       # the plan's first buildable row, not section 49
    shipped = {"n": 70, "class": "NEEDS_WORK"}                    # shipped, owing its review: no plan_order
    assert m._next_section(secs + [shipped])["n"] == 70
    assert m._next_section([{**s, "waiting": True} for s in secs])["n"] == 49   # all wait: the first, so the stall shows


def test_classify_marks_each_buildable_section_with_its_plan_row():
    import importlib.util
    hook = pathlib.Path(__file__).resolve().parents[3] / ".claude/hooks/sequencer_triage.py"
    spec = importlib.util.spec_from_file_location("_triage_for_order", hook)
    t = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(t)
    entry = {"file_path": "todo/a.md", "sections": [{"n": 48, "status": "/"}, {"n": 49, "status": " "},
                                                    {"n": 60, "status": " "}, {"n": 70, "status": "x"}]}
    per = [(48, t.NEEDS_WORK), (49, t.NEEDS_WORK), (60, t.NEEDS_WORK), (70, t.NEEDS_WORK)]
    ready = [("todo/b.md", 1), ("todo/a.md", 48), ("todo/a.md", 60), ("todo/a.md", 49)]
    out = {s["n"]: s for s in t.section_list(entry, per, ready)}
    assert (out[48]["plan_order"], out[60]["plan_order"], out[49]["plan_order"]) == (0, 1, 2)   # place among this file's rows
    assert "plan_order" not in out[70]                            # shipped: owes only its review, goes first


def test_the_classify_array_is_in_work_order_so_every_reader_picks_the_same_section():
    """run_phase_guard derives its cursor from the first open section of the array; the packet picks with
    _next_section. Both must name the same one."""
    import importlib.util
    hook = pathlib.Path(__file__).resolve().parents[3] / ".claude/hooks/sequencer_triage.py"
    spec = importlib.util.spec_from_file_location("_triage_for_work_order", hook)
    t = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(t)
    entry = {"file_path": "todo/a.md", "sections": [{"n": 1, "status": "x"}, {"n": 49, "status": " "},
                                                    {"n": 52, "status": " "}, {"n": 60, "status": " "},
                                                    {"n": 61, "status": " "}]}
    per = [(1, t.DONE), (49, t.NEEDS_WORK), (52, t.NEEDS_WORK), (60, t.NEEDS_WORK), (61, t.NEEDS_WORK)]
    ready = [("todo/a.md", 60), ("todo/a.md", 61), ("todo/a.md", 49)]
    secs = t.section_list(entry, per, ready)
    assert [s["n"] for s in secs] == [60, 61, 49, 52, 1]
    first_open = next(s["n"] for s in secs if s["class"] == "NEEDS_WORK")     # run_phase_guard's derivation
    assert first_open == _mod()._next_section(secs)["n"] == 60
