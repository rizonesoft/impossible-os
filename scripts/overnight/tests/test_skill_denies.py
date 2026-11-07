#!/usr/bin/env python3
"""A deny that refuses a pipeline skill stops the arm and trips the breaker."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import skill_denies as sd  # noqa: E402

OUTCOME = HERE.parent / "run-outcome.py"
GUARD = HERE.parent.parent.parent / ".claude/hooks/run_phase_guard.py"


def test_matcher_prefix_semantics_measured_on_the_real_rule():
    assert sd.rule_matches("Skill(review *)", "review-todo-section")
    assert sd.rule_matches("Skill(review:*)", "review-todo-section")
    assert sd.rule_matches("Skill(review*)", "review-todo-section")
    assert sd.rule_matches("Skill", "review-todo-section")
    assert sd.rule_matches("Skill(review-todo-section)", "review-todo-section")
    # REFUSAL'S MIRROR: the narrowed rules the operator keeps must stay clear.
    assert not sd.rule_matches("Skill(review)", "review-todo-section")
    assert not sd.rule_matches("Skill(code-review *)", "review-todo-section")
    assert not sd.rule_matches("Skill(security-review)", "review-todo-section")
    assert not sd.rule_matches("Bash(rm *)", "review-todo-section")


def _fixture(d, rules):
    home = pathlib.Path(d) / "home"
    (home / ".claude").mkdir(parents=True)
    (home / ".claude/settings.json").write_text(
        json.dumps({"permissions": {"deny": rules}}))
    project = pathlib.Path(d) / "proj"
    (project / ".claude/hooks").mkdir(parents=True)
    (project / ".claude/hooks/run_phase_guard.py").write_text(GUARD.read_text())
    subprocess.run(["git", "init", "-q", str(project)], check=True)
    return home, project


def test_the_real_phase_table_is_what_gets_checked():
    skills = sd._pipeline_skills(HERE.parent.parent.parent)
    assert {"review-todo-section", "implement-todo-section",
            "validate-todo-file", "gap-audit-todo",
            "complete-todo-file"} <= skills, skills


def test_cli_refuses_the_old_rule_and_passes_the_narrowed_set():
    with tempfile.TemporaryDirectory() as d:
        home, project = _fixture(d, ["Skill(review *)", "Skill(code-review *)"])
        env = {**os.environ, "HOME": str(home)}
        r = subprocess.run([sys.executable, str(HERE.parent / "skill_denies.py"),
                            str(project)], env=env, capture_output=True, text=True)
        assert r.returncode == 1 and "review-todo-section" in r.stdout, r
        (home / ".claude/settings.json").write_text(json.dumps({"permissions": {
            "deny": ["Skill(review)", "Skill(code-review)", "Skill(code-review *)"]}}))
        r = subprocess.run([sys.executable, str(HERE.parent / "skill_denies.py"),
                            str(project)], env=env, capture_output=True, text=True)
        assert r.returncode == 0, r


def _outcome(project, home, rolling):
    state = project / ".claude/state"
    state.mkdir(parents=True, exist_ok=True)
    (state / "sequencer-run.json").write_text(
        json.dumps({"rollover": {"pending": rolling}}))
    r = subprocess.run(
        [sys.executable, str(OUTCOME), str(project), "--exit", "0",
         "--start-head", "aaa", "--end-head", "bbb", "--run-secs", "2000",
         "--now", "1000"],
        env={**os.environ, "HOME": str(home)}, capture_output=True, text=True)
    return json.loads(r.stdout)


def test_breaker_trips_on_a_denied_pipeline_skill_even_at_a_rollover():
    with tempfile.TemporaryDirectory() as d:
        home, project = _fixture(d, ["Skill(review *)"])
        for rolling in (True, False):
            got = _outcome(project, home, rolling)
            assert got["breaker"] is True and got["skill_denied"], got
            assert got["outcome"] == "skill-denied", got
            assert (project / ".claude/overnight/watchdog-backoff-until").exists()


def test_breaker_stays_quiet_without_the_deny():
    with tempfile.TemporaryDirectory() as d:
        home, project = _fixture(d, ["Skill(review)"])
        assert _outcome(project, home, True)["outcome"] == "checkpoint"
        assert _outcome(project, home, False)["outcome"] == "productive"


def test_arm_refuses_before_it_touches_anything_and_force_is_no_way_past():
    text = (HERE.parent.parent.parent
            / ".claude/skills/overnight-sequencer/arm-sequencer.sh").read_text()
    check = text.index('skill_denies.py" "$REPO_ROOT"')
    assert "exit 1" in text[check:check + 500]
    assert "ARM_FORCE" not in text[check:check + 500]
    assert check < text.index("reset-breaker.sh")


def test_status_page_is_refreshed_at_each_stop_path():
    launch = (HERE.parent / "overnight-launch.sh").read_text()
    after = launch[launch.index('echo "run outcome:'):]
    assert "run-status.py" in after[:600]
    arm = (HERE.parent.parent.parent
           / ".claude/skills/overnight-sequencer/arm-sequencer.sh").read_text()
    disarm = arm[arm.index('if [ "${1:-}" = "--disarm" ]'):]
    assert "run-status.py" in disarm[:1200]


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
