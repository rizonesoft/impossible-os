#!/usr/bin/env python3
"""The live-site check waits for a running deploy, and never shares its concurrency group."""
import pathlib

import yaml

WF = pathlib.Path(__file__).resolve().parents[3] / ".github/workflows/site-live.yml"


def test_the_wait_comes_before_the_comparison():
    steps = [s.get("name", "") for s in yaml.safe_load(WF.read_text())["jobs"]["verify"]["steps"]]
    wait = steps.index("Wait for any Pages deploy to finish")
    assert wait < steps.index("Live site is byte-identical to main and the retained releases")


def test_refusal_no_shared_group_that_could_cancel_a_pending_deploy():
    wf = yaml.safe_load(WF.read_text())
    assert "concurrency" not in wf and "concurrency" not in wf["jobs"]["verify"]


if __name__ == "__main__":
    test_the_wait_comes_before_the_comparison()
    test_refusal_no_shared_group_that_could_cancel_a_pending_deploy()
    print("OK")
