#!/usr/bin/env python3
"""Auto-undo and health: the frozen part that recovers from everything else."""
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import supervisor as sup  # noqa: E402

T0 = 1_800_000_000
HOUR = 3600


def _git(root, *a):
    subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t", *a],
                   check=True, capture_output=True)


def _commit(root, path, text):
    p = pathlib.Path(root) / path
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    _git(root, "add", "-A")
    _git(root, "commit", "-qm", f"edit {path}")


def _fixture(d):
    root = pathlib.Path(d)
    _git(root, "init", "-q")
    (root / ".gitignore").write_text(".claude/overnight/\n")
    _commit(root, "README", "x\n")
    alerts = []
    sup.alert = lambda r, m: alerts.append(m)
    sup.PUSH_TIMEOUT = 30
    sup.check(root, False, T0)  # records the starting head
    return root, alerts


def test_a_machinery_commit_nothing_worked_with_is_reverted_when_idle():
    with tempfile.TemporaryDirectory() as d:
        root, alerts = _fixture(d)
        _commit(root, ".claude/hooks/x.py", "bad\n")
        h = sup.check(root, True, T0 + 60)
        assert h["machinery_commits_pending"] == 1 and not h["machinery_reverts"], h
        h = sup.check(root, True, T0 + 3 * HOUR + 60)
        assert len(h["machinery_reverts"]) == 1, h
        assert not (root / ".claude/hooks/x.py").exists()
        assert any("reverted" in a for a in alerts), alerts


def test_refusal_a_working_session_never_has_the_tree_changed_underneath_it():
    with tempfile.TemporaryDirectory() as d:
        root, _ = _fixture(d)
        _commit(root, ".claude/hooks/x.py", "bad\n")
        sup.check(root, False, T0 + 60)
        h = sup.check(root, False, T0 + 50 * HOUR)
        assert not h["machinery_reverts"] and h["machinery_commits_pending"] == 1, h
        assert (root / ".claude/hooks/x.py").exists()


def test_refusal_a_change_the_run_worked_with_is_not_reverted():
    with tempfile.TemporaryDirectory() as d:
        root, _ = _fixture(d)
        _commit(root, ".claude/hooks/x.py", "good\n")
        sup.check(root, True, T0 + 60)
        _commit(root, "src/a.c", "1\n")
        h = sup.check(root, True, T0 + 5 * HOUR)
        assert h["machinery_commits_pending"] == 0 and not h["machinery_reverts"], h
        assert (root / ".claude/hooks/x.py").exists()


def test_a_fix_on_top_of_a_bad_commit_undoes_as_one_batch():
    # Review, 2026-11-07: undoing only the fix would have restored the bug.
    with tempfile.TemporaryDirectory() as d:
        root, _ = _fixture(d)
        _commit(root, ".claude/hooks/x.py", "bad\n")
        sup.check(root, True, T0 + 60)
        _commit(root, ".claude/hooks/x.py", "fixed\n")
        sup.check(root, True, T0 + 2 * HOUR)
        h = sup.check(root, True, T0 + 3 * HOUR + 120)
        assert len(h["machinery_reverts"]) == 2 and h["machinery_commits_pending"] == 0, h
        assert not (root / ".claude/hooks/x.py").exists()


def test_a_failed_undo_alerts_keeps_its_commit_and_never_raises():
    with tempfile.TemporaryDirectory() as d:
        root, alerts = _fixture(d)
        _commit(root, ".claude/hooks/x.py", "one\n")
        sup.check(root, True, T0 + 60)
        # A human edit landing outside the record makes the revert conflict.
        (root / ".claude/hooks/x.py").write_text("two\n")
        _git(root, "add", "-A")
        _git(root, "commit", "-qm", "edit")
        state = root / ".claude/overnight/supervisor-state.json"
        data = json.loads(state.read_text())
        data["head"] = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                                      capture_output=True, text=True).stdout.strip()
        state.write_text(json.dumps(data))
        h = sup.check(root, True, T0 + 3 * HOUR + 120)
        assert any("Could not undo" in a for a in alerts), alerts
        assert not (root / ".git/REVERT_HEAD").exists()
        assert h["machinery_commits_pending"] == 1, h


def test_a_dirty_tree_keeps_the_undo_for_later():
    with tempfile.TemporaryDirectory() as d:
        root, alerts = _fixture(d)
        _commit(root, ".claude/hooks/x.py", "bad\n")
        sup.check(root, True, T0 + 60)
        (root / "dirty").write_text("x")
        h = sup.check(root, True, T0 + 4 * HOUR)
        assert h["machinery_commits_pending"] == 1 and not h["machinery_reverts"], h
        assert any("dirty" in a for a in alerts), alerts
        (root / "dirty").unlink()
        h = sup.check(root, True, T0 + 5 * HOUR)
        assert len(h["machinery_reverts"]) == 1, h


def test_health_counts_ships_and_flags_a_stall_and_clears_it():
    with tempfile.TemporaryDirectory() as d:
        root, alerts = _fixture(d)
        _commit(root, "todo/T.md", "> **Verified:** 2026-11-07\n")
        h = sup.check(root, False, T0 + 60)
        assert h["sections_shipped_today"] == 1, h
        assert not h["repair_needed"]
        h = sup.check(root, False, T0 + 7 * HOUR)
        assert h["repair_needed"] is True and h["hours_since_ship"] > 6, h
        assert any("Nothing has shipped" in a for a in alerts), alerts
        assert json.loads((root / ".claude/overnight/run-health.json").read_text())["repair_needed"]
        _commit(root, "todo/T.md", "> **Verified:** 2026-11-07\n> **Quality reviewed:** x\n")
        h = sup.check(root, False, T0 + 8 * HOUR)
        assert h["repair_needed"] is False, h


def test_health_reports_no_dollars():
    with tempfile.TemporaryDirectory() as d:
        root, _ = _fixture(d)
        assert not [k for k in sup.check(root, False, T0 + 60) if "cost" in k]


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
