#!/usr/bin/env python3
"""Contract tests for run_phase_guard.py structural waiting + rollover +
lifecycle routing (2026-07-11). Runs the guard from a FIXTURE repo (the hook
resolves repo_root from its own location) so real run state is never touched.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent  # scripts/overnight/tests -> repo root


def _mk_fixture(d: pathlib.Path) -> pathlib.Path:
    fx = d / "fx"
    (fx / ".claude/hooks").mkdir(parents=True)
    (fx / ".claude/state").mkdir(parents=True)
    (fx / "todo/00-infrastructure").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", str(fx)], check=True)
    for hook in ("run_phase_guard.py", "sequencer_triage.py"):
        (fx / ".claude/hooks" / hook).write_bytes(
            (REPO / ".claude/hooks" / hook).read_bytes())
    # Mature cursor TODO: both preamble stamps, one section.
    (fx / "todo/00-infrastructure/TODO-01-fixture.md").write_text(
        "# TODO-01 fixture\n\n"
        "> **Validated:** 2026-07-01 | fixture\n\n"
        "> **Gap-audited:** 2026-07-02 | fixture\n\n"
        "## 1. Section\n\n- [ ] item\n")
    return fx


def _guard(fx, *args, env_extra=None, stdin=None):
    env = {**os.environ, **(env_extra or {})}
    return subprocess.run(
        [sys.executable, str(fx / ".claude/hooks/run_phase_guard.py"), *args],
        capture_output=True, text=True, env=env, input=stdin, cwd=str(fx))


HEADLESS = {"OVERNIGHT_SEQUENCER_RUN": "1"}


def test_wait_declare_ready_wake():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        _guard(fx, "start", "2026-07-11")
        art = fx / "task.output"
        art.write_text("dispatch running...\n")
        # declare (watcher script absent in fixture -> pid 0, harmless)
        r = _guard(fx, "wait", "3600", "codex verdict", str(art), "Turn completed")
        assert r.returncode == 0, r.stderr
        assert "WAITING_REVIEW declared" in r.stderr
        # still waiting -> rc 3
        r = _guard(fx, "wait-ready")
        assert r.returncode == 3, (r.returncode, r.stdout)
        assert json.loads(r.stdout)["waiting"] is True
        # Stop hook: unsatisfied wait -> allow (rc 0)
        r = _guard(fx, "stop", env_extra=HEADLESS)
        # needs armed marker for the wait branch to be reached
        (fx / ".claude/state/sequencer-armed").write_text("")
        r = _guard(fx, "stop", env_extra=HEADLESS)
        assert r.returncode == 0, (r.returncode, r.stderr)
        assert "WAITING_REVIEW" in r.stderr
        # artifact completes -> wait-ready rc 0, Stop now BLOCKS (read verdict)
        art.write_text("dispatch running...\nTurn completed after 200s\n")
        r = _guard(fx, "wait-ready")
        assert r.returncode == 0 and json.loads(r.stdout)["ready"] is True
        r = _guard(fx, "stop", env_extra=HEADLESS)
        assert r.returncode == 2 and "already complete" in r.stderr
        # wake consumes the wait and leaves the one-shot note
        r = _guard(fx, "wake")
        assert r.returncode == 0
        woke = json.loads(r.stdout)["woke_from_wait"]
        assert woke and woke["ready"] is True
        state = json.loads((fx / ".claude/state/sequencer-run.json").read_text())
        assert "waiting" not in state and state.get("woke_from_wait")


def test_wait_refused_when_already_complete():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        _guard(fx, "start", "2026-07-11")
        art = fx / "done.output"
        art.write_text("Turn completed\n")
        r = _guard(fx, "wait", "3600", "already done", str(art), "Turn completed")
        assert r.returncode == 1 and "REFUSED" in r.stderr


def test_rollover_refused_on_dirty_or_unpushed():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        _guard(fx, "start", "2026-07-11")
        subprocess.run(["git", "-C", str(fx), "add", "-A"], check=True,
                       capture_output=True)
        subprocess.run(["git", "-C", str(fx), "-c", "user.email=t@t",
                        "-c", "user.name=t", "commit", "-qm", "init"],
                       check=True, capture_output=True)
        # no upstream + no build receipt -> REFUSED with reasons
        r = _guard(fx, "rollover")
        assert r.returncode == 1, r.stderr
        assert "REFUSED" in r.stderr
        assert "upstream" in r.stderr or "pushed" in r.stderr
        # and the Stop hook does NOT honor a rollover that never verified
        (fx / ".claude/state/sequencer-armed").write_text("")
        r = _guard(fx, "stop", env_extra=HEADLESS)
        assert r.returncode == 2


def test_lifecycle_routing_block_and_override():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        _guard(fx, "start", "2026-07-11")
        _guard(fx, "cursor", "00-infrastructure",
               "todo/00-infrastructure/TODO-01-fixture.md")
        _guard(fx, "phase", "VALIDATE")
        payload = json.dumps({"tool_name": "Skill",
                              "tool_input": {"skill": "validate-todo-file"}})
        # mature file (both stamps) -> Stage 1-2 skill blocked
        r = _guard(fx, "pretool", env_extra=HEADLESS, stdin=payload)
        assert r.returncode == 2 and "stages_1_2_done" in r.stderr
        # relifecycle override -> allowed once, then consumed
        r = _guard(fx, "relifecycle", "new section 2 appeared in fixture")
        assert r.returncode == 0
        r = _guard(fx, "pretool", env_extra=HEADLESS, stdin=payload)
        assert r.returncode == 0, r.stderr
        state = json.loads((fx / ".claude/state/sequencer-run.json").read_text())
        assert "lifecycle_override" not in state
        r = _guard(fx, "pretool", env_extra=HEADLESS, stdin=payload)
        assert r.returncode == 2  # consumed: blocked again
        # interactive session never blocked
        r = _guard(fx, "pretool", stdin=payload)
        assert r.returncode == 0


if __name__ == "__main__":
    test_wait_declare_ready_wake()
    test_wait_refused_when_already_complete()
    test_rollover_refused_on_dirty_or_unpushed()
    test_lifecycle_routing_block_and_override()
    print("PASS: phase-guard wait/rollover/lifecycle")
