#!/usr/bin/env python3
"""The container tier runs in the image CI runs in, as CI's unprivileged uid."""
import pathlib
import re
import subprocess

import yaml

REPO = pathlib.Path(__file__).resolve().parents[3]
WF = (REPO / ".github/workflows/build.yml").read_text()
TIER = (REPO / "scripts/ci-container.sh").read_text()


def test_the_tier_runs_ci_s_image_as_ci_s_uid():
    """The image comes from build.yml's build job, so the tier cannot drift from CI's
    toolchain; it installs nothing; and it runs as uid 1001, as the jobs do."""
    assert '["jobs"]["build"]["container"]["image"]' in TIER
    image = yaml.safe_load(WF)["jobs"]["build"]["container"]["image"]
    assert re.fullmatch(r"ghcr\.io/[a-z0-9-]+/impossible-os-ci@sha256:[0-9a-f]{64}", image), image
    assert "apt-get" not in TIER and "ci_container_setup" not in TIER
    assert "-u 1001:1001" in TIER
    assert not (REPO / "scripts/ci_container_setup.py").exists(), "retired with the install steps"


def test_test_sh_hands_off_before_touching_this_tree():
    text = (REPO / "scripts/test.sh").read_text()
    assert text.index('exec bash "$PROJECT/scripts/ci-container.sh"') < text.index("# --- Step 1: Build ---")


def test_split_keeps_required_check_and_the_pack_still_runs():
    jobs = yaml.safe_load(WF)["jobs"]
    # The ruleset requires this name, and it must answer for both jobs even when one
    # fails or is skipped, or a failing pack could merge.
    gate = jobs["required-check"]
    assert gate["name"] == "Build Impossible OS"
    assert set(gate["needs"]) == {"build", "tooling-pack"}
    assert gate["if"] == "always()"
    script = gate["steps"][0]["run"]
    assert 'OS_RESULT" = success' in script and 'PACK_RESULT" = success' in script
    assert not [j for k, j in jobs.items() if k != "required-check"
                and j.get("name") == "Build Impossible OS"]
    names = [s.get("name") for s in jobs["tooling-pack"]["steps"]]
    assert "Run tooling regression pack" in names
    assert not [s for s in jobs["build"]["steps"] if s.get("name") == "Run tooling regression pack"]
    build_names = [s.get("name", "") for s in jobs["build"]["steps"]]
    step = "Verify toolchain sentinels and version floors"
    assert step in names and step in build_names, step
    assert jobs["tooling-pack"]["container"] == jobs["build"]["container"], "one image for both"
    run = [s for s in jobs["tooling-pack"]["steps"] if s.get("name") == "Run tooling regression pack"][0]
    assert run["env"]["TEST_TOOLING_SKIP_LSP_MCP"] == "1"


def test_container_gets_full_history():
    text = (REPO / "scripts/ci-container.sh").read_text()
    assert "git bundle create" in text and "git clone" in text
    assert "git archive" not in text, "a snapshot has no history for lint to judge against"


def test_shell_syntax():
    subprocess.run(["bash", "-n", str(REPO / "scripts/ci-container.sh")], check=True)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
