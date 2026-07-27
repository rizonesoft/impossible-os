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


def _cp(root, cursor, **extra):
    body = {"cursor": cursor}
    body.update(extra)
    (root / ".claude" / "state" / "section-checkpoint.json").write_text(
        json.dumps(body))


def test_checkpoint_reaches_the_resume_brief():
    """T1-3 (1c). section-checkpoint.py gather() captures phase, open findings,
    outstanding-review and a derived next_action -- and until this landed
    NOTHING read them: full_brief recomputes from scratch, so every one of those
    fields was dead weight and each resumed session re-derived facts already on
    disk."""
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=True)
        run = json.loads((root / ".claude/state/sequencer-run.json").read_text())
        _cp(root, {"file": run["file"], "section_idx": run.get("section_idx")},
            phase="SECTIONS", next_action="receive the outstanding review",
            outstanding_review=True, findings_recorded=3,
            open_findings=[{"loc": "src/a.c:10", "decision": "fix",
                            "title": "off-by-one"}],
            decisions_indexed=7)
        b = sbi.brief_for_event({"source": "compact"}, root)
        assert "resume checkpoint" in b, b
        assert "next action: receive the outstanding review" in b, b
        assert "OUTSTANDING" in b, b
        assert "src/a.c:10" in b, b
        assert "WHERE" in b, "the base brief must still be present"


def test_checkpoint_for_another_section_is_ignored():
    """A checkpoint bound to different work is WORSE than none -- it hands the
    resumed worker confident stale facts about a section it is not doing."""
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=True)
        _cp(root, {"file": "todo/other/TODO-9.md", "section_idx": 99},
            phase="SECTIONS", next_action="do something unrelated")
        b = sbi.brief_for_event({"source": "compact"}, root)
        assert "resume checkpoint" not in b, b
        assert "unrelated" not in b, b


def test_missing_or_malformed_checkpoint_leaves_the_brief_intact():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=True)
        b = sbi.brief_for_event({"source": "compact"}, root)
        assert b is not None and "resume checkpoint" not in b
        (root / ".claude/state/section-checkpoint.json").write_text("{not json")
        b2 = sbi.brief_for_event({"source": "compact"}, root)
        assert b2 is not None and "resume checkpoint" not in b2


if __name__ == "__main__":
    test_compact_active_returns_brief()
    test_resume_active_returns_brief()
    test_startup_returns_none()
    test_inactive_returns_none()
    test_checkpoint_reaches_the_resume_brief()
    test_checkpoint_for_another_section_is_ignored()
    test_missing_or_malformed_checkpoint_leaves_the_brief_intact()
    print("PASS: session_brief_inject")
