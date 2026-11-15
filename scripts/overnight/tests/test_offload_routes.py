#!/usr/bin/env python3
"""Every offload route the hooks print matches CLAUDE.md's executor split.

CLAUDE.md "Interactive offload" (2026-07-11): green mechanics never spend a
model. A build, test, lint or smoke run goes through
scripts/overnight/run-artifact.sh (diagnostic-digester digests a FAIL); CI
status is a direct `gh run list --json` query, and gh-query-runner is only for
`--log-failed` slices of a run already confirmed red; TODO structure starts
with the scripts/todo-graph tools. Both hooks that print routes were found
still sending build and test runs to checks-runner (operator, 2026-11-15).
"""
import importlib.util
import json
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOKS = REPO / ".claude" / "hooks"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HOOKS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


inline_churn_monitor = _load("inline_churn_monitor")
router = _load("interactive_offload_router")


def _hint(prompt):
    r = subprocess.run([sys.executable, str(HOOKS / "interactive_offload_router.py")],
                       input=json.dumps({"prompt": prompt, "session_id": f"t-{prompt}"}),
                       text=True, capture_output=True, timeout=30, cwd=str(REPO))
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)["systemMessage"] if r.stdout.strip() else ""


def test_no_route_names_a_retired_runner_for_green_mechanics():
    for _pat, _kind, target, _note in router._ROUTES:
        assert target != "checks-runner", "build/test runs go to run-artifact.sh"
    assert "checks-runner" not in inline_churn_monitor._MSG


def test_build_and_test_runs_route_to_run_artifact():
    for p in ["re-run the mm test suite", "run the build", "rerun lint"]:
        h = _hint(p)
        assert "scripts/overnight/run-artifact.sh" in h, (p, h)
        assert "Agent(subagent_type" not in h, (p, h)
    assert "run-artifact.sh" in inline_churn_monitor._MSG


def test_ci_status_is_a_direct_query():
    h = _hint("is the CI status green for the last push?")
    assert "gh run list" in h and "--json" in h, h
    assert "Agent(subagent_type" not in h, h
    msg = inline_churn_monitor._MSG
    assert "gh run list --json" in msg and "--log-failed" in msg


def test_pr_and_issue_state_is_not_sent_the_ci_command():
    for p in ["check issue 123 status", "show PR 42 state"]:
        h = _hint(p)
        assert "gh-query-runner" in h and "gh run list" not in h, (p, h)


def test_todo_structure_starts_with_the_scripts():
    assert "scripts/todo-graph" in inline_churn_monitor._MSG
    for _pat, _kind, target, note in router._ROUTES:
        if target == "todo-validation-mapper":
            assert "scripts/todo-graph" in note


def test_every_agent_route_names_a_real_agent():
    agents = {p.stem for p in (REPO / ".claude" / "agents").glob("*.md")} | {"Explore"}
    for _pat, kind, target, _note in router._ROUTES:
        assert kind in ("agent", "script"), kind
        if kind == "agent":
            assert target in agents, target


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("PASS: offload routes")
