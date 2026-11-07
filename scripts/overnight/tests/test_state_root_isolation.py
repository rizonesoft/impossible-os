#!/usr/bin/env python3
"""Hook state must be redirectable, or no test of a hook can be trusted.

2026-07-31: an attended attempt to test a state-writing hook against a tempfile
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


def test_override_redirects_to_the_given_directory():
    sys.path.insert(0, str(REPO / ".claude/hooks"))
    import _state_root
    with tempfile.TemporaryDirectory() as td:
        os.environ["CLAUDE_HOOK_STATE_ROOT"] = td
        try:
            assert str(_state_root.override()) == td
            assert str(_state_root.resolve(lambda: "/fallback")) == td
        finally:
            del os.environ["CLAUDE_HOOK_STATE_ROOT"]


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
    test_override_redirects_to_the_given_directory()
    test_absent_override_keeps_the_original_resolution()
    print("PASS: hook state-root isolation")
