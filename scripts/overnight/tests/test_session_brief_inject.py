#!/usr/bin/env python3
import json, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOKS = REPO / ".claude" / "hooks"
sys.path.insert(0, str(HOOKS))
import session_brief_inject as sbi  # noqa: E402


def _mkroot(d, active):
    root = pathlib.Path(d)
    (root / ".claude" / "state").mkdir(parents=True, exist_ok=True)
    (root / ".claude" / "state" / "sequencer-run.json").write_text(
        json.dumps({"active": active, "phase": "SECTIONS", "file": "TODO-X.md"}))
    return root


def test_compact_active_returns_brief():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=True)
        b = sbi.brief_for_event({"source": "compact"}, root)
        assert b is not None and "WHERE" in b


def test_resume_active_returns_brief():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=True)
        assert sbi.brief_for_event({"source": "resume"}, root) is not None


def test_startup_returns_none():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=True)
        assert sbi.brief_for_event({"source": "startup"}, root) is None


def test_inactive_returns_none():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=False)
        assert sbi.brief_for_event({"source": "compact"}, root) is None


if __name__ == "__main__":
    test_compact_active_returns_brief()
    test_resume_active_returns_brief()
    test_startup_returns_none()
    test_inactive_returns_none()
    print("PASS: session_brief_inject")
