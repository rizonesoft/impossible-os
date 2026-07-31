#!/usr/bin/env python3
# Protects P4.1 (2026-07-12 plan): rotate_hint.py is an ADVISORY turn-count proxy
# for the context-rotation band. In the SECTIONS phase it counts tool events and
# sets hint:true past ROTATE_HINT_TURNS; it never blocks, never fires outside
# SECTIONS, and run_phase_guard clears it on a verified rollover.
import importlib.util
import io
import json
import os
import sys
import tempfile
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parents[2] / ".claude/hooks/rotate_hint.py"


def _load(root, enabled=True):
    spec = importlib.util.spec_from_file_location("rotate_hint", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod._repo_root = lambda: root
    mod.ROTATE_HINT_TURNS = 3  # low threshold for a fast test
    # B1: the hook is RETIRED (inert) by default; the mechanism tests re-enable it
    # to exercise the (dormant, re-enablable) logic.
    mod.ROTATE_HINT_ENABLED = enabled
    return mod


def _sections(root):
    (root / ".claude/state").mkdir(parents=True)
    (root / ".claude/state/sequencer-run.json").write_text(
        json.dumps({"active": True, "phase": "SECTIONS"}))


def _feed(mod, headless=True):
    """Run one tool-event turn; return anything the hook printed to stdout.
    headless=True sets OVERNIGHT_SEQUENCER_RUN=1 (the armed run); False clears it
    (an interactive operator session in the same checkout -- must stay silent)."""
    old_in, old_out = sys.stdin, sys.stdout
    old_env = os.environ.get("OVERNIGHT_SEQUENCER_RUN")
    if headless:
        os.environ["OVERNIGHT_SEQUENCER_RUN"] = "1"
    else:
        os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)
    sys.stdin = io.StringIO(json.dumps({"tool_name": "Read"}))
    sys.stdout = io.StringIO()
    try:
        mod.main()
        return sys.stdout.getvalue()
    finally:
        sys.stdin, sys.stdout = old_in, old_out
        if old_env is None:
            os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)
        else:
            os.environ["OVERNIGHT_SEQUENCER_RUN"] = old_env


def _hint(root):
    p = root / ".claude/state/rotate-hint.json"
    return json.loads(p.read_text()) if p.exists() else None


def test_hint_fires_at_threshold():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        _sections(root)
        mod = _load(root)
        _feed(mod); _feed(mod)                 # 2 turns, below threshold(3)
        h = _hint(root)
        assert h["count"] == 2 and not h.get("hint"), h
        _feed(mod)                             # 3rd turn crosses the band
        h = _hint(root)
        assert h["count"] == 3 and h["hint"] is True, h


def test_silent_outside_sections():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        (root / ".claude/state/sequencer-run.json").write_text(
            json.dumps({"active": True, "phase": "TRIAGE"}))
        mod = _load(root)
        for _ in range(5):
            _feed(mod)
        assert _hint(root) is None, "must not count outside SECTIONS"


def test_no_run_state_silent():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude").mkdir()
        mod = _load(root)
        for _ in range(5):
            _feed(mod)
        assert _hint(root) is None


def _has_msg(out):
    if not out.strip():
        return False
    return "rollover-wip" in json.loads(out.strip()).get("systemMessage", "")


def test_reminder_emitted_on_crossing_turn_only():         # P4.6 consumer surface
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        _sections(root)
        mod = _load(root)                                  # ROTATE_HINT_TURNS = 3
        assert not _has_msg(_feed(mod))                    # turn 1: below band
        assert not _has_msg(_feed(mod))                    # turn 2: below band
        assert _has_msg(_feed(mod))                        # turn 3: crossing -> emit


def test_reminder_renudges_but_does_not_spam():            # P4.6 re-nudge cadence
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        _sections(root)
        mod = _load(root)
        mod.ROTATE_RENUDGE_TURNS = 2                       # crossing at 3, re-nudge at 5,7
        outs = [_has_msg(_feed(mod)) for _ in range(7)]
        # turns 1..7 -> indices 0..6: emit at crossing(3=idx2), re-nudge(5=idx4, 7=idx6)
        assert outs == [False, False, True, False, True, False, True], outs


