#!/usr/bin/env python3
"""CI installs LLVM from the Ubuntu archive on a pinned image. `ubuntu-latest` moved to
26.04 without a commit here, and that image has no apt-key, so the old apt.llvm.org setup failed every workflow before it built
anything. Control: each workflow must still install the pinned toolchain.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
FLOWS = ["build.yml", "release.yml", "visual-regression.yml"]


def test_no_removed_tooling_and_the_toolchain_is_still_installed():
    for name in FLOWS:
        text = (REPO / ".github/workflows" / name).read_text()
        assert "apt-key" not in text, name
        for pkg in ("clang-", "lld-"):
            assert pkg in text, (name, pkg)


def _steps(text):
    return re.findall(r"\n\s*- name: .*?(?=\n\s*- name:|\Z)", text, re.S)


def test_every_install_is_bounded_and_retried():
    """A stalled mirror used whole step timeouts four times on 2026-11-10. Every package install
    goes through scripts/ci/apt-install.sh (bounded download, retried) inside a step with its own
    timeout. The one exception installs GNU timeout itself, which the helper needs."""
    for path in sorted((REPO / ".github/workflows").glob("*.yml")):
        for step in _steps(path.read_text()):
            head = step.strip().splitlines()[0]
            if "apt-get install" in step:
                assert "name: Use GNU coreutils" in head, (path.name, head, "use scripts/ci/apt-install.sh")
                assert "-qq" not in step, (path.name, head, "keep apt output in the log")
            if "apt-get install" in step or "apt-install.sh" in step:
                assert re.search(r"^\s*timeout-minutes: \d+$", step, re.M), (path.name, head, "a hang must fail fast")
    helper = (REPO / "scripts/ci/apt-install.sh").read_text()
    assert "--download-only" in helper and "--no-download" in helper and "timeout -k" in helper


def _helper_worst_case_seconds():
    helper = (REPO / "scripts/ci/apt-install.sh").read_text()
    cap = {k: int(v) for k, v in re.findall(r"\$\{APT_(\w+):-(\d+)\}", helper)}
    grace = int(re.search(r"timeout -k (\d+)", helper).group(1))
    n = cap["INSTALL_ATTEMPTS"]
    pauses = sum(i * 5 for i in range(1, n))
    return n * (cap["UPDATE_SECONDS"] + cap["DOWNLOAD_SECONDS"] + 2 * grace) + pauses + cap["INSTALL_SECONDS"] + grace


def test_the_helper_worst_case_fits_every_step_and_job():
    """A step timeout shorter than the helper's worst case cancels it mid-recovery (Codex,
    2026-11-10): two stalls then a good download, killed during the install."""
    worst = _helper_worst_case_seconds()
    for path in sorted((REPO / ".github/workflows").glob("*.yml")):
        text = path.read_text()
        for job in re.split(r"\n(?=  [A-Za-z0-9_-]+:\n)", text.split("\njobs:\n", 1)[-1]):
            budgets = [int(re.search(r"^\s*timeout-minutes: (\d+)$", s, re.M).group(1))
                       for s in _steps(job) if "apt-install.sh" in s]
            if not budgets:
                continue
            name = job.strip().splitlines()[0]
            for b in budgets:
                assert b * 60 >= worst, (path.name, name, b, worst)
            job_budget = int(re.search(r"^    timeout-minutes: (\d+)$", job, re.M).group(1))
            assert job_budget >= sum(budgets) + 5, (path.name, name, job_budget, budgets)


def test_every_toolchain_job_uses_gnu_coreutils_first():
    """uutils `timeout -s KILL` leaves a stalled child running, so the gate's bounds fail."""
    for name in FLOWS + ["todo-graph.yml"]:
        text = (REPO / ".github/workflows" / name).read_text()
        jobs = text.count("actions/checkout")
        assert text.count("name: Use GNU coreutils") == jobs, (name, jobs)
        assert "coreutils-from-gnu coreutils-from-uutils-" in text, name
        if "name: Install LLVM" in text:
            assert text.index("Use GNU coreutils") < text.index("name: Install LLVM"), name


def test_setup_verify_refuses_a_non_gnu_timeout():
    with tempfile.TemporaryDirectory() as d:
        fake = pathlib.Path(d, "timeout")
        fake.write_text("#!/bin/sh\necho 'timeout (uutils coreutils) 0.10.0'\n")
        fake.chmod(0o755)
        env = dict(os.environ, PATH=f"{d}:{os.environ['PATH']}")
        r = subprocess.run(["bash", "scripts/setup.sh", "--verify"], cwd=REPO, env=env,
                           capture_output=True, text=True, timeout=120)
    assert r.returncode != 0 and "GNU timeout" in r.stdout, (r.returncode, r.stdout[-300:])
    r = subprocess.run(["bash", "scripts/setup.sh", "--verify"], cwd=REPO, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, ("a GNU host must verify clean", r.stdout[-400:])


def test_no_workflow_follows_a_floating_image():
    for path in (REPO / ".github/workflows").glob("*.yml"):
        text = path.read_text()
        assert "-latest" not in "".join(re.findall(r"runs-on:.*", text)), path.name
        assert re.findall(r"runs-on:\s*ubuntu-\d", text), (path.name, "no pinned image")


if __name__ == "__main__":
    test_no_removed_tooling_and_the_toolchain_is_still_installed()
    test_every_install_is_bounded_and_retried()
    test_the_helper_worst_case_fits_every_step_and_job()
    test_no_workflow_follows_a_floating_image()
    test_every_toolchain_job_uses_gnu_coreutils_first()
    test_setup_verify_refuses_a_non_gnu_timeout()
    print("PASS test_ci_toolchain_install")
    sys.exit(0)
