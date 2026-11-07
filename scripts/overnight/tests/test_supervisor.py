#!/usr/bin/env python3
"""Auto-undo, health and repair: the frozen part that recovers from everything else."""
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import supervisor as sup  # noqa: E402

DAY = 1_800_000_000


def _git(root, *a):
    subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t", *a],
                   check=True, capture_output=True)


def _head(root):
    return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True,
                          text=True).stdout.strip()


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
    return root, alerts


def test_a_machinery_commit_followed_by_two_empty_segments_is_reverted():
    with tempfile.TemporaryDirectory() as d:
        root, alerts = _fixture(d)
        start = _head(root)
        _commit(root, ".claude/hooks/x.py", "bad\n")
        h = sup.segment_end(root, start, "", DAY)
        assert h["machinery_commits_pending"] == 1 and not h["machinery_reverts"]
        h = sup.segment_end(root, _head(root), "", DAY + 60)
        assert not h["machinery_reverts"]
        h = sup.segment_end(root, _head(root), "", DAY + 120)
        assert len(h["machinery_reverts"]) == 1, h
        assert not (root / ".claude/hooks/x.py").exists()
        assert any("reverted" in a for a in alerts), alerts


def test_refusal_a_change_the_run_worked_with_is_not_reverted():
    with tempfile.TemporaryDirectory() as d:
        root, _ = _fixture(d)
        start = _head(root)
        _commit(root, ".claude/hooks/x.py", "good\n")
        sup.segment_end(root, start, "", DAY)
        for i in range(1, 4):
            mark = _head(root)
            _commit(root, "src/a.c", f"{i}\n")
            h = sup.segment_end(root, mark, "", DAY + i * 60)
        assert h["machinery_commits_pending"] == 0 and not h["machinery_reverts"]
        assert (root / ".claude/hooks/x.py").exists()


def test_a_failed_undo_alerts_and_never_raises():
    with tempfile.TemporaryDirectory() as d:
        root, alerts = _fixture(d)
        start = _head(root)
        _commit(root, ".claude/hooks/x.py", "one\n")
        sup.segment_end(root, start, "", DAY)
        sup.segment_end(root, _head(root), "", DAY + 60)
        # A later edit to the same lines makes the revert conflict, without producing.
        mark = _head(root)
        _commit(root, ".claude/hooks/x.py", "two\n")
        h = sup.segment_end(root, mark, "", DAY + 120)
        assert any("Could not undo" in a for a in alerts), alerts
        assert not (pathlib.Path(root) / ".git/REVERT_HEAD").exists()
        assert h["machinery_commits_pending"] >= 1


def test_a_dirty_tree_keeps_the_undo_for_later():
    with tempfile.TemporaryDirectory() as d:
        root, alerts = _fixture(d)
        start = _head(root)
        _commit(root, ".claude/hooks/x.py", "bad\n")
        sup.segment_end(root, start, "", DAY)
        sup.segment_end(root, _head(root), "", DAY + 60)
        (root / "dirty").write_text("x")
        h = sup.segment_end(root, _head(root), "", DAY + 120)
        assert h["machinery_commits_pending"] == 1 and not h["machinery_reverts"]
        assert any("dirty" in a for a in alerts), alerts
        (root / "dirty").unlink()
        h = sup.segment_end(root, _head(root), "", DAY + 180)
        assert len(h["machinery_reverts"]) == 1, h


def test_health_counts_ships_and_cost_and_flags_a_stall():
    with tempfile.TemporaryDirectory() as d:
        root, alerts = _fixture(d)
        report = root / "report.log"
        report.write_text("  TOTAL      $12.50\n")
        start = _head(root)
        _commit(root, "todo/T.md", "> **Verified:** 2026-11-07\n")
        h = sup.segment_end(root, start, str(report), DAY)
        assert h["sections_shipped_today"] == 1 and h["cost_per_section"] == 12.5, h
        assert not (root / ".claude/overnight/repair-needed").exists()
        h = sup.segment_end(root, _head(root), str(report), DAY + 7 * 3600)
        assert h["repair_needed"] is True and h["hours_since_ship"] == 7.0, h
        assert any("Nothing has shipped" in a for a in alerts), alerts
        assert json.loads((root / ".claude/overnight/run-health.json").read_text())["repair_needed"]
        mark = _head(root)
        _commit(root, "todo/T.md", "> **Verified:** 2026-11-07\n> **Quality reviewed:** x\n")
        h = sup.segment_end(root, mark, "", DAY + 8 * 3600)
        assert h["repair_needed"] is False, h


def test_a_section_far_above_average_cost_alerts():
    with tempfile.TemporaryDirectory() as d:
        root, alerts = _fixture(d)
        report = root / "report.log"
        for i, cost in enumerate(("10.00", "100.00")):
            report.write_text(f"  TOTAL   ${cost}\n")
            mark = _head(root)
            _commit(root, "todo/T.md", "".join(f"> **Verified:** {n}\n" for n in range(i + 1)))
            sup.segment_end(root, mark, str(report), DAY + i * 60)
        assert any("Cost per section" in a for a in alerts), alerts


def test_launcher_and_gate_are_wired():
    root = HERE.parents[2]
    launch = (root / "scripts/overnight/overnight-launch.sh").read_text()
    end = launch[launch.index('echo "run outcome:'):]
    assert "supervisor.py" in end and "|| true" in end[end.index("supervisor.py"):][:200]
    assert "repair-needed" in launch
    gate = (root / ".claude/hooks/gate.py").read_text()
    assert "scripts/overnight/supervisor.py" in gate and '.claude/overnight/gate-log.jsonl' in gate


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
