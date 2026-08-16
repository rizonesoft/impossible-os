#!/usr/bin/env python3
# Protects the crash-aware review envelope (Tier-A cost fix, 2026-07-12):
# a Codex leg that crashes (rc != 0 / "app-server exited unexpectedly") must be
# reported so the runner re-dispatches ONLY that leg, never the whole 3-leg
# bundle. Regression here silently restores the full-bundle re-dispatch that
# was a top measured token sink.
import json, subprocess, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "review-envelope.py"


def _build(root: pathlib.Path, legs: dict) -> None:
    """legs: {kind: body_text}. Writes leg .out files + manifest.jsonl."""
    rev = root / ".claude" / "overnight" / "reviews"
    rev.mkdir(parents=True, exist_ok=True)
    lines = []
    for i, (kind, body) in enumerate(legs.items(), 1):
        out = rev / f"{kind}.out"
        out.write_text(body, encoding="utf-8")
        lines.append(json.dumps({"ts": i, "kind": kind, "todo": "todo/x.md",
                                 "jobId": f"j{i}", "logFile": str(out),
                                 "prompt_sha256": "z"}))
    (rev / "manifest.jsonl").write_text("\n".join(lines) + "\n", encoding="utf-8")


def _run(root: pathlib.Path):
    r = subprocess.run([sys.executable, str(SCRIPT), str(root)],
                       text=True, capture_output=True)
    return r.returncode, json.loads(r.stdout)


def test_crashed_leg_flagged_and_isolated():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        _build(root, {
            "adversarial": "[HIGH] finding\nTurn completed (rc=0)\n",
            "consistency": "codex app-server exited unexpectedly, rc=1\n"
                           "Turn completed (rc=1)\n",
            "perf": "ok\nTurn completed (rc=0)\n",
        })
        rc, env = _run(root)
        assert rc == 1, "crashed leg must make exit non-zero"
        assert env["all_complete"] is True, "all legs have a completion marker"
        assert env["all_clean"] is False, "a crashed leg is not clean"
        assert env["crashed"] == ["consistency"], env["crashed"]
        # ONLY the crashed leg is re-dispatched -- clean legs are reused.
        assert env["needs_redispatch"] == ["consistency"], env["needs_redispatch"]
        assert env["kinds"]["consistency"]["crashed"] is True
        assert env["kinds"]["consistency"]["rc"] == 1
        assert env["kinds"]["adversarial"]["crashed"] is False
        assert env["kinds"]["adversarial"]["rc"] == 0


def test_all_clean_exits_zero():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        _build(root, {
            "adversarial": "ok\nTurn completed (rc=0)\n",
            "consistency": "ok\nTurn completed (rc=0)\n",
            "perf": "ok\nTurn completed (rc=0)\n",
        })
        rc, env = _run(root)
        assert rc == 0, "all clean legs -> exit 0"
        assert env["all_clean"] is True
        assert env["crashed"] == []
        assert env["needs_redispatch"] == []


def test_missing_leg_needs_redispatch():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        _build(root, {
            "adversarial": "ok\nTurn completed (rc=0)\n",
            "perf": "ok\nTurn completed (rc=0)\n",
        })
        rc, env = _run(root)
        assert rc == 1
        assert "consistency" in env["missing"]
        assert "consistency" in env["needs_redispatch"]


def _run_args(root, *extra):
    r = subprocess.run([sys.executable, str(SCRIPT), str(root), *extra],
                       text=True, capture_output=True)
    return r.returncode, json.loads(r.stdout)


