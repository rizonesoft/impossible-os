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
    """Run 43096022843 sat queued 16 h behind three finished deploys (2026-11-12)."""
    old = {"status": "queued", "createdAt": "2026-11-11T20:26:02Z"}
    done = {"status": "completed", "createdAt": "2026-11-12T02:04:29Z"}
    assert _busy([done, old]) == 0
    assert _busy([{"status": "in_progress", "createdAt": "2026-11-12T03:00:00Z"}, done, old]) == 1
    assert _busy([old]) == 1, "with no finished deploy, every unfinished run counts"


if __name__ == "__main__":
    test_a_run_stranded_behind_a_finished_deploy_does_not_hold_the_wait()
    test_the_wait_comes_before_the_comparison()
    test_refusal_no_shared_group_that_could_cancel_a_pending_deploy()
    print("OK")
