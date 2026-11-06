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
        assert "apt-get install" in text, name
        step = re.search(r"- name: Install LLVM.*?(?=\n\s*- name:|\n\s*#\s*──|\Z)", text, re.S).group(0)
        assert re.search(r"^\s*timeout-minutes: \d+$", step, re.M), (name, "a hang must fail fast")
        assert "-qq" not in step.split("apt-get install")[1].split("\\")[0], (name, "keep apt output in the log")


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
    test_no_workflow_follows_a_floating_image()
    test_every_toolchain_job_uses_gnu_coreutils_first()
    test_setup_verify_refuses_a_non_gnu_timeout()
    print("PASS test_ci_toolchain_install")
    sys.exit(0)
