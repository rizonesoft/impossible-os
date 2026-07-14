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
    old = sys.stdin
    sys.stdin = io.StringIO(json.dumps({"tool_name": "Read"}))
    try:
        mod.main()
    finally:
        sys.stdin = old


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


if __name__ == "__main__":
    test_hint_fires_at_threshold()
    test_silent_outside_sections()
    test_no_run_state_silent()
    print("PASS: rotate-hint (P4.1)")