def test_silent_in_interactive_session():                  # A2 session isolation
    # OVERNIGHT_SEQUENCER_RUN unset (interactive) -> never counts, never emits,
    # even with an active SECTIONS cursor from a concurrent armed run.
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        _sections(root)
        mod = _load(root)
        for _ in range(5):
            out = _feed(mod, headless=False)
            assert not _has_msg(out), out
        assert _hint(root) is None, "interactive session must not touch the counter"


def test_no_emit_when_persist_fails():                     # A4 emit-after-persist
    if os.geteuid() == 0:
        return  # root bypasses fs perms; the write-failure path is untestable
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        _sections(root)
        mod = _load(root)                                  # ROTATE_HINT_TURNS = 3
        _feed(mod); _feed(mod)                             # count -> 2 (persisted)
        state_dir = root / ".claude/state"
        assert _hint(root)["count"] == 2
        # Atomic persist writes a tmp then os.replace -- both need DIR write; a
        # read-only dir (still r-x) fails the write but allows the read.
        os.chmod(state_dir, 0o555)
        try:
            out = _feed(mod)                               # crossing turn, write fails
            assert not _has_msg(out), "must not emit from unpersisted state"
            assert _hint(root)["count"] == 2, "count must not have persisted"
        finally:
            os.chmod(state_dir, 0o755)


def test_disable_switch_is_inert():                        # B1 retirement path
    # The kill switch must still work: with ROTATE_HINT_ENABLED False the hook
    # never counts and never emits, even in a headless SECTIONS run past the
    # threshold. (This was the SHIPPED default from 2026-07-14 until 2026-07-30,
    # when the rotation was re-enabled on corrected evidence -- see
    # test_shipped_default_is_enabled below. Keep this path working so the
    # mechanism can be switched off again without code surgery.)
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        _sections(root)
        mod = _load(root, enabled=False)
        for _ in range(6):                                 # well past threshold(3)
            assert not _has_msg(_feed(mod))
        assert _hint(root) is None, "retired hook must not touch the counter"


def test_shipped_default_is_enabled():
    """The mid-section rotation was re-enabled 2026-07-30 after the retirement's
    load-bearing premise ("140 tool-events ~= one section") was falsified by
    measurement: three consecutive segments ran 334 / 420 / 768 tool-events, and
    the 334 one shipped a SINGLE section at $179.27 with cache-read 85.3% of
    spend. Pin the shipped values so a silent revert is caught -- flipping either
    back is a deliberate decision that must update the doctrine with it.

    The THRESHOLD was re-derived 2026-07-31 (200 -> 90) after the first live
    segment showed the old number was computed in the wrong units: it counts
    SECTIONS-phase matcher events, which run 0.65 per turn, so 200 landed at
    turn ~307 of a 308-turn segment -- the hint fired at event 201 of 201, with
    no runway left to act on it."""
    spec = importlib.util.spec_from_file_location("rh_shipped", HOOK)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    assert m.ROTATE_HINT_ENABLED is True, "mid-section rotation must stay enabled"
    assert m.ROTATE_HINT_TURNS == 90, "threshold is derived, not a free knob"
    skill = (HOOK.parents[1] / "skills/overnight-sequencer/SKILL.md"
             ).read_text(encoding="utf-8")
    assert "Mid-section context-cap rotation -- ACTIVE" in skill, \
        "the SKILL doctrine must agree with the flag"


if __name__ == "__main__":
    test_disable_switch_is_inert()
    test_shipped_default_is_enabled()
    test_hint_fires_at_threshold()
    test_silent_outside_sections()
    test_no_run_state_silent()
    test_reminder_emitted_on_crossing_turn_only()
    test_reminder_renudges_but_does_not_spam()
    test_silent_in_interactive_session()
    test_no_emit_when_persist_fails()
    print("PASS: rotate-hint (P4.1)")
