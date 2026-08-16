#!/usr/bin/env python3
"""Contract tests for run_phase_guard.py rollover + lifecycle routing.
Runs the guard from a FIXTURE repo (the hook resolves repo_root from its own
location) so real run state is never touched. (The structural-wait verbs were
removed 2026-07-11; reviews are polled in-session, not exit-and-waited.)
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
    # sequencer_triage.file_lifecycle now routes through the shared `## N.`
    # parser (v14 close-out): copy the parser chain it lazy-loads from
    # `<root>/scripts` so the stripped fixture can classify a mature file.
    (fx / "scripts/todo-graph").mkdir(parents=True)
    (fx / "scripts/todo_fence.py").write_bytes(
        (REPO / "scripts/todo_fence.py").read_bytes())
    (fx / "scripts/todo-graph/cache_schema.py").write_bytes(
        (REPO / "scripts/todo-graph/cache_schema.py").read_bytes())
    # Mature cursor TODO: both preamble stamps, one section.
    (fx / "todo/00-infrastructure/TODO-01-fixture.md").write_text(
        "# TODO-01 fixture\n\n"
        "> **Validated:** 2026-07-01 | fixture\n\n"
        "> **Gap-audited:** 2026-07-02 | fixture\n\n"
        "## 1. Section\n\n- [ ] item\n")
    return fx


def _guard(fx, *args, env_extra=None, stdin=None):
    # Strip the launcher's headless marker from the inherited env: a case must
    # opt IN to headless via HEADLESS, never inherit it. Running this suite from
    # inside a live run (the pre-arm health check does exactly that) otherwise
    # leaks OVERNIGHT_SEQUENCER_RUN=1 into the interactive cases, which the
    # guard then correctly blocks -- failing the test on a working guard.
    env = {k: v for k, v in os.environ.items()
           if k != "OVERNIGHT_SEQUENCER_RUN"}
    env.update(env_extra or {})
    return subprocess.run(
        [sys.executable, str(fx / ".claude/hooks/run_phase_guard.py"), *args],
        capture_output=True, text=True, env=env, input=stdin, cwd=str(fx))


HEADLESS = {"OVERNIGHT_SEQUENCER_RUN": "1"}


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
        assert r.returncode == 2 and "SEQ-LIFECYCLE" in r.stderr
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


def test_self_teardown_word_boundary():
    # _is_self_teardown must not let the substring "kill " alias inside the
    # word "skill ": a headless Bash command mentioning a skill and a .claude/
    # path was blocked 4x live (2026-07-16), yet the real pkill/killall/kill
    # teardown must stay blocked. Both directions pinned here.
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        _guard(fx, "start", "2026-07-16")

        def bash(cmd):
            return _guard(fx, "pretool", env_extra=HEADLESS,
                          stdin=json.dumps({"tool_name": "Bash",
                                            "tool_input": {"command": cmd}}))

        # "skill " + a .claude/ path is innocuous -> allowed even headless.
        r = bash('echo "the skill still documents X" && ls .claude/hooks')
        assert r.returncode == 0, r.stderr
        r = bash('git commit -m "docs: fix the sequencer skill doctrine" .claude/skills/x')
        assert r.returncode == 0, r.stderr

        # Real teardown of the headless claude must still be blocked.
        for cmd in ("pkill -f claude", "killall claude", "kill $(pgrep claude)"):
            r = bash(cmd)
            assert r.returncode == 2 and "SEQ-TEARDOWN" in r.stderr, (cmd, r.stderr)


if __name__ == "__main__":
    test_rollover_refused_on_dirty_or_unpushed()
    test_lifecycle_routing_block_and_override()
    test_self_teardown_word_boundary()
    print("PASS: phase-guard rollover/lifecycle/self-teardown")
