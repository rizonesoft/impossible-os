#!/usr/bin/env python3
"""The live-site check waits for a running deploy, and never shares its concurrency group."""
import json
import pathlib
import re
import shutil
import subprocess

import yaml

WF = pathlib.Path(__file__).resolve().parents[3] / ".github/workflows/site-live.yml"


def test_the_wait_comes_before_the_comparison():
    steps = [s.get("name", "") for s in yaml.safe_load(WF.read_text())["jobs"]["verify"]["steps"]]
    wait = steps.index("Wait for any Pages deploy to finish")
    assert wait < steps.index("Live site is byte-identical to main and the retained releases")


def test_refusal_no_shared_group_that_could_cancel_a_pending_deploy():
    wf = yaml.safe_load(WF.read_text())
    assert "concurrency" not in wf and "concurrency" not in wf["jobs"]["verify"]


def _busy(runs):
    step = next(s for s in yaml.safe_load(WF.read_text())["jobs"]["verify"]["steps"]
                if s.get("name") == "Wait for any Pages deploy to finish")
    jq = re.search(r"--jq '(.*?)'\)", step["run"], re.S).group(1)
    assert shutil.which("jq"), "jq is needed to read the wait's filter"
    done = subprocess.run(["jq", jq], input=json.dumps(runs), capture_output=True, text=True,
                          timeout=30, check=True)
    return int(done.stdout)


def test_a_run_stranded_behind_a_finished_deploy_does_not_hold_the_wait():
    """Run 43096022843 sat queued 16 h behind three successful deploys (2026-11-12)."""
    def run(status, at, conclusion=""):
        return {"status": status, "conclusion": conclusion, "createdAt": f"2026-11-12T{at}:00Z"}
    stranded, ok = run("queued", "01:00"), run("completed", "02:00", "success")
    assert _busy([ok, stranded]) == 0
    assert _busy([run("queued", "03:00"), ok, stranded]) == 1, "queued after the deploy"
    assert _busy([stranded]) == 1, "with no successful deploy, every unfinished run counts"
    active = run("in_progress", "01:00")
    for later in ("cancelled", "failure"):  # proves nothing about the site
        assert _busy([run("completed", "02:00", later), active]) == 1, later
        assert _busy([run("completed", "02:00", later), stranded]) == 1, later
    assert _busy([ok, active]) == 1, "a running deploy always counts"


def test_the_wait_ends_by_the_clock_before_its_step_times_out():
    step = next(s for s in yaml.safe_load(WF.read_text())["jobs"]["verify"]["steps"]
                if s.get("name") == "Wait for any Pages deploy to finish")
    budget = re.search(r"deadline=\$\(\( \$\(date \+%s\) \+ (\d+) \* 60 \)\)", step["run"])
    assert budget, "the loop is bounded by elapsed time, not an iteration count"
    query = re.search(r"timeout (\d+) gh run list", step["run"])
    assert query, "each query has its own deadline"
    worst = int(budget.group(1)) * 60 + int(query.group(1)) + 30  # one last query and sleep
    assert worst < step["timeout-minutes"] * 60, (worst, step["timeout-minutes"])


if __name__ == "__main__":
    test_the_wait_ends_by_the_clock_before_its_step_times_out()
    test_a_run_stranded_behind_a_finished_deploy_does_not_hold_the_wait()
    test_the_wait_comes_before_the_comparison()
    test_refusal_no_shared_group_that_could_cancel_a_pending_deploy()
    print("OK")
