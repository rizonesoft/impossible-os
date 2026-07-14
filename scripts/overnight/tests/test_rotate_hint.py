#!/usr/bin/env python3
# Protects P4.1 (2026-07-12 plan): rotate_hint.py is an ADVISORY turn-count proxy
# for the context-rotation band. In the SECTIONS phase it counts tool events and
# sets hint:true past ROTATE_HINT_TURNS; it never blocks, never fires outside
# SECTIONS, and run_phase_guard clears it on a verified rollover.
import importlib.util
import io
import json
import sys
import tempfile
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parents[2] / ".claude/hooks/rotate_hint.py"


def _load(root):
    spec = importlib.util.spec_from_file_location("rotate_hint", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod._repo_root = lambda: root
    mod.ROTATE_HINT_TURNS = 3  # low threshold for a fast test
    return mod


def _sections(root):
    (root / ".claude/state").mkdir(parents=True)
    (root / ".claude/state/sequencer-run.json").write_text(
        json.dumps({"active": True, "phase": "SECTIONS"}))


def _feed(mod):
    """Run one tool-event turn; return anything the hook printed to stdout."""
    old_in, old_out = sys.stdin, sys.stdout
    sys.stdin = io.StringIO(json.dumps({"tool_name": "Read"}))
    sys.stdout = io.StringIO()
    try:
        mod.main()
        return sys.stdout.getvalue()
    finally:
        sys.stdin, sys.stdout = old_in, old_out


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


if __name__ == "__main__":
    test_hint_fires_at_threshold()
    test_silent_outside_sections()
    test_no_run_state_silent()
    test_reminder_emitted_on_crossing_turn_only()
    test_reminder_renudges_but_does_not_spam()
    print("PASS: rotate-hint (P4.1)")