def test_todo_scope_excludes_other_sections():
    # E3: the manifest accumulates; --todo must NOT pull a different section's
    # (newer) review when scoped to this one.
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        rev = root / ".claude/overnight/reviews"
        rev.mkdir(parents=True)
        old = rev / "old.out"
        new = rev / "new.out"
        old.write_text("[CRITICAL] stale TODO-12\nTurn completed (rc=0)\n")
        new.write_text("[HIGH] fresh TODO-22\nTurn completed (rc=0)\n")
        (rev / "manifest.jsonl").write_text(
            json.dumps({"ts": 100, "kind": "adversarial", "todo": "todo/TODO-12.md",
                        "logFile": str(old)}) + "\n"
            + json.dumps({"ts": 200, "kind": "adversarial", "todo": "todo/TODO-22.md",
                          "logFile": str(new)}) + "\n")
        # Unscoped: newest (TODO-22) wins.
        _, env = _run_args(root, "--kinds", "adversarial")
        assert env["kinds"]["adversarial"]["artifact"].endswith("new.out")
        # Scoped to TODO-12: must pull the OLD one, not the newest.
        _, env = _run_args(root, "--kinds", "adversarial", "--todo", "todo/TODO-12.md")
        assert env["kinds"]["adversarial"]["artifact"].endswith("old.out"), env
        # Scoped to TODO-22: current section only.
        _, env = _run_args(root, "--kinds", "adversarial", "--todo", "todo/TODO-22.md")
        assert env["kinds"]["adversarial"]["artifact"].endswith("new.out"), env


def test_since_window_excludes_prior_round():
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        rev = root / ".claude/overnight/reviews"
        rev.mkdir(parents=True)
        (rev / "a.out").write_text("[HIGH] x\nTurn completed (rc=0)\n")
        (rev / "manifest.jsonl").write_text(
            json.dumps({"ts": 100, "kind": "adversarial", "todo": "todo/x.md",
                        "logFile": str(rev / "a.out")}) + "\n")
        _, env = _run_args(root, "--kinds", "adversarial", "--since", "150")
        assert "adversarial" in env["missing"], env  # ts=100 < 150 -> scoped out


def test_section_scope_excludes_other_sections_and_unattributed():
    # v14 close-out (2026-08-16): --todo alone ingested an EARLIER section's
    # legs from the same TODO. --section must exclude a different section AND
    # an entry with no section field (fail toward redispatch: one extra
    # dispatch, never another section's findings triaged as this one's).
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        rev = root / ".claude/overnight/reviews"
        rev.mkdir(parents=True)
        for name in ("s25.out", "s26.out", "nosec.out"):
            (rev / name).write_text("[HIGH] x\nTurn completed (rc=0)\n")
        (rev / "manifest.jsonl").write_text("\n".join([
            json.dumps({"ts": 100, "kind": "adversarial", "todo": "todo/x.md",
                        "section": 25, "logFile": str(rev / "s25.out")}),
            json.dumps({"ts": 200, "kind": "adversarial", "todo": "todo/x.md",
                        "section": 26, "logFile": str(rev / "s26.out")}),
            json.dumps({"ts": 300, "kind": "consistency", "todo": "todo/x.md",
                        "logFile": str(rev / "nosec.out")}),
        ]) + "\n")
        _, env = _run_args(root, "--kinds", "adversarial", "--todo",
                           "todo/x.md", "--section", "25")
        assert env["kinds"]["adversarial"]["artifact"].endswith("s25.out"), env
        _, env = _run_args(root, "--kinds", "adversarial", "--todo",
                           "todo/x.md", "--section", "26")
        assert env["kinds"]["adversarial"]["artifact"].endswith("s26.out"), env
        # REFUSAL: an unattributed leg never satisfies a section-scoped ask.
        _, env = _run_args(root, "--kinds", "consistency", "--todo",
                           "todo/x.md", "--section", "26")
        assert "consistency" in env["missing"], env


if __name__ == "__main__":
    test_crashed_leg_flagged_and_isolated()
    test_all_clean_exits_zero()
    test_missing_leg_needs_redispatch()
    test_todo_scope_excludes_other_sections()
    test_since_window_excludes_prior_round()
    test_section_scope_excludes_other_sections_and_unattributed()
    print("PASS: review-envelope")
