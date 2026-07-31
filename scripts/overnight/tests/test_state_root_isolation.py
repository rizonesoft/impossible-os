#!/usr/bin/env python3
"""Hook state must be redirectable, or no test of a hook can be trusted.

2026-07-31: an attended attempt to test `rotate_hint.py` against a tempfile
fixture -- passed via BOTH `CLAUDE_PROJECT_DIR` and `cwd` -- drove the LIVE
counter to `{"count": 95, "hint": true}` and never touched the fixture, because
the hook resolves its root from `__file__`. The same class made four
`phase1_evidence_gate` assertions flap across suite runs with no intervening
edits. These are the gates protecting the section-commit and evidence
contracts, so a flapping verdict there means a real regression reads as "the
usual flake".

This test IS the contaminating experiment, re-run. It fails if live state moves.
"""
from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
ROTATE = REPO / ".claude/hooks/rotate_hint.py"
LIVE = REPO / ".claude/state/rotate-hint.json"


def test_override_redirects_and_live_state_is_untouched():
    before = LIVE.read_text() if LIVE.exists() else None
    with tempfile.TemporaryDirectory() as td:
        st = pathlib.Path(td) / ".claude/state"
        st.mkdir(parents=True)
        (st / "sequencer-run.json").write_text(
            json.dumps({"active": True, "phase": "SECTIONS"}))
        env = dict(os.environ, OVERNIGHT_SEQUENCER_RUN="1",
                   CLAUDE_HOOK_STATE_ROOT=td)
        payload = json.dumps({"tool_name": "Bash", "tool_input": {"command": "ls"}})
        for _ in range(3):
            subprocess.run([sys.executable, str(ROTATE)], input=payload,
                           capture_output=True, text=True, env=env, timeout=60)
        fixture = st / "rotate-hint.json"
        assert fixture.exists(), "override ignored -- the fixture was never written"
        assert json.loads(fixture.read_text())["count"] == 3, fixture.read_text()
    after = LIVE.read_text() if LIVE.exists() else None
    assert before == after, (
        f"LIVE runner state was mutated by a test: {before!r} -> {after!r}")


def test_absent_override_keeps_the_original_resolution():
    """Additive only: with no override, behaviour is exactly as before."""
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    import _state_root
    os.environ.pop("CLAUDE_HOOK_STATE_ROOT", None)
    assert _state_root.override() is None
    assert _state_root.resolve(lambda: "/fallback") == "/fallback"
    os.environ["CLAUDE_HOOK_STATE_ROOT"] = "/definitely/not/a/dir"
    try:
        assert _state_root.override() is None, "non-directory override must be ignored"
    finally:
        os.environ.pop("CLAUDE_HOOK_STATE_ROOT", None)


if __name__ == "__main__":
    test_override_redirects_and_live_state_is_untouched()
    test_absent_override_keeps_the_original_resolution()
    print("PASS: hook state-root isolation")
