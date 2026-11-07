#!/usr/bin/env python3
"""A deny that refuses a pipeline skill stops `run.sh start`."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import skill_denies as sd  # noqa: E402

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


def test_run_refuses_before_starting():
    text = (HERE.parents[1] / "run.sh").read_text()
    check = text.index("skill_denies.py")
    assert "exit 1" in text[check:check + 300]
    assert check < text.index("run_launch resume")


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
